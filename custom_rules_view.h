#pragma once
#include <memory>
#include <vector>
#include <QDockWidget>
#include <QString>
#include <QPlainTextEdit>
#include "shrink_engine.hpp"

class QLineEdit;
class QPushButton;
class QTableWidget;

class CustomRulesView : public QDockWidget {
	Q_OBJECT

public:
	explicit CustomRulesView(QWidget *parent = nullptr);

	std::vector<shrink::CustomRule> rules() const;
	void setRules(const std::vector<shrink::CustomRule> &rulesList);

signals:
	void rulesUpdated(const std::vector<shrink::CustomRule> &rules);

private slots:
	void addRule();
	void removeSelectedRule();
	void loadRulesFile();
	void saveRulesFile();
	void updateLiveTest();

private:
	QTableWidget *m_table = nullptr;
	QPushButton *m_btnAdd = nullptr;
	QPushButton *m_btnRemove = nullptr;
	QPushButton *m_btnLoad = nullptr;
	QPushButton *m_btnSave = nullptr;
	QLineEdit *m_testRegex = nullptr;
	QLineEdit *m_testReplace = nullptr;
	QPlainTextEdit *m_testInput = nullptr;
	QPlainTextEdit *m_testOutput = nullptr;
	void setupUi();
};
