#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QStringList>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <filesystem>
#include <memory>

#include "InstallDialog.h"
#include "JobRunner.h"
#include "ReportPane.h"
#include "Decomp2Gecko/dol_extract.h"
#include "Decomp2Gecko/gameconfig.h"
#include "Decomp2Gecko/relink.h"
#include "Decomp2Gecko/slippi.h"
#include "Decomp2Gecko/verify.h"
#include "Decomp2Gecko/userini.h"
#include "Decomp2Gecko/util.h"
#include "Decomp2Gecko/version.h"

namespace {
QString tip(const QStringList& beats) {
    QString html;
    for (const QString& beat : beats) {
        html += "<p style='margin-bottom: 6px'>" + beat.toHtmlEscaped() + "</p>";
    }
    return html;
}

QString game_id() { return QString::fromStdString(Decomp2Gecko::melee_game_config().id); }

QWidget* path_row(QLineEdit* edit, QWidget* parent, std::function<void()> browse) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit, 1);
    auto* button = new QPushButton("Select", row);
    button->setObjectName("selectBtn");
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, row, [browse = std::move(browse)] { browse(); });
    return row;
}

void add_section(QVBoxLayout* into, const QString& title, const QString& description, QWidget* content) {
    auto* titleLabel = new QLabel(title, into->parentWidget());
    titleLabel->setObjectName("sectionTitle");
    into->addWidget(titleLabel);
    if (!description.isEmpty()) {
        auto* descLabel = new QLabel(description, into->parentWidget());
        descLabel->setObjectName("sectionDesc");
        into->addWidget(descLabel);
    }
    into->addWidget(content);
    into->addSpacing(16);
}

// worktrees keep a "gitdir: ..." pointer in .git instead of a directory
QString git_branch(const QString& directory) {
    QString gitPath = directory + "/.git";
    QFileInfo gitInfo(gitPath);
    QString headPath;
    if (gitInfo.isDir()) {
        headPath = gitPath + "/HEAD";
    } else if (gitInfo.isFile()) {
        QFile pointer(gitPath);
        if (!pointer.open(QIODevice::ReadOnly)) {
            return QString();
        }
        QString line = QString::fromUtf8(pointer.readLine()).trimmed();
        if (!line.startsWith("gitdir: ")) {
            return QString();
        }
        QString gitDir = line.mid(8);
        headPath = (QDir::isAbsolutePath(gitDir) ? gitDir : directory + "/" + gitDir) + "/HEAD";
    } else {
        return QString();
    }
    QFile head(headPath);
    if (!head.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QString reference = QString::fromUtf8(head.readLine()).trimmed();
    const QString prefix = "ref: refs/heads/";
    return reference.startsWith(prefix) ? reference.mid(prefix.size()) : QString();
}

// filled on the worker thread, read on the GUI thread
struct GenerateRun {
    Decomp2Gecko::GenerateResult generated;
    Decomp2Gecko::VerifyResult verified;
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Decomp2Gecko");
    jobs_ = new JobRunner(this);
    buildForm();
    loadSettings();

    connect(jobs_, &JobRunner::started, this, [this] { setBusy(true); });
    connect(jobs_, &JobRunner::logLine, this, [this](const QString& line) { log_->appendStderr(line + "\n"); });
    connect(jobs_, &JobRunner::idle, this, [this] { setBusy(false); });
    connect(jobs_, &JobRunner::failed, this, [this](const QString&, const QString& message) {
        log_->appendStderr("error: " + message + "\n");
        QMessageBox::critical(this, "Generate", message);
    });

    buildProcess_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&buildProcess_, &QProcess::readyReadStandardOutput, this,
        [this] { log_->appendStdout(QString::fromUtf8(buildProcess_.readAllStandardOutput())); });
    connect(&buildProcess_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        buildKillTimer_.stop();
        onBuildFinished(exitCode, status == QProcess::CrashExit);
    });
    connect(&buildProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        generatePending_ = false;

        QString message =
            QStringLiteral("could not start '%1': ninja is not installed or not on PATH").arg(buildProcess_.program());
        log_->appendStderr("error: " + message + "\n");
        QMessageBox::critical(this, "Build", message);
        setBusy(false);
    });

    buildKillTimer_.setSingleShot(true);
    buildKillTimer_.setInterval(3000);
    connect(&buildKillTimer_, &QTimer::timeout, this, [this] {
        if (buildRunning()) {
            buildProcess_.kill();
        }
    });

    refreshButtons();
}

void MainWindow::buildForm() {
    qApp->setFont(QFont({"Rubik", "Noto Sans", "Segoe UI", ".AppleSystemUIFont", "Cantarell", "sans-serif"}, 12));
    qApp->setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #21132c; color: #E8E6EA; }
        QLabel { background: transparent; }
        QLabel#sectionTitle { font-weight: bold; font-size: 16px; color: #E8E6EA; }
        QLabel#sectionDesc { font-size: 13px; color: #9B8FA2; margin-bottom: 4px; }
        QLineEdit {
            background: #2C1F37; color: #E8E6EA; border: 1px solid #3D2E4A;
            border-radius: 6px; padding: 10px 14px; font-size: 14px;
        }
        QLineEdit:focus { border-color: #72D07C; }
        QLineEdit:read-only { color: #C0BBC4; }
        QPushButton {
            background: #2C1F37; color: #E8E6EA; border: none;
            border-radius: 6px; padding: 10px 20px; font-size: 14px;
        }
        QPushButton:hover { background: #241835; }
        QPushButton:pressed { background: #1C1028; }
        QPushButton:disabled { background: #21132c; color: #4F3F5C; }
        QPushButton#selectBtn {
            background: #b883ba; color: #21132c; font-weight: bold;
        }
        QPushButton#selectBtn:hover { background: #a570a7; }
        QPushButton#selectBtn:pressed { background: #925e94; }
        QPushButton#selectBtn:disabled { background: #3D2E4A; color: #6B5F73; }
        QPushButton#accentBtn {
            background: #21BA45; color: #21132c; font-weight: bold;
        }
        QPushButton#accentBtn:hover { background: #1AA038; }
        QPushButton#accentBtn:pressed { background: #14882E; }
        QPushButton#accentBtn:disabled { background: #21132c; color: #4F3F5C; }
        QPushButton:checked { background: #3D2E4A; }
        QPlainTextEdit {
            background: #1A1221; color: #E8E6EA; border: none;
            padding: 8px; selection-background-color: #3D2E4A;
        }
        QProgressBar {
            background: #1A1221; border: 1px solid #3D2E4A; border-radius: 4px;
            text-align: center; color: #E8E6EA;
        }
        QProgressBar::chunk { background: #21BA45; border-radius: 3px; }
        QStatusBar { background: #1A1221; color: #9B8FA2; }
        QSizeGrip { width: 0; height: 0; background: transparent; }
        QStackedWidget { background: transparent; }
        QToolTip { background: #1A1221; color: #E8E6EA; border: 1px solid #3D2E4A; padding: 6px; }
        QMessageBox { background: #21132c; }
        QMessageBox QLabel { color: #E8E6EA; background: transparent; }
        QMessageBox QPushButton { min-width: 80px; }
        QWidget#isoInputBox {
            background: #2C1F37; border: 1px solid #3D2E4A;
            border-radius: 6px;
        }
        QLineEdit#isoEditInner {
            background: transparent; border: none; border-radius: 0;
        }
        QLineEdit#isoEditInner:focus { border: none; }
        QLabel#isoStatusInline {
            background: transparent; font-size: 13px; font-weight: bold;
            padding: 0; border: none;
        }
        QScrollBar:vertical {
            background: transparent; width: 10px;
            border: none; margin: 0; padding: 2px;
        }
        QScrollBar::groove:vertical { background: transparent; }
        QScrollBar::handle:vertical {
            background: rgba(180, 160, 190, 90); border-radius: 3px; min-height: 40px;
        }
        QScrollBar::handle:vertical:hover { background: rgba(180, 160, 190, 140); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: none; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
        QScrollBar:horizontal {
            background: transparent; height: 10px;
            border: none; margin: 0; padding: 2px;
        }
        QScrollBar::groove:horizontal { background: transparent; }
        QScrollBar::handle:horizontal {
            background: rgba(180, 160, 190, 90); border-radius: 3px; min-width: 40px;
        }
        QScrollBar::handle:horizontal:hover { background: rgba(180, 160, 190, 140); }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; background: none; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
    )"));

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(32, 28, 32, 16);
    layout->setSpacing(6);

    // --- vanilla ISO ---
    isoEdit_ = new QLineEdit(central);
    isoEdit_->setReadOnly(true);
    isoEdit_->setObjectName("isoEditInner");
    isoStatus_ = new QLabel(central);
    isoStatus_->setObjectName("isoStatusInline");
    isoStatus_->hide();
    {
        auto* row = new QWidget(central);
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);

        auto* inputBox = new QWidget(row);
        inputBox->setObjectName("isoInputBox");
        auto* boxLayout = new QHBoxLayout(inputBox);
        boxLayout->setContentsMargins(0, 0, 8, 0);
        boxLayout->setSpacing(6);
        boxLayout->addWidget(isoEdit_, 1);
        boxLayout->addWidget(isoStatus_);

        hl->addWidget(inputBox, 1);
        auto* selectBtn = new QPushButton("Select", row);
        selectBtn->setObjectName("selectBtn");
        hl->addWidget(selectBtn);
        connect(selectBtn, &QPushButton::clicked, this, [this] {
            QString chosen = QFileDialog::getOpenFileName(this, "Vanilla ISO",
                isoEdit_->text().isEmpty() ? QDir::homePath() : isoEdit_->text(),
                "GameCube images (*.iso *.gcm);;All files (*)");
            if (!chosen.isEmpty()) {
                isoEdit_->setText(chosen);
                isoEdit_->setCursorPosition(0);
                validateIso();
            }
        });
        add_section(layout, "Melee ISO File",
            "The path to an unmodified NTSC Melee 1.02 ISO.", row);
    }

    // --- modded decomp ---
    decompEdit_ = new QLineEdit(central);
    decompEdit_->setPlaceholderText("the melee decomp checkout with your changes");
    {
        QPalette pal = decompEdit_->palette();
        pal.setColor(QPalette::PlaceholderText, QColor(0x6B, 0x5F, 0x73));
        decompEdit_->setPalette(pal);
    }
    {
        auto* row = path_row(decompEdit_, central, [this] {
            QString chosen = QFileDialog::getExistingDirectory(this, "Modded decomp", decompEdit_->text());
            if (!chosen.isEmpty()) {
                decompEdit_->setText(chosen);
            }
        });
        add_section(layout, "Modified Decomp",
            "The directory containing your modified Melee decompilation.", row);
    }
    connect(decompEdit_, &QLineEdit::textChanged, this, [this] { updateSuggestedName(); });
    connect(decompEdit_, &QLineEdit::textChanged, this, &MainWindow::invalidateOutput);
    connect(isoEdit_, &QLineEdit::textChanged, this, &MainWindow::invalidateOutput);

    // --- mod name ---
    nameEdit_ = new QLineEdit(central);
    add_section(layout, "Gecko Code Name",
        "The name to show in Dolphin's Gecko code list.", nameEdit_);
    connect(nameEdit_, &QLineEdit::textEdited, this, [this] {
        nameEditedByUser_ = !nameEdit_->text().trimmed().isEmpty();
        invalidateOutput();
    });

    generateButton_ = new QPushButton("Generate", central);
    generateButton_->setObjectName("accentBtn");
    generateButton_->setDefault(true);
    generateButton_->setMinimumHeight(50);
    generateButton_->setStyleSheet("QPushButton { font-size: 16px; margin: 0 0 16px 0; }");
    connect(generateButton_, &QPushButton::clicked, this, &MainWindow::onGenerate);
    layout->addWidget(generateButton_);

    viewsHeader_ = new QWidget(central);
    viewsHeader_->setFixedHeight(36);
    viewsHeader_->hide();
    auto* viewsHeaderLayout = new QHBoxLayout(viewsHeader_);
    viewsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    viewsHeaderLayout->setSpacing(8);
    viewsTitle_ = new QLabel("Gecko Code", viewsHeader_);
    viewsTitle_->setObjectName("sectionTitle");
    viewsHeaderLayout->addWidget(viewsTitle_);
    viewsHeaderLayout->addStretch();
    regenButton_ = new QPushButton("Regenerate", viewsHeader_);
    regenButton_->setObjectName("selectBtn");
    regenButton_->hide();
    connect(regenButton_, &QPushButton::clicked, this, &MainWindow::onGenerate);
    viewsHeaderLayout->addWidget(regenButton_);
    layout->addWidget(viewsHeader_);

    views_ = new QStackedWidget(central);
    codeView_ = new QPlainTextEdit(views_);
    codeView_->setReadOnly(true);
    codeView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    codeView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    codeView_->setPlaceholderText("Your Gecko code will appear here.");
    log_ = new ReportPane(views_);
    views_->addWidget(codeView_);
    views_->addWidget(log_);
    layout->addWidget(views_, 1);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    logButton_ = new QPushButton("Show Log", central);
    logButton_->setObjectName("selectBtn");
    logButton_->setCheckable(true);
    logButton_->setToolTip(
        tip({"The build output and the relink report.", "What changed, where it went, and the Slippi cross-check."}));
    saveButton_ = new QPushButton("Save .ini...", central);
    saveButton_->setObjectName("selectBtn");
    installButton_ = new QPushButton("Install into Slippi...", central);
    installButton_->setObjectName("selectBtn");
    installButton_->setToolTip(
        tip({"Append the codes to Dolphin's user ini.", "The existing file is backed up first."}));

    actions->addWidget(logButton_);
    actions->addStretch();
    actions->addWidget(saveButton_);
    actions->addWidget(installButton_);
    layout->addLayout(actions);

    connect(logButton_, &QPushButton::clicked, this, [this] {
        if (buildRunning()) {
            stopBuild();
        } else {
            showLog(logButton_->isChecked());
        }
    });
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::onSave);
    connect(installButton_, &QPushButton::clicked, this, &MainWindow::onInstall);

    setCentralWidget(central);

    statusBar()->setContentsMargins(32, 6, 32, 6);

    auto* versionLabel = new QLabel(
        QStringLiteral("Version %1 (%2)").arg(Decomp2Gecko::VERSION_STRING, Decomp2Gecko::GIT_HASH),
        this);
    versionLabel->setStyleSheet("QLabel { font-size: 11px; color: #6B5F73; }");
    statusBar()->addWidget(versionLabel);

    resultLabel_ = new QLabel(this);
    resultLabel_->setStyleSheet("QLabel { font-size: 11px; color: #9B8FA2; }");
    statusBar()->addPermanentWidget(resultLabel_);
    resize(900, 640);
}

void MainWindow::loadSettings() {
    QSettings settings;
    isoEdit_->setText(settings.value("paths/iso").toString());
    isoEdit_->setCursorPosition(0);
    decompEdit_->setText(settings.value("paths/decomp").toString());

    QString savedName = settings.value("mod/name").toString();
    if (!savedName.isEmpty()) {
        nameEdit_->setText(savedName);
        nameEditedByUser_ = true;
    } else {
        updateSuggestedName();
    }

    lastInstallTarget_ = settings.value("install/lastTarget").toString();
    restoreGeometry(settings.value("window/geometry").toByteArray());
}

void MainWindow::saveSettings() {
    QSettings settings;

    settings.setValue("paths/iso", isoEdit_->text());
    settings.setValue("paths/decomp", decompEdit_->text());
    settings.setValue("mod/name", nameEditedByUser_ ? nameEdit_->text() : QString());
    settings.setValue("install/lastTarget", lastInstallTarget_);
    settings.setValue("window/geometry", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (jobs_->isRunning()) {
        resultLabel_->setText("wait for the running job to finish before closing");
        event->ignore();
        return;
    }
    if (buildRunning()) {
        stopBuild();
    }
    saveSettings();
    event->accept();
}

bool MainWindow::busy() const { return jobs_->isRunning() || buildRunning(); }

void MainWindow::setBusy(bool isBusy) {
    for (QLineEdit* field : {isoEdit_, decompEdit_, nameEdit_}) {
        field->setEnabled(!isBusy);
    }
    for (auto* btn : centralWidget()->findChildren<QPushButton*>("selectBtn")) {
        btn->setEnabled(!isBusy);
    }
    generateButton_->setVisible(false);
    if (buildRunning()) {
        logButton_->setText("Stop Build");
        logButton_->setVisible(true);
        logButton_->setEnabled(true);
        logButton_->setCheckable(false);
    } else if (isBusy) {
        logButton_->setText("Show Log");
        logButton_->setCheckable(true);
        logButton_->setVisible(false);
    } else {
        generateButton_->setEnabled(true);
        bool logShown = views_->currentWidget() == log_;
        logButton_->setText(logShown ? "Show Code" : "Show Log");
        logButton_->setCheckable(true);
        logButton_->setChecked(logShown);
    }

    refreshButtons();
}

void MainWindow::refreshButtons() {
    bool haveCodes = !iniText_.isEmpty() && !busy();
    generateButton_->setVisible(!haveCodes && !busy());
    regenButton_->setVisible(haveCodes);
    logButton_->setVisible(haveCodes || logHasContent_ || buildRunning());
    saveButton_->setEnabled(haveCodes);
    installButton_->setEnabled(haveCodes);
}

void MainWindow::validateIso() {
    auto setStatus = [this](const QString& text, const QString& color) {
        isoStatus_->setText(text);
        isoStatus_->setStyleSheet(QStringLiteral("QLabel#isoStatusInline { color: %1; }").arg(color));
        isoStatus_->show();
    };

    QString path = isoPath();
    if (path.isEmpty()) {
        isoStatus_->hide();
        return;
    }
    if (!QFileInfo::exists(path)) {
        setStatus("Not found", "#888");
        return;
    }

    namespace dolx = Decomp2Gecko::dolx;
    dolx::IsoInfo info;
    try {
        info = dolx::probe_iso(path.toStdString());
    } catch (const std::exception&) {
        setStatus("Invalid", "#E04040");
        return;
    }

    if (QString::fromStdString(info.game_id) != game_id()) {
        setStatus("Wrong game", "#E04040");
        return;
    }

    if (info.disc_number != 0 || info.version != 2) {
        setStatus("Wrong version (need NTSC 1.02)", "#E04040");
        return;
    }

    setStatus("NTSC 1.02 ✔", "#21BA45");
}

void MainWindow::invalidateOutput() {
    iniText_.clear();
    reportText_.clear();
    lastHadSlippiConflicts_ = false;
    logHasContent_ = false;
    codeView_->setPlainText(QString());
    viewsHeader_->hide();
    refreshButtons();
}

void MainWindow::showLog(bool show) {
    views_->setCurrentWidget(show ? static_cast<QWidget*>(log_) : static_cast<QWidget*>(codeView_));
    viewsTitle_->setText(show ? "Build Log" : "Gecko Code");
    viewsHeader_->show();
    logButton_->setText(show ? "Show Code" : "Show Log");
    if (logButton_->isChecked() != show) {
        logButton_->setChecked(show);
    }
}

QString MainWindow::isoPath() const {
    return QString::fromStdString(Decomp2Gecko::expanduser(isoEdit_->text().trimmed().toStdString()));
}

QString MainWindow::decompDir() const {
    return QString::fromStdString(Decomp2Gecko::expanduser(decompEdit_->text().trimmed().toStdString()));
}

QString MainWindow::modName() const { return nameEdit_->text().trimmed(); }

QString MainWindow::suggestedModName() const {
    QString branch = git_branch(decompDir());
    if (!branch.isEmpty() && branch != "master" && branch != "main") {
        return branch;
    }
    QString folder = QFileInfo(decompDir()).fileName();
    return folder.isEmpty() ? QStringLiteral("decomp mod") : folder;
}

void MainWindow::updateSuggestedName() {
    if (!nameEditedByUser_) {
        nameEdit_->setText(suggestedModName());
    }
}

bool MainWindow::preflight() {
    if (busy()) {
        return false;
    }

    QDir decomp(decompDir());
    if (decompEdit_->text().trimmed().isEmpty() || !decomp.exists()) {
        QMessageBox::warning(this, "Modded decomp", decompDir() + " does not exist.");
        decompEdit_->setFocus();
        return false;
    }

    QString id = game_id();
    if (!decomp.exists("config/" + id + "/symbols.txt") || !decomp.exists("config/" + id + "/splits.txt")) {
        QMessageBox::warning(this, "Modded decomp",
            decompDir() + " is not a melee decomp checkout (config/" + id + "/symbols.txt missing).");
        return false;
    }

    if (modName().isEmpty()) {
        nameEditedByUser_ = false;
        updateSuggestedName();
    }

    if (!ensureVanillaDol()) {
        return false;
    }
    saveSettings();
    return true;
}

bool MainWindow::ensureVanillaDol() {
    namespace dolx = Decomp2Gecko::dolx;
    QString id = game_id();
    QString target = QDir(decompDir()).filePath("orig/" + id + "/sys/main.dol");
    if (QFileInfo::exists(target)) {
        std::optional<std::string> expected =
            dolx::expected_dol_sha1(QDir(decompDir()).filePath("config/" + id + "/build.sha1").toStdString());
        if (expected) {
            QFile dolFile(target);
            if (dolFile.open(QIODevice::ReadOnly)) {
                QString actualSha1 =
                    QString::fromLatin1(QCryptographicHash::hash(dolFile.readAll(), QCryptographicHash::Sha1).toHex());
                if (*expected != actualSha1.toStdString()) {
                    QMessageBox box(QMessageBox::Warning, "Not the vanilla DOL",
                        QStringLiteral("The existing %1 has SHA-1 %2 but the decomp expects %3. "
                                       "This may be a modded DOL; a modded baseline makes every diff wrong."
                                       "\n\nUse it anyway?")
                            .arg(target, actualSha1, QString::fromStdString(*expected)),
                        QMessageBox::NoButton, this);
                    QPushButton* useAnyway = box.addButton("Use anyway", QMessageBox::AcceptRole);
                    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
                    box.setDefaultButton(cancel);
                    box.exec();
                    if (box.clickedButton() != useAnyway) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    if (isoEdit_->text().trimmed().isEmpty() || !QFileInfo::exists(isoPath())) {
        QMessageBox::warning(this, "Vanilla ISO",
            "The decomp has no " + target + " yet. Pick your vanilla NTSC 1.02 ISO so it can be extracted.");
        isoEdit_->setFocus();
        return false;
    }

    dolx::IsoInfo info;
    Decomp2Gecko::Bytes dol;
    try {
        info = dolx::probe_iso(isoPath().toStdString());
        if (QString::fromStdString(info.game_id) != id) {
            QMessageBox::critical(this, "Wrong game",
                QStringLiteral("The ISO's game ID is '%1' but this tool targets %2.")
                    .arg(QString::fromStdString(info.game_id), id));
            return false;
        }
        if (info.disc_number != 0 || info.version != 2) {
            QMessageBox::critical(this, "Wrong version",
                QStringLiteral("The ISO is %1 disc %2 version %3, but this tool requires NTSC 1.02 (disc 0, version 2).")
                    .arg(QString::fromStdString(info.game_id))
                    .arg(info.disc_number)
                    .arg(info.version));
            return false;
        }
        dol = dolx::read_dol(isoPath().toStdString(), info);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Vanilla ISO", QString::fromUtf8(error.what()));
        return false;
    }

    QString actualSha1 = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(dol.data(), int(dol.size())), QCryptographicHash::Sha1).toHex());
    std::optional<std::string> expected =
        dolx::expected_dol_sha1(QDir(decompDir()).filePath("config/" + id + "/build.sha1").toStdString());

    if (expected && *expected != actualSha1.toStdString()) {
        QMessageBox box(QMessageBox::Warning, "Not the vanilla DOL",
            QStringLiteral("The main.dol inside %1 has SHA-1 %2 but the decomp expects %3. This is probably a "
                           "modded ISO; a modded DOL makes every diff wrong.\n\nUse it anyway?")
                .arg(isoPath(), actualSha1, QString::fromStdString(*expected)),
            QMessageBox::NoButton, this);
        QPushButton* useAnyway = box.addButton("Use anyway", QMessageBox::AcceptRole);
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != useAnyway) {
            return false;
        }
    }

    try {
        dolx::write_file_atomically(target.toStdString(), dol);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Cannot write main.dol", QString::fromUtf8(error.what()));
        return false;
    }

    log_->appendNote(QStringLiteral("extracted main.dol (%1 bytes, sha1 %2) from %3 to %4")
            .arg(QLocale().toString(qlonglong(dol.size())), actualSha1, isoPath(), target));
    return true;
}

// w/o a build.ninja the build is skipped & the core uses whatever objects are there
void MainWindow::onGenerate() {
    if (buildRunning()) {
        stopBuild();
        return;
    }

    if (!preflight()) {
        return;
    }
    log_->clear();
    invalidateOutput();
    logHasContent_ = true;
    showLog(true);
    generatePending_ = true;

    if (QDir(decompDir()).exists("build.ninja")) {
        startBuild();
    } else {
        log_->appendNote("no build.ninja in the decomp; using the existing objects");
        runGenerate();
    }
}

void MainWindow::startBuild() {
    QString target = QStringLiteral("build/%1/main.elf").arg(game_id());
    buildProcess_.setWorkingDirectory(decompDir());
    buildProcess_.setProgram(QStringLiteral("ninja"));
    buildProcess_.setArguments({target});
    log_->appendCommand(buildProcess_.program() + " " + target);
    buildProcess_.start();
    if (buildRunning()) {
        setBusy(true);
    }
}

void MainWindow::stopBuild() {
    if (!buildRunning()) {
        return;
    }
    buildProcess_.terminate();
    buildKillTimer_.start();
}

void MainWindow::onBuildFinished(int exitCode, bool crashed) {
    bool failed = crashed || exitCode != 0;
    QString footer = QStringLiteral("----- ninja exited with code %1 -----").arg(exitCode);
    if (crashed) {
        footer = QStringLiteral("----- ninja stopped -----");
    }
    log_->appendFooter(footer, failed);

    bool pending = generatePending_;
    generatePending_ = false;
    if (failed || !pending) {
        setBusy(false);
        resultLabel_->setText(crashed ? "build stopped" : "build failed; see the log");
        return;
    }

    runGenerate();
}

void MainWindow::runGenerate() {
    generatePending_ = false;
    std::string decomp_root = decompDir().toStdString();
    Decomp2Gecko::GenerateOptions options;
    options.name = modName().toStdString();
    log_->appendCommand("generate \"" + modName() + "\"");
    auto run = std::make_shared<GenerateRun>();

    auto onGenerated = [this, run](const QString&) {
        using Status = Decomp2Gecko::GenerateResult::Status;
        const Decomp2Gecko::GenerateResult& generated = run->generated;
        for (const std::string& line : generated.report) {
            log_->appendStdout(QString::fromStdString(line) + "\n");
        }

        if (generated.status != Status::Ok) {
            QString reason;
            switch (generated.status) {
                case Status::LayoutErrors: reason = "the layout has errors (see the ERROR lines)"; break;
                case Status::ReservedConflicts:
                    for (const std::string& conflict : generated.reserved_conflicts) {
                        log_->appendStderr("error: " + QString::fromStdString(conflict) + "\n");
                    }
                    reason = "codes would write into a reserved range";
                    break;
                case Status::SimulationMismatch:
                    reason = QStringLiteral("simulation mismatch in %1 chunks, e.g. %2")
                                 .arg(generated.mismatched_chunks)
                                 .arg(QString::fromStdString(generated.mismatch_example));
                    QMessageBox::critical(this, "Simulation mismatch",
                        "The emitted codes do not reproduce the planned image. This is a Decomp2Gecko bug; "
                        "please report it together with the log text.");
                    break;
                case Status::Ok: break;
            }

            log_->appendFooter("----- generate failed: " + reason + " -----", true);
            resultLabel_->setText("generate failed; see the log");
            return;
        }

        const Decomp2Gecko::VerifyResult& verified = run->verified;
        QString verdict;
        QString brief;
        if (!verified.ran) {
            QString reason = QString::fromStdString(Decomp2Gecko::join(verified.problems, "; "));
            verdict = "not verified: " + reason;
            brief = "not verified";
        } else {
            log_->appendStdout("verification: comparing every changed function and object with the decomp's own "
                               "main.elf (bytes, relocation targets, branch targets)\n");
            for (const std::string& skipped : verified.skipped) {
                log_->appendStdout("  skipped: " + QString::fromStdString(skipped) + "\n");
            }
            for (const std::string& problem : verified.problems) {
                log_->appendStderr("  PROBLEM: " + QString::fromStdString(problem) + "\n");
            }
            int64_t differ = verified.checked - verified.ok;
            if (verified.checked == 0) {
                verdict = "nothing changed, nothing to verify";
                brief = "no changes to verify";
            } else if (differ == 0) {
                verdict = QStringLiteral("all %1 changed functions and objects match the decomp's own main.elf")
                              .arg(verified.checked);
                brief = QStringLiteral("%1/%1 match main.elf").arg(verified.checked);
            } else {
                verdict = QStringLiteral("%1 of %2 changed functions and objects differ from the decomp's own main.elf")
                              .arg(differ)
                              .arg(verified.checked);
                brief = QStringLiteral("%1/%2 match main.elf").arg(verified.checked - differ).arg(verified.checked);
            }
            if (!verified.skipped.empty()) {
                verdict += QStringLiteral(", %1 skipped (no address in main.elf)").arg(verified.skipped.size());
            }
        }
        log_->appendFooter("----- generated; " + verdict + " -----", verified.ran && !verified.problems.empty());

        if (verified.ran && !verified.problems.empty()) {
            resultLabel_->setText("codes blocked: " + brief + "; see the log");
            QMessageBox::warning(this, "Verification failed",
                QStringLiteral("%1 changed function(s) or object(s) come out different from the same code in the "
                               "decomp's own main.elf, so the codes might not do what your sources say. "
                               "They have been blocked; see the PROBLEM lines in the log.")
                    .arg(verified.problems.size()));
            return;
        }

        iniText_ = QString::fromStdString(generated.ini_text);
        reportText_ = QString::fromStdString(Decomp2Gecko::join(generated.report, "\n") + "\n");
        lastHadSlippiConflicts_ = !generated.slippi.conflicts.empty();
        codeView_->setPlainText(iniText_);
        showLog(false);
        resultLabel_->setText(
            QStringLiteral("%1 · %2 Gecko lines · %3").arg(modName()).arg(generated.codes.gecko_lines).arg(brief));

        if (lastHadSlippiConflicts_) {
            QMessageBox box(QMessageBox::Warning, "Slippi conflicts",
                QStringLiteral("These codes overwrite or relocate something Slippi hooks "
                               "(see the CONFLICT lines in the log). Installing them may break "
                               "Slippi netplay.\n\nKeep the codes anyway?"),
                QMessageBox::NoButton, this);
            box.addButton("Keep codes", QMessageBox::AcceptRole);
            QPushButton* discard = box.addButton("Discard", QMessageBox::RejectRole);
            box.setDefaultButton(discard);
            box.exec();
            if (box.clickedButton() == discard) {
                iniText_.clear();
                reportText_.clear();
                codeView_->setPlainText(QString());
                generateButton_->setText("Generate");
                showLog(true);
                refreshButtons();
                resultLabel_->setText("codes discarded due to Slippi conflicts; see the log");
                return;
            }
        }
    };
    connect(jobs_, &JobRunner::finished, this, onGenerated, Qt::SingleShotConnection);

    jobs_->start("generate", [decomp_root, options, run](const Decomp2Gecko::LogFn& log) {
        Decomp2Gecko::LayoutOptions layout_options;
        if (std::optional<std::filesystem::path> ini =
                Decomp2Gecko::find_slippi_ini(Decomp2Gecko::melee_game_config())) {
            for (const Decomp2Gecko::PatchSite& site : Decomp2Gecko::parse_ini(*ini).patch_sites()) {
                layout_options.avoid.emplace_back(site.address, site.address + site.size);
            }
        }
        Decomp2Gecko::Layout layout(decomp_root, layout_options, log);
        run->generated = Decomp2Gecko::generate(layout, options);
        if (run->generated.status == Decomp2Gecko::GenerateResult::Status::Ok) {
            run->verified = Decomp2Gecko::verify(layout);
        }
    });
}

void MainWindow::onSave() {
    QString suggested = QDir::homePath() + "/" + modName() + ".ini";
    QString chosen = QFileDialog::getSaveFileName(this, "Save Gecko codes", suggested, "Dolphin ini (*.ini)");
    if (chosen.isEmpty()) {
        return;
    }
    if (QFileInfo(chosen).suffix().isEmpty()) {
        chosen += ".ini";
    }

    try {
        Decomp2Gecko::write_file_bytes(chosen.toStdString(), iniText_.toStdString());
        Decomp2Gecko::write_file_bytes(
            std::filesystem::path(chosen.toStdString()).replace_extension(".report.txt"), reportText_.toStdString());
        resultLabel_->setText("saved " + chosen);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Save", QString::fromUtf8(error.what()));
    }
}

void MainWindow::onInstall() {
    if (busy() || iniText_.isEmpty()) {
        return;
    }

    Decomp2Gecko::userini::GeneratedCode code;
    try {
        code = Decomp2Gecko::userini::parse_generated_ini(iniText_.toStdString());
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Nothing to install", QString::fromUtf8(error.what()));
        return;
    }

    QString defaultTarget = lastInstallTarget_;
    if (defaultTarget.isEmpty()) {
        defaultTarget =
            QString::fromStdString(Decomp2Gecko::expanduser(Decomp2Gecko::melee_game_config().slippi_user_ini));
    }
    InstallDialog dialog(code, "generated just now", defaultTarget, lastHadSlippiConflicts_, this);
    if (dialog.exec() != QDialog::Accepted || dialog.targetPath().isEmpty()) {
        return;
    }

    QString target = dialog.targetPath();
    try {
        Decomp2Gecko::userini::InstallResult result = Decomp2Gecko::userini::install_code(target.toStdString(), code);
        QString backup =
            result.backup ? QString::fromStdString(result.backup->string()) : QStringLiteral("(none, new file)");
        log_->appendNote(
            QStringLiteral("installed $%1 into %2; backup: %3").arg(QString::fromStdString(code.name), target, backup));
        {
            QDialog dlg(this);
            dlg.setWindowTitle("Installed");
            dlg.setMinimumWidth(560);
            auto* dl = new QVBoxLayout(&dlg);
            dl->setContentsMargins(32, 24, 32, 24);
            dl->setSpacing(2);
            auto* t = new QLabel("Installed", &dlg);
            t->setObjectName("sectionTitle");
            dl->addWidget(t);
            auto* d = new QLabel(
                QStringLiteral("Installed into %1\nBackup: %2").arg(target, backup), &dlg);
            d->setObjectName("sectionDesc");
            d->setTextInteractionFlags(Qt::TextSelectableByMouse);
            dl->addWidget(d);
            dl->addSpacing(16);
            auto* row = new QHBoxLayout();
            row->addStretch();
            auto* ok = new QPushButton("OK", &dlg);
            ok->setObjectName("selectBtn");
            ok->setMinimumHeight(40);
            ok->setMinimumWidth(100);
            row->addWidget(ok);
            dl->addLayout(row);
            connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
            dlg.exec();
        }
        lastInstallTarget_ = target;
        saveSettings();
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Install failed", QString::fromUtf8(error.what()));
    }
}
