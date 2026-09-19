#pragma once

#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QTimer>

class JobRunner;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class ReportPane;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildForm();
    void loadSettings();
    void saveSettings();
    bool busy() const;
    void setBusy(bool isBusy);
    void refreshButtons();
    void showLog(bool show);

    QString isoPath() const;
    QString decompDir() const;
    QString modName() const;
    QString suggestedModName() const;
    void updateSuggestedName();

    bool preflight();
    bool ensureVanillaDol();
    void validateIso();
    void invalidateOutput();

    void onGenerate();
    bool buildRunning() const { return buildProcess_.state() != QProcess::NotRunning; }
    void startBuild();
    void stopBuild();
    void onBuildFinished(int exitCode, bool crashed);
    void runGenerate();
    void onSave();
    void onInstall();

    QWidget* viewsHeader_ = nullptr;
    QLabel* viewsTitle_ = nullptr;
    QLineEdit* isoEdit_ = nullptr;
    QLabel* isoStatus_ = nullptr;
    QLineEdit* decompEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* generateButton_ = nullptr;
    QPushButton* regenButton_ = nullptr;
    QStackedWidget* views_ = nullptr;
    QPlainTextEdit* codeView_ = nullptr;
    ReportPane* log_ = nullptr;
    QPushButton* logButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* installButton_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    JobRunner* jobs_ = nullptr;
    QProcess buildProcess_;
    QTimer buildKillTimer_; // kills ninja if it ignores the polite stop

    bool nameEditedByUser_ = false;
    bool generatePending_ = false; // build is running & generate follows it
    bool logHasContent_ = false;
    QString iniText_;
    QString reportText_;
    bool lastHadSlippiConflicts_ = false;
    QString lastInstallTarget_;
};
