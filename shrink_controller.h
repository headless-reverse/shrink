#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <QFutureWatcher>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDataStream>
#include <filesystem>
#include <vector>
#include <atomic>
#include <mutex>
#include "shrink_engine.hpp"

Q_DECLARE_METATYPE(shrink::Result)

struct BatchQueueItem {
	std::filesystem::path fullPath;
	std::string relPath;
	size_t inBytes = 0;
	size_t outBytes = 0;
	size_t inLines = 0;
	size_t outLines = 0;
	bool processed = false;
	bool syntaxValid = true;
	std::string errorMsg;
	bool excluded = false;
};

struct BatchSummary {
	int processedCount = 0;
	int totalCount = 0;
	quint64 totalInBytes = 0;
	quint64 totalOutBytes = 0;
	quint64 totalInLines = 0;
	quint64 totalOutLines = 0;
};

struct BundleResult {
	QString markdown;
	BatchSummary metrics;
};

Q_DECLARE_METATYPE(BundleResult)

class ShrinkController : public QObject {
	Q_OBJECT
public:
	explicit ShrinkController(QObject *parent = nullptr);
	~ShrinkController() override;

	shrink::Options options() const {
		std::lock_guard<std::mutex> lock(m_optionsMutex);
		return m_options;
	}
	void setOptions(const shrink::Options &opt) {
		std::lock_guard<std::mutex> lock(m_optionsMutex);
		m_options = opt;
	}
	std::vector<BatchQueueItem> queue() const {
		std::lock_guard<std::mutex> lock(m_queueMutex);
		return m_queue;
	}
	bool isBusy() const noexcept { return m_isBusy.load(std::memory_order_relaxed); }
	void addPathToQueue(const QString &rawPath);
	void clearQueue();
	bool loadRulesFromJsonFile(const QString &filePath);

public slots:
	void runSingleTransform(const QString &sourceCode, const std::vector<size_t> &excludedLines = {});
	void runBatchQueue(const QString &outputDir, unsigned int threadCount = 0);
	void cancelBatch();
	void generateLlmBundle();
	void removeQueueItems(std::vector<int> indices);
	void setQueueItemsExcluded(std::vector<int> indices, bool excluded);

signals:
	void transformFinished(const shrink::Result &result);
	void batchProgress(int current, int total, const QString &currentFile);
	void batchStarted();
	void batchItemFinished(int index, bool success, quint64 inBytes, quint64 outBytes, const QString &errorMsg);
	void batchFinished(int processedCount, int totalCount, quint64 totalInBytes, quint64 totalOutBytes);
	void llmBundleGenerated(const BundleResult &result);
	void rulesLoaded(int ruleCount);
	void statusMessage(const QString &message, bool isError = false);
	void queueChanged(int totalItems);
	void queueUpdated(const std::vector<BatchQueueItem> &items);

private:
	shrink::Options m_options;
	mutable std::mutex m_optionsMutex;
	std::vector<BatchQueueItem> m_queue;
	std::filesystem::path m_baseScanRoot;
	mutable std::mutex m_queueMutex;
	std::atomic<bool> m_isBusy{false};
	std::atomic<bool> m_cancelRequested{false};
	QFutureWatcher<shrink::Result> m_singleWatcher;
	QFutureWatcher<BatchSummary> m_batchWatcher;
	QFutureWatcher<BundleResult> m_bundleWatcher;
	void scanDirectoryRecursive(const std::filesystem::path &root);
	static shrink::Language detectLanguage(const std::filesystem::path &p) noexcept;
	static QString languageToFence(shrink::Language l) noexcept;
	static QByteArray computeOptionsSignature(const shrink::Options &opt);
	static QString computeFileHash(std::string_view content, const QByteArray &optionsSig);
};
