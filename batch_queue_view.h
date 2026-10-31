#pragma once
#include <vector>
#include <QDockWidget>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include "shrink_controller.h"

class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QLineEdit;
class QPoint;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

class BatchQueueView : public QDockWidget {
	Q_OBJECT

public:
	explicit BatchQueueView(QWidget *parent = nullptr);
	QString outputDirectory() const;
	unsigned int threadCount() const;

public slots:
	void onBatchProgress(int current, int total, const QString &currentFile);
	void onBatchStarted();
	void onBatchItemFinished(int index, bool success, quint64 inBytes, quint64 outBytes, const QString &errorMsg);
	void onBatchFinished(int processedCount, int totalCount, quint64 totalInBytes, quint64 totalOutBytes);
	void onQueueChanged(int totalItems);
	void setQueueItems(const std::vector<BatchQueueItem> &items);

signals:
	void startBatchRequested(const QString &outputDir, unsigned int threads);
	void cancelBatchRequested();
	void clearQueueRequested();
	void pathsDropped(const QStringList &paths);
	void removeItemsRequested(std::vector<int> indices);
	void setItemsExcludedRequested(std::vector<int> indices, bool excluded);

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void browseOutputDir();
	void browseInputDir();
	void addFilesDialog();
	void triggerStart();
	void triggerCancel();
	void triggerClear();
	void showContextMenu(const QPoint &pos);

private:
	QTableWidget *m_table = nullptr;
	QLineEdit *m_editOutputDir = nullptr;
	QPushButton *m_btnBrowse = nullptr;
	QPushButton *m_btnAddFolder = nullptr;
	QPushButton *m_btnAddFiles = nullptr;
	QSpinBox *m_spinThreads = nullptr;
	QProgressBar *m_progressBar = nullptr;
	QPushButton *m_btnStart = nullptr;
	QPushButton *m_btnCancel = nullptr;
	QPushButton *m_btnClear = nullptr;
	void setupUi();
	bool handleDrop(QDropEvent *event);
};
