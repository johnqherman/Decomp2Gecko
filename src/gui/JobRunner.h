#pragma once

#include <QObject>
#include <QString>
#include <functional>

#include "Decomp2Gecko/layout.h"

class QThread;

class JobRunner : public QObject {
    Q_OBJECT

public:
    // results come back through what the job captured. exceptions arrive as failed
    using Job = std::function<void(const Decomp2Gecko::LogFn&)>;

    explicit JobRunner(QObject* parent = nullptr);
    ~JobRunner() override;

    bool isRunning() const { return thread_ != nullptr; }
    void start(const QString& title, Job job);

signals:
    void started(const QString& title);
    void logLine(const QString& line);
    void finished(const QString& title);
    void failed(const QString& title, const QString& message);
    void idle();

private:
    QThread* thread_ = nullptr;
};
