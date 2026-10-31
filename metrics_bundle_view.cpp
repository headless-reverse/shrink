#include "metrics_bundle_view.h"
#include <fstream>
#include <string>
#include <QClipboard>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

MetricsBundleView::MetricsBundleView(QWidget *parent) : QDockWidget(tr("Metryki & LLM Bundle"), parent) {
	setObjectName(QStringLiteral("metrics_bundle"));
	setAllowedAreas(Qt::AllDockWidgetAreas);
	setupUi();
}

void MetricsBundleView::setupUi() {
	auto *container = new QWidget(this);
	auto *mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(4, 4, 4, 4);
	mainLayout->setSpacing(4);
	auto *grpMetrics = new QGroupBox(tr("Telemetria i oszczędność"), container);
	auto *metricsGrid = new QGridLayout(grpMetrics);
	metricsGrid->setContentsMargins(6, 6, 6, 6);
	m_lblBytes = new QLabel(QStringLiteral("0 B -> 0 B"), grpMetrics);
	m_lblReduction = new QLabel(QStringLiteral("0.0%"), grpMetrics);
	m_lblLines = new QLabel(QStringLiteral("0 LOC -> 0 LOC"), grpMetrics);
	m_lblTokens = new QLabel(QStringLiteral("~0"), grpMetrics);
	metricsGrid->addWidget(new QLabel(tr("Rozmiar:"), grpMetrics), 0, 0);
	metricsGrid->addWidget(m_lblBytes, 0, 1);
	metricsGrid->addWidget(new QLabel(tr("Redukcja:"), grpMetrics), 0, 2);
	metricsGrid->addWidget(m_lblReduction, 0, 3);
	metricsGrid->addWidget(new QLabel(tr("Linie kodu:"), grpMetrics), 1, 0);
	metricsGrid->addWidget(m_lblLines, 1, 1);
	metricsGrid->addWidget(new QLabel(tr("Szacowane tokeny BPE:"), grpMetrics), 1, 2);
	metricsGrid->addWidget(m_lblTokens, 1, 3);
	mainLayout->addWidget(grpMetrics);
	m_bundleView = new QPlainTextEdit(container);
	m_bundleView->setReadOnly(true);
	m_bundleView->setPlaceholderText(tr("Podgląd wygenerowanej paczki Markdown dla modeli LLM..."));
	mainLayout->addWidget(m_bundleView, 1);
	auto *btnLayout = new QHBoxLayout();
	m_btnGenerate = new QPushButton(tr("Generuj Bundle"), container);
	m_btnCopy = new QPushButton(tr("Kopiuj do schowka"), container);
	m_btnSave = new QPushButton(tr("Zapisz plik Markdown"), container);
	btnLayout->addWidget(m_btnGenerate);
	btnLayout->addWidget(m_btnCopy);
	btnLayout->addWidget(m_btnSave);
	mainLayout->addLayout(btnLayout);
	setWidget(container);
	connect(m_btnGenerate, &QPushButton::clicked, this, &MetricsBundleView::generateBundleRequested);
	connect(m_btnCopy, &QPushButton::clicked, this, &MetricsBundleView::copyBundleToClipboard);
	connect(m_btnSave, &QPushButton::clicked, this, &MetricsBundleView::saveBundleToFile);
}

void MetricsBundleView::onTransformFinished(const shrink::Result &res) {
	m_lblBytes->setText(QStringLiteral("%1 B -> %2 B").arg(res.in_bytes).arg(res.out_bytes));
	const double reduction = res.in_bytes > 0 ? (1.0 - static_cast<double>(res.out_bytes) / static_cast<double>(res.in_bytes)) * 100.0 : 0.0;
	m_lblReduction->setText(QString::asprintf("%.1f%%", reduction));
	m_lblLines->setText(QStringLiteral("%1 LOC -> %2 LOC").arg(res.in_lines).arg(res.out_lines));
	const size_t inTokens = res.in_bytes / 4;
	const size_t outTokens = res.out_bytes / 4;
	const size_t savedTokens = inTokens > outTokens ? inTokens - outTokens : 0;
	m_lblTokens->setText(QStringLiteral("Zysk: ~%1 tok (%2 -> %3)").arg(static_cast<qulonglong>(savedTokens)).arg(static_cast<qulonglong>(inTokens)).arg(static_cast<qulonglong>(outTokens)));
}

void MetricsBundleView::onBatchFinished(int, int, quint64 totalInBytes, quint64 totalOutBytes) {
	m_lblBytes->setText(QStringLiteral("%1 B -> %2 B").arg(totalInBytes).arg(totalOutBytes));
	const double reduction = totalInBytes > 0 ? (1.0 - static_cast<double>(totalOutBytes) / static_cast<double>(totalInBytes)) * 100.0 : 0.0;
	m_lblReduction->setText(QString::asprintf("%.1f%%", reduction));
	const quint64 inTokens = totalInBytes / 4;
	const quint64 outTokens = totalOutBytes / 4;
	const quint64 savedTokens = inTokens > outTokens ? inTokens - outTokens : 0;
	m_lblTokens->setText(QStringLiteral("Zysk: ~%1 tok (%2 -> %3)").arg(savedTokens).arg(inTokens).arg(outTokens));
}

void MetricsBundleView::onLlmBundleGenerated(const QString &bundleMarkdown) {
	m_bundleView->setPlainText(bundleMarkdown);
}

void MetricsBundleView::copyBundleToClipboard() {
	QClipboard *clipboard = QGuiApplication::clipboard();
	clipboard->setText(m_bundleView->toPlainText());
}

void MetricsBundleView::saveBundleToFile() {
	const QString path = QFileDialog::getSaveFileName(this, tr("Zapisz LLM Bundle"), QString(), tr("Markdown (*.md)"));
	if (path.isEmpty()) return;
	std::ofstream file(path.toStdString(), std::ios::binary);
	if (!file.is_open()) return;
	const std::string text = m_bundleView->toPlainText().toStdString();
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
}
