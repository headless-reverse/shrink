#pragma once
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QWidget>
#include <QSet>
#include <QList>
#include <QString>
#include <QSplitter>
#include <QToolBar>
#include <QAction>
#include <QSyntaxHighlighter>
#include <vector>
#include <cstddef>
#include "shrink_engine.hpp"

class CodeEditor;

class LineNumberArea : public QWidget {
public:
	explicit LineNumberArea(CodeEditor *editor);
	QSize sizeHint() const override;
protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
private:
	CodeEditor *m_editor;
};

class CodeEditor : public QPlainTextEdit {
	Q_OBJECT
public:
	explicit CodeEditor(QWidget *parent = nullptr);
	void lineNumberAreaPaintEvent(QPaintEvent *event);
	void lineNumberAreaMousePressEvent(QMouseEvent *event);
	int lineNumberAreaWidth() const;
	std::vector<size_t> excludedLines() const;
	void setExcludedLines(const std::vector<size_t> &lines);
	void setDiffLines(const QSet<int> &added, const QSet<int> &removed, const QSet<int> &modified);
	void clearDiff();
	void updateLineSelections();
	void updateExtraSelections();
signals:
	void excludedLinesChanged();
protected:
	void resizeEvent(QResizeEvent *event) override;
private slots:
	void updateLineNumberAreaWidth(int newBlockCount);
	void updateLineNumberArea(const QRect &rect, int dy);
	void onCursorPositionChanged();
private:
	QWidget *m_lineNumberArea;
	QSyntaxHighlighter *m_highlighter = nullptr;
	QSet<int> m_excludedLines, m_diffAdded, m_diffRemoved, m_diffModified;
	QList<QTextEdit::ExtraSelection> m_cachedLineSelections;
	void matchParentheses(QList<QTextEdit::ExtraSelection> &selections);
};

class CodeEditorView : public QDockWidget {
	Q_OBJECT
public:
	explicit CodeEditorView(QWidget *parent = nullptr);
	QString sourceCode() const;
	QString transformedCode() const;
	void setSourceCode(const QString &code);
	void setTransformedCode(const QString &code);
	std::vector<size_t> excludedLines() const;
public slots:
	void onTransformFinished(const shrink::Result &result);
signals:
	void transformRequested(const QString &sourceCode, const std::vector<size_t> &excludedLines);
private slots:
	void triggerTransform();
	void toggleOrientation();
	void copyTransformedCode();
	void toggleDiffOnly(bool checked);
	void syncScrollFromLeft(int value);
	void syncScrollFromRight(int value);
private:
	CodeEditor *m_leftEditor;
	CodeEditor *m_rightEditor;
	QSplitter *m_splitter;
	QToolBar *m_toolBar;
	QAction *m_actionTransform;
	QAction *m_actionToggleOrientation;
	QAction *m_actionUndo;
	QAction *m_actionRedo;
	QAction *m_actionCopy;
	QAction *m_actionDiffOnly;
	bool m_isSyncing = false;
	bool m_diffOnly = false;
	shrink::Result m_lastResult;
	void recalculateDiff();
};
