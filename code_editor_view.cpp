#include "code_editor_view.h"
#include <QPainter>
#include <QTextBlock>
#include <QScrollBar>
#include <QClipboard>
#include <QGuiApplication>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <algorithm>
#include <utility>

namespace {

class GenericSyntaxHighlighter final : public QSyntaxHighlighter {
public:
	explicit GenericSyntaxHighlighter(QTextDocument *document)
		: QSyntaxHighlighter(document)
	{
		m_keywordFormat.setForeground(QColor("#569CD6"));
		m_keywordFormat.setFontWeight(QFont::Bold);
		m_typeFormat.setForeground(QColor("#4EC9B0"));
		m_stringFormat.setForeground(QColor("#D69D85"));
		m_commentFormat.setForeground(QColor("#6A9955"));
		m_commentFormat.setFontItalic(true);
		m_preprocessorFormat.setForeground(QColor("#9B9B9B"));
		const QStringList keywords = {
			"alignas", "alignof", "auto", "bool", "break", "case", "catch", "char",
			"class", "const", "constexpr", "continue", "decltype", "default", "delete",
			"do", "double", "else", "enum", "explicit", "export", "extern", "false",
			"float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
			"namespace", "new", "noexcept", "nullptr", "operator", "private", "protected",
			"public", "register", "return", "short", "signed", "sizeof", "static",
			"static_assert", "struct", "switch", "template", "this", "thread_local",
			"throw", "true", "try", "typedef", "typename", "union", "unsigned",
			"using", "virtual", "void", "volatile", "while", "def", "import", "from",
			"pass", "lambda", "elif", "print"
		};
		for (const QString &kw : keywords) {
			m_rules.append({QRegularExpression(QStringLiteral("\\b") + kw + QStringLiteral("\\b")), m_keywordFormat});
		}
		const QStringList types = {
			"size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
			"int16_t", "int32_t", "int64_t", "string", "string_view", "vector",
			"map", "set", "shared_ptr", "unique_ptr"
		};
		for (const QString &tp : types) {
			m_rules.append({QRegularExpression(QStringLiteral("\\b") + tp + QStringLiteral("\\b")), m_typeFormat});
		}
		m_rules.append({QRegularExpression(QStringLiteral("#\\s*[a-zA-Z_]+")), m_preprocessorFormat});
		m_rules.append({QRegularExpression(QStringLiteral("\".*?\"")), m_stringFormat});
		m_rules.append({QRegularExpression(QStringLiteral("'.*?'")), m_stringFormat});
		m_rules.append({QRegularExpression(QStringLiteral("//[^\n]*")), m_commentFormat});
		m_rules.append({QRegularExpression(QStringLiteral("#[^\n]*")), m_commentFormat});
	}

protected:
	void highlightBlock(const QString &text) override {
		for (const auto &rule : m_rules) {
			auto it = rule.pattern.globalMatch(text);
			while (it.hasNext()) {
				auto match = it.next();
				setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), rule.format);
			}
		}

		setCurrentBlockState(0);
		int startIndex = 0;
		if (previousBlockState() != 1) {
			startIndex = static_cast<int>(text.indexOf(QStringLiteral("/*")));
		}

		while (startIndex >= 0) {
			int endIndex = static_cast<int>(text.indexOf(QStringLiteral("*/"), startIndex));
			int commentLength;
			if (endIndex == -1) {
				setCurrentBlockState(1);
				commentLength = static_cast<int>(text.length()) - startIndex;
			} else {
				commentLength = endIndex - startIndex + 2;
			}
			setFormat(startIndex, commentLength, m_commentFormat);
			startIndex = static_cast<int>(text.indexOf(QStringLiteral("/*"), startIndex + commentLength));
		}
	}

private:
	struct Rule {
		QRegularExpression pattern;
		QTextCharFormat format;
	};
	QList<Rule> m_rules;
	QTextCharFormat m_keywordFormat;
	QTextCharFormat m_typeFormat;
	QTextCharFormat m_stringFormat;
	QTextCharFormat m_commentFormat;
	QTextCharFormat m_preprocessorFormat;
};

void computeLinearDiff(const QStringList &src, const QStringList &dst,
					   QSet<int> &leftRemoved, QSet<int> &rightAdded) {
	leftRemoved.clear();
	rightAdded.clear();
	const int n = static_cast<int>(src.size());
	const int m = static_cast<int>(dst.size());
	if (n == 0) {
		for (int j = 1; j <= m; ++j) rightAdded.insert(j);
		return;
	}
	if (m == 0) {
		for (int i = 1; i <= n; ++i) leftRemoved.insert(i);
		return;
	}
	if (static_cast<qint64>(n) * m > 4000000LL) {
		QSet<QString> dstSet;
		dstSet.reserve(m);
		for (const auto &line : dst) dstSet.insert(line.trimmed());
		QSet<QString> srcSet;
		srcSet.reserve(n);
		for (const auto &line : src) srcSet.insert(line.trimmed());
		for (int i = 0; i < n; ++i) {
			if (!dstSet.contains(src[i].trimmed())) leftRemoved.insert(i + 1);
		}
		for (int j = 0; j < m; ++j) {
			if (!srcSet.contains(dst[j].trimmed())) rightAdded.insert(j + 1);
		}
		return;
	}

	const int maxD = n + m;
	const int offset = maxD;
	std::vector<int> v(2 * maxD + 1, 0);

	struct EditPath {
		int x;
		int y;
		int prevIdx;
	};
	std::vector<EditPath> trace;
	trace.reserve(maxD * 4);

	bool reached = false;
	for (int d = 0; d <= maxD && !reached; ++d) {
		for (int k = -d; k <= d; k += 2) {
			int kIdx = k + offset;
			int x, prevKIdx;

			if (k == -d || (k != d && v[kIdx - 1] < v[kIdx + 1])) {
				prevKIdx = kIdx + 1;
				x = v[prevKIdx];
			} else {
				prevKIdx = kIdx - 1;
				x = v[prevKIdx] + 1;
			}

			int y = x - k;
			trace.push_back({x, y, (d == 0) ? -1 : prevKIdx});

			while (x < n && y < m && src[x] == dst[y]) {
				++x;
				++y;
			}
			v[kIdx] = x;

			if (x >= n && y >= m) {
				reached = true;
				break;
			}
		}
	}

	int currX = n;
	int currY = m;
	while (currX > 0 || currY > 0) {
		if (currX > 0 && currY > 0 && src[currX - 1] == dst[currY - 1]) {
			--currX;
			--currY;
		} else if (currY > 0 && (currX == 0 || currX - currY < n - m)) {
			rightAdded.insert(currY);
			--currY;
		} else if (currX > 0 && (currY == 0 || currX - currY >= n - m)) {
			leftRemoved.insert(currX);
			--currX;
		} else {
			break;
		}
	}
}

}

LineNumberArea::LineNumberArea(CodeEditor *editor)
	: QWidget(editor)
	, m_editor(editor)
{}

QSize LineNumberArea::sizeHint() const {
	return QSize(m_editor->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent *event) {
	m_editor->lineNumberAreaPaintEvent(event);
}

void LineNumberArea::mousePressEvent(QMouseEvent *event) {
	m_editor->lineNumberAreaMousePressEvent(event);
}

CodeEditor::CodeEditor(QWidget *parent) : QPlainTextEdit(parent) {
	m_lineNumberArea = new LineNumberArea(this);
	m_highlighter = new GenericSyntaxHighlighter(document());
	QFont font(QStringLiteral("Consolas"), 10);
	font.setStyleHint(QFont::Monospace);
	setFont(font);
	setWordWrapMode(QTextOption::NoWrap);
	connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
	connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
	connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::onCursorPositionChanged);
	updateLineNumberAreaWidth(0);
	updateLineSelections();
	updateExtraSelections();
}

int CodeEditor::lineNumberAreaWidth() const {
	int digits = 1;
	int maxLines = std::max(1, blockCount());
	while (maxLines >= 10) {
		maxLines /= 10;
		++digits;
	}
	return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int) {
	setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy) {
	if (dy) {
		m_lineNumberArea->scroll(0, dy);
	} else {
		m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
	}
	if (rect.contains(viewport()->rect())) {
		updateLineNumberAreaWidth(0);
	}
}

void CodeEditor::resizeEvent(QResizeEvent *event) {
	QPlainTextEdit::resizeEvent(event);
	QRect cr = contentsRect();
	m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event) {
	QPainter painter(m_lineNumberArea);
	painter.fillRect(event->rect(), QColor("#1E1E1E"));
	QTextBlock block = firstVisibleBlock();
	int blockNumber = block.blockNumber();
	int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
	int bottom = top + static_cast<int>(blockBoundingRect(block).height());
	while (block.isValid() && top <= event->rect().bottom()) {
		if (block.isVisible() && bottom >= event->rect().top()) {
			int lineNum = blockNumber + 1;
			QString number = QString::number(lineNum);
			if (m_excludedLines.contains(lineNum)) {
				painter.fillRect(0, top, m_lineNumberArea->width(), fontMetrics().height(), QColor(200, 160, 20, 120));
				painter.setPen(QColor("#FFFFFF"));
			} else if (m_diffAdded.contains(lineNum)) {
				painter.fillRect(0, top, m_lineNumberArea->width(), fontMetrics().height(), QColor(35, 120, 55, 120));
				painter.setPen(QColor("#B8FFB8"));
			} else if (m_diffRemoved.contains(lineNum)) {
				painter.fillRect(0, top, m_lineNumberArea->width(), fontMetrics().height(), QColor(140, 40, 40, 120));
				painter.setPen(QColor("#FFB8B8"));
			} else {
				painter.setPen(QColor("#6A6A6A"));
			}
			painter.drawText(0, top, m_lineNumberArea->width() - 4, fontMetrics().height(),
							 Qt::AlignRight | Qt::AlignVCenter, number);
		}
		block = block.next();
		top = bottom;
		bottom = top + static_cast<int>(blockBoundingRect(block).height());
		++blockNumber;
	}
}

void CodeEditor::lineNumberAreaMousePressEvent(QMouseEvent *event) {
	if (isReadOnly()) return;
	QTextBlock block = firstVisibleBlock();
	int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
	int bottom = top + static_cast<int>(blockBoundingRect(block).height());
	int y = event->pos().y();
	while (block.isValid() && top <= y) {
		if (y >= top && y <= bottom) {
			int lineNum = block.blockNumber() + 1;
			if (m_excludedLines.contains(lineNum)) {
				m_excludedLines.remove(lineNum);
			} else {
				m_excludedLines.insert(lineNum);
			}
			updateLineSelections();
			updateExtraSelections();
			m_lineNumberArea->update();
			emit excludedLinesChanged();
			break;
		}
		block = block.next();
		top = bottom;
		bottom = top + static_cast<int>(blockBoundingRect(block).height());
	}
}

std::vector<size_t> CodeEditor::excludedLines() const {
	std::vector<size_t> lines;
	lines.reserve(m_excludedLines.size());
	for (int l : m_excludedLines) {
		lines.push_back(static_cast<size_t>(l));
	}
	std::sort(lines.begin(), lines.end());
	return lines;
}

void CodeEditor::setExcludedLines(const std::vector<size_t> &lines) {
	m_excludedLines.clear();
	for (size_t l : lines) {
		m_excludedLines.insert(static_cast<int>(l));
	}
	updateLineSelections();
	updateExtraSelections();
	m_lineNumberArea->update();
}

void CodeEditor::setDiffLines(const QSet<int> &added, const QSet<int> &removed, const QSet<int> &modified) {
	m_diffAdded = added;
	m_diffRemoved = removed;
	m_diffModified = modified;
	updateLineSelections();
	updateExtraSelections();
	m_lineNumberArea->update();
}

void CodeEditor::clearDiff() {
	m_diffAdded.clear();
	m_diffRemoved.clear();
	m_diffModified.clear();
	updateLineSelections();
	updateExtraSelections();
	m_lineNumberArea->update();
}

void CodeEditor::updateLineSelections() {
	m_cachedLineSelections.clear();
	QTextBlock block = document()->begin();
	while (block.isValid()) {
		int lineNum = block.blockNumber() + 1;
		QColor bg;
		if (m_excludedLines.contains(lineNum)) {
			bg = QColor(180, 140, 25, 45);
		} else if (m_diffAdded.contains(lineNum)) {
			bg = QColor(35, 105, 55, 60);
		} else if (m_diffRemoved.contains(lineNum)) {
			bg = QColor(130, 45, 45, 60);
		} else if (m_diffModified.contains(lineNum)) {
			bg = QColor(40, 75, 130, 60);
		}
		if (bg.isValid()) {
			QTextEdit::ExtraSelection sel;
			sel.format.setBackground(bg);
			sel.format.setProperty(QTextFormat::FullWidthSelection, true);
			sel.cursor = QTextCursor(block);
			sel.cursor.clearSelection();
			m_cachedLineSelections.append(sel);
		}
		block = block.next();
	}
}

void CodeEditor::onCursorPositionChanged() {
	updateExtraSelections();
}

void CodeEditor::updateExtraSelections() {
	QList<QTextEdit::ExtraSelection> selections = m_cachedLineSelections;
	if (!isReadOnly()) {
		QTextEdit::ExtraSelection curLine;
		curLine.format.setBackground(QColor(40, 44, 52, 90));
		curLine.format.setProperty(QTextFormat::FullWidthSelection, true);
		curLine.cursor = textCursor();
		curLine.cursor.clearSelection();
		selections.append(curLine);
	}
	matchParentheses(selections);
	setExtraSelections(selections);
}

void CodeEditor::matchParentheses(QList<QTextEdit::ExtraSelection> &selections) {
	QTextCursor cur = textCursor();
	int pos = cur.position();
	QString docText = toPlainText();
	if (docText.isEmpty()) return;
	auto checkPair = [&](int openPos, char o, char c, bool forward) {
		int depth = 1;
		int i = forward ? openPos + 1 : openPos - 1;
		while (forward ? (i < docText.size()) : (i >= 0)) {
			if (docText[i] == o) {
				++depth;
			} else if (docText[i] == c) {
				--depth;
				if (depth == 0) {
					QTextEdit::ExtraSelection s1, s2;
					s1.format.setBackground(QColor(80, 140, 220, 140));
					s1.format.setForeground(QColor(255, 255, 255));
					s2.format.setBackground(QColor(80, 140, 220, 140));
					s2.format.setForeground(QColor(255, 255, 255));
					s1.cursor = textCursor();
					s1.cursor.setPosition(openPos);
					s1.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
					s2.cursor = textCursor();
					s2.cursor.setPosition(i);
					s2.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
					selections.append(s1);
					selections.append(s2);
					return;
				}
			}
			i += forward ? 1 : -1;
		}
	};
	if (pos < docText.size()) {
		char ch = docText[pos].toLatin1();
		if (ch == '{') checkPair(pos, '{', '}', true);
		else if (ch == '(') checkPair(pos, '(', ')', true);
		else if (ch == '[') checkPair(pos, '[', ']', true);
	}
	if (pos > 0) {
		char prev = docText[pos - 1].toLatin1();
		if (prev == '}') checkPair(pos - 1, '}', '{', false);
		else if (prev == ')') checkPair(pos - 1, ')', '(', false);
		else if (prev == ']') checkPair(pos - 1, ']', '[', false);
	}
}

CodeEditorView::CodeEditorView(QWidget *parent)
	: QDockWidget(tr("Edytor Kodu"), parent)
{
	setObjectName(QStringLiteral("code_editor"));
	setAllowedAreas(Qt::AllDockWidgetAreas);
	QWidget *container = new QWidget(this);
	m_splitter = new QSplitter(Qt::Horizontal, container);
	m_leftEditor = new CodeEditor(m_splitter);
	m_rightEditor = new CodeEditor(m_splitter);
	m_rightEditor->setReadOnly(true);
	m_splitter->addWidget(m_leftEditor);
	m_splitter->addWidget(m_rightEditor);
	m_splitter->setSizes({600, 600});
	m_toolBar = new QToolBar(this);
	m_actionTransform = m_toolBar->addAction(tr("Uruchom"), this, &CodeEditorView::triggerTransform);
	m_actionUndo = m_toolBar->addAction(tr("Cofnij"), m_leftEditor, &CodeEditor::undo);
	m_actionRedo = m_toolBar->addAction(tr("Ponów"), m_leftEditor, &CodeEditor::redo);
	m_toolBar->addSeparator();
	m_actionToggleOrientation = m_toolBar->addAction(tr("Orientacja"), this, &CodeEditorView::toggleOrientation);
	m_actionDiffOnly = m_toolBar->addAction(tr("Tylko różnice"), this, &CodeEditorView::toggleDiffOnly);
	m_actionDiffOnly->setCheckable(true);
	m_toolBar->addSeparator();
	m_actionCopy = m_toolBar->addAction(tr("Kopiuj wynik"), this, &CodeEditorView::copyTransformedCode);
	QVBoxLayout *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(m_toolBar);
	layout->addWidget(m_splitter);
	setWidget(container);
	connect(m_leftEditor->verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeEditorView::syncScrollFromLeft);
	connect(m_rightEditor->verticalScrollBar(), &QScrollBar::valueChanged, this, &CodeEditorView::syncScrollFromRight);
	connect(m_leftEditor, &CodeEditor::excludedLinesChanged, this, [this]() {
		recalculateDiff();
	});
}

QString CodeEditorView::sourceCode() const {
	return m_leftEditor->toPlainText();
}

QString CodeEditorView::transformedCode() const {
	return m_rightEditor->toPlainText();
}

void CodeEditorView::setSourceCode(const QString &code) {
	m_leftEditor->setPlainText(code);
	m_lastResult = {};
	m_leftEditor->clearDiff();
	m_rightEditor->clearDiff();
}

void CodeEditorView::setTransformedCode(const QString &code) {
	m_rightEditor->setPlainText(code);
}

std::vector<size_t> CodeEditorView::excludedLines() const {
	return m_leftEditor->excludedLines();
}

void CodeEditorView::triggerTransform() {
	emit transformRequested(sourceCode(), excludedLines());
}

void CodeEditorView::toggleOrientation() {
	m_splitter->setOrientation(m_splitter->orientation() == Qt::Horizontal ? Qt::Vertical : Qt::Horizontal);
}

void CodeEditorView::copyTransformedCode() {
	QClipboard *clipboard = QGuiApplication::clipboard();
	clipboard->setText(m_rightEditor->toPlainText());
}

void CodeEditorView::toggleDiffOnly(bool checked) {
	m_diffOnly = checked;
	recalculateDiff();
}

void CodeEditorView::syncScrollFromLeft(int value) {
	if (m_isSyncing) return;
	m_isSyncing = true;
	m_rightEditor->verticalScrollBar()->setValue(value);
	m_isSyncing = false;
}

void CodeEditorView::syncScrollFromRight(int value) {
	if (m_isSyncing) return;
	m_isSyncing = true;
	m_leftEditor->verticalScrollBar()->setValue(value);
	m_isSyncing = false;
}

void CodeEditorView::onTransformFinished(const shrink::Result &result) {
	m_lastResult = result;
	setTransformedCode(QString::fromStdString(result.output));
	recalculateDiff();
}

void CodeEditorView::recalculateDiff() {
	if (m_lastResult.output.empty() && m_rightEditor->toPlainText().isEmpty()) {
		m_leftEditor->clearDiff();
		m_rightEditor->clearDiff();
		return;
	}
	const QStringList srcLines = m_leftEditor->toPlainText().split(QLatin1Char('\n'));
	const QStringList dstLines = m_rightEditor->toPlainText().split(QLatin1Char('\n'));
	QSet<int> leftRemoved;
	QSet<int> rightAdded;
	computeLinearDiff(srcLines, dstLines, leftRemoved, rightAdded);
	m_leftEditor->setDiffLines({}, leftRemoved, {});
	m_rightEditor->setDiffLines(rightAdded, {}, {});
	if (m_diffOnly) {
		QTextBlock block = m_rightEditor->document()->begin();
		while (block.isValid()) {
			int line = block.blockNumber() + 1;
			block.setVisible(rightAdded.contains(line));
			block = block.next();
		}
		m_rightEditor->viewport()->update();
	} else {
		QTextBlock block = m_rightEditor->document()->begin();
		while (block.isValid()) {
			block.setVisible(true);
			block = block.next();
		}
		m_rightEditor->viewport()->update();
	}
}
