#include "ReportPane.h"

#include <QFontDatabase>
#include <QPalette>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>

ReportPane::ReportPane(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // report has aligned columns, wrapping would scramble them
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMaximumBlockCount(500000);
}

void ReportPane::appendStyled(const QString& text, const QColor& color, bool bold, bool italic) {
    bool follow_output = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(italic);
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    if (follow_output) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

void ReportPane::appendCommand(const QString& commandLine) {
    appendStyled("$ " + commandLine + "\n", palette().color(QPalette::Text), true, false);
}

void ReportPane::appendStdout(const QString& text) {
    appendStyled(text, palette().color(QPalette::Text), false, false);
}

void ReportPane::appendStderr(const QString& text) {
    bool is_error = text.startsWith("error:");
    appendStyled(text, is_error ? QColor(0xF0, 0x50, 0x50) : QColor(0x90, 0x90, 0xA0), false, false);
}

void ReportPane::appendFooter(const QString& text, bool isFailure) {
    appendStyled(text + "\n", isFailure ? QColor(0xF0, 0x50, 0x50) : QColor(0x90, 0x90, 0xA0), false, true);
}

void ReportPane::appendNote(const QString& text) { appendStyled(text + "\n", QColor(0x60, 0x90, 0xE0), false, false); }
