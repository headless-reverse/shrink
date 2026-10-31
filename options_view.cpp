#include "options_view.h"
#include <fstream>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

namespace {
class CollapsibleSection : public QWidget {
public:
	explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr)
		: QWidget(parent), m_title(title) {
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(2);
		m_toggleButton = new QToolButton(this);
		m_toggleButton->setAutoRaise(true);
		m_toggleButton->setCheckable(true);
		m_toggleButton->setChecked(true);
		m_toggleButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
		m_toggleButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		m_toggleButton->setStyleSheet(QStringLiteral("QToolButton { text-align: left; font-weight: bold; padding: 4px; }"));
		m_contentArea = new QWidget(this);
		m_contentLayout = new QVBoxLayout(m_contentArea);
		m_contentLayout->setContentsMargins(12, 2, 4, 6);
		m_contentLayout->setSpacing(4);
		layout->addWidget(m_toggleButton);
		layout->addWidget(m_contentArea);
		connect(m_toggleButton, &QToolButton::toggled, this, [this](bool expanded) {
			m_contentArea->setVisible(expanded);
			updateTitle(expanded);
			updateGeometry();
			if (auto *container = parentWidget()) {
				if (container->layout()) container->layout()->activate();
				container->updateGeometry();
			}
		});
		updateTitle(true);
	}

	void setContentLayout(QLayout *layout) {
		if (layout) m_contentLayout->addLayout(layout);
	}

private:
	QToolButton *m_toggleButton = nullptr;
	QWidget *m_contentArea = nullptr;
	QVBoxLayout *m_contentLayout = nullptr;
	QString m_title;

	void updateTitle(bool expanded) {
		m_toggleButton->setText((expanded ? QStringLiteral("▼ ") : QStringLiteral("▶ ")) + m_title);
	}
};
}

OptionsView::OptionsView(QWidget *parent) : QDockWidget(tr("Konfiguracja silnika"), parent) {
	setObjectName(QStringLiteral("options"));
	setupUi();
}

void OptionsView::setupUi() {
	m_scrollContent = new QWidget(this);
	auto *mainLayout = new QVBoxLayout(m_scrollContent);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(6);
	auto *languageLayout = new QHBoxLayout();
	languageLayout->addWidget(new QLabel(tr("Język:"), m_scrollContent));
	m_comboLang = new QComboBox(m_scrollContent);
	m_comboLang->addItem(QStringLiteral("Auto"), static_cast<int>(shrink::Language::Auto));
	m_comboLang->addItem(QStringLiteral("C++"), static_cast<int>(shrink::Language::Cpp));
	m_comboLang->addItem(QStringLiteral("Python"), static_cast<int>(shrink::Language::Python));
	m_comboLang->addItem(QStringLiteral("Shell"), static_cast<int>(shrink::Language::Shell));
	m_comboLang->addItem(QStringLiteral("Plain"), static_cast<int>(shrink::Language::Plain));
	languageLayout->addWidget(m_comboLang, 1);
	mainLayout->addLayout(languageLayout);
	auto *commonSection = new CollapsibleSection(tr("Wspólne / Czyszczenie"), m_scrollContent);
	auto *commonLayout = new QVBoxLayout();
	auto *blankLayout = new QHBoxLayout();
	m_comboBlankMode = new QComboBox(commonSection);
	m_comboBlankMode->addItem(tr("Usuń wszystkie"), static_cast<int>(shrink::BlankLineMode::RemoveAll));
	m_comboBlankMode->addItem(tr("Tylko między funkcjami"), static_cast<int>(shrink::BlankLineMode::BetweenFunctionsOnly));
	m_comboBlankMode->addItem(tr("Zwijaj wielokrotne"), static_cast<int>(shrink::BlankLineMode::Collapse));
	m_comboBlankMode->addItem(tr("Zachowaj"), static_cast<int>(shrink::BlankLineMode::Keep));
	blankLayout->addWidget(new QLabel(tr("Puste linie:"), commonSection));
	blankLayout->addWidget(m_comboBlankMode, 1);
	commonLayout->addLayout(blankLayout);
	auto *indentLayout = new QHBoxLayout();
	m_comboIndentMode = new QComboBox(commonSection);
	m_comboIndentMode->addItem(tr("Zachowaj wcięcia"), static_cast<int>(shrink::IndentMode::Keep));
	m_comboIndentMode->addItem(tr("Spacje ➔ Tabulatory"), static_cast<int>(shrink::IndentMode::SpacesToTabs));
	m_comboIndentMode->addItem(tr("Tabulatory ➔ Spacje"), static_cast<int>(shrink::IndentMode::TabsToSpaces));
	m_spinTabSize = new QSpinBox(commonSection);
	m_spinTabSize->setRange(1, 16);
	m_spinTabSize->setValue(4);
	m_spinTabSize->setPrefix(tr("Tab: "));
	indentLayout->addWidget(new QLabel(tr("Wcięcia:"), commonSection));
	indentLayout->addWidget(m_comboIndentMode, 1);
	indentLayout->addWidget(m_spinTabSize);
	commonLayout->addLayout(indentLayout);
	m_chkStripComments = new QCheckBox(tr("Usuwaj komentarze"), commonSection);
	m_chkStripComments->setChecked(true);
	m_chkBpeCompress = new QCheckBox(tr("Agresywna minifikacja BPE"), commonSection);
	commonLayout->addWidget(m_chkStripComments);
	commonLayout->addWidget(m_chkBpeCompress);
	commonSection->setContentLayout(commonLayout);
	mainLayout->addWidget(commonSection);
	auto *semanticSection = new CollapsibleSection(tr("Semantyka kodu"), m_scrollContent);
	auto *semanticLayout = new QVBoxLayout();
	m_chkExtractSkeleton = new QCheckBox(tr("Ekstrakcja szkieletu"), semanticSection);
	m_chkSkeletonPublicOnly = new QCheckBox(tr("Tylko metody publiczne"), semanticSection);
	semanticLayout->addWidget(m_chkExtractSkeleton);
	semanticLayout->addWidget(m_chkSkeletonPublicOnly);
	semanticSection->setContentLayout(semanticLayout);
	mainLayout->addWidget(semanticSection);
	m_cppSection = new CollapsibleSection(tr("C++ Transformacje"), m_scrollContent);
	auto *cppLayout = new QVBoxLayout();
	m_chkModernizeGuards = new QCheckBox(tr("Konwersja guards na #pragma once"), m_cppSection);
	m_chkStripRedundant = new QCheckBox(tr("Usuń redundantne virtual"), m_cppSection);
	m_chkModernizeNamespaces = new QCheckBox(tr("Spłaszczanie namespace (C++17)"), m_cppSection);
	m_chkCompactTypes = new QCheckBox(tr("Zwijanie struktur jednowierszowych"), m_cppSection);
	m_chkCompactConstructors = new QCheckBox(tr("Zwijanie list inicjalizacyjnych"), m_cppSection);
	m_chkIfToTernary = new QCheckBox(tr("Konwersja if/else na operator ternary"), m_cppSection);
	m_chkStripAttributes = new QCheckBox(tr("Usuwaj atrybuty ([[nodiscard]], noexcept)"), m_cppSection);
	m_chkStripDebug = new QCheckBox(tr("Usuwaj instrukcje debugujące (cout, qDebug)"), m_cppSection);
	m_chkUnwrapParentheses = new QCheckBox(tr("Spłaszczanie nawiasów (...)"), m_cppSection);
	m_chkCompactShortFunctions = new QCheckBox(tr("Zwijanie krótkich funkcji do 1 linii"), m_cppSection);
	cppLayout->addWidget(m_chkModernizeGuards);
	cppLayout->addWidget(m_chkStripRedundant);
	cppLayout->addWidget(m_chkModernizeNamespaces);
	cppLayout->addWidget(m_chkCompactTypes);
	cppLayout->addWidget(m_chkCompactConstructors);
	cppLayout->addWidget(m_chkIfToTernary);
	cppLayout->addWidget(m_chkStripAttributes);
	cppLayout->addWidget(m_chkStripDebug);
	cppLayout->addWidget(m_chkUnwrapParentheses);
	cppLayout->addWidget(m_chkCompactShortFunctions);
	static_cast<CollapsibleSection *>(m_cppSection)->setContentLayout(cppLayout);
	mainLayout->addWidget(m_cppSection);
	m_pythonSection = new CollapsibleSection(tr("Python Transformacje"), m_scrollContent);
	auto *pythonLayout = new QVBoxLayout();
	m_chkStripDocstrings = new QCheckBox(tr("Usuwaj docstringi"), m_pythonSection);
	m_chkStripTypes = new QCheckBox(tr("Usuwaj adnotacje typów"), m_pythonSection);
	pythonLayout->addWidget(m_chkStripDocstrings);
	pythonLayout->addWidget(m_chkStripTypes);
	static_cast<CollapsibleSection *>(m_pythonSection)->setContentLayout(pythonLayout);
	mainLayout->addWidget(m_pythonSection);
	auto *verifySection = new CollapsibleSection(tr("Weryfikacja"), m_scrollContent);
	auto *vrfLayout = new QVBoxLayout();
	m_chkVerifySyntax = new QCheckBox(tr("Weryfikacja poprawności składni"), verifySection);
	vrfLayout->addWidget(m_chkVerifySyntax);
	verifySection->setContentLayout(vrfLayout);
	mainLayout->addWidget(verifySection);
	auto *profileSection = new CollapsibleSection(tr("Profile"), m_scrollContent);
	auto *profLayout = new QHBoxLayout();
	m_btnExportProfile = new QPushButton(tr("Eksportuj JSON"), profileSection);
	m_btnImportProfile = new QPushButton(tr("Importuj JSON"), profileSection);
	profLayout->addWidget(m_btnExportProfile);
	profLayout->addWidget(m_btnImportProfile);
	profileSection->setContentLayout(profLayout);
	mainLayout->addWidget(profileSection);
	mainLayout->addStretch(1);
	auto *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setWidget(m_scrollContent);
	setWidget(scrollArea);
	const auto connectNotify = [this]() { notifyOptionsChanged(); };
	connect(m_comboLang, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
		updateLanguageVisibility(static_cast<shrink::Language>(m_comboLang->currentData().toInt()));
		notifyOptionsChanged();
	});
	connect(m_comboBlankMode, qOverload<int>(&QComboBox::currentIndexChanged), this, connectNotify);
	connect(m_comboIndentMode, qOverload<int>(&QComboBox::currentIndexChanged), this, connectNotify);
	connect(m_spinTabSize, qOverload<int>(&QSpinBox::valueChanged), this, connectNotify);
	connect(m_chkExtractSkeleton, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkSkeletonPublicOnly, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkModernizeGuards, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripRedundant, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkBpeCompress, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkIfToTernary, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkCompactConstructors, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkCompactTypes, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkModernizeNamespaces, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripComments, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkUnwrapParentheses, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkCompactShortFunctions, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripAttributes, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripDebug, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripDocstrings, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkStripTypes, &QCheckBox::toggled, this, connectNotify);
	connect(m_chkVerifySyntax, &QCheckBox::toggled, this, connectNotify);
	connect(m_btnExportProfile, &QPushButton::clicked, this, &OptionsView::exportProfileJson);
	connect(m_btnImportProfile, &QPushButton::clicked, this, &OptionsView::importProfileJson);
	updateLanguageVisibility(static_cast<shrink::Language>(m_comboLang->currentData().toInt()));
}

void OptionsView::updateLanguageVisibility(shrink::Language lang) {
	const bool showCpp = lang == shrink::Language::Auto || lang == shrink::Language::Cpp;
	const bool showPython = lang == shrink::Language::Auto || lang == shrink::Language::Python;
	m_cppSection->setVisible(showCpp);
	m_pythonSection->setVisible(showPython);
	m_chkSkeletonPublicOnly->setVisible(lang == shrink::Language::Auto || lang == shrink::Language::Cpp);
	if (m_scrollContent) {
		if (m_scrollContent->layout()) m_scrollContent->layout()->activate();
		m_scrollContent->updateGeometry();
		m_scrollContent->adjustSize();
	}
	updateGeometry();
}

shrink::Options OptionsView::getSelectedOptions() const {
	shrink::Options opt;
	opt.lang = static_cast<shrink::Language>(m_comboLang->currentData().toInt());
	opt.blank_mode = static_cast<shrink::BlankLineMode>(m_comboBlankMode->currentData().toInt());
	opt.indent_mode = static_cast<shrink::IndentMode>(m_comboIndentMode->currentData().toInt());
	opt.tab_size = m_spinTabSize->value();
	opt.extract_skeleton = m_chkExtractSkeleton->isChecked();
	opt.skeleton_public_only = m_chkSkeletonPublicOnly->isChecked();
	opt.modernize_guards = m_chkModernizeGuards->isChecked();
	opt.strip_redundant = m_chkStripRedundant->isChecked();
	opt.bpe_compress = m_chkBpeCompress->isChecked();
	opt.if_to_ternary = m_chkIfToTernary->isChecked();
	opt.compact_constructors = m_chkCompactConstructors->isChecked();
	opt.compact_types = m_chkCompactTypes->isChecked();
	opt.modernize_namespaces = m_chkModernizeNamespaces->isChecked();
	opt.strip_comments = m_chkStripComments->isChecked();
	opt.unwrap_parentheses = m_chkUnwrapParentheses->isChecked();
	opt.compact_short_functions = m_chkCompactShortFunctions->isChecked();
	opt.strip_attributes = m_chkStripAttributes->isChecked();
	opt.strip_debug = m_chkStripDebug->isChecked();
	opt.strip_docstrings = m_chkStripDocstrings->isChecked();
	opt.strip_types = m_chkStripTypes->isChecked();
	opt.verify_syntax = m_chkVerifySyntax->isChecked();
	return opt;
}

void OptionsView::applyOptions(const shrink::Options &opt) {
	const QSignalBlocker langBlocker(m_comboLang);
	const QSignalBlocker blankBlocker(m_comboBlankMode);
	const QSignalBlocker indentBlocker(m_comboIndentMode);
	const QSignalBlocker tabBlocker(m_spinTabSize);
	const QSignalBlocker skeletonBlocker(m_chkExtractSkeleton);
	const QSignalBlocker publicBlocker(m_chkSkeletonPublicOnly);
	const QSignalBlocker guardsBlocker(m_chkModernizeGuards);
	const QSignalBlocker redundantBlocker(m_chkStripRedundant);
	const QSignalBlocker bpeBlocker(m_chkBpeCompress);
	const QSignalBlocker ternaryBlocker(m_chkIfToTernary);
	const QSignalBlocker constructorsBlocker(m_chkCompactConstructors);
	const QSignalBlocker typesBlocker(m_chkCompactTypes);
	const QSignalBlocker namespacesBlocker(m_chkModernizeNamespaces);
	const QSignalBlocker commentsBlocker(m_chkStripComments);
	const QSignalBlocker unwrapBlocker(m_chkUnwrapParentheses);
	const QSignalBlocker compactShortBlocker(m_chkCompactShortFunctions);
	const QSignalBlocker attributesBlocker(m_chkStripAttributes);
	const QSignalBlocker debugBlocker(m_chkStripDebug);
	const QSignalBlocker docstringsBlocker(m_chkStripDocstrings);
	const QSignalBlocker stripTypesBlocker(m_chkStripTypes);
	const QSignalBlocker verifyBlocker(m_chkVerifySyntax);
	const int idxLang = m_comboLang->findData(static_cast<int>(opt.lang));
	if (idxLang >= 0) m_comboLang->setCurrentIndex(idxLang);
	const int idxBlank = m_comboBlankMode->findData(static_cast<int>(opt.blank_mode));
	if (idxBlank >= 0) m_comboBlankMode->setCurrentIndex(idxBlank);
	const int idxIndent = m_comboIndentMode->findData(static_cast<int>(opt.indent_mode));
	if (idxIndent >= 0) m_comboIndentMode->setCurrentIndex(idxIndent);
	m_spinTabSize->setValue(opt.tab_size > 0 ? opt.tab_size : 4);
	m_chkExtractSkeleton->setChecked(opt.extract_skeleton);
	m_chkSkeletonPublicOnly->setChecked(opt.skeleton_public_only);
	m_chkModernizeGuards->setChecked(opt.modernize_guards);
	m_chkStripRedundant->setChecked(opt.strip_redundant);
	m_chkBpeCompress->setChecked(opt.bpe_compress);
	m_chkIfToTernary->setChecked(opt.if_to_ternary);
	m_chkCompactConstructors->setChecked(opt.compact_constructors);
	m_chkCompactTypes->setChecked(opt.compact_types);
	m_chkModernizeNamespaces->setChecked(opt.modernize_namespaces);
	m_chkStripComments->setChecked(opt.strip_comments);
	m_chkUnwrapParentheses->setChecked(opt.unwrap_parentheses);
	m_chkCompactShortFunctions->setChecked(opt.compact_short_functions);
	m_chkStripAttributes->setChecked(opt.strip_attributes);
	m_chkStripDebug->setChecked(opt.strip_debug);
	m_chkStripDocstrings->setChecked(opt.strip_docstrings);
	m_chkStripTypes->setChecked(opt.strip_types);
	m_chkVerifySyntax->setChecked(opt.verify_syntax);
	updateLanguageVisibility(opt.lang);
	emit optionsChanged(getSelectedOptions());
}

void OptionsView::notifyOptionsChanged() {
	emit optionsChanged(getSelectedOptions());
}

void OptionsView::exportProfileJson() {
	const QString path = QFileDialog::getSaveFileName(this, tr("Eksportuj profil"), QString(), tr("JSON (*.json)"));
	if (path.isEmpty()) return;
	const shrink::Options opt = getSelectedOptions();
	nlohmann::json json;
	json["lang"] = static_cast<int>(opt.lang);
	json["blank_mode"] = static_cast<int>(opt.blank_mode);
	json["indent_mode"] = static_cast<int>(opt.indent_mode);
	json["tab_size"] = opt.tab_size;
	json["extract_skeleton"] = opt.extract_skeleton;
	json["skeleton_public_only"] = opt.skeleton_public_only;
	json["modernize_guards"] = opt.modernize_guards;
	json["strip_redundant"] = opt.strip_redundant;
	json["bpe_compress"] = opt.bpe_compress;
	json["if_to_ternary"] = opt.if_to_ternary;
	json["compact_constructors"] = opt.compact_constructors;
	json["compact_types"] = opt.compact_types;
	json["modernize_namespaces"] = opt.modernize_namespaces;
	json["strip_comments"] = opt.strip_comments;
	json["unwrap_parentheses"] = opt.unwrap_parentheses;
	json["compact_short_functions"] = opt.compact_short_functions;
	json["strip_attributes"] = opt.strip_attributes;
	json["strip_debug"] = opt.strip_debug;
	json["strip_docstrings"] = opt.strip_docstrings;
	json["strip_types"] = opt.strip_types;
	json["verify_syntax"] = opt.verify_syntax;
	std::ofstream file(path.toStdString());
	if (file.is_open()) file << json.dump(4);
}

void OptionsView::importProfileJson() {
	const QString path = QFileDialog::getOpenFileName(this, tr("Importuj profil"), QString(), tr("JSON (*.json)"));
	if (path.isEmpty()) return;
	std::ifstream file(path.toStdString());
	if (!file.is_open()) return;
	try {
		nlohmann::json json;
		file >> json;
		shrink::Options opt;
		opt.lang = static_cast<shrink::Language>(json.value("lang", 0));
		opt.blank_mode = static_cast<shrink::BlankLineMode>(json.value("blank_mode", 0));
		opt.indent_mode = static_cast<shrink::IndentMode>(json.value("indent_mode", 0));
		opt.tab_size = json.value("tab_size", 4);
		opt.extract_skeleton = json.value("extract_skeleton", false);
		opt.skeleton_public_only = json.value("skeleton_public_only", false);
		opt.modernize_guards = json.value("modernize_guards", false);
		opt.strip_redundant = json.value("strip_redundant", false);
		opt.bpe_compress = json.value("bpe_compress", false);
		opt.if_to_ternary = json.value("if_to_ternary", false);
		opt.compact_constructors = json.value("compact_constructors", false);
		opt.compact_types = json.value("compact_types", false);
		opt.modernize_namespaces = json.value("modernize_namespaces", false);
		opt.strip_comments = json.value("strip_comments", true);
		opt.unwrap_parentheses = json.value("unwrap_parentheses", false);
		opt.compact_short_functions = json.value("compact_short_functions", false);
		opt.strip_attributes = json.value("strip_attributes", false);
		opt.strip_debug = json.value("strip_debug", false);
		opt.strip_docstrings = json.value("strip_docstrings", false);
		opt.strip_types = json.value("strip_types", false);
		opt.verify_syntax = json.value("verify_syntax", false);
		applyOptions(opt);
	} catch (const nlohmann::json::exception &) {
	}
}
