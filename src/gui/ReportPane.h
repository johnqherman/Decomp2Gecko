#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QString>

class ReportPane : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ReportPane(QWidget* parent = nullptr);

    void appendCommand(const QString& commandLine);
    void appendStdout(const QString& text);
    // lines starting w/ "error:" get highlighted
    void appendStderr(const QString& text);
    void appendFooter(const QString& text, bool isFailure);
    void appendNote(const QString& text);

private:
    void appendStyled(const QString& text, const QColor& color, bool bold, bool italic);
};
