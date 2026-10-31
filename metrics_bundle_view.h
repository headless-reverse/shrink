#pragma once
#include <QDockWidget>
#include <QString>
#include <QtGlobal>
#include "shrink_engine.hpp"

class QLabel;
class QPlainTextEdit;
class QPushButton;
struct BundleResult;

class MetricsBundleView : public QDockWidget {
	Q_OBJECT

public:
	explicit MetricsBundleView(QWidget *parent = nullptr);

public slots:
	void onTransformFinished(const shrink::Result &result);
	void onBatchFinished(int processedCount, int totalCount, quint64 totalInBytes, quint64 totalOutBytes);
	void onLlmBundleGenerated(const BundleResult &result);

signals:
	void generateBundleRequested();

private slots:
	void copyBundleToClipboard();
	void saveBundleToFile();

private:
	QLabel *m_lblBytes = nullptr;
	QLabel *m_lblReduction = nullptr;
	QLabel *m_lblLines = nullptr;
	QLabel *m_lblTokens = nullptr;
	QPlainTextEdit *m_bundleView = nullptr;
	QPushButton *m_btnGenerate = nullptr;
	QPushButton *m_btnCopy = nullptr;
	QPushButton *m_btnSave = nullptr;
	void setupUi();
};
