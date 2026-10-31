#pragma once
#include <QMainWindow>
#include "shrink_controller.h"
#include "code_editor_view.h"
#include "options_view.h"
#include "batch_queue_view.h"
#include "custom_rules_view.h"
#include "metrics_bundle_view.h"

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow() override = default;
	void openSourceFile(const QString &path);
protected:
	void closeEvent(QCloseEvent *event) override;
private slots:
	void openFileDialog();
	void saveResultDialog();
	void selectBatchFolderDialog();
	void executeTransform();
	void resetLayoutSettings();
private:
	ShrinkController *m_controller;
	CodeEditorView *m_editorDock;
	OptionsView *m_optionsDock;
	BatchQueueView *m_batchDock;
	CustomRulesView *m_rulesDock;
	MetricsBundleView *m_metricsDock;
	void setupDocks();
	void setupMenus();
	void setupConnections();
	void saveLayoutSettings();
	void restoreLayoutSettings();
};
