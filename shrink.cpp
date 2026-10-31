#include "shrink_engine.hpp"
#include <iostream>
#include <vector>
#include <filesystem>
#include <thread>
#include <atomic>
#include <iomanip>
#include <cstring>
#include <set>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

namespace fs = std::filesystem;

namespace {

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr size_t hardware_destructive_interference_size = 64;
#endif

constexpr size_t BATCH_CHUNK = 16;

struct MappedFile {
	int fd = -1;
	void* data = MAP_FAILED;
	size_t size = 0;

	MappedFile(const char* path) noexcept {
		fd = ::open(path, O_RDONLY);
		if (fd < 0) return;
		struct stat st;
		if (::fstat(fd, &st) < 0) {
			::close(fd);
			fd = -1;
			return;
		}
		size = static_cast<size_t>(st.st_size);
		if (size == 0) {
			::close(fd);
			fd = -1;
			data = nullptr;
			return;
		}
		data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (data != MAP_FAILED) {
			::posix_madvise(data, size, POSIX_MADV_SEQUENTIAL);
		}
	}

	~MappedFile() noexcept {
		if (data != MAP_FAILED && data != nullptr && size > 0) {
			::munmap(data, size);
		}
		if (fd >= 0) {
			::close(fd);
		}
	}

	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;

	MappedFile(MappedFile&& o) noexcept : fd(o.fd), data(o.data), size(o.size) {
		o.fd = -1; o.data = MAP_FAILED; o.size = 0;
	}

	MappedFile& operator=(MappedFile&& o) noexcept {
		if (this != &o) {
			if (data != MAP_FAILED && data != nullptr && size > 0) ::munmap(data, size);
			if (fd >= 0) ::close(fd);
			fd = o.fd; data = o.data; size = o.size;
			o.fd = -1; o.data = MAP_FAILED; o.size = 0;
		}
		return *this;
	}

	bool is_valid() const noexcept {
		return size == 0 ? (data == nullptr) : (fd >= 0 && data != MAP_FAILED);
	}

	std::string_view view() const noexcept {
		if (!is_valid() || size == 0) return {};
		return {static_cast<const char*>(data), size};
	}
};

bool write_all(int fd, std::string_view content) noexcept {
	size_t written = 0;
	while (written < content.size()) {
		ssize_t res = ::write(fd, content.data() + written, content.size() - written);
		if (res < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		written += static_cast<size_t>(res);
	}
	return true;
}

bool write_file(const fs::path& path, std::string_view content) noexcept {
	int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	bool ok = write_all(fd, content);
	::close(fd);
	return ok;
}

std::string read_stdin_all() {
	std::string content;
	std::array<char, 65536> buf;
	while (true) {
		ssize_t res = ::read(STDIN_FILENO, buf.data(), buf.size());
		if (res > 0) {
			content.append(buf.data(), static_cast<size_t>(res));
		} else if (res < 0) {
			if (errno == EINTR) continue;
			break;
		} else {
			break;
		}
	}
	return content;
}

shrink::Language detect_lang(const fs::path& p) noexcept {
	auto ext = p.extension().native();
	if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".c" || ext == ".h") return shrink::Language::Cpp;
	if (ext == ".py") return shrink::Language::Python;
	if (ext == ".sh" || ext == ".bash") return shrink::Language::Shell;
	if (ext == ".java") return shrink::Language::Java;
	if (ext == ".xml" || ext == ".xhtml" || ext == ".pom") return shrink::Language::Xml;
	if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return shrink::Language::JavaScript;
	if (ext == ".css") return shrink::Language::Css;
	if (ext == ".html" || ext == ".htm") return shrink::Language::Html;
	if (ext == ".php" || ext == ".phtml") return shrink::Language::Php;
	return shrink::Language::Plain;
}

std::string_view lang_to_fence(shrink::Language l) noexcept {
	switch (l) {
		case shrink::Language::Cpp: return "cpp";
		case shrink::Language::Python: return "python";
		case shrink::Language::Shell: return "bash";
		case shrink::Language::Java: return "java";
		case shrink::Language::Xml: return "xml";
		case shrink::Language::JavaScript: return "javascript";
		case shrink::Language::Css: return "css";
		case shrink::Language::Html: return "html";
		case shrink::Language::Php: return "php";
		case shrink::Language::Plain: return "text";
		default: return "text";
	}
}

void print_help() noexcept {
	const char help_text[] =
		"Shrink CLI (C++20 Engine)\n"
		"Uzycie: shrink [opcje] [sciezka/plik]\n\n"
		"Wybor silnika:\n"
		"  -l, --lang <cpp|python|shell|plain> Wymuszenie jezyka\n"
		"  -r, --recursive <katalog>           Przetwarzanie rekurencyjne drzewa (uwzglednia .gitignore)\n"
		"  --out-dir <katalog>                 Katalog zapisu dla przetwarzania wsadowego\n"
		"  --bundle-llm [plik.md]              Generuje pojedynczy kontekst Markdown dla modeli LLM\n"
		"  -j, --jobs <N>                      Liczba watkow (domyslnie: CPU cores)\n\n"
		"Transformacje semantyczne:\n"
		"  --extract-skeleton                  Usuwa ciala funkcji/metod (pozostawia interfejs)\n"
		"  --skeleton-public                   Wycina metody prywatne ze szkieletu (czyste API)\n"
		"  --modernize-guards                  Zamienia #ifndef/#define/#endif na #pragma once\n\n"
		"Transformacje C++:\n"
		"  --strip-attributes                  Usuwa [[nodiscard]], noexcept, inline\n"
		"  --strip-redundant                   Usuwa zbedne virtual przy override/final\n"
		"  --strip-debug                       Usuwa cout, printf, qDebug, assert\n"
		"  --modernize-namespaces              Splaszcza namespace A { namespace B {}\n"
		"  --compact-types                     Zwijanie prostych struktur do jednej linii\n"
		"  --compact-constructors              Zwijanie wieloliniowych list inicjalizacyjnych\n"
		"  --if-to-ternary                     Zastepuje proste if-else operatorem ?:\n"
		"  --unwrap-parentheses                Splaszcza wieloliniowe wyrazenia w nawiasach ()\n"
		"  --compact-short-functions          Zwijanie jednoinstrukcyjnych funkcji do jednej linii\n"
		"  --bpe-compress                      Agresywna minifikacja spacji pod tokenizery LLM\n\n"
		"Transformacje Python:\n"
		"  --strip-docstrings                  Usuwa docstringi klas i metod\n"
		"  --strip-types                       Usuwa adnotacje typow z sygnatur\n\n"
		"Formatowanie i Reguly:\n"
		"  --rules <plik.json>                 Wczytuje zewnetrzne reguly JSON z silnikiem RE2\n"
		"  --keep-comments                     Nie usuwaj komentarzy\n"
		"  --collapse-blanks                   Zwijaj wielokrotne puste linie\n"
		"  --keep-blanks                       Nie modyfikuj pustych linii\n"
		"  --strip-inner-blanks                Zachowuje puste linie tylko miedzy funkcjami\n"
		"  --verify                            Weryfikuje poprawnosc skladniowa (kod 4 przy bledzie)\n"
		"  -o <plik>                           Zapisz wynik pojedynczego pliku\n";
	::write(STDOUT_FILENO, help_text, sizeof(help_text) - 1);
}

struct BundleEntry {
	std::string rel_path;
	std::string content;
	shrink::Language lang;
};

struct alignas(hardware_destructive_interference_size) WorkerContext {
	std::vector<BundleEntry> bundle_entries;
	size_t in_bytes = 0;
	size_t out_bytes = 0;
	size_t in_lines = 0;
	size_t out_lines = 0;
	size_t failed_syntax = 0;
};

std::string generate_llm_bundle(const std::vector<BundleEntry>& entries) {
	std::string out = "# LLM Context Bundle\n\n";
	out += "Pliki: " + std::to_string(entries.size()) + "\n\n";
	out += "## Drzewo projektu\n\n```text\n";

	std::set<std::string> printed_dirs;
	for (const auto& e : entries) {
		fs::path p(e.rel_path);
		std::string accum;
		auto it = p.begin();
		auto end = p.end();
		size_t depth = 0;

		for (; it != end; ++it) {
			auto next = it;
			++next;
			if (next == end) {
				for (size_t d = 0; d < depth; ++d) out += "  ";
				out += it->string() + "\n";
			} else {
				accum += it->string() + "/";
				if (printed_dirs.insert(accum).second) {
					for (size_t d = 0; d < depth; ++d) out += "  ";
					out += it->string() + "/\n";
				}
				++depth;
			}
		}
	}
	out += "```\n";

	for (const auto& e : entries) {
		size_t max_consecutive_backticks = 0;
		size_t cur = 0;
		for (char c : e.content) {
			if (c == '`') {
				++cur;
				if (cur > max_consecutive_backticks) max_consecutive_backticks = cur;
			} else {
				cur = 0;
			}
		}

		size_t fence_len = std::max<size_t>(3, max_consecutive_backticks + 1);
		std::string fence(fence_len, '`');

		out += "\n### Plik: " + e.rel_path + "\n\n";
		out += fence;
		out.append(lang_to_fence(e.lang));
		out += "\n";
		out.append(e.content);
		if (!e.content.empty() && e.content.back() != '\n') out += "\n";
		out += fence + "\n";
	}

	return out;
}

}

int main(int argc, char* argv[]) {
	shrink::Options opt;
	std::string input_path;
	std::string output_path;
	std::string out_dir;
	std::string bundle_output;
	std::string rules_file;
	bool recursive = false;
	bool bundle_mode = false;
	unsigned int jobs = std::max(1u, std::thread::hardware_concurrency());
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "-h" || arg == "--help") { print_help(); return 0; }
		else if (arg == "-l" || arg == "--lang") {
			if (++i < argc) {
				std::string_view l = argv[i];
				if (l == "cpp") opt.lang = shrink::Language::Cpp;
				else if (l == "python") opt.lang = shrink::Language::Python;
				else if (l == "shell") opt.lang = shrink::Language::Shell;
				else if (l == "java") opt.lang = shrink::Language::Java;
				else if (l == "xml") opt.lang = shrink::Language::Xml;
				else if (l == "javascript" || l == "js") opt.lang = shrink::Language::JavaScript;
				else if (l == "css") opt.lang = shrink::Language::Css;
				else if (l == "html") opt.lang = shrink::Language::Html;
				else if (l == "php") opt.lang = shrink::Language::Php;
				else opt.lang = shrink::Language::Plain;
			}
		}
		else if (arg == "-r" || arg == "--recursive") {
			recursive = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') input_path = argv[++i];
		}
		else if (arg == "--out-dir") { if (++i < argc) out_dir = argv[i]; }
		else if (arg == "--bundle-llm") {
			bundle_mode = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') bundle_output = argv[++i];
		}
		else if (arg == "-j" || arg == "--jobs") { if (++i < argc) jobs = std::max(1, std::atoi(argv[i])); }
		else if (arg == "--rules") { if (++i < argc) rules_file = argv[i]; }
		else if (arg == "--extract-skeleton") opt.extract_skeleton = true;
		else if (arg == "--skeleton-public") { opt.extract_skeleton = true; opt.skeleton_public_only = true; }
		else if (arg == "--modernize-guards") opt.modernize_guards = true;
		else if (arg == "--strip-attributes") opt.strip_attributes = true;
		else if (arg == "--strip-redundant") opt.strip_redundant = true;
		else if (arg == "--strip-debug") opt.strip_debug = true;
		else if (arg == "--modernize-namespaces") opt.modernize_namespaces = true;
		else if (arg == "--compact-types") opt.compact_types = true;
		else if (arg == "--compact-constructors") opt.compact_constructors = true;
		else if (arg == "--if-to-ternary") opt.if_to_ternary = true;
		else if (arg == "--unwrap-parentheses") opt.unwrap_parentheses = true;
		else if (arg == "--compact-short-functions") opt.compact_short_functions = true;
		else if (arg == "--strip-inner-blanks") opt.blank_mode = shrink::BlankLineMode::BetweenFunctionsOnly;
		else if (arg == "--bpe-compress") opt.bpe_compress = true;
		else if (arg == "--strip-docstrings") opt.strip_docstrings = true;
		else if (arg == "--strip-types") opt.strip_types = true;
		else if (arg == "--keep-comments") opt.strip_comments = false;
		else if (arg == "--collapse-blanks") opt.blank_mode = shrink::BlankLineMode::Collapse;
		else if (arg == "--keep-blanks") opt.blank_mode = shrink::BlankLineMode::Keep;
		else if (arg == "--verify") opt.verify_syntax = true;
		else if (arg == "-o") { if (++i < argc) output_path = argv[i]; }
		else if (arg[0] != '-') input_path = arg;
	}
	if (!rules_file.empty()) {
		MappedFile rf(rules_file.c_str());
		if (rf.is_valid()) {
			opt.custom_rules = shrink::parse_rules_json(rf.view());
		}
	}
	if (recursive || bundle_mode) {
		if (input_path.empty()) input_path = ".";
		fs::path root_path = fs::canonical(input_path);
		shrink::GitIgnoreEngine gitignore;
		fs::path gitignore_path = root_path / ".gitignore";
		if (fs::exists(gitignore_path)) {
			gitignore.load_file(gitignore_path);
		}
		std::vector<fs::path> file_list;
		for (auto it = fs::recursive_directory_iterator(root_path, fs::directory_options::skip_permission_denied);
			 it != fs::recursive_directory_iterator(); ++it) {

			fs::path rel = fs::relative(it->path(), root_path);
			std::string rel_str = rel.generic_string();

			if (it->is_directory()) {
				if (gitignore.is_ignored(rel_str, true)) {
					it.disable_recursion_pending();
					continue;
				}
			} else if (it->is_regular_file()) {
				if (gitignore.is_ignored(rel_str, false)) {
					continue;
				}

				file_list.push_back(it->path());
			}
		}

		std::sort(file_list.begin(), file_list.end());

		std::atomic<size_t> file_idx = 0;
		std::vector<WorkerContext> worker_contexts(jobs);

		auto worker = [&](size_t thread_id) {
			auto& ctx = worker_contexts[thread_id];

			while (true) {
				size_t start_idx = file_idx.fetch_add(BATCH_CHUNK, std::memory_order_relaxed);
				if (start_idx >= file_list.size()) break;
				size_t end_idx = std::min(start_idx + BATCH_CHUNK, file_list.size());

				for (size_t idx = start_idx; idx < end_idx; ++idx) {
					const auto& path = file_list[idx];
					MappedFile mf(path.c_str());
					if (!mf.is_valid()) continue;

					shrink::Options file_opt = opt;
					if (file_opt.lang == shrink::Language::Auto) file_opt.lang = detect_lang(path);

					auto res = shrink::process(mf.view(), file_opt);
					if (!res.syntax_valid) {
						ctx.failed_syntax += 1;
						continue;
					}

					ctx.in_bytes += res.in_bytes;
					ctx.out_bytes += res.out_bytes;
					ctx.in_lines += res.in_lines;
					ctx.out_lines += res.out_lines;

					fs::path rel = fs::relative(path, root_path);

					if (!out_dir.empty()) {
						fs::path target = fs::path(out_dir) / rel;
						fs::create_directories(target.parent_path());
						write_file(target, res.output);
					}

					if (bundle_mode) {
						ctx.bundle_entries.push_back({rel.generic_string(), std::move(res.output), file_opt.lang});
					}
				}
			}
		};

		std::vector<std::jthread> pool;
		pool.reserve(jobs);
		for (size_t j = 0; j < jobs; ++j) pool.emplace_back(worker, j);
		for (auto& th : pool) th.join();

		size_t in_bytes = 0, out_bytes = 0, in_lines = 0, out_lines = 0, failed_syntax = 0;
		std::vector<BundleEntry> bundle_entries;

		for (auto& ctx : worker_contexts) {
			in_bytes += ctx.in_bytes;
			out_bytes += ctx.out_bytes;
			in_lines += ctx.in_lines;
			out_lines += ctx.out_lines;
			failed_syntax += ctx.failed_syntax;
			if (bundle_mode) {
				bundle_entries.insert(bundle_entries.end(),
					std::make_move_iterator(ctx.bundle_entries.begin()),
					std::make_move_iterator(ctx.bundle_entries.end()));
			}
		}

		double reduction = in_bytes > 0 ? (1.0 - static_cast<double>(out_bytes) / in_bytes) * 100.0 : 0.0;

		std::cerr << "\n--- RAPORT BATCH (" << file_list.size() << " plikow, " << jobs << " watkow) ---\n"
				  << "Rozmiar: " << in_bytes << " B -> " << out_bytes << " B ("
				  << std::fixed << std::setprecision(1) << reduction << "% redukcji)\n"
				  << "Linie:   " << in_lines << " LOC -> " << out_lines << " LOC\n";

		if (bundle_mode) {
			std::sort(bundle_entries.begin(), bundle_entries.end(), [](const BundleEntry& a, const BundleEntry& b) {
				return a.rel_path < b.rel_path;
			});
			std::string bundle_text = generate_llm_bundle(bundle_entries);
			if (!bundle_output.empty()) {
				write_file(bundle_output, bundle_text);
				std::cerr << "Zapisano bundle LLM: " << bundle_output << " (" << bundle_text.size() << " B)\n";
			} else {
				write_all(STDOUT_FILENO, bundle_text);
			}
		}

		if (failed_syntax > 0) {
			std::cerr << "Odrzucono ze wzgledu na blad skladni: " << failed_syntax << " plikow\n";
			return 4;
		}
		return 0;
	}

	std::string stdin_buf;
	std::string_view content;
	MappedFile mf(input_path.c_str());

	if (input_path.empty()) {
		stdin_buf = read_stdin_all();
		content = stdin_buf;
	} else {
		if (!mf.is_valid()) return 2;
		if (opt.lang == shrink::Language::Auto) opt.lang = detect_lang(input_path);
		content = mf.view();
	}

	auto res = shrink::process(content, opt);

	if (opt.verify_syntax && !res.syntax_valid) {
		std::cerr << "Blad weryfikacji skladni [4]: " << res.syntax_error << '\n';
		return 4;
	}

	if (!output_path.empty()) {
		if (!write_file(output_path, res.output)) return 3;
	} else {
		write_all(STDOUT_FILENO, res.output);
	}

	return 0;
}
