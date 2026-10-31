#pragma once
#include <QDockWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QWidget>
#include "shrink_engine.hpp"

class OptionsView : public QDockWidget {
	Q_OBJECT
public:
	explicit OptionsView(QWidget *parent = nullptr);
	shrink::Options getSelectedOptions() const;
	void applyOptions(const shrink::Options &opt);
signals:
	void optionsChanged(const shrink::Options &opt);
private slots:
	void notifyOptionsChanged();
	void exportProfileJson();
	void importProfileJson();
private:
	QComboBox *m_comboLang = nullptr;
	QComboBox *m_comboBlankMode = nullptr;
	QComboBox *m_comboIndentMode = nullptr;
	QSpinBox *m_spinTabSize = nullptr;
	QCheckBox *m_chkExtractSkeleton = nullptr;
	QCheckBox *m_chkSkeletonPublicOnly = nullptr;
	QCheckBox *m_chkModernizeGuards = nullptr;
	QCheckBox *m_chkStripRedundant = nullptr;
	QCheckBox *m_chkBpeCompress = nullptr;
	QCheckBox *m_chkIfToTernary = nullptr;
	QCheckBox *m_chkCompactConstructors = nullptr;
	QCheckBox *m_chkCompactTypes = nullptr;
	QCheckBox *m_chkModernizeNamespaces = nullptr;
	QCheckBox *m_chkStripComments = nullptr;
	QCheckBox *m_chkUnwrapParentheses = nullptr;
	QCheckBox *m_chkCompactShortFunctions = nullptr;
	QCheckBox *m_chkStripAttributes = nullptr;
	QCheckBox *m_chkStripDebug = nullptr;
	QCheckBox *m_chkStripDocstrings = nullptr;
	QCheckBox *m_chkStripTypes = nullptr;
	QCheckBox *m_chkVerifySyntax = nullptr;
	QPushButton *m_btnExportProfile = nullptr;
	QPushButton *m_btnImportProfile = nullptr;
	QWidget *m_cppSection = nullptr;
	QWidget *m_pythonSection = nullptr;
	QWidget *m_scrollContent = nullptr;
	void setupUi();
	void updateLanguageVisibility(shrink::Language lang);
};
