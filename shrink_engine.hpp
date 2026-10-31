#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstddef>
#include <filesystem>
#include <unordered_set>
#include <span>
#include <re2/re2.h>
#include "nlohmann/json.hpp"

namespace shrink {

enum class Language {
	Auto,
	Cpp,
	Python,
	Shell,
	Plain
};

enum class BlankLineMode {
	Keep,
	Collapse,
	RemoveAll,
	BetweenFunctionsOnly
};

enum class TokenType {
	Code,
	String,
	Char,
	RawString,
	Comment,
	Docstring,
	Preprocessor,
	HereDoc
};

enum class IndentMode {
	Keep,
	SpacesToTabs,
	TabsToSpaces
};

struct CustomRule {
	std::string name;
	std::string regex_str;
	std::string replace;
	std::shared_ptr<re2::RE2> compiled_re;
};

struct Options {
	Language lang = Language::Auto;
	bool strip_comments = true;
	bool unwrap_parentheses = false;
	bool compact_short_functions = false;
	BlankLineMode blank_mode = BlankLineMode::RemoveAll;
	IndentMode indent_mode = IndentMode::Keep;
	int tab_size = 4;
	bool extract_skeleton = false;
	bool skeleton_public_only = false;
	bool modernize_guards = false;
	bool strip_attributes = false;
	bool strip_redundant = false;
	bool modernize_namespaces = false;
	bool compact_types = false;
	bool compact_constructors = false;
	bool if_to_ternary = false;
	bool bpe_compress = false;
	bool strip_debug = false;
	bool strip_docstrings = false;
	bool strip_types = false;
	bool preserve_shebang = true;
	bool verify_syntax = false;
	std::vector<size_t> excluded_lines;
	std::vector<CustomRule> custom_rules;
};

struct Result {
	std::string output;
	bool syntax_valid = true;
	std::string syntax_error;
	size_t in_bytes = 0;
	size_t out_bytes = 0;
	size_t in_lines = 0;
	size_t out_lines = 0;
};

struct Token {
	TokenType type;
	std::string_view view{};
	std::string mutated{};
	bool is_mutated = false;
	bool excluded = false;

	std::string_view text() const noexcept {
		return is_mutated ? std::string_view(mutated) : view;
	}

	void set_text(std::string s) {
		mutated = std::move(s);
		is_mutated = true;
	}

	void clear() noexcept {
		view = {};
		mutated.clear();
		is_mutated = true;
	}

	bool empty() const noexcept {
		return text().empty();
	}
};

struct GitIgnoreRule {
	std::string pattern;
	bool is_negation = false;
	bool dir_only = false;
	bool anchored = false;
};

class GitIgnoreEngine {
	std::vector<GitIgnoreRule> rules;

public:
	void add_rule(std::string_view raw);
	void load_file(const std::filesystem::path& path);
	bool is_ignored(std::string_view rel_path, bool is_dir) const noexcept;
};

std::vector<CustomRule> parse_rules_json(std::string_view json_str);
Result process(std::string_view src, const Options& opt);

}
