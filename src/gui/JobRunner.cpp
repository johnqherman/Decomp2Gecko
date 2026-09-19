#include "JobRunner.h"

#include <QThread>
#include <exception>

JobRunner::JobRunner(QObject* parent) : QObject(parent) {}

JobRunner::~JobRunner() {
    if (thread_ != nullptr) {
        thread_->wait();
    }
}

void JobRunner::start(const QString& title, Job job) {
    Q_ASSERT(!isRunning());
    thread_ = QThread::create([this, title, job = std::move(job)] {
        Decomp2Gecko::LogFn log = [this](const std::string& line) { emit logLine(QString::fromStdString(line)); };
        try {
            job(log);
            emit finished(title);
        } catch (const std::exception& error) {
            emit failed(title, QString::fromUtf8(error.what()));
        } catch (...) {
            emit failed(title, QStringLiteral("unknown error"));
        }
    });
    connect(thread_, &QThread::finished, this, [this] {
        thread_->deleteLater();
        thread_ = nullptr;
        emit idle();
    });
    emit started(title);
    thread_->start();
}
