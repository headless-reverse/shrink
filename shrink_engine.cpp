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

bool glob_match(std::string_view pat, std::string_view str) noexcept {
	if (pat.empty()) return str.empty();
	if (pat == "*") return true;

	if (pat.starts_with("**/")) {
		std::string_view rest = pat.substr(3);
		if (glob_match(rest, str)) return true;
		for (size_t i = 0; i < str.size(); ++i) {
			if (str[i] == '/' && glob_match(pat, str.substr(i + 1))) return true;
		}
		return false;
	}

	size_t p = 0, s = 0;
	size_t star_p = std::string_view::npos, star_s = 0;

	while (s < str.size()) {
		if (p < pat.size() && (pat[p] == str[s] || (pat[p] == '?' && str[s] != '/'))) {
			++p;
			++s;
		} else if (p < pat.size() && pat[p] == '*') {
			star_p = p++;
			star_s = s;
		} else if (star_p != std::string_view::npos) {
			if (str[star_s] == '/') return false;
			p = star_p + 1;
			s = ++star_s;
		} else {
			return false;
		}
	}

	while (p < pat.size() && pat[p] == '*') ++p;
	return p == pat.size();
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

	r.pattern = std::string(raw);
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

	std::string_view basename = rel_path;
	size_t slash = rel_path.rfind('/');
	if (slash != std::string_view::npos) basename = rel_path.substr(slash + 1);

	bool ignored = false;
	for (const auto& r : rules) {
		if (r.dir_only && !is_dir) continue;
		bool match = false;
		if (r.anchored) {
			match = glob_match(r.pattern, rel_path);
		} else {
			match = glob_match(r.pattern, basename) || glob_match(r.pattern, rel_path);
		}

		if (match) {
			ignored = !r.is_negation;
		}
	}
	return ignored;
}

namespace {

std::vector<Token> tokenize_cpp(std::string_view src, std::span<const size_t> excluded_lines) {
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
			size_t start = i++;
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

std::pair<bool, std::string> verify_syntax_posix(std::string_view code, Language lang) {
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
	else if (lang == Language::Python) tokens = tokenize_python(src, opt.excluded_lines);
	else if (lang == Language::Shell) tokens = tokenize_shell(src, opt.excluded_lines);
	else { tokens.push_back({TokenType::Code, src, {}, false}); split_excluded_code_lines(tokens, opt.excluded_lines); }
	if (lang == Language::Cpp) apply_cpp_transforms(tokens, opt);
	else if (lang == Language::Python) apply_python_transforms(tokens, opt);
	if (!opt.custom_rules.empty()) {
		for (auto& t : tokens) {
			if (t.excluded || t.type != TokenType::Code || t.empty()) continue;
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
		intermediate.append(t.text());
	}
	if (opt.excluded_lines.empty()) res.output = clean_whitespace(intermediate, opt.blank_mode, opt.indent_mode, opt.tab_size);
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
