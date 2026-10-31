#include "shrink_controller.h"
#include <algorithm>
#include <functional>
#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>
#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <fstream>
#include <sstream>
#include <set>
#include <cctype>
#include <latch>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

namespace fs = std::filesystem;

namespace {
bool write_all_bytes(const fs::path &path, std::string_view content) noexcept {
	int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	size_t written = 0;
	while (written < content.size()) {
		ssize_t res = ::write(fd, content.data() + written, content.size() - written);
		if (res < 0) {
			if (errno == EINTR) continue;
			::close(fd);
			return false;
		}
		if (res == 0) { ::close(fd); return false; }
		written += static_cast<size_t>(res);
	}
	return ::close(fd) == 0;
}
}

ShrinkController::ShrinkController(QObject *parent) : QObject(parent) {
	qRegisterMetaType<shrink::Result>("shrink::Result");
	connect(&m_singleWatcher, &QFutureWatcher<shrink::Result>::finished, this, [this]() {
		m_isBusy.store(false, std::memory_order_release);
		shrink::Result res = m_singleWatcher.result();
		emit transformFinished(res);
		emit statusMessage(res.syntax_valid ? QStringLiteral("Transformacja ukończona pomyślnie") : QStringLiteral("Błąd weryfikacji składni: %1").arg(QString::fromStdString(res.syntax_error)), !res.syntax_valid);
	});
	connect(&m_batchWatcher, &QFutureWatcher<BatchSummary>::finished, this, [this]() {
		m_isBusy.store(false, std::memory_order_release);
		BatchSummary s = m_batchWatcher.result();
		emit batchFinished(s.processedCount, s.totalCount, s.totalInBytes, s.totalOutBytes);
		emit statusMessage(m_cancelRequested.load(std::memory_order_relaxed) ? QStringLiteral("Przetwarzanie wsadowe przerwane") : QStringLiteral("Zakończono przetwarzanie wsadowe"), false);
	});
	connect(&m_bundleWatcher, &QFutureWatcher<QString>::finished, this, [this]() {
		m_isBusy.store(false, std::memory_order_release);
		emit llmBundleGenerated(m_bundleWatcher.result());
		emit statusMessage(QStringLiteral("Wygenerowano LLM Context Bundle"), false);
	});
}

ShrinkController::~ShrinkController() {
	m_cancelRequested.store(true, std::memory_order_release);
	m_singleWatcher.waitForFinished();
	m_batchWatcher.waitForFinished();
	m_bundleWatcher.waitForFinished();
}

QByteArray ShrinkController::computeOptionsSignature(const shrink::Options &opt) {
	QByteArray bytes;
	QDataStream stream(&bytes, QIODevice::WriteOnly);
	stream << static_cast<qint32>(opt.lang) << opt.strip_comments << static_cast<qint32>(opt.blank_mode) << static_cast<qint32>(opt.indent_mode) << static_cast<qint32>(opt.tab_size) << opt.extract_skeleton << opt.skeleton_public_only << opt.modernize_guards << opt.strip_attributes << opt.strip_redundant << opt.modernize_namespaces << opt.compact_types << opt.compact_constructors << opt.if_to_ternary << opt.bpe_compress << opt.strip_debug << opt.strip_docstrings << opt.strip_types << opt.preserve_shebang << opt.verify_syntax << opt.unwrap_parentheses << opt.compact_short_functions;
	for (const auto &rule : opt.custom_rules) stream << QString::fromStdString(rule.name) << QString::fromStdString(rule.regex_str) << QString::fromStdString(rule.replace);
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QString ShrinkController::computeFileHash(std::string_view content, const QByteArray &optionsSig) {
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(content.data(), static_cast<qsizetype>(content.size()));
	hash.addData(optionsSig);
	return QString::fromLatin1(hash.result().toHex());
}

shrink::Language ShrinkController::detectLanguage(const fs::path &p) noexcept {
	auto ext = p.extension().native();
	if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".c" || ext == ".h") return shrink::Language::Cpp;
	if (ext == ".py") return shrink::Language::Python;
	if (ext == ".sh" || ext == ".bash") return shrink::Language::Shell;
	return shrink::Language::Plain;
}

QString ShrinkController::languageToFence(shrink::Language l) noexcept {
	switch (l) {
	case shrink::Language::Cpp: return QStringLiteral("cpp");
	case shrink::Language::Python: return QStringLiteral("python");
	case shrink::Language::Shell: return QStringLiteral("bash");
	default: return QStringLiteral("text");
	}
}

void ShrinkController::runSingleTransform(const QString &sourceCode, const std::vector<size_t> &excludedLines) {
	if (m_isBusy.load(std::memory_order_relaxed)) { emit statusMessage(QStringLiteral("Silnik jest zajęty inną operacją"), true); return; }
	m_isBusy.store(true, std::memory_order_release);
	emit statusMessage(QStringLiteral("Przetwarzanie kodu..."), false);
	std::string src = sourceCode.toStdString();
	shrink::Options opt = options();
	opt.excluded_lines = excludedLines;
	m_singleWatcher.setFuture(QtConcurrent::run([src = std::move(src), opt = std::move(opt)]() mutable { return shrink::process(src, opt); }));
}

void ShrinkController::addPathToQueue(const QString &rawPath) {
	if (m_isBusy.load(std::memory_order_relaxed)) return;
	fs::path p(rawPath.toStdString());
	std::error_code ec;
	if (!fs::exists(p, ec)) return;
	if (fs::is_directory(p, ec)) {
		m_baseScanRoot = fs::canonical(p, ec);
		scanDirectoryRecursive(m_baseScanRoot);
	} else if (fs::is_regular_file(p, ec)) {
		fs::path canonical_p = fs::canonical(p, ec);
		if (ec) return;
		std::lock_guard<std::mutex> lock(m_queueMutex);
		for (const auto &item : m_queue) if (item.fullPath == canonical_p) return;
		std::string rel = canonical_p.filename().string();
		if (!m_baseScanRoot.empty()) {
			auto relative_p = fs::relative(canonical_p, m_baseScanRoot, ec);
			if (!ec) {
				std::string rel_str = relative_p.generic_string();
				if (rel_str.rfind("..", 0) != 0 && !relative_p.is_absolute()) rel = rel_str;
			}
		}
		m_queue.push_back({canonical_p, rel, 0, 0, 0, 0, false, true, ""});
	}
	int sz;
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		sz = static_cast<int>(m_queue.size());
	}
	emit queueChanged(sz);
	emit queueUpdated(queue());
}

void ShrinkController::scanDirectoryRecursive(const fs::path &root) {
	shrink::GitIgnoreEngine gitignore;
	fs::path gitignore_path = root / ".gitignore";
	std::error_code ec;
	if (fs::exists(gitignore_path, ec)) gitignore.load_file(gitignore_path);
	std::vector<BatchQueueItem> scanned;
	auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
	const fs::recursive_directory_iterator end;
	if (ec) ec.clear();
	while (it != end) {
		const fs::path path = it->path();
		fs::path rel = fs::relative(path, root, ec);
		if (ec) {
			ec.clear();
		} else {
			const std::string rel_str = rel.generic_string();
			const bool isDirectory = it->is_directory(ec);
			if (ec) ec.clear();
			if (isDirectory) {
				if (gitignore.is_ignored(rel_str, true)) it.disable_recursion_pending();
			} else {
				const bool isRegularFile = it->is_regular_file(ec);
				if (ec) ec.clear();
				if (isRegularFile && !gitignore.is_ignored(rel_str, false)) {
					scanned.push_back({path, rel_str, 0, 0, 0, 0, false, true, ""});
				}
			}
		}
		it.increment(ec);
		if (ec) ec.clear();
	}
	std::lock_guard<std::mutex> lock(m_queueMutex);
	for (auto &item : scanned) {
		bool exists = false;
		for (const auto &existing : m_queue) if (existing.fullPath == item.fullPath) { exists = true; break; }
		if (!exists) m_queue.push_back(std::move(item));
	}
}

void ShrinkController::clearQueue() {
	if (m_isBusy.load(std::memory_order_relaxed)) return;
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		m_queue.clear();
		m_baseScanRoot.clear();
	}
	emit queueChanged(0);
	emit queueUpdated(queue());
	emit statusMessage(QStringLiteral("Wyczyszczono kolejkę"), false);
}

void ShrinkController::removeQueueItems(std::vector<int> indices) {
	if (m_isBusy.load(std::memory_order_acquire) || indices.empty()) return;
	std::sort(indices.begin(), indices.end(), std::greater<int>());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_isBusy.load(std::memory_order_acquire)) return;
		for (int index : indices) {
			if (index >= 0 && static_cast<size_t>(index) < m_queue.size())
				m_queue.erase(m_queue.begin() + index);
		}
	}
	const auto items = queue();
	emit queueUpdated(items);
	emit queueChanged(static_cast<int>(items.size()));
}

void ShrinkController::setQueueItemsExcluded(std::vector<int> indices, bool excluded) {
	if (m_isBusy.load(std::memory_order_acquire) || indices.empty()) return;
	std::vector<BatchQueueItem> items;
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_isBusy.load(std::memory_order_acquire)) return;
		for (int index : indices) {
			if (index >= 0 && static_cast<size_t>(index) < m_queue.size())
				m_queue[static_cast<size_t>(index)].excluded = excluded;
		}
		items = m_queue;
	}
	emit queueUpdated(items);
}

void ShrinkController::cancelBatch() {
	if (m_isBusy.load(std::memory_order_relaxed)) {
		m_cancelRequested.store(true, std::memory_order_release);
		emit statusMessage(QStringLiteral("Przerywanie kolejki wsadowej..."), false);
	}
}

void ShrinkController::runBatchQueue(const QString &outputDir, unsigned int threadCount) {
	if (m_isBusy.load(std::memory_order_relaxed)) return;
	size_t queueSize;
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		queueSize = m_queue.size();
	}
	if (queueSize == 0) {
		m_isBusy.store(false, std::memory_order_release);
		emit batchFinished(0, 0, 0, 0);
		emit statusMessage(QStringLiteral("Kolejka wsadowa jest pusta"), true);
		return;
	}
	m_isBusy.store(true, std::memory_order_release);
	m_cancelRequested.store(false, std::memory_order_release);
	unsigned int threads = threadCount > 0 ? threadCount : std::max(1u, std::thread::hardware_concurrency());
	fs::path outRootPath = outputDir.isEmpty() ? fs::path() : fs::path(outputDir.toStdString());
	const shrink::Options optCopy = options();
	emit statusMessage(QStringLiteral("Uruchamianie przetwarzania wsadowego na %1 wątkach...").arg(threads), false);
	m_batchWatcher.setFuture(QtConcurrent::run([this, outRootPath, optCopy, threads]() -> BatchSummary {
		size_t total;
		{
			std::lock_guard<std::mutex> lock(m_queueMutex);
			total = m_queue.size();
			for (auto &item : m_queue) { item.processed = false; item.errorMsg.clear(); }
		}
		QByteArray optSig = computeOptionsSignature(optCopy);
		fs::path cachePath;
		QJsonObject cacheMap;
		std::mutex cacheMutex;
		if (!outRootPath.empty()) {
			cachePath = outRootPath / ".shrinkcache";
			std::ifstream cfile(cachePath, std::ios::binary);
			if (cfile.is_open()) {
				std::string cstr((std::istreambuf_iterator<char>(cfile)), std::istreambuf_iterator<char>());
				QJsonDocument doc = QJsonDocument::fromJson(QByteArray(cstr.data(), static_cast<qsizetype>(cstr.size())));
				if (doc.isObject()) cacheMap = doc.object();
			}
		}
		std::atomic<size_t> fileIdx{0}, processedCount{0};
		std::atomic<quint64> sumInBytes{0}, sumOutBytes{0};
		auto worker = [&]() {
			while (!m_cancelRequested.load(std::memory_order_relaxed)) {
				size_t idx = fileIdx.fetch_add(1, std::memory_order_relaxed);
				if (idx >= total) break;
				fs::path p;
				std::string rel;
				bool excluded = false;
				{
					std::lock_guard<std::mutex> lock(m_queueMutex);
					p = m_queue[idx].fullPath;
					rel = m_queue[idx].relPath;
					excluded = m_queue[idx].excluded;
				}
				if (excluded) {
					{
						std::lock_guard<std::mutex> lock(m_queueMutex);
						m_queue[idx].processed = true;
						m_queue[idx].errorMsg = "Pominięto";
					}
					const size_t done = processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
					QMetaObject::invokeMethod(this, [this, idx, done, total, rel]() {
						emit batchProgress(static_cast<int>(done), static_cast<int>(total), QString::fromStdString(rel));
						emit batchItemFinished(static_cast<int>(idx), true, 0, 0, QStringLiteral("Pominięto"));
					}, Qt::QueuedConnection);
					continue;
				}
				std::ifstream file(p, std::ios::binary);
				if (!file.is_open()) {
					{
						std::lock_guard<std::mutex> lock(m_queueMutex);
						m_queue[idx].processed = true;
						m_queue[idx].errorMsg = "Błąd odczytu pliku";
					}
					QMetaObject::invokeMethod(this, [this, idx]() { emit batchItemFinished(static_cast<int>(idx), false, 0, 0, QStringLiteral("Błąd odczytu")); }, Qt::QueuedConnection);
					continue;
				}
				std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				file.close();
				QString qRel = QString::fromStdString(rel);
				QString fileHash = computeFileHash(content, optSig);
				fs::path safeRel = (rel.rfind("..", 0) == 0 || fs::path(rel).is_absolute()) ? p.filename() : fs::path(rel);
				fs::path target = outRootPath.empty() ? fs::path() : outRootPath / safeRel;
				bool cacheHit = false;
				size_t resInBytes = 0, resOutBytes = 0, resInLines = 0, resOutLines = 0;
				bool resValid = true;
				std::string resErr;
				if (!outRootPath.empty()) {
					std::lock_guard<std::mutex> lock(cacheMutex);
					if (cacheMap.contains(qRel)) {
						QJsonObject entry = cacheMap.value(qRel).toObject();
						if (entry.value(QStringLiteral("hash")).toString() == fileHash) {
							std::error_code ec;
							if (fs::exists(target, ec)) {
								cacheHit = true;
								resInBytes = static_cast<size_t>(entry.value(QStringLiteral("inBytes")).toInteger());
								resOutBytes = static_cast<size_t>(entry.value(QStringLiteral("outBytes")).toInteger());
								resInLines = static_cast<size_t>(entry.value(QStringLiteral("inLines")).toInteger());
								resOutLines = static_cast<size_t>(entry.value(QStringLiteral("outLines")).toInteger());
								resValid = entry.value(QStringLiteral("valid")).toBool(true);
								resErr = entry.value(QStringLiteral("err")).toString().toStdString();
							}
						}
					}
				}
				if (!cacheHit) {
					shrink::Options fileOpt = optCopy;
					if (fileOpt.lang == shrink::Language::Auto) fileOpt.lang = detectLanguage(p);
					auto res = shrink::process(content, fileOpt);
					resInBytes = res.in_bytes;
					resOutBytes = res.out_bytes;
					resInLines = res.in_lines;
					resOutLines = res.out_lines;
					resValid = res.syntax_valid;
					resErr = res.syntax_error;
					if (resValid && !outRootPath.empty()) {
						std::error_code ec;
						fs::create_directories(target.parent_path(), ec);
						if (write_all_bytes(target, res.output)) {
							QJsonObject newEntry;
							newEntry[QStringLiteral("hash")] = fileHash;
							newEntry[QStringLiteral("inBytes")] = static_cast<qint64>(resInBytes);
							newEntry[QStringLiteral("outBytes")] = static_cast<qint64>(resOutBytes);
							newEntry[QStringLiteral("inLines")] = static_cast<qint64>(resInLines);
							newEntry[QStringLiteral("outLines")] = static_cast<qint64>(resOutLines);
							newEntry[QStringLiteral("valid")] = resValid;
							newEntry[QStringLiteral("err")] = QString::fromStdString(resErr);
							std::lock_guard<std::mutex> lock(cacheMutex);
							cacheMap[qRel] = newEntry;
						} else resErr = "Błąd zapisu pliku wynikowego";
					}
				}
				{
					std::lock_guard<std::mutex> lock(m_queueMutex);
					m_queue[idx].inBytes = resInBytes;
					m_queue[idx].outBytes = resOutBytes;
					m_queue[idx].inLines = resInLines;
					m_queue[idx].outLines = resOutLines;
					m_queue[idx].syntaxValid = resValid;
					m_queue[idx].errorMsg = resErr;
					m_queue[idx].processed = true;
				}
				sumInBytes.fetch_add(resInBytes, std::memory_order_relaxed);
				sumOutBytes.fetch_add(resOutBytes, std::memory_order_relaxed);
				size_t done = processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				QMetaObject::invokeMethod(this, [this, done, total, rel]() { emit batchProgress(static_cast<int>(done), static_cast<int>(total), QString::fromStdString(rel)); }, Qt::QueuedConnection);
				QMetaObject::invokeMethod(this, [this, idx, resValid, resInBytes, resOutBytes, resErr]() { emit batchItemFinished(static_cast<int>(idx), resValid, resInBytes, resOutBytes, QString::fromStdString(resErr)); }, Qt::QueuedConnection);
			}
		};
		if (threads <= 1) worker();
		else {
			std::latch sync(threads);
			for (unsigned int t = 0; t < threads; ++t) QThreadPool::globalInstance()->start([&worker, &sync]() { worker(); sync.count_down(); });
			sync.wait();
		}
		if (!outRootPath.empty()) {
			QByteArray outData = QJsonDocument(cacheMap).toJson(QJsonDocument::Compact);
			write_all_bytes(cachePath, std::string_view(outData.constData(), static_cast<size_t>(outData.size())));
		}
		BatchSummary summary;
		summary.processedCount = static_cast<int>(processedCount.load());
		summary.totalCount = static_cast<int>(total);
		summary.totalInBytes = sumInBytes.load();
		summary.totalOutBytes = sumOutBytes.load();
		return summary;
	}));
	emit batchStarted();
}

void ShrinkController::generateLlmBundle() {
	if (m_isBusy.load(std::memory_order_relaxed)) return;
	std::vector<BatchQueueItem> localQueue;
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		if (m_queue.empty()) { emit llmBundleGenerated(QStringLiteral("# LLM Context Bundle\n\nBrak plików w kolejce.\n")); return; }
		for (const auto &item : m_queue) if (!item.excluded) localQueue.push_back(item);
	}
	if (localQueue.empty()) {
		emit llmBundleGenerated(QStringLiteral("# LLM Context Bundle\n\nBrak plików w kolejce.\n"));
		return;
	}
	m_isBusy.store(true, std::memory_order_release);
	emit statusMessage(QStringLiteral("Generowanie LLM Context Bundle..."), false);
	const shrink::Options optCopy = options();
	m_bundleWatcher.setFuture(QtConcurrent::run([this, q = std::move(localQueue), optCopy]() {
		struct TreeNode {
			std::string name;
			bool is_file = false;
			std::map<std::string, TreeNode> children;
		};
		TreeNode rootNode{"."};
		for (const auto &item : q) {
			fs::path p(item.relPath);
			TreeNode *current = &rootNode;
			for (auto it = p.begin(); it != p.end(); ++it) {
				auto next = it;
				++next;
				std::string part = it->string();
				if (next == p.end()) {
					TreeNode fileNode{part, true, {}};
					current->children[part] = std::move(fileNode);
				} else {
					if (!current->children.contains(part)) {
						TreeNode dirNode{part, false, {}};
						current->children[part] = std::move(dirNode);
					}
					current = &current->children[part];
				}
			}
		}
		std::string out = "# LLM Context Bundle\n\nPliki: " + std::to_string(q.size()) + "\n\n## Drzewo projektu\n\n```text\n.\n";
		std::function<void(const TreeNode &, const std::string &)> printTree = [&](const TreeNode &node, const std::string &prefix) {
			size_t total = node.children.size();
			size_t current = 0;
			for (const auto &[name, child] : node.children) {
				++current;
				bool is_last = (current == total);
				out += prefix + (is_last ? "└── " : "├── ") + name + (child.is_file ? "" : "/") + "\n";
				if (!child.is_file) {
					printTree(child, prefix + (is_last ? "    " : "│   "));
				}
			}
		};
		printTree(rootNode, "");
		out += "```\n";
		for (const auto &item : q) {
			std::ifstream file(item.fullPath, std::ios::binary);
			if (!file.is_open()) continue;
			std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			shrink::Options fileOpt = optCopy;
			if (fileOpt.lang == shrink::Language::Auto) fileOpt.lang = detectLanguage(item.fullPath);
			auto res = shrink::process(raw, fileOpt);
			if (!res.syntax_valid) continue;
			size_t maxTicks = 0, cur = 0;
			for (char c : res.output) { if (c == '`') { ++cur; maxTicks = std::max(maxTicks, cur); } else cur = 0; }
			std::string fence(std::max<size_t>(3, maxTicks + 1), '`');
			out += "\n### Plik: " + item.relPath + "\n\n" + fence + languageToFence(fileOpt.lang).toStdString() + "\n";
			out += res.output;
			if (!res.output.empty() && res.output.back() != '\n') out += "\n";
			out += fence + "\n";
		}
		return QString::fromStdString(out);
	}));
}

bool ShrinkController::loadRulesFromJsonFile(const QString &filePath) {
	std::ifstream file(filePath.toStdString(), std::ios::binary);
	if (!file.is_open()) { emit statusMessage(QStringLiteral("Błąd odczytu pliku reguł: %1").arg(filePath), true); return false; }
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	auto rules = shrink::parse_rules_json(content);
	int count = static_cast<int>(rules.size());
	{
		std::lock_guard<std::mutex> lock(m_optionsMutex);
		m_options.custom_rules = std::move(rules);
	}
	emit rulesLoaded(count);
	emit statusMessage(QStringLiteral("Wczytano reguł RE2: %1").arg(count), false);
	return true;
}
