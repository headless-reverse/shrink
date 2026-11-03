#include "shrink_engine.hpp"
#include <cctype>
#include <array>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <cerrno>
#include <algorithm>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <ctime>

namespace shrink {

namespace {

inline bool is_ident(char c) noexcept {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline size_t count_lines(std::string_view s) noexcept {
	if (s.empty()) return 0;
	size_t lines = 0;
	for (char c : s) {
		if (c == '\n') ++lines;
	}
	return lines + (s.back() == '\n' ? 0 : 1);
}

void split_excluded_code_lines(std::vector<Token>& tokens, std::span<const size_t> excluded_lines) {
	if (excluded_lines.empty()) return;
	std::vector<Token> split;
	size_t total_lines = 0;
	for (const Token& token : tokens) total_lines += static_cast<size_t>(std::count(token.text().begin(), token.text().end(), '\n')) + 1;
	size_t split_budget = std::min(excluded_lines.size(), total_lines);
	size_t reserve_count = tokens.size();
	if (split_budget <= (std::numeric_limits<size_t>::max() - reserve_count) / 2) reserve_count += split_budget * 2;
	split.reserve(reserve_count);
	size_t line = 1;
	for (const Token& token : tokens) {
		const std::string_view text = token.text();
		if (token.type != TokenType::Code) {
			size_t newlines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
			size_t end_line = line + newlines - (newlines && !text.empty() && text.back() == '\n' ? 1 : 0);
			auto first = std::lower_bound(excluded_lines.begin(), excluded_lines.end(), line);
			Token copy = token;
			copy.excluded = first != excluded_lines.end() && *first <= end_line;
			split.push_back(std::move(copy));
			line += newlines;
			continue;
		}
		size_t pos = 0, regular_start = 0;
		while (pos < text.size()) {
			size_t end = text.find('\n', pos);
			end = end == std::string_view::npos ? text.size() : end + 1;
			if (std::binary_search(excluded_lines.begin(), excluded_lines.end(), line)) {
				if (regular_start < pos) split.push_back({TokenType::Code, text.substr(regular_start, pos - regular_start)});
				Token excluded{TokenType::Code, text.substr(pos, end - pos)};
				excluded.excluded = true;
				split.push_back(std::move(excluded));
				regular_start = end;
			}
			if (text[end - 1] == '\n') ++line;
			pos = end;
		}
		if (regular_start < text.size()) split.push_back({TokenType::Code, text.substr(regular_start)});
	}
	tokens = std::move(split);
}

inline size_t skip_escaped_newline(std::string_view s, size_t i) noexcept {
	if (i + 1 < s.size() && s[i] == '\\') {
		if (s[i + 1] == '\n') return 2;
		if (i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') return 3;
	}
	return 0;
}

inline bool is_boundary(std::string_view s, size_t pos, size_t len) noexcept {
	if (pos > 0 && is_ident(s[pos - 1])) return false;
	if (pos + len < s.size() && is_ident(s[pos + len])) return false;
	return true;
}

std::string gitignore_regex(std::string_view pattern, bool anchored) {
	std::string out = anchored ? "^" : "(?:^|.*/)";
	for (size_t i = 0; i < pattern.size(); ++i) {
		const char c = pattern[i];
		if (c == '\\' && i + 1 < pattern.size()) {
			const char literal = pattern[++i];
			if (std::string_view(".()+|{}[]^$\\").find(literal) != std::string_view::npos) out.push_back('\\');
			out.push_back(literal);
		} else if (c == '*') {
			if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
				++i;
				if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
					++i;
					out += "(?:.*/)?";
				} else out += ".*";
			} else out += "[^/]*";
		} else if (c == '[') {
			size_t close = i + 1;
			if (close < pattern.size() && pattern[close] == '!') ++close;
			if (close < pattern.size() && pattern[close] == ']') ++close;
			while (close < pattern.size() && pattern[close] != ']') ++close;
			if (close == pattern.size()) out += "\\[";
			else {
				out.push_back('[');
				size_t member = i + 1;
				if (member < close && pattern[member] == '!') { out.push_back('^'); ++member; }
				for (; member < close; ++member) {
					if (pattern[member] == '\\' && member + 1 < close) ++member;
					out.push_back(pattern[member]);
				}
				out.push_back(']');
				i = close;
			}
		} else if (c == '?') out += "[^/]";
		else {
			if (std::string_view(".()+|{}[]^$\\").find(c) != std::string_view::npos) out.push_back('\\');
			out.push_back(c);
		}
	}
	out += "(/.*)?$";
	return out;
}

}

void GitIgnoreEngine::add_rule(std::string_view raw) {
	while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.remove_prefix(1);
	while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) raw.remove_suffix(1);
	if (raw.empty() || raw.front() == '#') return;

	GitIgnoreRule r;
	if (raw.front() == '!') {
		r.is_negation = true;
		raw.remove_prefix(1);
	}

	if (raw.ends_with('/')) {
		r.dir_only = true;
		raw.remove_suffix(1);
	}

	if (raw.front() == '/') {
		r.anchored = true;
		raw.remove_prefix(1);
	} else if (raw.find('/') == std::string_view::npos) {
		r.anchored = false;
	} else {
		r.anchored = true;
	}

	if (raw.empty()) return;
	r.pattern = std::string(raw);
	r.compiled_re = std::make_unique<re2::RE2>(gitignore_regex(r.pattern, r.anchored));
	if (!r.compiled_re->ok()) return;
	rules.push_back(std::move(r));
}

void GitIgnoreEngine::load_file(const std::filesystem::path& path) {
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) return;
	struct stat st;
	if (::fstat(fd, &st) < 0 || st.st_size == 0) {
		::close(fd);
		return;
	}
	std::string content(static_cast<size_t>(st.st_size), '\0');
	ssize_t read_bytes = ::read(fd, content.data(), content.size());
	::close(fd);
	if (read_bytes <= 0) return;

	size_t pos = 0;
	while (pos < content.size()) {
		size_t end = content.find('\n', pos);
		if (end == std::string::npos) end = content.size();
		add_rule(std::string_view(content.data() + pos, end - pos));
		pos = end + 1;
	}
}

bool GitIgnoreEngine::is_ignored(std::string_view rel_path, bool is_dir) const noexcept {
	if (rel_path == ".git" || rel_path.starts_with(".git/")) return true;
	bool ignored = false;
	bool parent_locked_out = false;
	for (const auto& r : rules) {
		if (!r.compiled_re) continue;
		std::string suffix;
		const bool match = re2::RE2::FullMatch(std::string(rel_path), *r.compiled_re, &suffix)
			&& (!r.dir_only || is_dir || !suffix.empty());

		if (match) {
			if (r.is_negation) {
				if (!parent_locked_out) ignored = false;
			} else {
				ignored = true;
				if (rel_path.find('/') != std::string_view::npos && !suffix.empty()) parent_locked_out = true;
			}
		}
	}
	return ignored;
}

namespace {

std::vector<Token> tokenize_cpp(std::string_view src, std::span<const size_t> excluded_lines, bool java = false) {
	std::vector<Token> tokens;
	tokens.reserve(src.size() / 16);
	size_t i = 0, n = src.size(), code_start = 0;
	auto flush_code = [&](size_t start, size_t end) {
		if (end > start) {
			tokens.push_back({TokenType::Code, src.substr(start, end - start), {}, false});
		}
	};
	while (i < n) {
		char c = src[i];
		char next = (i + 1 < n) ? src[i + 1] : '\0';
		if (c == '#') {
			size_t back = i;
			bool only_spaces = true;
			while (back > 0) {
				--back;
				if (src[back] == '\n') break;
				if (!std::isspace(static_cast<unsigned char>(src[back]))) {
					only_spaces = false;
					break;
				}
			}
			if (only_spaces) {
				flush_code(code_start, i);
				size_t start = i++;
				while (i < n) {
					size_t esc = skip_escaped_newline(src, i);
					if (esc > 0) { i += esc; continue; }
					if (src[i] == '\n') break;
					if (src[i] == '/' && i + 1 < n && (src[i + 1] == '/' || src[i + 1] == '*')) break;
					if (src[i] == '"') {
						++i;
						while (i < n && src[i] != '"' && src[i] != '\n') {
							size_t esc2 = skip_escaped_newline(src, i);
							if (esc2 > 0) i += esc2;
							else if (src[i] == '\\' && i + 1 < n) i += 2;
							else ++i;
						}
						if (i < n && src[i] == '"') ++i;
						continue;
					}
					++i;
				}
				tokens.push_back({TokenType::Preprocessor, src.substr(start, i - start), {}, false});
				code_start = i;
				continue;
			}
		}
		if (c == 'R' && next == '"' && (i == 0 || !is_ident(src[i - 1]))) {
			size_t probe = i + 2;
			size_t delim_start = probe;
			while (probe < n && src[probe] != '(' && probe - delim_start <= 16) ++probe;
			if (probe < n && src[probe] == '(') {
				flush_code(code_start, i);
				std::string_view delim = src.substr(delim_start, probe - delim_start);
				std::string term = ")" + std::string(delim) + "\"";
				size_t end = src.find(term, probe + 1);
				i = (end == std::string_view::npos) ? n : end + term.size();
				tokens.push_back({TokenType::RawString, src.substr(delim_start - 2, i - (delim_start - 2)), {}, false});
				code_start = i;
				continue;
			}
		}
		if (c == '"') {
			flush_code(code_start, i);
			size_t start = i;
			if (java && i + 2 < n && src[i + 1] == '"' && src[i + 2] == '"') {
				i += 3;
				while (i + 2 < n && src.substr(i, 3) != "\"\"\"") ++i;
				if (i + 2 < n) i += 3;
				tokens.push_back({TokenType::String, src.substr(start, i - start), {}, false});
				code_start = i;
				continue;
			}
			++i;
			while (i < n) {
				size_t esc = skip_escaped_newline(src, i);
				if (esc > 0) i += esc;
				else if (src[i] == '\\' && i + 1 < n) i += 2;
				else if (src[i] == '"') { ++i; break; }
				else ++i;
			}
			tokens.push_back({TokenType::String, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		if (c == '\'' && (i == 0 || !std::isxdigit(static_cast<unsigned char>(src[i - 1])))) {
			flush_code(code_start, i);
			size_t start = i++;
			while (i < n) {
				size_t esc = skip_escaped_newline(src, i);
				if (esc > 0) i += esc;
				else if (src[i] == '\\' && i + 1 < n) i += 2;
				else if (src[i] == '\'') { ++i; break; }
				else if (src[i] == '\n') break;
				else ++i;
			}
			tokens.push_back({TokenType::Char, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		if (c == '/' && (next == '/' || next == '*')) {
			flush_code(code_start, i);
			size_t start = i;
			if (next == '/') {
				i += 2;
				while (i < n) {
					size_t esc = skip_escaped_newline(src, i);
					if (esc > 0) i += esc;
					else if (src[i] == '\n') break;
					else ++i;
				}
			} else {
				i += 2;
				while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
				if (i + 1 < n) i += 2;
			}
			tokens.push_back({TokenType::Comment, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		++i;
	}
	flush_code(code_start, n);
	split_excluded_code_lines(tokens, excluded_lines);
	return tokens;
}

std::vector<Token> tokenize_xml(std::string_view src, std::span<const size_t> excluded_lines) {
	std::vector<Token> tokens;
	size_t i = 0, codeStart = 0;
	while (i < src.size()) {
		if (i + 3 < src.size() && src.substr(i, 4) == "<!--") {
			if (i > codeStart) tokens.push_back({TokenType::Code, src.substr(codeStart, i - codeStart), {}, false});
			const size_t commentStart = i;
			size_t end = src.find("-->", i + 4);
			i = end == std::string_view::npos ? src.size() : end + 3;
			tokens.push_back({TokenType::Comment, src.substr(commentStart, i - commentStart), {}, false});
			codeStart = i;
		} else if (src[i] == '<') {
			char quote = '\0';
			size_t end = i + 1;
			for (; end < src.size(); ++end) {
				if (quote) { if (src[end] == quote) quote = '\0'; }
				else if (src[end] == '\'' || src[end] == '"') quote = src[end];
				else if (src[end] == '>') break;
			}
			if (end < src.size()) {
				if (i > codeStart) tokens.push_back({TokenType::Code, src.substr(codeStart, i - codeStart), {}, false});
				tokens.push_back({TokenType::XmlTag, src.substr(i, end - i + 1), {}, false});
				i = end + 1;
				codeStart = i;
			} else ++i;
		} else {
			++i;
		}
	}
	if (codeStart < src.size()) tokens.push_back({TokenType::Code, src.substr(codeStart), {}, false});
	split_excluded_code_lines(tokens, excluded_lines);
	return tokens;
}

std::string java_code_mask(std::string_view source) {
	std::string mask(source);
	for (size_t i = 0; i < source.size();) {
		size_t end = i;
		if (source[i] == '"' && i + 2 < source.size() && source.substr(i, 3) == "\"\"\"") {
			end = source.find("\"\"\"", i + 3);
			end = end == std::string_view::npos ? source.size() : end + 3;
		} else if (source[i] == '"' || source[i] == '\'') {
			const char quote = source[i];
			end = i + 1;
			while (end < source.size()) {
				if (source[end] == '\\') { end = std::min(source.size(), end + 2); continue; }
				if (source[end++] == quote) break;
			}
		} else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
			end = source.find('\n', i + 2);
			end = end == std::string_view::npos ? source.size() : end;
		} else if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
			end = source.find("*/", i + 2);
			end = end == std::string_view::npos ? source.size() : end + 2;
		} else { ++i; continue; }
		for (size_t j = i; j < end; ++j) if (mask[j] != '\n' && mask[j] != '\r') mask[j] = ' ';
		i = end;
	}
	return mask;
}

void transform_java(std::string &source, const Options &opt) {
	if (opt.strip_unused_imports) {
		const std::string mask = java_code_mask(source);
		static const re2::RE2 importPattern("[A-Za-z_$][A-Za-z0-9_$]*(\\.[A-Za-z_$][A-Za-z0-9_$]*)*");
		std::unordered_map<std::string, size_t> identifierCounts;
		for (size_t pos = 0; pos < mask.size();) {
			if (!is_ident(mask[pos])) { ++pos; continue; }
			const size_t start = pos++;
			while (pos < mask.size() && is_ident(mask[pos])) ++pos;
			++identifierCounts[mask.substr(start, pos - start)];
		}
		for (size_t lineStart = 0; lineStart < source.size();) {
			size_t lineEnd = source.find('\n', lineStart);
			if (lineEnd == std::string::npos) lineEnd = source.size();
			std::string_view line(mask.data() + lineStart, lineEnd - lineStart);
			size_t first = line.find_first_not_of(" \t\r");
			if (first != std::string_view::npos && line.substr(first).starts_with("import ")) {
				size_t semi = line.find(';', first + 7);
				if (semi != std::string_view::npos) {
					size_t nameStart = line.find_first_not_of(" \t", first + 7);
					if (nameStart != std::string_view::npos && line.substr(nameStart).starts_with("static ")) nameStart = line.find_first_not_of(" \t", nameStart + 7);
					if (nameStart == std::string_view::npos || nameStart >= semi) { lineStart = lineEnd + (lineEnd < source.size() ? 1 : 0); continue; }
					std::string_view importName = line.substr(nameStart, semi - nameStart);
					if (!re2::RE2::FullMatch(std::string(importName), importPattern)) {
						lineStart = lineEnd + (lineEnd < source.size() ? 1 : 0);
						continue;
					}
					size_t dot = importName.rfind('.');
					std::string_view symbol = dot == std::string_view::npos ? importName : importName.substr(dot + 1);
					if (!symbol.empty() && symbol != "*") {
						auto count = identifierCounts.find(std::string(symbol));
						if (count != identifierCounts.end() && count->second == 1) for (size_t i = lineStart; i < lineEnd; ++i) if (source[i] != '\r') source[i] = ' ';
					}
				}
			}
			lineStart = lineEnd + (lineEnd < source.size() ? 1 : 0);
		}
	}
	if (opt.strip_debug) {
		const std::string_view targets[] = {"System.out.println", "Log.d", "Log.v", "Log.i", "e.printStackTrace"};
		const std::string mask = java_code_mask(source);
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t search = 0; search < mask.size();) {
			size_t start = std::string::npos, targetLength = 0;
			for (auto target : targets) {
				size_t at = mask.find(target, search);
				if (at != std::string::npos && at < start) { start = at; targetLength = target.size(); }
			}
			if (start == std::string::npos) break;
			size_t open = start + targetLength;
			while (open < mask.size() && std::isspace(static_cast<unsigned char>(mask[open]))) ++open;
			if (open == mask.size() || mask[open] != '(') { search = start + targetLength; continue; }
			int depth = 0;
			size_t close = open;
			for (; close < mask.size(); ++close) {
				if (mask[close] == '(') ++depth;
				else if (mask[close] == ')' && --depth == 0) { ++close; break; }
			}
			while (close < mask.size() && (mask[close] == ' ' || mask[close] == '\t' || mask[close] == '\r')) ++close;
			if (close < mask.size() && mask[close] == ';') ++close;
			while (close < mask.size() && (mask[close] == ' ' || mask[close] == '\t')) ++close;
			if (close < mask.size() && mask[close] == '\n' && source.find('\n', start) == close) ++close;
			ranges.emplace_back(start, close);
			search = close;
		}
		for (const auto &[start, end] : ranges) for (size_t i = start; i < end; ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
	}
	if (opt.strip_annotations) {
		const std::string mask = java_code_mask(source);
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t search = 0; search < mask.size();) {
			size_t at = mask.find('@', search);
			if (at == std::string::npos) break;
			size_t end = at + 1;
			while (end < mask.size() && (std::isalnum(static_cast<unsigned char>(mask[end])) || mask[end] == '_' || mask[end] == '.' || mask[end] == '$')) ++end;
			if (end == at + 1) { search = at + 1; continue; }
			while (end < mask.size() && std::isspace(static_cast<unsigned char>(mask[end]))) ++end;
			if (end < mask.size() && mask[end] == '(') {
				int depth = 0;
				for (; end < mask.size(); ++end) {
					if (mask[end] == '(') ++depth;
					else if (mask[end] == ')' && --depth == 0) { ++end; break; }
				}
			}
			ranges.emplace_back(at, end);
			search = end;
		}
		for (const auto &[start, end] : ranges) for (size_t i = start; i < end; ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
	}
	if (opt.extract_skeleton) {
		std::string mask = java_code_mask(source);
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t i = 0; i < source.size(); ++i) {
			if (mask[i] != '{') continue;
			size_t prev = i;
			while (prev > 0 && std::isspace(static_cast<unsigned char>(mask[prev - 1]))) --prev;
			if (prev == 0 || mask[prev - 1] != ')') continue;
			size_t headerStart = prev - 1;
			while (headerStart > 0 && mask[headerStart - 1] != ';' && mask[headerStart - 1] != '{' && mask[headerStart - 1] != '}') --headerStart;
			std::string_view header(mask.data() + headerStart, prev - headerStart);
			auto hasKeyword = [&](std::string_view keyword) {
				size_t at = header.find(keyword);
				while (at != std::string_view::npos) {
					if ((at == 0 || !is_ident(header[at - 1])) && (at + keyword.size() == header.size() || !is_ident(header[at + keyword.size()]))) return true;
					at = header.find(keyword, at + 1);
				}
				return false;
			};
			if (hasKeyword("if") || hasKeyword("for") || hasKeyword("while") || hasKeyword("switch") ||
				hasKeyword("catch") || hasKeyword("synchronized")) continue;
			int depth = 1;
			size_t end = i + 1;
			for (; end < mask.size() && depth; ++end) {
				if (mask[end] == '{') ++depth;
				else if (mask[end] == '}') --depth;
			}
			if (depth == 0) {
				ranges.emplace_back(i, end);
				i += 1;
			}
		}
		for (const auto &[start, end] : ranges) {
			for (size_t i = start + 1; i + 1 < end; ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
		}
	}
}

void transform_xml(std::string &source, const Options &opt) {
	std::string out;
	out.reserve(source.size());
	bool inTag = false;
	char activeQuote = '\0';
	for (size_t i = 0; i < source.size();) {
		if (source.compare(i, 4, "<!--") == 0) {
			size_t end = source.find("-->", i + 4);
			end = end == std::string::npos ? source.size() : end + 3;
			out.append(source, i, end - i);
		i = end;
			inTag = false;
			activeQuote = '\0';
			continue;
		}
		if (source.compare(i, 9, "<![CDATA[") == 0) {
			size_t end = source.find("]]>", i + 9);
			end = end == std::string::npos ? source.size() : end + 3;
			out.append(source, i, end - i);
			i = end;
			inTag = false;
			activeQuote = '\0';
			continue;
		}
		if (opt.collapse_empty_tags && source[i] == '<' && i + 1 < source.size() && source[i + 1] != '/' && source[i + 1] != '!' && source[i + 1] != '?') {
			char quote = '\0';
			size_t tagEnd = i + 1;
			for (; tagEnd < source.size(); ++tagEnd) {
				if (quote) { if (source[tagEnd] == quote) quote = '\0'; }
				else if (source[tagEnd] == '\'' || source[tagEnd] == '"') quote = source[tagEnd];
				else if (source[tagEnd] == '>') break;
			}
			if (tagEnd < source.size() && source[tagEnd - 1] != '/') {
				size_t nameEnd = i + 1;
				while (nameEnd < tagEnd && !std::isspace(static_cast<unsigned char>(source[nameEnd])) && source[nameEnd] != '/') ++nameEnd;
				std::string closeTag = "</" + source.substr(i + 1, nameEnd - i - 1) + ">";
				size_t contentEnd = tagEnd + 1;
				while (contentEnd < source.size() && std::isspace(static_cast<unsigned char>(source[contentEnd]))) ++contentEnd;
				if (source.compare(contentEnd, closeTag.size(), closeTag) == 0) {
					out.append(source, i, tagEnd - i);
					out += "/>";
					i = contentEnd + closeTag.size();
					continue;
				}
			}
		}
		const char c = source[i];
		out.push_back(c);
		if (inTag && activeQuote) {
			if (c == activeQuote) activeQuote = '\0';
		} else if (inTag && (c == '\'' || c == '"')) activeQuote = c;
		else if (!inTag && c == '<') inTag = true;
		else if (inTag && c == '>') {
			inTag = false;
			if (!opt.collapse_tags_whitespace) { ++i; continue; }
			size_t end = i + 1;
			while (end < source.size() && std::isspace(static_cast<unsigned char>(source[end]))) ++end;
			if (end < source.size() && source[end] == '<') i = end - 1;
		}
		++i;
	}
	source = std::move(out);
}

size_t scan_js_template(std::string_view src, size_t start);

size_t scan_js_expression(std::string_view src, size_t start) {
	int depth = 1;
	for (size_t i = start; i < src.size();) {
		if (src[i] == '\'' || src[i] == '"') {
			const char quote = src[i++];
			while (i < src.size()) {
				if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
				if (src[i++] == quote) break;
			}
		} else if (src[i] == '`') i = scan_js_template(src, i);
		else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
			i += 2;
			while (i < src.size() && src[i] != '\n') ++i;
		} else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < src.size() && src.substr(i, 2) != "*/") ++i;
			i = std::min(src.size(), i + 2);
		} else if (src[i] == '/') {
			size_t prev = i;
			while (prev > start && std::isspace(static_cast<unsigned char>(src[prev - 1]))) --prev;
			const bool regexAllowed = prev == start || std::string_view("=(:,[!&|?{};>").find(src[prev - 1]) != std::string_view::npos;
			if (!regexAllowed) { ++i; continue; }
			++i;
			bool inClass = false;
			while (i < src.size() && src[i] != '\n') {
				if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
				if (src[i] == '[') inClass = true;
				else if (src[i] == ']') inClass = false;
				else if (src[i] == '/' && !inClass) { ++i; while (i < src.size() && std::isalpha(static_cast<unsigned char>(src[i]))) ++i; break; }
				++i;
			}
		} else if (src[i] == '{') { ++depth; ++i; }
		else if (src[i] == '}') { if (--depth == 0) return i + 1; ++i; }
		else ++i;
	}
	return src.size();
}

size_t scan_js_template(std::string_view src, size_t start) {
	size_t i = start + 1;
	while (i < src.size()) {
		if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
		if (src[i] == '`') return i + 1;
		if (src[i] == '$' && i + 1 < src.size() && src[i + 1] == '{') { i = scan_js_expression(src, i + 2); continue; }
		++i;
	}
	return src.size();
}

std::vector<Token> tokenize_web(std::string_view src, Language lang) {
	std::vector<Token> tokens;
	size_t i = 0, codeStart = 0;
	auto flushCode = [&](size_t end) { if (end > codeStart) tokens.push_back({TokenType::Code, src.substr(codeStart, end - codeStart), {}, false}); };
	while (i < src.size()) {
		const char c = src[i];
		if ((lang == Language::Php && c == '<' && src.substr(i, 5) == "<?php") ||
			(lang == Language::Php && c == '<' && src.substr(i, 3) == "<?=")) {
			const size_t len = src.substr(i, 5) == "<?php" ? 5 : 3;
			flushCode(i);
			tokens.push_back({TokenType::XmlTag, src.substr(i, len), {}, false});
			i += len;
			codeStart = i;
			continue;
		}
		if ((lang == Language::JavaScript || lang == Language::Php) && c == '`') {
			flushCode(i);
			const size_t start = i;
			i = lang == Language::JavaScript ? scan_js_template(src, i) : i + 1;
			if (lang == Language::Php) while (i < src.size()) { if (src[i] == '\\') i = std::min(src.size(), i + 2); else if (src[i++] == '`') break; }
			tokens.push_back({TokenType::Template, src.substr(start, i - start), {}, false});
			codeStart = i;
			continue;
		}
		if (c == '"' || c == '\'') {
			flushCode(i);
			const size_t start = i++;
			while (i < src.size()) {
				if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
				if (src[i++] == c) break;
			}
			tokens.push_back({TokenType::String, src.substr(start, i - start), {}, false});
			codeStart = i;
			continue;
		}
		if (c == '/' && i + 1 < src.size() &&
			(src[i + 1] == '*' || (src[i + 1] == '/' && (lang == Language::JavaScript || lang == Language::Php)))) {
			flushCode(i);
			const size_t start = i;
			if (src[i + 1] == '/') {
				i += 2;
				while (i < src.size() && src[i] != '\n') ++i;
			} else {
				i += 2;
				while (i + 1 < src.size() && src.substr(i, 2) != "*/") ++i;
				i = std::min(src.size(), i + 2);
			}
			tokens.push_back({TokenType::Comment, src.substr(start, i - start), {}, false});
			codeStart = i;
			continue;
		}
		if (lang == Language::Php && c == '#' && (i + 1 >= src.size() || src[i + 1] != '[')) {
			flushCode(i);
			const size_t start = i++;
			while (i < src.size() && src[i] != '\n') ++i;
			tokens.push_back({TokenType::Comment, src.substr(start, i - start), {}, false});
			codeStart = i;
			continue;
		}
		if (lang == Language::Php && c == '<' && src.substr(i, 3) == "<<<") {
			const size_t lineEnd = src.find('\n', i);
			if (lineEnd != std::string_view::npos) {
				size_t idStart = i + 3;
				while (idStart < lineEnd && std::isspace(static_cast<unsigned char>(src[idStart]))) ++idStart;
				char quote = '\0';
				if (idStart < lineEnd && (src[idStart] == '\'' || src[idStart] == '"')) quote = src[idStart++];
				size_t idEnd = idStart;
				while (idEnd < lineEnd && (std::isalnum(static_cast<unsigned char>(src[idEnd])) || src[idEnd] == '_')) ++idEnd;
				std::string_view id = src.substr(idStart, idEnd - idStart);
				if (!id.empty()) {
					const size_t bodyStart = lineEnd + 1;
					size_t end = bodyStart;
					while (end < src.size()) {
						size_t eol = src.find('\n', end);
						if (eol == std::string_view::npos) eol = src.size();
						size_t p = end;
						while (p < eol && (src[p] == ' ' || src[p] == '\t')) ++p;
						if (src.substr(p, id.size()) == id && (p + id.size() == eol || src[p + id.size()] == ';' || src[p + id.size()] == ',')) { end = eol; break; }
						end = eol < src.size() ? eol + 1 : eol;
					}
					flushCode(i);
					tokens.push_back({TokenType::HereDoc, src.substr(i, end - i), {}, false});
					i = end;
					codeStart = i;
					(void)quote;
					continue;
				}
			}
		}
		if (lang == Language::JavaScript && c == '/') {
			size_t prev = i;
			while (prev > codeStart && std::isspace(static_cast<unsigned char>(src[prev - 1]))) --prev;
			bool regexAllowed = prev == codeStart || std::string_view("=(:,[!&|?{};>").find(src[prev - 1]) != std::string_view::npos;
			if (!regexAllowed && prev >= codeStart + 6 && src.substr(prev - 6, 6) == "return") regexAllowed = true;
			if (regexAllowed) {
				flushCode(i);
				const size_t start = i++;
				bool inClass = false;
				while (i < src.size() && src[i] != '\n') {
					if (src[i] == '\\') { i = std::min(src.size(), i + 2); continue; }
					if (src[i] == '[') inClass = true;
					else if (src[i] == ']') inClass = false;
					else if (src[i] == '/' && !inClass) { ++i; while (i < src.size() && std::isalpha(static_cast<unsigned char>(src[i]))) ++i; break; }
					++i;
				}
				tokens.push_back({TokenType::Regex, src.substr(start, i - start), {}, false});
				codeStart = i;
				continue;
			}
		}
		++i;
	}
	flushCode(src.size());
	return tokens;
}

std::vector<Token> tokenize_html(std::string_view src) {
	std::vector<Token> tokens;
	size_t i = 0, codeStart = 0;
	auto flushCode = [&](size_t end) { if (end > codeStart) tokens.push_back({TokenType::Code, src.substr(codeStart, end - codeStart), {}, false}); };
	while (i < src.size()) {
		if (src.substr(i, 4) == "<!--") {
			flushCode(i);
			const size_t commentStart = i;
			const size_t end = src.find("-->", i + 4);
			i = end == std::string_view::npos ? src.size() : end + 3;
			tokens.push_back({TokenType::Comment, src.substr(commentStart, i - commentStart), {}, false});
			codeStart = i;
		} else if (src[i] == '<') {
			char quote = '\0';
			size_t end = i + 1;
			for (; end < src.size(); ++end) {
				if (quote) { if (src[end] == quote) quote = '\0'; }
				else if (src[end] == '\'' || src[end] == '"') quote = src[end];
				else if (src[end] == '>') break;
			}
			if (end == src.size()) { ++i; continue; }
			flushCode(i);
			const size_t tagStart = i;
			i = end + 1;
			std::string_view tag = src.substr(tagStart, i - tagStart);
			tokens.push_back({TokenType::XmlTag, tag, {}, false});
			std::string lower(tag);
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			Language innerLang = Language::Plain;
			if (lower.starts_with("<script") && (lower.size() == 7 || std::isspace(static_cast<unsigned char>(lower[7])) || lower[7] == '>')) innerLang = Language::JavaScript;
			else if (lower.starts_with("<style") && (lower.size() == 6 || std::isspace(static_cast<unsigned char>(lower[6])) || lower[6] == '>')) innerLang = Language::Css;
			if (innerLang != Language::Plain) {
				const std::string closeNeedle = innerLang == Language::JavaScript ? "</script" : "</style";
				std::string rest(src.substr(i));
				std::transform(rest.begin(), rest.end(), rest.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				const size_t relClose = rest.find(closeNeedle);
				const size_t contentEnd = relClose == std::string::npos ? src.size() : i + relClose;
				auto inner = tokenize_web(src.substr(i, contentEnd - i), innerLang);
				tokens.insert(tokens.end(), inner.begin(), inner.end());
				i = contentEnd;
				codeStart = i;
			}
			codeStart = i;
		} else ++i;
	}
	flushCode(src.size());
	return tokens;
}

std::string web_code_mask(std::string_view source, Language lang) {
	auto tokens = tokenize_web(source, lang);
	std::string mask(source);
	size_t offset = 0;
	for (const auto &token : tokens) {
		const size_t end = std::min(source.size(), offset + token.text().size());
		if (token.type != TokenType::Code) for (size_t i = offset; i < end; ++i) if (mask[i] != '\n' && mask[i] != '\r') mask[i] = ' ';
		offset = end;
	}
	return mask;
}

void transform_js_php(std::string &source, Language lang, const Options &opt) {
	const bool js = lang == Language::JavaScript;
	if (opt.strip_debug) {
		const std::string_view targets[] = {"console.log", "console.debug", "var_dump", "print_r", "error_log", "debugger"};
		const std::string mask = web_code_mask(source, lang);
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t search = 0; search < mask.size();) {
			size_t start = std::string::npos, length = 0;
			for (auto target : targets) {
				if ((!js && (target.starts_with("console") || target == "debugger")) || (js && !(target.starts_with("console") || target == "debugger"))) continue;
				size_t at = mask.find(target, search);
				if (at != std::string::npos && at < start) { start = at; length = target.size(); }
			}
			if (start == std::string::npos) break;
			size_t end = start + length;
			if (js && mask.compare(start, length, "debugger") == 0) {
				while (end < mask.size() && mask[end] != ';' && mask[end] != '\n') ++end;
				if (end < mask.size() && mask[end] == ';') ++end;
			} else {
				while (end < mask.size() && std::isspace(static_cast<unsigned char>(mask[end]))) ++end;
				if (end == mask.size() || mask[end] != '(') { search = start + length; continue; }
				int depth = 0;
				for (; end < mask.size(); ++end) {
					if (mask[end] == '(') ++depth;
					else if (mask[end] == ')' && --depth == 0) { ++end; break; }
				}
				while (end < mask.size() && (mask[end] == ' ' || mask[end] == '\t' || mask[end] == '\r')) ++end;
				if (end < mask.size() && mask[end] == ';') ++end;
			}
			while (start > 0 && (source[start - 1] == ' ' || source[start - 1] == '\t')) --start;
			if (end < source.size() && source[end] == '\n' && start < source.size() && source.find('\n', start) == end) ++end;
			ranges.emplace_back(start, end);
			search = end;
		}
		for (const auto &[start, end] : ranges) for (size_t i = start; i < end; ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
	}
	if (js && opt.compact_arrow_functions) {
		const std::string mask = web_code_mask(source, lang);
		std::vector<bool> remove(source.size(), false);
		for (size_t at = mask.find("=>"); at != std::string::npos; at = mask.find("=>", at + 2)) {
			size_t left = at, right = at + 2;
			while (left > 0 && (source[left - 1] == ' ' || source[left - 1] == '\t')) remove[--left] = true;
			while (right < source.size() && (source[right] == ' ' || source[right] == '\t')) remove[right++] = true;
		}
		std::string compact;
		compact.reserve(source.size());
		for (size_t i = 0; i < source.size(); ++i) if (!remove[i]) compact.push_back(source[i]);
		source = std::move(compact);
	}
	if (opt.extract_skeleton) {
		std::string mask = web_code_mask(source, lang);
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t i = 0; i < source.size(); ++i) {
			if (mask[i] != '{') continue;
			size_t prev = i;
			while (prev > 0 && std::isspace(static_cast<unsigned char>(mask[prev - 1]))) --prev;
			size_t headerStart = prev;
			while (headerStart > 0 && mask[headerStart - 1] != ';' && mask[headerStart - 1] != '{' && mask[headerStart - 1] != '}') --headerStart;
			std::string_view header(mask.data() + headerStart, prev - headerStart);
			const bool candidate = header.find("function") != std::string_view::npos || header.find("class") != std::string_view::npos ||
				(js && header.find("=>") != std::string_view::npos) || (prev > headerStart && mask[prev - 1] == ')');
			if (!candidate || header.find("if") != std::string_view::npos || header.find("for") != std::string_view::npos || header.find("while") != std::string_view::npos || header.find("switch") != std::string_view::npos) continue;
			if (header.find("class") != std::string_view::npos) continue;
			int depth = 1;
			size_t end = i + 1;
			for (; end < mask.size() && depth; ++end) { if (mask[end] == '{') ++depth; else if (mask[end] == '}') --depth; }
			if (depth == 0) { ranges.emplace_back(i, end); ++i; }
		}
		for (const auto &[start, end] : ranges) for (size_t i = start + 1; i + 1 < end; ++i) if (source[i] != '\n' && source[i] != '\r') source[i] = ' ';
	}
}

void transform_css(std::vector<Token> &tokens, const Options &opt) {
	if (!opt.compact_css_rules && !opt.bpe_compress) return;
	for (auto &token : tokens) {
		if (token.type != TokenType::Code || token.excluded || token.empty()) continue;
		std::string text(token.text());
		std::string out;
		out.reserve(text.size());
		bool inRule = false, pendingSpace = false;
		for (char c : text) {
			if (std::isspace(static_cast<unsigned char>(c))) { pendingSpace = true; continue; }
			if ((opt.compact_css_rules && inRule && (c == ':' || c == ';' || c == '{' || c == '}')) ||
				(opt.bpe_compress && (c == ':' || c == ';' || c == '{' || c == '}'))) pendingSpace = false;
			else if (pendingSpace && !out.empty() && out.back() != '{' && out.back() != ':' && out.back() != ';' && out.back() != '}' && c != '{' && c != ':' && c != ';' && c != '}') out.push_back(' ');
			if (c == '{') inRule = true;
			out.push_back(c);
			if (c == '}') inRule = false;
			pendingSpace = false;
		}
		token.set_text(std::move(out));
	}
}

void transform_html(std::vector<Token> &tokens, bool collapseWhitespace) {
	if (!collapseWhitespace) return;
	int preserveDepth = 0;
	int rawDepth = 0;
	for (size_t index = 0; index < tokens.size(); ++index) {
		auto &token = tokens[index];
		if (token.type == TokenType::XmlTag) {
			std::string tag(token.text());
			std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const bool closing = tag.starts_with("</");
			const bool selfClosing = tag.size() > 2 && tag[tag.size() - 2] == '/';
			size_t nameStart = closing ? 2 : 1, nameEnd = nameStart;
			while (nameEnd < tag.size() && std::isalpha(static_cast<unsigned char>(tag[nameEnd]))) ++nameEnd;
			const std::string_view name(tag.data() + nameStart, nameEnd - nameStart);
			const bool preserveTag = name == "pre" || name == "code";
			const bool rawTag = name == "script" || name == "style";
			if (preserveTag) {
				if (closing) preserveDepth = std::max(0, preserveDepth - 1);
				else if (!selfClosing) ++preserveDepth;
			}
			if (rawTag) {
				if (closing) rawDepth = std::max(0, rawDepth - 1);
				else if (!selfClosing) ++rawDepth;
			}
		} else if (token.type == TokenType::Code && preserveDepth == 0 && rawDepth == 0 && !token.excluded) {
			std::string text(token.text()), out;
			out.reserve(text.size());
			bool whitespace = false;
			const bool onlyWhitespace = text.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
			if (onlyWhitespace) { token.set_text(text.empty() ? std::string() : std::string(" ")); continue; }
			for (char c : text) {
				if (std::isspace(static_cast<unsigned char>(c))) whitespace = true;
				else { if (whitespace && !out.empty()) out.push_back(' '); out.push_back(c); whitespace = false; }
			}
			if (whitespace && !out.empty()) out.push_back(' ');
			token.set_text(std::move(out));
		}
	}
}

std::vector<Token> tokenize_region(std::string_view text, Language lang) {
	if (lang == Language::Java) return tokenize_cpp(text, {}, true);
	if (lang == Language::Xml) return tokenize_xml(text, {});
	return tokenize_web(text, lang);
}

void transform_token_regions(std::vector<Token> &tokens, Language lang, const std::function<void(std::string &)> &transform) {
	std::vector<Token> result;
	result.reserve(tokens.size());
	std::string region;
	auto flush = [&]() {
		if (region.empty()) return;
		transform(region);
		auto generated = tokenize_region(region, lang);
		for (auto &token : generated) token.set_text(std::string(token.text()));
		result.insert(result.end(), std::make_move_iterator(generated.begin()), std::make_move_iterator(generated.end()));
		region.clear();
	};
	for (auto &token : tokens) {
		if (token.excluded) {
			flush();
			result.push_back(std::move(token));
		} else region.append(token.text());
	}
	flush();
	tokens = std::move(result);
}

void transform_html_embedded(std::vector<Token> &tokens, const Options &opt) {
	std::vector<Token> result;
	result.reserve(tokens.size());
	Language active = Language::Plain;
	std::string region;
	auto flush = [&]() {
		if (region.empty() || active == Language::Plain) return;
		if (active == Language::JavaScript) transform_js_php(region, active, opt);
		else {
			auto css = tokenize_web(region, Language::Css);
			transform_css(css, opt);
			region.clear();
			for (const auto &token : css) region.append(token.text());
		}
		auto generated = tokenize_web(region, active);
		for (auto &token : generated) token.set_text(std::string(token.text()));
		result.insert(result.end(), std::make_move_iterator(generated.begin()), std::make_move_iterator(generated.end()));
		region.clear();
	};
	for (auto &token : tokens) {
		if (token.type == TokenType::XmlTag) {
			flush();
			std::string tag(token.text());
			std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const bool closing = tag.starts_with("</");
			if (closing && ((active == Language::JavaScript && tag.starts_with("</script")) || (active == Language::Css && tag.starts_with("</style")))) active = Language::Plain;
			result.push_back(std::move(token));
			if (!closing && tag.starts_with("<script") && (tag.size() == 7 || std::isspace(static_cast<unsigned char>(tag[7])) || tag[7] == '>')) active = Language::JavaScript;
			else if (!closing && tag.starts_with("<style") && (tag.size() == 6 || std::isspace(static_cast<unsigned char>(tag[6])) || tag[6] == '>')) active = Language::Css;
		} else if (active != Language::Plain && !token.excluded) region.append(token.text());
		else {
			flush();
			result.push_back(std::move(token));
		}
	}
	flush();
	tokens = std::move(result);
}

std::vector<Token> tokenize_python(std::string_view src, std::span<const size_t> excluded_lines) {
	std::vector<Token> tokens;
	tokens.reserve(src.size() / 16);
	size_t i = 0, n = src.size(), code_start = 0;
	bool can_be_docstring = true;
	auto flush_code = [&](size_t start, size_t end) {
		if (end > start) {
			tokens.push_back({TokenType::Code, src.substr(start, end - start), {}, false});
		}
	};
	while (i < n) {
		char c = src[i];
		if (c == '\n') can_be_docstring = true;
		else if (!std::isspace(static_cast<unsigned char>(c)) && c != ':') can_be_docstring = false;
		if ((c == '"' || c == '\'') && i + 2 < n && src[i + 1] == c && src[i + 2] == c) {
			flush_code(code_start, i);
			size_t start = i;
			std::string_view term = src.substr(i, 3);
			i += 3;
			size_t end = src.find(term, i);
			i = (end == std::string_view::npos) ? n : end + 3;
			tokens.push_back({can_be_docstring ? TokenType::Docstring : TokenType::String, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		if (c == '"' || c == '\'') {
			flush_code(code_start, i);
			size_t start = i++;
			while (i < n && src[i] != c) {
				if (src[i] == '\\' && i + 1 < n) i += 2;
				else ++i;
			}
			if (i < n) ++i;
			tokens.push_back({TokenType::String, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		if (c == '#') {
			flush_code(code_start, i);
			size_t start = i++;
			while (i < n && src[i] != '\n') ++i;
			tokens.push_back({TokenType::Comment, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		++i;
	}
	flush_code(code_start, n);
	split_excluded_code_lines(tokens, excluded_lines);
	return tokens;
}

std::vector<Token> tokenize_shell(std::string_view src, std::span<const size_t> excluded_lines) {
	std::vector<Token> tokens;
	tokens.reserve(src.size() / 16);
	size_t i = 0, n = src.size(), code_start = 0;
	auto flush_code = [&](size_t start, size_t end) {
		if (end > start) {
			tokens.push_back({TokenType::Code, src.substr(start, end - start), {}, false});
		}
	};
	while (i < n) {
		if (i == 0 && src.rfind("#!", 0) == 0) {
			size_t end = src.find('\n', 0);
			i = (end == std::string_view::npos) ? n : end;
			tokens.push_back({TokenType::Preprocessor, src.substr(0, i), {}, false});
			code_start = i;
			continue;
		}
		if (src[i] == '<' && i + 1 < n && src[i + 1] == '<') {
			size_t delim_pos = i + 2;
			while (delim_pos < n && (src[delim_pos] == ' ' || src[delim_pos] == '-' || src[delim_pos] == '\t')) ++delim_pos;
			size_t delim_end = delim_pos;
			while (delim_end < n && is_ident(src[delim_end])) ++delim_end;
			if (delim_end > delim_pos) {
				std::string_view delim = src.substr(delim_pos, delim_end - delim_pos);
				size_t nl = src.find('\n', delim_end);
				if (nl != std::string_view::npos) {
					std::string match_target = "\n";
					match_target.append(delim);
					size_t match_pos = src.find(match_target, nl);
					if (match_pos != std::string_view::npos) {
						flush_code(code_start, i);
						size_t heredoc_end = match_pos + delim.size() + 1;
						tokens.push_back({TokenType::HereDoc, src.substr(i, heredoc_end - i), {}, false});
						i = heredoc_end;
						code_start = i;
						continue;
					}
				}
			}
		}
		if (src[i] == '\'' || src[i] == '"') {
			char q = src[i];
			flush_code(code_start, i);
			size_t start = i++;
			while (i < n && src[i] != q) {
				if (q == '"' && src[i] == '\\' && i + 1 < n) i += 2;
				else ++i;
			}
			if (i < n) ++i;
			tokens.push_back({TokenType::String, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		if (src[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(src[i - 1])) || std::string_view(";|&()").find(src[i - 1]) != std::string_view::npos)) {
			flush_code(code_start, i);
			size_t start = i++;
			while (i < n && src[i] != '\n') ++i;
			tokens.push_back({TokenType::Comment, src.substr(start, i - start), {}, false});
			code_start = i;
			continue;
		}
		++i;
	}
	flush_code(code_start, n);
	split_excluded_code_lines(tokens, excluded_lines);
	return tokens;
}

void apply_indent_conversion(std::string_view leading_ws, IndentMode mode, int tab_size, std::string& out) {
	if (mode == IndentMode::Keep || tab_size <= 0) {
		out.append(leading_ws);
		return;
	}
	int total_spaces = 0;
	for (char c : leading_ws) {
		if (c == '\t') total_spaces += tab_size - (total_spaces % tab_size);
		else if (c == ' ') ++total_spaces;
	}
	if (mode == IndentMode::SpacesToTabs) {
		out.append(total_spaces / tab_size, '\t');
		out.append(total_spaces % tab_size, ' ');
	} else if (mode == IndentMode::TabsToSpaces) {
		out.append(total_spaces, ' ');
	}
}

void clean_whitespace_append(std::string_view src, BlankLineMode mode, IndentMode indent_mode, int tab_size, std::string& out, bool& last_blank, int* brace_depth = nullptr) {
	out.reserve(out.size() + src.size());
	size_t i = 0;
	while (i < src.size()) {
		size_t end = src.find('\n', i);
		if (end == std::string_view::npos) end = src.size();
		std::string_view line = src.substr(i, end - i);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		size_t non_ws = line.find_first_not_of(" \t");
		bool is_blank = (non_ws == std::string_view::npos);
		if (is_blank) {
			if (mode != BlankLineMode::BetweenFunctionsOnly || !brace_depth || *brace_depth == 0) {
				if (mode == BlankLineMode::BetweenFunctionsOnly || mode == BlankLineMode::Keep) {
					out += '\n';
					last_blank = true;
				} else if (mode == BlankLineMode::Collapse && !last_blank && !out.empty()) {
					out += '\n';
					last_blank = true;
				}
			}
		} else {
			size_t r_idx = line.find_last_not_of(" \t");
			apply_indent_conversion(line.substr(0, non_ws), indent_mode, tab_size, out);
			out.append(line.substr(non_ws, (r_idx - non_ws) + 1));
			out += '\n';
			last_blank = false;
			if (mode == BlankLineMode::BetweenFunctionsOnly && brace_depth) {
				bool in_string = false, in_char = false, escaped = false;
				for (char c : line) {
					if (escaped) { escaped = false; continue; }
					if ((in_string || in_char) && c == '\\') { escaped = true; continue; }
					if (!in_char && c == '"') { in_string = !in_string; continue; }
					if (!in_string && c == '\'') { in_char = !in_char; continue; }
					if (!in_string && !in_char) {
						if (c == '{') ++*brace_depth;
						else if (c == '}' && *brace_depth > 0) --*brace_depth;
					}
				}
			}
		}
		i = end + 1;
	}
}

void unwrap_parentheses_dfa(std::string& s, int& paren_depth, int& brace_depth) {
	for (size_t i = 0; i < s.size();) {
		char c = s[i];
		if (c == '{') { ++brace_depth; ++i; continue; }
		if (c == '}' && brace_depth > 0) { --brace_depth; ++i; continue; }
		if (c == '(') { ++paren_depth; ++i; continue; }
		if (c == ')') { if (paren_depth > 0) --paren_depth; ++i; continue; }
		if (paren_depth > 0 && brace_depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
			size_t end = i + 1;
			while (end < s.size() && std::isspace(static_cast<unsigned char>(s[end]))) ++end;
			s.replace(i, end - i, " ");
			++i;
			continue;
		}
		++i;
	}
}

void compact_short_functions_dfa(std::vector<Token>& tokens) {
	std::string flat;
	std::vector<size_t> token_starts;
	token_starts.reserve(tokens.size() + 1);
	for (const auto& token : tokens) {
		token_starts.push_back(flat.size());
		flat.append(token.text());
	}
	token_starts.push_back(flat.size());
	auto is_code = [&](size_t pos) {
		auto it = std::upper_bound(token_starts.begin(), token_starts.end(), pos);
		size_t index = static_cast<size_t>(it - token_starts.begin() - 1);
		return index < tokens.size() && !tokens[index].excluded && tokens[index].type == TokenType::Code;
	};
	auto token_type_at = [&](size_t pos) {
		auto it = std::upper_bound(token_starts.begin(), token_starts.end(), pos);
		size_t index = static_cast<size_t>(it - token_starts.begin() - 1);
		return index < tokens.size() ? tokens[index].type : TokenType::Code;
	};
	auto code_char = [&](size_t pos, char c) { return pos < flat.size() && is_code(pos) && flat[pos] == c; };
	std::vector<std::pair<size_t, size_t>> compact_ranges;
	for (size_t open = 0; open < flat.size(); ++open) {
		if (!code_char(open, '{')) continue;
		size_t close = open + 1;
		for (; close < flat.size(); ++close) {
			if (code_char(close, '{')) break;
			if (code_char(close, '}')) break;
		}
		if (close >= flat.size() || !code_char(close, '}')) continue;
		size_t paren_close = open;
		bool blocked_header = false;
		bool found_paren_close = false;
		while (paren_close > 0) {
			--paren_close;
			TokenType type = token_type_at(paren_close);
			if (type == TokenType::Preprocessor || type == TokenType::Comment) {
				blocked_header = true;
				break;
			}
			if (type == TokenType::String || type == TokenType::RawString || type == TokenType::Char) {
				continue;
			}
			if (code_char(paren_close, '#') || code_char(paren_close, ';') || code_char(paren_close, '{') || code_char(paren_close, '}')) {
				blocked_header = true;
				break;
			}
			if (code_char(paren_close, ')')) { found_paren_close = true; break; }
		}
		if (blocked_header || !found_paren_close) continue;
		int paren_depth = 1;
		size_t paren_open = paren_close;
		while (paren_open > 0 && paren_depth > 0) {
			--paren_open;
			if (!is_code(paren_open)) continue;
			if (flat[paren_open] == ')') ++paren_depth;
			else if (flat[paren_open] == '(') --paren_depth;
		}
		if (paren_depth != 0) continue;
		size_t name_end = paren_open;
		while (name_end > 0 && std::isspace(static_cast<unsigned char>(flat[name_end - 1]))) --name_end;
		size_t name_start = name_end;
		while (name_start > 0 && (std::isalnum(static_cast<unsigned char>(flat[name_start - 1])) || flat[name_start - 1] == '_')) --name_start;
		std::string_view name(flat.data() + name_start, name_end - name_start);
		if (name.empty() || name == "if" || name == "for" || name == "while" || name == "switch" || name == "catch" || name == "sizeof" || name == "decltype") continue;
		bool has_comment = false;
		int parens = 0, brackets = 0;
		size_t semi_count = 0, last_non_ws = open;
		for (size_t pos = open + 1; pos < close; ++pos) {
			auto it = std::upper_bound(token_starts.begin(), token_starts.end(), pos);
			size_t index = static_cast<size_t>(it - token_starts.begin() - 1);
			if (tokens[index].type == TokenType::Comment) { has_comment = true; break; }
			if (!std::isspace(static_cast<unsigned char>(flat[pos]))) last_non_ws = pos;
			if (!is_code(pos)) continue;
			char c = flat[pos];
			if (c == '(') ++parens;
			else if (c == ')' && parens > 0) --parens;
			else if (c == '[') ++brackets;
			else if (c == ']' && brackets > 0) --brackets;
			else if (c == ';' && parens == 0 && brackets == 0) ++semi_count;
		}
		if (has_comment || semi_count != 1 || !code_char(last_non_ws, ';')) continue;
		compact_ranges.emplace_back(open, close);
	}
	std::vector<std::vector<std::pair<size_t, size_t>>> normalize_ranges(tokens.size());
	std::vector<std::vector<size_t>> insert_positions(tokens.size());
	auto add_normalize_range = [&](size_t begin, size_t end) {
		for (size_t index = 0; index < tokens.size(); ++index) {
			if (tokens[index].type != TokenType::Code || tokens[index].excluded) continue;
			size_t from = std::max(begin, token_starts[index]);
			size_t to = std::min(end, token_starts[index + 1]);
			if (from < to) normalize_ranges[index].emplace_back(from - token_starts[index], to - token_starts[index]);
		}
	};
	auto add_insert = [&](size_t pos, size_t owner_pos) {
		auto owner = std::upper_bound(token_starts.begin(), token_starts.end(), owner_pos);
		size_t index = static_cast<size_t>(owner - token_starts.begin() - 1);
		if (index < tokens.size() && tokens[index].type == TokenType::Code && !tokens[index].excluded)
			insert_positions[index].push_back(pos - token_starts[index]);
	};
	for (auto it = compact_ranges.rbegin(); it != compact_ranges.rend(); ++it) {
		size_t open = it->first, close = it->second;
		if (close > open + 1 && !std::isspace(static_cast<unsigned char>(flat[close - 1]))) add_insert(close, close);
		size_t header_start = open;
		while (header_start > 0 && std::isspace(static_cast<unsigned char>(flat[header_start - 1]))) --header_start;
		const bool has_header_space = header_start < open;
		bool has_preprocessor = false;
		for (size_t pos = header_start; pos < open; ++pos) {
			if (token_type_at(pos) == TokenType::Preprocessor) { has_preprocessor = true; break; }
		}
		if (!has_preprocessor && open > 0 && token_type_at(open - 1) == TokenType::Code) add_normalize_range(header_start, open);
		add_normalize_range(open + 1, close);
		if (open + 1 < close && !std::isspace(static_cast<unsigned char>(flat[open + 1]))) add_insert(open + 1, open);
		if (!has_header_space) add_insert(open, open);
	}
	for (size_t index = 0; index < tokens.size(); ++index) {
		if (normalize_ranges[index].empty() && insert_positions[index].empty()) continue;
		std::string_view current = tokens[index].text();
		std::vector<bool> normalize(current.size(), false), insert_before(current.size() + 1, false);
		for (const auto& [begin, end] : normalize_ranges[index])
			for (size_t pos = begin; pos < end && pos < normalize.size(); ++pos) normalize[pos] = true;
		for (size_t pos : insert_positions[index]) if (pos < insert_before.size()) insert_before[pos] = true;
		std::string replacement;
		replacement.reserve(current.size() + insert_positions[index].size());
		for (size_t pos = 0; pos <= current.size();) {
			if (insert_before[pos]) replacement += ' ';
			if (pos == current.size()) break;
			if (normalize[pos] && std::isspace(static_cast<unsigned char>(current[pos]))) {
				replacement += ' ';
				++pos;
				while (pos < current.size() && normalize[pos] && std::isspace(static_cast<unsigned char>(current[pos]))) ++pos;
			} else {
				replacement += current[pos++];
			}
		}
		tokens[index].set_text(std::move(replacement));
	}
}

std::string clean_whitespace(std::string_view src, BlankLineMode mode, IndentMode indent_mode, int tab_size) {
	std::string out;
	bool last_blank = false;
	int brace_depth = 0;
	clean_whitespace_append(src, mode, indent_mode, tab_size, out, last_blank, &brace_depth);
	return out;
}

namespace {

std::string line_error(std::string_view message, size_t line) {
	return std::string(message) + " w linii " + std::to_string(line);
}

std::pair<bool, std::string> verify_delimited_syntax(std::string_view code, Language lang) {
	std::vector<std::pair<char, size_t>> stack;
	const bool java = lang == Language::Java;
	const bool js = lang == Language::JavaScript;
	const bool php = lang == Language::Php;
	size_t line = 1;
	for (size_t i = 0; i < code.size();) {
		const char c = code[i];
		if (c == '\n') { ++line; ++i; continue; }
		if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
			const size_t startLine = line;
			i += 2;
			bool closed = false;
			while (i < code.size()) {
				if (code[i] == '\n') ++line;
				if (code[i] == '*' && i + 1 < code.size() && code[i + 1] == '/') { i += 2; closed = true; break; }
				++i;
			}
			if (!closed) return {false, line_error("Niedomknięty komentarz blokowy otwarty", startLine)};
			continue;
		}
		if ((js || java || php) && c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
			while (i < code.size() && code[i] != '\n') ++i;
			continue;
		}
		if (php && c == '#') { while (i < code.size() && code[i] != '\n') ++i; continue; }
		if (java && code.substr(i, 3) == "\"\"\"") {
			const size_t startLine = line;
			i += 3;
			const size_t end = code.find("\"\"\"", i);
			if (end == std::string_view::npos) return {false, line_error("Niedomknięty text-block Java otwarty", startLine)};
			for (; i < end + 3; ++i) if (code[i] == '\n') ++line;
			continue;
		}
		if (c == '\'' || c == '"' || (js && c == '`')) {
			const char quote = c;
			const size_t startLine = line;
			++i;
			bool closed = false;
			while (i < code.size()) {
				if (code[i] == '\n') { ++line; if (quote != '`' && !php) break; }
				if (code[i] == '\\' && i + 1 < code.size()) { if (code[i + 1] == '\n') ++line; i += 2; continue; }
				if (code[i++] == quote) { closed = true; break; }
			}
			if (!closed) return {false, line_error("Niedomknięty literał otwarty", startLine)};
			continue;
		}
		bool regexStart = false;
		if (js && c == '/') {
			size_t previous = i;
			while (previous > 0 && std::isspace(static_cast<unsigned char>(code[previous - 1]))) --previous;
			if (previous == 0 || std::string_view("=(:,[!&|?{};>").find(code[previous - 1]) != std::string_view::npos) regexStart = true;
			else if (previous > 0 && is_ident(code[previous - 1])) {
				size_t begin = previous - 1;
				while (begin > 0 && is_ident(code[begin - 1])) --begin;
				const std::string_view word = code.substr(begin, previous - begin);
				regexStart = word == "return" || word == "throw" || word == "case" || word == "delete" || word == "typeof" || word == "void" || word == "yield" || word == "await" || word == "else";
			}
		}
		if (js && c == '/' && regexStart) {
			const size_t startLine = line;
			++i;
			bool inClass = false, closed = false;
			while (i < code.size() && code[i] != '\n') {
				if (code[i] == '\\' && i + 1 < code.size()) { i += 2; continue; }
				if (code[i] == '[') inClass = true;
				else if (code[i] == ']') inClass = false;
				else if (code[i] == '/' && !inClass) { ++i; closed = true; while (i < code.size() && std::isalpha(static_cast<unsigned char>(code[i]))) ++i; break; }
				++i;
			}
			if (!closed) return {false, line_error("Niedomknięty regex JavaScript otwarty", startLine)};
			continue;
		}
		if (php && code.substr(i, 3) == "<<<") {
			const size_t startLine = line;
			size_t p = i + 3;
			while (p < code.size() && (code[p] == ' ' || code[p] == '\t')) ++p;
			char quote = '\0';
			if (p < code.size() && (code[p] == '\'' || code[p] == '"')) quote = code[p++];
			const size_t nameStart = p;
			while (p < code.size() && (std::isalnum(static_cast<unsigned char>(code[p])) || code[p] == '_')) ++p;
			if (p > nameStart) {
				std::string label(code.substr(nameStart, p - nameStart));
				if (quote && p < code.size() && code[p] == quote) ++p;
				while (p < code.size() && code[p] != '\n') ++p;
				if (p < code.size()) ++p;
				bool closed = false;
				while (p < code.size()) {
					size_t q = p;
					while (q < code.size() && (code[q] == ' ' || code[q] == '\t')) ++q;
					if (code.substr(q, label.size()) == label && (q + label.size() == code.size() || code[q + label.size()] == ';' || code[q + label.size()] == '\n' || code[q + label.size()] == '\r')) { p = q + label.size(); closed = true; break; }
					while (p < code.size() && code[p++] != '\n') {}
				}
				if (!closed) return {false, line_error("Niedomknięty heredoc/nowdoc otwarty", startLine)};
				for (; i < p; ++i) if (code[i] == '\n') ++line;
				continue;
			}
		}
		if (c == '{' || c == '(' || c == '[') stack.emplace_back(c, line);
		else if (c == '}' || c == ')' || c == ']') {
			if (stack.empty()) return {false, line_error("Nieoczekiwany znak domykający", line)};
			const char open = stack.back().first;
			if ((c == '}' && open != '{') || (c == ')' && open != '(') || (c == ']' && open != '[')) return {false, line_error("Niezgodny delimiter; otwarcie w linii " + std::to_string(stack.back().second), line)};
			stack.pop_back();
		}
		++i;
	}
	if (!stack.empty()) return {false, line_error(std::string("Niedomknięty delimiter '") + stack.back().first + "' otwarty", stack.back().second)};
	return {true, {}};
}

std::pair<bool, std::string> verify_markup_syntax(std::string_view code, bool html) {
	struct OpenTag { std::string name; size_t line; };
	std::vector<OpenTag> stack;
	static const std::array<std::string_view, 14> voidTags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
	size_t line = 1;
	for (size_t i = 0; i < code.size();) {
		if (code[i] == '\n') { ++line; ++i; continue; }
		if (html && !stack.empty() && (stack.back().name == "script" || stack.back().name == "style")) {
			const std::string needle = "</" + stack.back().name;
			size_t end = i;
			for (; end + needle.size() <= code.size(); ++end) {
				bool match = true;
				for (size_t j = 0; j < needle.size(); ++j) if (std::tolower(static_cast<unsigned char>(code[end + j])) != needle[j]) { match = false; break; }
				if (match && end + needle.size() < code.size() && !std::isspace(static_cast<unsigned char>(code[end + needle.size()])) && code[end + needle.size()] != '>') match = false;
				if (match) break;
			}
			for (; i < end; ++i) if (code[i] == '\n') ++line;
			if (end == code.size()) break;
		}
		if (code.substr(i, 4) == "<!--") {
			const size_t startLine = line, end = code.find("-->", i + 4);
			if (end == std::string_view::npos) return {false, line_error("Niedomknięty komentarz", startLine)};
			for (; i < end + 3; ++i) if (code[i] == '\n') ++line;
			continue;
		}
		if (code.substr(i, 9) == "<![CDATA[") {
			const size_t end = code.find("]]>", i + 9);
			if (end == std::string_view::npos) return {false, line_error("Niedomknięty blok CDATA", line)};
			for (; i < end + 3; ++i) if (code[i] == '\n') ++line;
			continue;
		}
		if (code[i] != '<') { ++i; continue; }
		const size_t tagLine = line;
		if (i + 1 < code.size() && (code[i + 1] == '?' || code[i + 1] == '!')) {
			size_t end = i + 2;
			char quote = '\0';
			for (; end < code.size(); ++end) {
				if (quote) { if (code[end] == quote) quote = '\0'; }
				else if (code[end] == '\'' || code[end] == '"') quote = code[end];
				else if (code[end] == '>' && !(code[i + 1] == '?' && end > i && code[end - 1] != '?')) break;
			}
			if (end == code.size()) return {false, line_error("Niedomknięta deklaracja/tag", tagLine)};
			for (; i <= end; ++i) if (code[i] == '\n') ++line;
			continue;
		}
		size_t end = i + 1;
		char quote = '\0';
		for (; end < code.size(); ++end) {
			if (quote) { if (code[end] == quote) quote = '\0'; }
			else if (code[end] == '\'' || code[end] == '"') quote = code[end];
			else if (code[end] == '>') break;
		}
		if (end == code.size()) return {false, line_error(quote ? "Niedomknięty cudzysłów atrybutu" : "Niedomknięty tag", tagLine)};
		std::string_view inside = code.substr(i + 1, end - i - 1);
		bool closing = !inside.empty() && inside.front() == '/';
		if (closing) inside.remove_prefix(1);
		while (!inside.empty() && std::isspace(static_cast<unsigned char>(inside.front()))) inside.remove_prefix(1);
		size_t nameEnd = 0;
		while (nameEnd < inside.size() && !std::isspace(static_cast<unsigned char>(inside[nameEnd])) && inside[nameEnd] != '/') ++nameEnd;
		if (nameEnd == 0) return {false, line_error("Niepoprawny tag", tagLine)};
		std::string name(inside.substr(0, nameEnd));
		if (html) std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		bool selfClosing = end > i + 1 && code[end - 1] == '/';
		if (closing) {
			if (stack.empty() || stack.back().name != name) {
				const std::string expected = stack.empty() ? "brak" : "</" + stack.back().name + ">";
				return {false, line_error("Oczekiwano " + expected + ", napotkano </" + name + ">", tagLine)};
			}
			stack.pop_back();
		} else if (!selfClosing) {
			const bool isVoid = html && std::find(voidTags.begin(), voidTags.end(), name) != voidTags.end();
			if (!isVoid) stack.push_back({std::move(name), tagLine});
		}
		for (; i <= end; ++i) if (code[i] == '\n') ++line;
	}
	if (!stack.empty()) return {false, line_error("Nie zamknięto tagu <" + stack.back().name + "> otwartego", stack.back().line)};
	return {true, {}};
}

std::pair<bool, std::string> verify_external(std::string_view code, const char *program, const char *arg, const char *arg2 = nullptr) {
	int input[2] = {-1, -1}, errors[2] = {-1, -1};
	if (pipe(input) < 0) return {false, "pipe() failed: " + std::string(std::strerror(errno))};
	if (pipe(errors) < 0) { close(input[0]); close(input[1]); return {false, "pipe() failed: " + std::string(std::strerror(errno))}; }
	const pid_t pid = fork();
	if (pid < 0) { close(input[0]); close(input[1]); close(errors[0]); close(errors[1]); return {false, "fork() failed: " + std::string(std::strerror(errno))}; }
	if (pid == 0) {
		close(input[1]); close(errors[0]);
		if (dup2(input[0], STDIN_FILENO) < 0 || dup2(errors[1], STDOUT_FILENO) < 0 || dup2(errors[1], STDERR_FILENO) < 0) _exit(126);
		close(input[0]); close(errors[1]);
		if (arg2) execlp(program, program, arg, arg2, nullptr);
		else execlp(program, program, arg, nullptr);
		_exit(errno == ENOENT ? 127 : 126);
	}
	close(input[0]); close(errors[1]);
	sigset_t pipeSet, oldSet;
	sigemptyset(&pipeSet); sigaddset(&pipeSet, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipeSet, &oldSet);
	size_t written = 0;
	while (written < code.size()) {
		const ssize_t n = write(input[1], code.data() + written, code.size() - written);
		if (n > 0) written += static_cast<size_t>(n);
		else if (n < 0 && errno == EINTR) continue;
		else break;
	}
	close(input[1]);
	sigset_t pending;
	sigpending(&pending);
	if (sigismember(&pending, SIGPIPE)) { timespec zero{0, 0}; sigtimedwait(&pipeSet, nullptr, &zero); }
	std::string err;
	std::array<char, 512> buffer{};
	for (;;) {
		const ssize_t n = read(errors[0], buffer.data(), buffer.size());
		if (n > 0) err.append(buffer.data(), static_cast<size_t>(n));
		else if (n < 0 && errno == EINTR) continue;
		else break;
	}
	close(errors[0]);
	pthread_sigmask(SIG_SETMASK, &oldSet, nullptr);
	int status = 0;
	pid_t waited;
	do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
	if (waited < 0) return {false, "waitpid() failed: " + std::string(std::strerror(errno))};
	if (WIFEXITED(status) && (WEXITSTATUS(status) == 127 || WEXITSTATUS(status) == 126)) return {true, "__fallback__"};
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return {true, {}};
	return {false, err.empty() ? "Walidator zewnętrzny zakończył się błędem" : err};
}

} // namespace

std::pair<bool, std::string> verify_syntax_posix(std::string_view code, Language lang) {
	if (lang == Language::Java || lang == Language::Css || lang == Language::JavaScript || lang == Language::Php) {
		if (lang == Language::Java || lang == Language::Css) return verify_delimited_syntax(code, lang);
		auto external = lang == Language::JavaScript ? verify_external(code, "node", "--check", "-") : verify_external(code, "php", "-l");
		if (external.second != "__fallback__") return external;
		return verify_delimited_syntax(code, lang);
	}
	if (lang == Language::Xml || lang == Language::Html) return verify_markup_syntax(code, lang == Language::Html);
	if (lang == Language::Python) {
		int in_pipe[2], err_pipe[2];
		if (pipe(in_pipe) < 0 || pipe(err_pipe) < 0) return {false, "pipe() failed"};
		pid_t pid = fork();
		if (pid < 0) {
			close(in_pipe[0]); close(in_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
			return {false, "fork() failed"};
		}
		if (pid == 0) {
			close(in_pipe[1]); close(err_pipe[0]);
			dup2(in_pipe[0], STDIN_FILENO); dup2(err_pipe[1], STDERR_FILENO);
			close(in_pipe[0]); close(err_pipe[1]);
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
			execlp("python3", "python3", "-c", "import sys; compile(sys.stdin.read(), '<stdin>', 'exec')", nullptr);
			_exit(127);
		}
		close(in_pipe[0]); close(err_pipe[1]);
		write(in_pipe[1], code.data(), code.size());
		close(in_pipe[1]);
		std::string err_output;
		std::array<char, 512> buf;
		ssize_t r;
		while ((r = read(err_pipe[0], buf.data(), buf.size())) > 0) err_output.append(buf.data(), static_cast<size_t>(r));
		close(err_pipe[0]);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return {true, ""};
		return {false, err_output.empty() ? "Python syntax error" : err_output};
	}
	if (lang != Language::Cpp) return {true, ""};
	std::vector<std::pair<char, size_t>> delimiter_stack;
	int if_depth = 0;
	size_t current_line = 1;
	size_t i = 0, n = code.size();
	while (i < n) {
		char c = code[i];
		if (c == '\n') {
			++current_line;
			++i;
			continue;
		}
		if (c == '#') {
			++i;
			while (i < n && std::isspace(static_cast<unsigned char>(code[i])) && code[i] != '\n') ++i;
			size_t d_start = i;
			while (i < n && is_ident(code[i])) ++i;
			std::string_view dir = code.substr(d_start, i - d_start);
			if (dir == "if" || dir == "ifdef" || dir == "ifndef") ++if_depth;
			else if (dir == "endif") {
				--if_depth;
				if (if_depth < 0) return {false, "Niezbalansowane #endif w linii " + std::to_string(current_line)};
			}
			while (i < n && code[i] != '\n') {
				if (code[i] == '/' && i + 1 < n && code[i + 1] == '/') break;
				if (code[i] == '/' && i + 1 < n && code[i + 1] == '*') {
					i += 2;
					while (i < n && code[i] != '\n' && !(code[i] == '*' && i + 1 < n && code[i + 1] == '/')) ++i;
					if (i < n && code[i] == '*' && i + 1 < n && code[i + 1] == '/') i += 2;
					continue;
				}
				if (code[i] == '{' || code[i] == '}') return {false, "Klamra w dyrektywie preprocesora w linii " + std::to_string(current_line)};
				size_t esc = skip_escaped_newline(code, i);
				if (esc > 0) { i += esc; ++current_line; } else ++i;
			}
			continue;
		}
		if (c == 'R' && i + 1 < n && code[i + 1] == '"' && (i == 0 || !is_ident(code[i - 1]))) {
			size_t probe = i + 2;
			size_t delim_start = probe;
			while (probe < n && code[probe] != '(' && probe - delim_start <= 16) ++probe;
			if (probe < n && code[probe] == '(') {
				std::string_view delim = code.substr(delim_start, probe - delim_start);
				std::string term = ")" + std::string(delim) + "\"";
				size_t end = code.find(term, probe + 1);
				if (end == std::string_view::npos) return {false, "Niedomknięty raw string w linii " + std::to_string(current_line)};
				for (size_t p = i; p < end + term.size(); ++p) if (code[p] == '\n') ++current_line;
				i = end + term.size();
				continue;
			}
		}
		if (c == '"' || c == '\'') {
			char quote = c;
			size_t start_line = current_line;
			++i;
			bool closed = false;
			while (i < n) {
				if (code[i] == '\n') ++current_line;
				size_t esc = skip_escaped_newline(code, i);
				if (esc > 0) { i += esc; ++current_line; continue; }
				if (code[i] == '\\' && i + 1 < n) { i += 2; continue; }
				if (code[i] == quote) { closed = true; ++i; break; }
				++i;
			}
			if (!closed) return {false, "Niedomknięty literał tekstowy rozpoczynający się w linii " + std::to_string(start_line)};
			continue;
		}
		if (c == '{' || c == '(' || c == '[') {
			delimiter_stack.push_back({c, current_line});
		} else if (c == '}' || c == ')' || c == ']') {
			if (delimiter_stack.empty()) return {false, std::string("Nieoczekiwany znak domykający '") + c + "' w linii " + std::to_string(current_line)};
			char open = delimiter_stack.back().first;
			if ((c == '}' && open != '{') || (c == ')' && open != '(') || (c == ']' && open != '[')) {
				return {false, std::string("Błędne domknięcie '") + c + "' dla znaku '" + open + "' otwartego w linii " + std::to_string(delimiter_stack.back().second)};
			}
			delimiter_stack.pop_back();
		}
		++i;
	}
	if (!delimiter_stack.empty()) {
		auto unclosed = delimiter_stack.back();
		return {false, std::string("Niedomknięty znak '") + unclosed.first + "' otwarty w linii " + std::to_string(unclosed.second)};
	}
	if (if_depth != 0) return {false, "Niezbalansowane dyrektywy preprocesora #if/#endif (brak domknięcia)"};
	return {true, ""};
}

void strip_attributes_dfa(std::string& s) {
	static const std::string_view attrs[] = {
		"[[nodiscard]]", "[[maybe_unused]]", "[[likely]]", "[[unlikely]]"
	};
	for (auto attr : attrs) {
		size_t pos = 0;
		while ((pos = s.find(attr, pos)) != std::string::npos) {
			s.erase(pos, attr.size());
		}
	}

	size_t pos = 0;
	while ((pos = s.find("noexcept", pos)) != std::string::npos) {
		if (is_boundary(s, pos, 8)) {
			size_t end = pos + 8;
			while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) ++end;
			if (end < s.size() && s[end] == '(') {
				int depth = 1;
				++end;
				while (end < s.size() && depth > 0) {
					if (s[end] == '(') ++depth;
					else if (s[end] == ')') --depth;
					++end;
				}
			}
			s.erase(pos, end - pos);
		} else {
			pos += 8;
		}
	}

	pos = 0;
	while ((pos = s.find("inline", pos)) != std::string::npos) {
		if (is_boundary(s, pos, 6)) {
			size_t end = pos + 6;
			while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) ++end;
			s.erase(pos, end - pos);
		} else {
			pos += 6;
		}
	}
}

void strip_redundant_specifiers_dfa(std::string& s) {
	size_t pos = 0;
	while ((pos = s.find("virtual", pos)) != std::string::npos) {
		if (!is_boundary(s, pos, 7)) {
			pos += 7;
			continue;
		}
		size_t stmt_end = s.find_first_of(";{", pos + 7);
		if (stmt_end == std::string::npos) break;
		std::string_view stmt(s.data() + pos + 7, stmt_end - (pos + 7));
		if (stmt.find("override") != std::string_view::npos || stmt.find("final") != std::string_view::npos) {
			size_t erase_len = 7;
			while (pos + erase_len < s.size() && (s[pos + erase_len] == ' ' || s[pos + erase_len] == '\t')) ++erase_len;
			s.erase(pos, erase_len);
		} else {
			pos += 7;
		}
	}
}

void modernize_namespaces_dfa(std::string& s) {
	size_t pos = 0;
	while ((pos = s.find("namespace", pos)) != std::string::npos) {
		if (!is_boundary(s, pos, 9)) {
			pos += 9;
			continue;
		}
		size_t i = pos + 9;
		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
		size_t ns1_start = i;
		while (i < s.size() && is_ident(s[i])) ++i;
		size_t ns1_end = i;
		if (ns1_end == ns1_start) { pos = i; continue; }
		std::string_view ns1(s.data() + ns1_start, ns1_end - ns1_start);

		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
		if (i >= s.size() || s[i] != '{') { pos = i; continue; }
		++i;

		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
		if (s.compare(i, 9, "namespace") != 0 || !is_boundary(s, i, 9)) { pos = i; continue; }
		i += 9;

		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
		size_t ns2_start = i;
		while (i < s.size() && is_ident(s[i])) ++i;
		size_t ns2_end = i;
		if (ns2_end == ns2_start) { pos = i; continue; }
		std::string_view ns2(s.data() + ns2_start, ns2_end - ns2_start);

		while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
		if (i >= s.size() || s[i] != '{') { pos = i; continue; }
		size_t brace2_pos = i;

		int depth = 1;
		size_t scan = brace2_pos + 1;
		size_t inner_close = std::string::npos;
		while (scan < s.size() && depth > 0) {
			if (s[scan] == '{') ++depth;
			else if (s[scan] == '}') {
				--depth;
				if (depth == 0) {
					inner_close = scan;
					break;
				}
			}
			++scan;
		}

		if (inner_close != std::string::npos) {
			size_t next_char = inner_close + 1;
			while (next_char < s.size() && std::isspace(static_cast<unsigned char>(s[next_char]))) ++next_char;
			if (next_char < s.size() && s[next_char] == '}') {
				s.erase(next_char, 1);
			}
		}

		std::string replacement = "namespace ";
		replacement.append(ns1);
		replacement.append("::");
		replacement.append(ns2);
		replacement.append(" {");

		s.replace(pos, (brace2_pos - pos) + 1, replacement);
		pos += replacement.size();
	}
}

void compact_types_dfa(std::string& s) {
	static const std::string_view targets[] = {"struct", "union", "enum"};
	for (auto kw : targets) {
		size_t pos = 0;
		while ((pos = s.find(kw, pos)) != std::string::npos) {
			if (!is_boundary(s, pos, kw.size())) {
				pos += kw.size();
				continue;
			}
			size_t open_brace = s.find('{', pos + kw.size());
			if (open_brace == std::string::npos) break;
			size_t close_brace = s.find('}', open_brace + 1);
			if (close_brace == std::string::npos) break;
			if (s.find('{', open_brace + 1) < close_brace) {
				pos = open_brace + 1;
				continue;
			}
			size_t semi = close_brace + 1;
			while (semi < s.size() && (s[semi] == ' ' || s[semi] == '\t')) ++semi;
			if (semi < s.size() && s[semi] == ';') {
				std::string_view body(s.data() + open_brace + 1, close_brace - (open_brace + 1));
				size_t first = body.find_first_not_of(" \t\r\n");
				size_t last = body.find_last_not_of(" \t\r\n");
				if (first != std::string_view::npos && last != std::string_view::npos) {
					std::string compact_body(body.substr(first, (last - first) + 1));
					std::string replacement = "{ " + compact_body + " };";
					s.replace(open_brace, (semi - open_brace) + 1, replacement);
					pos = open_brace + replacement.size();
					continue;
				}
			}
			pos = open_brace + 1;
		}
	}
}

void compact_constructors_dfa(std::string& s) {
	size_t pos = 0;
	while ((pos = s.find(':', pos)) != std::string::npos) {
		if (pos == 0 || s[pos - 1] == ':') {
			pos += 2;
			continue;
		}
		if (pos + 1 < s.size() && s[pos + 1] == ':') {
			pos += 2;
			continue;
		}

		size_t back = pos;
		while (back > 0 && std::isspace(static_cast<unsigned char>(s[back - 1]))) --back;
		if (back == 0 || s[back - 1] != ')') {
			++pos;
			continue;
		}

		size_t brace = s.find('{', pos + 1);
		if (brace == std::string::npos) break;

		std::string_view init_list(s.data() + pos, brace - pos);
		std::string compacted;
		compacted.reserve(init_list.size());

		bool in_ws = false;
		for (char c : init_list) {
			if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
				if (!in_ws) {
					compacted += ' ';
					in_ws = true;
				}
			} else {
				compacted += c;
				in_ws = false;
			}
		}

		s.replace(pos, brace - pos, compacted);
		pos = brace + 1;
	}
}

void convert_if_to_ternary(std::string& s) {
	static const re2::RE2 ternary_re(R"(if\s*\(([^()]+)\)\s*([A-Za-z_]\w*)\s*=\s*([^;\n]+);\s*else\s*\2\s*=\s*([^;\n]+);)");
	re2::RE2::GlobalReplace(&s, ternary_re, "\\2 = (\\1) ? (\\3) : (\\4);");
}

void compress_bpe_whitespace(std::string& s) {
	std::string out;
	out.reserve(s.size());
	size_t n = s.size();

	auto is_op = [](char c) noexcept {
		return c == '=' || c == '+' || c == '-' || c == '*' || c == '/' ||
			   c == '%' || c == '&' || c == '|' || c == '^' || c == '<' ||
			   c == '>' || c == '!' || c == '?' || c == ':' || c == ',' ||
			   c == ';' || c == '(' || c == ')' || c == '{' || c == '}' ||
			   c == '[' || c == ']';
	};

	for (size_t i = 0; i < n; ++i) {
		char c = s[i];
		if (c == ' ' || c == '\t') {
			if (out.empty() || out.back() == ' ' || out.back() == '\n') continue;
			size_t next_idx = i + 1;
			while (next_idx < n && (s[next_idx] == ' ' || s[next_idx] == '\t')) ++next_idx;
			if (next_idx < n) {
				char next_c = s[next_idx];
				char prev_c = out.back();
				if (is_op(prev_c) || is_op(next_c)) {
					if ((prev_c == '+' && next_c == '+') || (prev_c == '-' && next_c == '-')) {
						out += ' ';
					}
					continue;
				}
			}
			out += ' ';
		} else {
			out += c;
		}
	}
	s = std::move(out);
}

void extract_skeleton_cpp(std::vector<Token>& tokens, bool public_only) {
	bool in_func_body = false;
	bool inside_class = false;
	bool is_private = false;
	int class_depth = 0;
	int body_depth = 0;
	size_t decl_start_in_out = 0;

	for (size_t idx = 0; idx < tokens.size(); ++idx) {
		auto& t = tokens[idx];
		if (t.excluded) {

			in_func_body = false;
			body_depth = 0;
			decl_start_in_out = 0;
			continue;
		}
		if (t.type != TokenType::Code) {
			if (in_func_body) t.clear();
			continue;
		}

		std::string s = std::string(t.text());
		std::string out;
		out.reserve(s.size());

		for (size_t i = 0; i < s.size(); ++i) {
			char c = s[i];

			if (in_func_body) {
				if (c == '{') {
					++body_depth;
				} else if (c == '}') {
					--body_depth;
					if (body_depth == 0) {
						in_func_body = false;
						if (inside_class) {
							if (public_only && is_private) {
								out.erase(decl_start_in_out);
							} else {
								out += " {}";
							}
						} else {
							out += ';';
						}
					}
				}
				continue;
			}

			if (s.compare(i, 7, "public:") == 0 && is_boundary(s, i, 6)) {
				is_private = false;
			} else if (s.compare(i, 8, "private:") == 0 && is_boundary(s, i, 7)) {
				is_private = true;
			} else if (s.compare(i, 10, "protected:") == 0 && is_boundary(s, i, 9)) {
				is_private = false;
			}

			if (c == '{') {
				size_t check = i;
				while (check > 0 && std::isspace(static_cast<unsigned char>(s[check - 1]))) --check;
				bool is_func = false;

				if (check > 0) {
					size_t paren_search = check;
					while (paren_search > 0 && s[paren_search - 1] != ';' && s[paren_search - 1] != '{' && s[paren_search - 1] != '}') {
						if (s[paren_search - 1] == ')') {
							is_func = true;
							break;
						}
						--paren_search;
					}

					if (is_func) {
						size_t kw_start = paren_search;
						while (kw_start > 0 && is_ident(s[kw_start - 1])) --kw_start;
						std::string_view kw(s.data() + kw_start, paren_search - kw_start);
						if (kw == "if" || kw == "for" || kw == "while" || kw == "switch" || kw == "catch") {
							is_func = false;
						}
					}

					size_t decl_scan = paren_search > 0 ? paren_search - 1 : 0;
					while (decl_scan > 0 && s[decl_scan] != ';' && s[decl_scan] != '{' && s[decl_scan] != '}') --decl_scan;
					std::string_view prefix(s.data() + decl_scan, i - decl_scan);
					if (prefix.find("class ") != std::string_view::npos ||
						prefix.find("struct ") != std::string_view::npos ||
						prefix.find("union ") != std::string_view::npos ||
						prefix.find("enum ") != std::string_view::npos ||
						prefix.find("namespace ") != std::string_view::npos) {
						is_func = false;
						if (prefix.find("class ") != std::string_view::npos) {
							inside_class = true;
							is_private = true;
							++class_depth;
						} else if (prefix.find("struct ") != std::string_view::npos) {
							inside_class = true;
							is_private = false;
							++class_depth;
						}
					} else if (is_func) {
						decl_start_in_out = out.size() - (i - decl_scan);
					}
				}

				if (is_func) {
					in_func_body = true;
					body_depth = 1;
					continue;
				} else if (!inside_class) {
					size_t decl_scan = i;
					while (decl_scan > 0 && s[decl_scan] != ';' && s[decl_scan] != '{' && s[decl_scan] != '}') --decl_scan;
					std::string_view prefix(s.data() + decl_scan, i - decl_scan);
					if (prefix.find("class ") != std::string_view::npos) {
						inside_class = true;
						is_private = true;
						++class_depth;
					} else if (prefix.find("struct ") != std::string_view::npos) {
						inside_class = true;
						is_private = false;
						++class_depth;
					}
				}
			} else if (c == '}') {
				if (inside_class && class_depth > 0) {
					--class_depth;
					if (class_depth == 0) inside_class = false;
				}
			}

			out += c;
		}

		t.set_text(std::move(out));
	}
}

void extract_skeleton_python(std::vector<Token>& tokens) {
	for (auto& t : tokens) {
		if (t.excluded || t.type != TokenType::Code || t.empty()) continue;
		std::string s = std::string(t.text());
		size_t pos = 0;
		bool modified = false;

		while ((pos = s.find("def ", pos)) != std::string::npos) {
			if (!is_boundary(s, pos, 4)) { pos += 4; continue; }

			size_t open_paren = s.find('(', pos + 4);
			if (open_paren == std::string::npos) break;

			int paren_depth = 1;
			int bracket_depth = 0;
			int brace_depth = 0;
			size_t scan = open_paren + 1;

			while (scan < s.size() && paren_depth > 0) {
				char c = s[scan];
				if (c == '(') ++paren_depth;
				else if (c == ')') --paren_depth;
				else if (c == '[') ++bracket_depth;
				else if (c == ']') --bracket_depth;
				else if (c == '{') ++brace_depth;
				else if (c == '}') --brace_depth;
				++scan;
			}

			if (paren_depth != 0) break;

			size_t colon = s.find(':', scan - 1);
			if (colon == std::string::npos) break;

			size_t next_line = s.find('\n', colon);
			if (next_line == std::string::npos) {
				s.append("\n    pass");
				modified = true;
				break;
			}

			size_t body_start = next_line + 1;
			size_t indent = 0;
			while (body_start + indent < s.size() && (s[body_start + indent] == ' ' || s[body_start + indent] == '\t')) ++indent;

			size_t end_body = body_start;
			while (end_body < s.size()) {
				size_t line_end = s.find('\n', end_body);
				if (line_end == std::string_view::npos) line_end = s.size();
				std::string_view line(s.data() + end_body, line_end - end_body);
				size_t cur_indent = 0;
				while (cur_indent < line.size() && (line[cur_indent] == ' ' || line[cur_indent] == '\t')) ++cur_indent;
				if (cur_indent < line.size() && cur_indent < indent) break;
				if (line_end == s.size()) { end_body = s.size(); break; }
				end_body = line_end + 1;
			}

			std::string replacement = "\n    pass\n";
			s.replace(next_line, end_body - next_line, replacement);
			modified = true;
			pos = next_line + replacement.size();
		}

		if (modified) {
			t.set_text(std::move(s));
		}
	}
}

void modernize_guards_cpp(std::vector<Token>& tokens) {
	size_t ifndef_idx = std::string::npos;
	size_t define_idx = std::string::npos;
	std::string guard_macro;

	for (size_t i = 0; i < tokens.size(); ++i) {
		if (tokens[i].excluded) continue;
		if (tokens[i].type != TokenType::Preprocessor) continue;
		std::string_view text = tokens[i].text();
		size_t p = text.find("#ifndef");
		if (p != std::string_view::npos && is_boundary(text, p + 1, 6)) {
			p += 7;
			while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
			size_t start = p;
			while (p < text.size() && is_ident(text[p])) ++p;
			if (p > start) {
				guard_macro = std::string(text.substr(start, p - start));
				ifndef_idx = i;
				break;
			}
		}
	}

	if (ifndef_idx != std::string::npos && !guard_macro.empty()) {
		for (size_t i = ifndef_idx + 1; i < tokens.size(); ++i) {
			if (tokens[i].type != TokenType::Preprocessor) continue;
			std::string_view text = tokens[i].text();
			size_t p = text.find("#define");
			if (p != std::string_view::npos && is_boundary(text, p + 1, 6)) {
				p += 7;
				while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
				if (text.substr(p, guard_macro.size()) == guard_macro &&
					(p + guard_macro.size() >= text.size() || !is_ident(text[p + guard_macro.size()]))) {
					define_idx = i;
					break;
				}
			}
		}
	}

	if (ifndef_idx != std::string::npos && define_idx != std::string::npos) {
		int if_depth = 1;
		size_t target_endif = std::string::npos;

		for (size_t k = define_idx + 1; k < tokens.size(); ++k) {
			if (tokens[k].type != TokenType::Preprocessor) continue;
			std::string_view text = tokens[k].text();

			size_t hash = text.find('#');
			if (hash == std::string_view::npos) continue;
			size_t d = hash + 1;
			while (d < text.size() && std::isspace(static_cast<unsigned char>(text[d]))) ++d;

			size_t w_end = d;
			while (w_end < text.size() && is_ident(text[w_end])) ++w_end;
			std::string_view word = text.substr(d, w_end - d);

			if (word == "if" || word == "ifdef" || word == "ifndef") {
				++if_depth;
			} else if (word == "endif") {
				--if_depth;
				if (if_depth == 0) {
					target_endif = k;
					break;
				}
			}
		}

		if (target_endif != std::string::npos && !tokens[ifndef_idx].excluded &&
			!tokens[define_idx].excluded && !tokens[target_endif].excluded) {
			tokens[ifndef_idx].set_text("#pragma once");
			tokens[define_idx].clear();
			tokens[target_endif].clear();
		}
	}
}

void apply_cpp_transforms(std::vector<Token>& tokens, const Options& opt) {
	if (opt.unwrap_parentheses) {
		int paren_depth = 0;
		int brace_depth = 0;
		for (auto& t : tokens) {
			if (t.excluded || t.type != TokenType::Code || t.empty()) continue;
			std::string s(t.text());
			unwrap_parentheses_dfa(s, paren_depth, brace_depth);
			t.set_text(std::move(s));
		}
	}
	if (opt.compact_short_functions) compact_short_functions_dfa(tokens);
	if (opt.modernize_guards) {
		modernize_guards_cpp(tokens);
	}

	if (opt.extract_skeleton) {
		extract_skeleton_cpp(tokens, opt.skeleton_public_only);
	}

	if (opt.strip_debug) {
		static const std::string_view debug_targets[] = {
			"std::cout", "printf", "qDebug", "assert"
		};
		bool in_debug_stmt = false;

		for (auto& t : tokens) {
			if (t.excluded) { in_debug_stmt = false; continue; }
			if (t.type == TokenType::Code) {
				std::string text_val = std::string(t.text());
				bool modified = false;

				while (!text_val.empty()) {
					if (in_debug_stmt) {
						size_t semi = text_val.find(';');
						if (semi != std::string::npos) {
							text_val.erase(0, semi + 1);
							in_debug_stmt = false;
							modified = true;
						} else {
							text_val.clear();
							modified = true;
							break;
						}
					} else {
						size_t earliest_pos = std::string::npos;
						size_t target_len = 0;

						for (auto dbg : debug_targets) {
							size_t p = 0;
							while ((p = text_val.find(dbg, p)) != std::string::npos) {
								if (is_boundary(text_val, p, dbg.size())) {
									if (p < earliest_pos) {
										earliest_pos = p;
										target_len = dbg.size();
									}
									break;
								}
								p += dbg.size();
							}
						}

						if (earliest_pos != std::string::npos) {
							size_t search_semi = earliest_pos + target_len;
							size_t semi = text_val.find(';', search_semi);
							if (semi != std::string::npos) {
								text_val.erase(earliest_pos, (semi - earliest_pos) + 1);
								modified = true;
							} else {
								text_val.erase(earliest_pos);
								in_debug_stmt = true;
								modified = true;
								break;
							}
						} else {
							break;
						}
					}
				}

				if (modified) {
					t.set_text(std::move(text_val));
				}
			} else if (in_debug_stmt) {
				t.clear();
			}
		}
	}

	for (auto& t : tokens) {
		if (t.excluded || t.type != TokenType::Code || t.empty()) continue;
		std::string s = std::string(t.text());
		size_t original_size = s.size();

		if (opt.strip_attributes) {
			strip_attributes_dfa(s);
		}
		if (opt.strip_redundant) {
			strip_redundant_specifiers_dfa(s);
		}
		if (opt.modernize_namespaces) {
			modernize_namespaces_dfa(s);
		}
		if (opt.compact_types) {
			compact_types_dfa(s);
		}
		if (opt.compact_constructors) {
			compact_constructors_dfa(s);
		}
		if (opt.if_to_ternary) {
			convert_if_to_ternary(s);
		}
		if (opt.bpe_compress) {
			compress_bpe_whitespace(s);
		}

		if (s.size() != original_size) {
			t.set_text(std::move(s));
		}
	}
}

void apply_python_transforms(std::vector<Token>& tokens, const Options& opt) {
	if (opt.extract_skeleton) {
		extract_skeleton_python(tokens);
	}

	static const re2::RE2 ret_type_re(R"(\s*->\s*[^:\n]+)");
	static const re2::RE2 arg_type_re(R"(:\s*[\w\.]+(?:\[[^\]]*\])?(?=\s*[,=\)]))");

	for (auto& t : tokens) {
		if (t.excluded) continue;
		if (t.type == TokenType::Docstring && opt.strip_docstrings) {
			t.clear();
			continue;
		}
		if (t.type == TokenType::Code && opt.strip_types && !t.empty()) {
			std::string s = std::string(t.text());
			size_t pos = 0;
			bool modified = false;

			while ((pos = s.find("def ", pos)) != std::string::npos) {
				if (!is_boundary(s, pos, 4)) { pos += 4; continue; }
				size_t open_paren = s.find('(', pos + 4);
				if (open_paren == std::string::npos) break;

				int paren_depth = 1;
				int bracket_depth = 0;
				int brace_depth = 0;
				size_t scan = open_paren + 1;

				while (scan < s.size() && paren_depth > 0) {
					char c = s[scan];
					if (c == '(') ++paren_depth;
					else if (c == ')') --paren_depth;
					else if (c == '[') ++bracket_depth;
					else if (c == ']') --bracket_depth;
					else if (c == '{') ++brace_depth;
					else if (c == '}') --brace_depth;
					++scan;
				}

				if (paren_depth != 0) break;

				size_t close_paren = scan - 1;
				size_t colon = s.find(':', close_paren);
				if (colon == std::string::npos) break;

				std::string args = s.substr(open_paren, close_paren - open_paren + 1);
				re2::RE2::GlobalReplace(&args, arg_type_re, "");

				std::string ret_part = s.substr(close_paren + 1, colon - (close_paren + 1));
				re2::RE2::GlobalReplace(&ret_part, ret_type_re, "");

				std::string new_sig = args + ret_part + ":";
				s.replace(open_paren, (colon - open_paren) + 1, new_sig);
				modified = true;
				pos = open_paren + new_sig.size();
			}

			if (modified) {
				t.set_text(std::move(s));
			}
		}
	}
}

}

std::vector<CustomRule> parse_rules_json(std::string_view json_str) {
	std::vector<CustomRule> rules;
	try {
		auto j = nlohmann::json::parse(json_str);
		if (j.is_array()) {
			for (const auto& item : j) {
				CustomRule r;
				r.name = item.value("name", "");
				r.regex_str = item.value("regex", "");
				r.replace = item.value("replace", "");
				if (!r.regex_str.empty()) {
					r.compiled_re = std::make_shared<re2::RE2>(r.regex_str);
					if (r.compiled_re->ok()) {
						rules.push_back(std::move(r));
					}
				}
			}
		}
	} catch (...) {}
	return rules;
}

Result process(std::string_view src, const Options& opt) {
	Result res;
	res.in_bytes = src.size();
	res.in_lines = count_lines(src);
	if (src.empty()) {
		res.output = "";
		res.out_bytes = 0;
		res.out_lines = 0;
		return res;
	}
	Language lang = opt.lang;
	if (lang == Language::Auto) lang = Language::Cpp;
	std::vector<Token> tokens;
	if (lang == Language::Cpp) tokens = tokenize_cpp(src, opt.excluded_lines);
	else if (lang == Language::Java) tokens = tokenize_cpp(src, opt.excluded_lines, true);
	else if (lang == Language::Xml) tokens = tokenize_xml(src, opt.excluded_lines);
	else if (lang == Language::JavaScript || lang == Language::Css || lang == Language::Php) {
		tokens = tokenize_web(src, lang);
		split_excluded_code_lines(tokens, opt.excluded_lines);
	} else if (lang == Language::Html) {
		tokens = tokenize_html(src);
		split_excluded_code_lines(tokens, opt.excluded_lines);
	}
	else if (lang == Language::Python) tokens = tokenize_python(src, opt.excluded_lines);
	else if (lang == Language::Shell) tokens = tokenize_shell(src, opt.excluded_lines);
	else { tokens.push_back({TokenType::Code, src, {}, false}); split_excluded_code_lines(tokens, opt.excluded_lines); }
	if (lang == Language::Cpp) apply_cpp_transforms(tokens, opt);
	else if (lang == Language::Python) apply_python_transforms(tokens, opt);
	else if (lang == Language::Java) transform_token_regions(tokens, lang, [&](std::string &region) { transform_java(region, opt); });
	else if (lang == Language::JavaScript || lang == Language::Php) transform_token_regions(tokens, lang, [&](std::string &region) { transform_js_php(region, lang, opt); });
	else if (lang == Language::Xml) transform_token_regions(tokens, lang, [&](std::string &region) { transform_xml(region, opt); });
	else if (lang == Language::Css) transform_css(tokens, opt);
	else if (lang == Language::Html) {
		transform_html_embedded(tokens, opt);
		transform_html(tokens, opt.collapse_html_whitespace);
	}
	if (!opt.custom_rules.empty()) {
		for (auto& t : tokens) {
			const bool markup = lang == Language::Xml || lang == Language::Html;
			if (t.excluded || (t.type != TokenType::Code && !(markup && t.type == TokenType::XmlTag)) || t.empty()) continue;
			std::string s(t.text());
			bool changed = false;
			for (const auto& r : opt.custom_rules) if (r.compiled_re && r.compiled_re->ok()) changed |= re2::RE2::GlobalReplace(&s, *r.compiled_re, r.replace);
			if (changed) {
				t.set_text(std::move(s));
			}
		}
	}
	std::erase_if(tokens, [](const Token& t) noexcept {
		return t.empty();
	});
	std::string intermediate;
	intermediate.reserve(src.size());
	std::string preserved_output;
	bool last_blank = false;
	int brace_depth = 0;
	auto flush_before_protected = [&]() {
		if (intermediate.empty()) return;
		if (intermediate.back() == '\n') clean_whitespace_append(intermediate, opt.blank_mode, opt.indent_mode, opt.tab_size, preserved_output, last_blank, &brace_depth);
		else { preserved_output += intermediate; last_blank = false; }
		intermediate.clear();
	};
	for (size_t idx = 0; idx < tokens.size(); ++idx) {
		const auto& t = tokens[idx];
		if (t.excluded) {
			flush_before_protected();
			preserved_output.append(t.text());
			std::string_view protected_text = t.text(); size_t end = protected_text.size();
			if (end && protected_text[end-1] == '\n') --end;
			if (end == 0) last_blank = true;
			else {
				size_t newline = protected_text.rfind('\n', end - 1);
				size_t start = newline == std::string_view::npos ? 0 : newline + 1;
				std::string_view tail = protected_text.substr(start, end - start);
				last_blank = tail.find_first_not_of(" \t\r") == std::string_view::npos;
			}
			continue;
		}
		if (t.type == TokenType::Comment && opt.strip_comments) {
			if (!intermediate.empty() && idx + 1 < tokens.size()) {
				std::string_view next_text = tokens[idx + 1].text();
				if (!next_text.empty() && is_ident(intermediate.back()) && is_ident(next_text.front())) {
					intermediate += ' ';
				}
			}
			continue;
		}
		const bool webLanguage = lang == Language::JavaScript || lang == Language::Css || lang == Language::Php;
		const bool protectedHtmlLiteral = lang == Language::Html && (t.type == TokenType::String || t.type == TokenType::Regex || t.type == TokenType::Template || t.type == TokenType::HereDoc);
		if ((webLanguage && t.type != TokenType::Code) || protectedHtmlLiteral) {
			flush_before_protected();
			preserved_output.append(t.text());
			last_blank = false;
			continue;
		}
		intermediate.append(t.text());
	}
	const bool protectedWeb = lang == Language::JavaScript || lang == Language::Css || lang == Language::Php;
	if (protectedWeb) {
		if (!intermediate.empty()) {
			if (intermediate.back() == '\n') clean_whitespace_append(intermediate, opt.blank_mode, opt.indent_mode, opt.tab_size, preserved_output, last_blank, &brace_depth);
			else preserved_output += intermediate;
		}
		res.output = std::move(preserved_output);
	}
	else if (opt.excluded_lines.empty()) res.output = clean_whitespace(intermediate, opt.blank_mode, opt.indent_mode, opt.tab_size);
	else { if (!intermediate.empty()) clean_whitespace_append(intermediate, opt.blank_mode, opt.indent_mode, opt.tab_size, preserved_output, last_blank, &brace_depth); res.output = std::move(preserved_output); }
	res.out_bytes = res.output.size();
	res.out_lines = count_lines(res.output);
	if (opt.verify_syntax) {
		auto [valid, err] = verify_syntax_posix(res.output, lang);
		res.syntax_valid = valid;
		res.syntax_error = std::move(err);
	}
	return res;
}

}
