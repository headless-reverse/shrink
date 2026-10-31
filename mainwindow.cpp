#include "mainwindow.h"
#include <QMenuBar>
#include <QMenu>
#include <QFileDialog>
#include <QStatusBar>
#include <QSettings>
#include <QKeySequence>
#include <QCloseEvent>
#include <fstream>
#include <string>

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
	, m_controller(new ShrinkController(this))
{
	setDockNestingEnabled(true);
	setupDocks();
	setupMenus();
	setupConnections();
	restoreLayoutSettings();
}

void MainWindow::setupDocks() {
	m_editorDock = new CodeEditorView(this);
	m_optionsDock = new OptionsView(this);
	m_batchDock = new BatchQueueView(this);
	m_rulesDock = new CustomRulesView(this);
	m_metricsDock = new MetricsBundleView(this);
	addDockWidget(Qt::LeftDockWidgetArea, m_optionsDock);
	addDockWidget(Qt::LeftDockWidgetArea, m_rulesDock);
	tabifyDockWidget(m_optionsDock, m_rulesDock);
	addDockWidget(Qt::TopDockWidgetArea, m_editorDock);
	addDockWidget(Qt::RightDockWidgetArea, m_batchDock);
	addDockWidget(Qt::RightDockWidgetArea, m_metricsDock);
	tabifyDockWidget(m_batchDock, m_metricsDock);
}

void MainWindow::setupMenus() {
	QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
	QAction *actOpen = fileMenu->addAction(tr("Wczytaj plik do edytora..."), this, &MainWindow::openFileDialog);
	actOpen->setShortcut(QKeySequence::Open);
	QAction *actSave = fileMenu->addAction(tr("Zapisz wynik..."), this, &MainWindow::saveResultDialog);
	actSave->setShortcut(QKeySequence::Save);
	fileMenu->addAction(tr("Wybierz folder wsadowy..."), this, &MainWindow::selectBatchFolderDialog);
	fileMenu->addSeparator();
	QAction *actQuit = fileMenu->addAction(tr("Zakończ"), this, &QWidget::close);
	actQuit->setShortcut(QKeySequence::Quit);
	QMenu *procMenu = menuBar()->addMenu(tr("&Process"));
	QAction *actTransform = procMenu->addAction(tr("Wykonaj transformację"), this, &MainWindow::executeTransform);
	actTransform->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
	procMenu->addAction(tr("Przetwórz kolejkę wsadową"), this, [this]() {
		m_controller->runBatchQueue(m_batchDock->outputDirectory(), m_batchDock->threadCount());
	});
	procMenu->addAction(tr("Generuj LLM Bundle"), m_controller, &ShrinkController::generateLlmBundle);
	procMenu->addAction(tr("Zatrzymaj"), m_controller, &ShrinkController::cancelBatch);
	QMenu *settingsMenu = menuBar()->addMenu(tr("&Settings"));
	settingsMenu->addAction(tr("Resetuj układ okien"), this, &MainWindow::resetLayoutSettings);
	QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
	viewMenu->addAction(m_editorDock->toggleViewAction());
	viewMenu->addAction(m_optionsDock->toggleViewAction());
	viewMenu->addAction(m_batchDock->toggleViewAction());
	viewMenu->addAction(m_rulesDock->toggleViewAction());
	viewMenu->addAction(m_metricsDock->toggleViewAction());
}

void MainWindow::setupConnections() {
	connect(m_editorDock, &CodeEditorView::transformRequested, m_controller, &ShrinkController::runSingleTransform);
	connect(m_controller, &ShrinkController::transformFinished, m_editorDock, &CodeEditorView::onTransformFinished);
	connect(m_controller, &ShrinkController::transformFinished, m_metricsDock, &MetricsBundleView::onTransformFinished);

	connect(m_optionsDock, &OptionsView::optionsChanged, this, [this](const shrink::Options &opt) {
		shrink::Options current = opt;
		current.custom_rules = m_rulesDock->rules();
		m_controller->setOptions(current);
	});

	connect(m_rulesDock, &CustomRulesView::rulesUpdated, this, [this](const std::vector<shrink::CustomRule> &rules) {
		shrink::Options opt = m_controller->options();
		opt.custom_rules = rules;
		m_controller->setOptions(opt);
	});

	connect(m_batchDock, &BatchQueueView::startBatchRequested, m_controller, &ShrinkController::runBatchQueue);
	connect(m_batchDock, &BatchQueueView::cancelBatchRequested, m_controller, &ShrinkController::cancelBatch);
	connect(m_batchDock, &BatchQueueView::clearQueueRequested, m_controller, &ShrinkController::clearQueue);
	connect(m_batchDock, &BatchQueueView::removeItemsRequested, m_controller, &ShrinkController::removeQueueItems);
	connect(m_batchDock, &BatchQueueView::setItemsExcludedRequested, m_controller, &ShrinkController::setQueueItemsExcluded);

	connect(m_batchDock, &BatchQueueView::pathsDropped, this, [this](const QStringList &paths) {
		for (const auto &p : paths) {
			m_controller->addPathToQueue(p);
		}
		m_batchDock->setQueueItems(m_controller->queue());
	});

	connect(m_controller, &ShrinkController::batchProgress, m_batchDock, &BatchQueueView::onBatchProgress);
	connect(m_controller, &ShrinkController::batchStarted, m_batchDock, &BatchQueueView::onBatchStarted);
	connect(m_controller, &ShrinkController::batchItemFinished, m_batchDock, &BatchQueueView::onBatchItemFinished);
	connect(m_controller, &ShrinkController::batchFinished, m_batchDock, &BatchQueueView::onBatchFinished);
	connect(m_controller, &ShrinkController::batchFinished, m_metricsDock, &MetricsBundleView::onBatchFinished);
	connect(m_controller, &ShrinkController::queueUpdated, m_batchDock, &BatchQueueView::setQueueItems);

	connect(m_metricsDock, &MetricsBundleView::generateBundleRequested, m_controller, &ShrinkController::generateLlmBundle);
	connect(m_controller, &ShrinkController::llmBundleGenerated, m_metricsDock, &MetricsBundleView::onLlmBundleGenerated);

	connect(m_controller, &ShrinkController::statusMessage, this, [this](const QString &msg, bool isErr) {
		statusBar()->showMessage(msg, 5000);
		if (isErr) {
			statusBar()->setStyleSheet(QStringLiteral("color: #FF5555; font-weight: bold;"));
		} else {
			statusBar()->setStyleSheet(QString());
		}
	});
}

void MainWindow::openSourceFile(const QString &path) {
	std::ifstream file(path.toStdString(), std::ios::binary);
	if (!file.is_open()) {
		statusBar()->showMessage(tr("Nie udało się otworzyć pliku: %1").arg(path), 5000);
		return;
	}
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	m_editorDock->setSourceCode(QString::fromStdString(content));
	statusBar()->showMessage(tr("Wczytano plik: %1").arg(path), 5000);
}

void MainWindow::openFileDialog() {
	QString path = QFileDialog::getOpenFileName(
		this,
		tr("Wczytaj plik"),
		QString(),
		tr("Kod źródłowy (*.cpp *.hpp *.cc *.c *.h *.py *.sh);;Wszystkie pliki (*)")
	);
	if (!path.isEmpty()) {
		openSourceFile(path);
	}
}

void MainWindow::saveResultDialog() {
	QString path = QFileDialog::getSaveFileName(
		this,
		tr("Zapisz wynik"),
		QString(),
		tr("Wszystkie pliki (*)")
	);
	if (path.isEmpty()) return;
	std::ofstream file(path.toStdString(), std::ios::binary);
	if (file.is_open()) {
		std::string text = m_editorDock->transformedCode().toStdString();
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		statusBar()->showMessage(tr("Zapisano wynik do: %1").arg(path), 5000);
	} else {
		statusBar()->showMessage(tr("Błąd zapisu do pliku: %1").arg(path), 5000);
	}
}

void MainWindow::selectBatchFolderDialog() {
	QString dir = QFileDialog::getExistingDirectory(this, tr("Wybierz folder wsadowy"));
	if (!dir.isEmpty()) {
		m_controller->addPathToQueue(dir);
		m_batchDock->setQueueItems(m_controller->queue());
	}
}

void MainWindow::executeTransform() {
	m_controller->runSingleTransform(m_editorDock->sourceCode(), m_editorDock->excludedLines());
}

void MainWindow::resetLayoutSettings() {
	m_editorDock->show();
	m_optionsDock->show();
	m_batchDock->show();
	m_rulesDock->show();
	m_metricsDock->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
	saveLayoutSettings();
	QMainWindow::closeEvent(event);
}

void MainWindow::saveLayoutSettings() {
	QSettings settings(QStringLiteral("ShrinkPro"), QStringLiteral("ShrinkGUI"));
	settings.setValue(QStringLiteral("geometry"), saveGeometry());
	settings.setValue(QStringLiteral("windowState"), saveState());
}

void MainWindow::restoreLayoutSettings() {
	QSettings settings(QStringLiteral("ShrinkPro"), QStringLiteral("ShrinkGUI"));
	restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
	restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
}
