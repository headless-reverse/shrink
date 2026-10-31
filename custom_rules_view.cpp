#include "custom_rules_view.h"
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <re2/re2.h>

CustomRulesView::CustomRulesView(QWidget *parent) : QDockWidget(tr("Reguły RE2"), parent) {
	setObjectName(QStringLiteral("custom_rules"));
	setAllowedAreas(Qt::AllDockWidgetAreas);
	setupUi();
}

void CustomRulesView::setupUi() {
	auto *container = new QWidget(this);
	auto *mainLayout = new QVBoxLayout(container);
	mainLayout->setContentsMargins(4, 4, 4, 4);
	mainLayout->setSpacing(4);
	m_table = new QTableWidget(0, 3, container);
	m_table->setHorizontalHeaderLabels({tr("Nazwa"), tr("Wzorzec (RE2)"), tr("Zastąpienie")});
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
	mainLayout->addWidget(m_table, 1);
	auto *btnLayout = new QHBoxLayout();
	m_btnAdd = new QPushButton(tr("Dodaj regułę"), container);
	m_btnRemove = new QPushButton(tr("Usuń zaznaczone"), container);
	m_btnLoad = new QPushButton(tr("Wczytaj JSON"), container);
	m_btnSave = new QPushButton(tr("Zapisz JSON"), container);
	btnLayout->addWidget(m_btnAdd);
	btnLayout->addWidget(m_btnRemove);
	btnLayout->addWidget(m_btnLoad);
	btnLayout->addWidget(m_btnSave);
	mainLayout->addLayout(btnLayout);
	auto *testHeader = new QLabel(tr("Podgląd testowy RE2 na żywo:"), container);
	testHeader->setStyleSheet(QStringLiteral("font-weight: bold; margin-top: 4px;"));
	mainLayout->addWidget(testHeader);
	auto *patternLayout = new QHBoxLayout();
	m_testRegex = new QLineEdit(container);
	m_testRegex->setPlaceholderText(tr(R"(Wzorzec RE2, np. \bDEBUG_LOG\\([^)]*\);)"));
	m_testReplace = new QLineEdit(container);
	m_testReplace->setPlaceholderText(tr("Zamiennik (np. /* clean */)"));
	patternLayout->addWidget(m_testRegex);
	patternLayout->addWidget(m_testReplace);
	mainLayout->addLayout(patternLayout);
	auto *ioLayout = new QHBoxLayout();
	m_testInput = new QPlainTextEdit(container);
	m_testInput->setPlaceholderText(tr("Tekst źródłowy do testu..."));
	m_testOutput = new QPlainTextEdit(container);
	m_testOutput->setPlaceholderText(tr("Wynik podstawienia..."));
	m_testOutput->setReadOnly(true);
	ioLayout->addWidget(m_testInput);
	ioLayout->addWidget(m_testOutput);
	mainLayout->addLayout(ioLayout);
	setWidget(container);
	connect(m_btnAdd, &QPushButton::clicked, this, &CustomRulesView::addRule);
	connect(m_btnRemove, &QPushButton::clicked, this, &CustomRulesView::removeSelectedRule);
	connect(m_btnLoad, &QPushButton::clicked, this, &CustomRulesView::loadRulesFile);
	connect(m_btnSave, &QPushButton::clicked, this, &CustomRulesView::saveRulesFile);
	connect(m_testRegex, &QLineEdit::textChanged, this, &CustomRulesView::updateLiveTest);
	connect(m_testReplace, &QLineEdit::textChanged, this, &CustomRulesView::updateLiveTest);
	connect(m_testInput, &QPlainTextEdit::textChanged, this, &CustomRulesView::updateLiveTest);
	connect(m_table, &QTableWidget::cellChanged, this, [this](int, int) { emit rulesUpdated(rules()); });
}

std::vector<shrink::CustomRule> CustomRulesView::rules() const {
	std::vector<shrink::CustomRule> result;
	for (int i = 0; i < m_table->rowCount(); ++i) {
		const auto *itName = m_table->item(i, 0);
		const auto *itRegex = m_table->item(i, 1);
		const auto *itReplace = m_table->item(i, 2);
		if (!itRegex || itRegex->text().isEmpty()) continue;
		shrink::CustomRule rule;
		rule.name = itName ? itName->text().toStdString() : "";
		rule.regex_str = itRegex->text().toStdString();
		rule.replace = itReplace ? itReplace->text().toStdString() : "";
		rule.compiled_re = std::make_shared<re2::RE2>(rule.regex_str);
		if (rule.compiled_re->ok()) result.push_back(std::move(rule));
	}
	return result;
}

void CustomRulesView::setRules(const std::vector<shrink::CustomRule> &rulesList) {
	m_table->blockSignals(true);
	m_table->setRowCount(static_cast<int>(rulesList.size()));
	for (int i = 0; i < static_cast<int>(rulesList.size()); ++i) {
		const auto &rule = rulesList[static_cast<std::size_t>(i)];
		m_table->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(rule.name)));
		m_table->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(rule.regex_str)));
		m_table->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(rule.replace)));
	}
	m_table->blockSignals(false);
	emit rulesUpdated(rules());
}

void CustomRulesView::addRule() {
	const int row = m_table->rowCount();
	m_table->insertRow(row);
	m_table->setItem(row, 0, new QTableWidgetItem(tr("Reguła %1").arg(row + 1)));
	m_table->setItem(row, 1, new QTableWidgetItem());
	m_table->setItem(row, 2, new QTableWidgetItem());
	m_table->setCurrentCell(row, 1);
	m_table->editItem(m_table->item(row, 1));
}

void CustomRulesView::removeSelectedRule() {
	const int row = m_table->currentRow();
	if (row >= 0) {
		m_table->removeRow(row);
		emit rulesUpdated(rules());
	}
}

void CustomRulesView::loadRulesFile() {
	const QString path = QFileDialog::getOpenFileName(this, tr("Wczytaj reguły"), QString(), tr("JSON (*.json)"));
	if (path.isEmpty()) return;
	std::ifstream file(path.toStdString(), std::ios::binary);
	if (!file.is_open()) return;
	const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	setRules(shrink::parse_rules_json(content));
}

void CustomRulesView::saveRulesFile() {
	const QString path = QFileDialog::getSaveFileName(this, tr("Zapisz reguły"), QString(), tr("JSON (*.json)"));
	if (path.isEmpty()) return;
	nlohmann::json json = nlohmann::json::array();
	for (const auto &rule : rules()) json.push_back({{"name", rule.name}, {"regex", rule.regex_str}, {"replace", rule.replace}});
	std::ofstream file(path.toStdString());
	if (file.is_open()) file << json.dump(4);
}

void CustomRulesView::updateLiveTest() {
	const std::string pattern = m_testRegex->text().toStdString();
	if (pattern.empty()) {
		m_testOutput->setPlainText(m_testInput->toPlainText());
		return;
	}
	const re2::RE2 regex(pattern);
	if (!regex.ok()) {
		m_testOutput->setPlainText(tr("[Błąd składni RE2: %1]").arg(QString::fromStdString(regex.error())));
		return;
	}
	std::string text = m_testInput->toPlainText().toStdString();
	const std::string replacement = m_testReplace->text().toStdString();
	re2::RE2::GlobalReplace(&text, regex, replacement);
	m_testOutput->setPlainText(QString::fromStdString(text));
}
