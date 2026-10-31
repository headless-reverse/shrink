#include "batch_queue_view.h"
#include <algorithm>
#include <thread>
#include <vector>
#include <QAbstractItemView>
#include <QColor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QAction>
#include <QKeyEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QProgressBar>
#include <QPoint>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QUrl>
#include <QVBoxLayout>

BatchQueueView::BatchQueueView(QWidget *parent) : QDockWidget(tr("Kolejka wsadowa"), parent) {
	setObjectName(QStringLiteral("batch_queue"));
	setAllowedAreas(Qt::AllDockWidgetAreas);
	setAcceptDrops(true);
	setupUi();
}

void BatchQueueView::setupUi() {
	auto *container = new QWidget(this);
	auto *mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(4, 4, 4, 4);
	mainLayout->setSpacing(4);
	auto *topLayout = new QHBoxLayout();
	topLayout->addWidget(new QLabel(tr("Katalog wyjściowy:"), container));
	m_editOutputDir = new QLineEdit(container);
	m_btnBrowse = new QPushButton(tr("Przeglądaj..."), container);
	topLayout->addWidget(m_editOutputDir, 1);
	topLayout->addWidget(m_btnBrowse);
	topLayout->addWidget(new QLabel(tr("Wątki:"), container));
	m_spinThreads = new QSpinBox(container);
	m_spinThreads->setRange(1, 128);
	const unsigned int hardwareThreads = std::thread::hardware_concurrency();
	const int defaultThreads = std::max(1, static_cast<int>(hardwareThreads == 0 ? 1 : hardwareThreads));
	m_spinThreads->setValue(std::min(defaultThreads, 128));
	topLayout->addWidget(m_spinThreads);
	mainLayout->addLayout(topLayout);
	m_table = new QTableWidget(0, 5, container);
	m_table->setAcceptDrops(true);
	m_table->setContextMenuPolicy(Qt::CustomContextMenu);
	m_table->installEventFilter(this);
	m_table->viewport()->installEventFilter(this);
	m_table->setHorizontalHeaderLabels({tr("Ścieżka"), tr("Przed"), tr("Po"), tr("Zysk %"), tr("Status")});
	QHeaderView *header = m_table->horizontalHeader();
	header->setSectionResizeMode(0, QHeaderView::Stretch);
	header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setAlternatingRowColors(true);
	mainLayout->addWidget(m_table, 1);
	m_progressBar = new QProgressBar(container);
	m_progressBar->setRange(0, 100);
	m_progressBar->setValue(0);
	mainLayout->addWidget(m_progressBar);
	auto *btnLayout = new QHBoxLayout();
	m_btnAddFolder = new QPushButton(tr("Dodaj folder..."), container);
	m_btnAddFiles = new QPushButton(tr("Dodaj pliki..."), container);
	m_btnStart = new QPushButton(tr("Przetwórz kolejkę"), container);
	m_btnCancel = new QPushButton(tr("Zatrzymaj"), container);
	m_btnClear = new QPushButton(tr("Wyczyść"), container);
	m_btnCancel->setEnabled(false);
	btnLayout->addWidget(m_btnAddFolder);
	btnLayout->addWidget(m_btnAddFiles);
	btnLayout->addStretch(1);
	btnLayout->addWidget(m_btnStart);
	btnLayout->addWidget(m_btnCancel);
	btnLayout->addWidget(m_btnClear);
	mainLayout->addLayout(btnLayout);
	setWidget(container);
	connect(m_btnBrowse, &QPushButton::clicked, this, &BatchQueueView::browseOutputDir);
	connect(m_btnAddFolder, &QPushButton::clicked, this, &BatchQueueView::browseInputDir);
	connect(m_btnAddFiles, &QPushButton::clicked, this, &BatchQueueView::addFilesDialog);
	connect(m_btnStart, &QPushButton::clicked, this, &BatchQueueView::triggerStart);
	connect(m_btnCancel, &QPushButton::clicked, this, &BatchQueueView::triggerCancel);
	connect(m_btnClear, &QPushButton::clicked, this, &BatchQueueView::triggerClear);
	connect(m_table, &QTableWidget::customContextMenuRequested, this, &BatchQueueView::showContextMenu);
}

QString BatchQueueView::outputDirectory() const {
	return m_editOutputDir->text().trimmed();
}

unsigned int BatchQueueView::threadCount() const {
	return static_cast<unsigned int>(m_spinThreads->value());
}

void BatchQueueView::dragEnterEvent(QDragEnterEvent *event) {
	if (event->mimeData()->hasUrls()) event->acceptProposedAction();
	else event->ignore();
}

void BatchQueueView::dropEvent(QDropEvent *event) {
	handleDrop(event);
}

bool BatchQueueView::eventFilter(QObject *watched, QEvent *event) {
	if ((watched == m_table || watched == m_table->viewport()) && event->type() == QEvent::KeyPress) {
		auto *keyEvent = static_cast<QKeyEvent *>(event);
		if (keyEvent->key() == Qt::Key_Delete) {
			if (m_btnStart->isEnabled()) {
				std::vector<int> indices;
				for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
					indices.push_back(index.row());
				if (!indices.empty()) emit removeItemsRequested(std::move(indices));
			}
			return true;
		}
	}
	if ((watched == m_table || watched == m_table->viewport()) &&
		(event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)) {
		auto *dragEvent = static_cast<QDropEvent *>(event);
		if (dragEvent->mimeData()->hasUrls()) {
			dragEvent->acceptProposedAction();
			return true;
		}
	}
	if ((watched == m_table || watched == m_table->viewport()) && event->type() == QEvent::Drop) {
		return handleDrop(static_cast<QDropEvent *>(event));
	}
	return QDockWidget::eventFilter(watched, event);
}

bool BatchQueueView::handleDrop(QDropEvent *event) {
	QStringList paths;
	for (const QUrl &url : event->mimeData()->urls()) {
		if (url.isLocalFile()) paths.append(url.toLocalFile());
	}
	if (!paths.isEmpty()) {
		emit pathsDropped(paths);
		event->acceptProposedAction();
		return true;
	} else {
		event->ignore();
		return false;
	}
}

void BatchQueueView::browseOutputDir() {
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Wybierz katalog wyjściowy"), outputDirectory());
	if (!dir.isEmpty()) m_editOutputDir->setText(dir);
}

void BatchQueueView::browseInputDir() {
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Wybierz folder wejściowy"));
	if (!dir.isEmpty()) emit pathsDropped({dir});
}

void BatchQueueView::addFilesDialog() {
	const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Wybierz pliki do kolejki"));
	if (!paths.isEmpty()) emit pathsDropped(paths);
}

void BatchQueueView::triggerStart() {
	if (m_table->rowCount() == 0) return;
	m_progressBar->setValue(0);
	emit startBatchRequested(outputDirectory(), threadCount());
}

void BatchQueueView::triggerCancel() {
	m_btnCancel->setEnabled(false);
	emit cancelBatchRequested();
}

void BatchQueueView::triggerClear() {
	m_table->setRowCount(0);
	m_progressBar->setValue(0);
	emit clearQueueRequested();
}

void BatchQueueView::showContextMenu(const QPoint &pos) {
	int clickedRow = m_table->rowAt(pos.y());
	if (clickedRow >= 0) {
		bool clickedRowSelected = false;
		for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
			if (index.row() == clickedRow) { clickedRowSelected = true; break; }
		if (!clickedRowSelected) {
			m_table->clearSelection();
			m_table->selectRow(clickedRow);
		}
	}
	std::vector<int> indices;
	for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
		indices.push_back(index.row());
	if (indices.empty()) return;
	std::sort(indices.begin(), indices.end());
	bool allExcluded = true;
	for (int row : indices) {
		const auto *item = m_table->item(row, 0);
		if (!item || !item->data(Qt::UserRole).toBool()) { allExcluded = false; break; }
	}
	const bool busy = !m_btnStart->isEnabled();
	QMenu menu(this);
	QAction *removeAction = menu.addAction(tr("Usuń zaznaczone"));
	removeAction->setShortcut(QKeySequence::Delete);
	removeAction->setEnabled(!busy);
	connect(removeAction, &QAction::triggered, this, [this, indices]() { emit removeItemsRequested(indices); });
	QAction *excludeAction = menu.addAction(allExcluded ? tr("Przywróć do kolejki") : tr("Wyklucz z kolejki"));
	excludeAction->setEnabled(!busy);
	connect(excludeAction, &QAction::triggered, this, [this, indices, allExcluded]() {
		emit setItemsExcludedRequested(indices, !allExcluded);
	});
	menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void BatchQueueView::onBatchProgress(int current, int total, const QString &) {
	if (total <= 0) {
		m_progressBar->setValue(0);
		return;
	}
	const int boundedCurrent = std::clamp(current, 0, total);
	const int percentage = static_cast<int>((static_cast<double>(boundedCurrent) / static_cast<double>(total)) * 100.0);
	m_progressBar->setValue(percentage);
}

void BatchQueueView::onBatchItemFinished(int index, bool success, quint64 inBytes, quint64 outBytes, const QString &errorMsg) {
	if (index < 0 || index >= m_table->rowCount()) return;
	const bool excluded = m_table->item(index, 0) && m_table->item(index, 0)->data(Qt::UserRole).toBool();
	if (excluded && errorMsg == QStringLiteral("Pominięto")) {
		for (int column = 1; column <= 3; ++column) m_table->setItem(index, column, new QTableWidgetItem(QStringLiteral("-")));
		auto *itemStatus = new QTableWidgetItem(tr("Wykluczony"));
		itemStatus->setForeground(QColor(130, 130, 130));
		m_table->setItem(index, 4, itemStatus);
		return;
	}
	m_table->setItem(index, 1, new QTableWidgetItem(QString::number(inBytes) + QStringLiteral(" B")));
	m_table->setItem(index, 2, new QTableWidgetItem(QString::number(outBytes) + QStringLiteral(" B")));
	double gain = 0.0;
	if (inBytes > 0) gain = (1.0 - static_cast<double>(outBytes) / static_cast<double>(inBytes)) * 100.0;
	m_table->setItem(index, 3, new QTableWidgetItem(QString::asprintf("%.1f%%", gain)));
	auto *itemStatus = new QTableWidgetItem();
	if (!success) {
		itemStatus->setText(errorMsg.isEmpty() ? tr("Błąd") : errorMsg);
		itemStatus->setForeground(QColor(230, 25, 75));
	} else {
		itemStatus->setText(tr("OK"));
		itemStatus->setForeground(QColor(60, 180, 75));
	}
	m_table->setItem(index, 4, itemStatus);
}

void BatchQueueView::onBatchFinished(int, int, quint64, quint64) {
	m_btnStart->setEnabled(true);
	m_btnCancel->setEnabled(false);
	m_btnClear->setEnabled(true);
}

void BatchQueueView::onBatchStarted() {
	m_btnStart->setEnabled(false);
	m_btnCancel->setEnabled(true);
	m_btnClear->setEnabled(false);
}

void BatchQueueView::onQueueChanged(int totalItems) {
	Q_UNUSED(totalItems);
}

void BatchQueueView::setQueueItems(const std::vector<BatchQueueItem> &items) {
	m_table->clearContents();
	m_table->setRowCount(static_cast<int>(items.size()));
	for (int i = 0; i < static_cast<int>(items.size()); ++i) {
		const auto &it = items[static_cast<std::size_t>(i)];
		auto *pathItem = new QTableWidgetItem(QString::fromStdString(it.relPath));
		pathItem->setData(Qt::UserRole, it.excluded);
		m_table->setItem(i, 0, pathItem);
		const QString inputSize = it.processed ? QString::number(it.inBytes) + QStringLiteral(" B") : QStringLiteral("-");
		const QString outputSize = it.processed ? QString::number(it.outBytes) + QStringLiteral(" B") : QStringLiteral("-");
		m_table->setItem(i, 1, new QTableWidgetItem(inputSize));
		m_table->setItem(i, 2, new QTableWidgetItem(outputSize));
		if (it.processed && it.inBytes > 0) {
			const double gain = (1.0 - static_cast<double>(it.outBytes) / static_cast<double>(it.inBytes)) * 100.0;
			m_table->setItem(i, 3, new QTableWidgetItem(QString::asprintf("%.1f%%", gain)));
		} else {
			m_table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("-")));
		}
		auto *itemStatus = new QTableWidgetItem();
		if (it.excluded) {
			itemStatus->setText(tr("Wykluczony"));
		} else if (!it.processed) {
			itemStatus->setText(tr("Oczekuje"));
		} else if (it.syntaxValid) {
			itemStatus->setText(tr("OK"));
			itemStatus->setForeground(QColor(60, 180, 75));
		} else {
			itemStatus->setText(it.errorMsg.empty() ? tr("Błąd") : QString::fromStdString(it.errorMsg));
			itemStatus->setForeground(QColor(230, 25, 75));
		}
		m_table->setItem(i, 4, itemStatus);
		if (it.excluded) {
			for (int column = 0; column < m_table->columnCount(); ++column) {
				if (auto *cell = m_table->item(i, column)) cell->setForeground(QColor(130, 130, 130));
			}
		}
	}
}
