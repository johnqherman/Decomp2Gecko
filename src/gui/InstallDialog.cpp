#include "InstallDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Decomp2Gecko/util.h"

using Decomp2Gecko::userini::GeneratedCode;
using Decomp2Gecko::userini::InstallPlan;

InstallDialog::InstallDialog(const GeneratedCode& code, const QString& sourceDescription, const QString& defaultTarget,
    bool lastGenHadConflicts, QWidget* parent)
    : QDialog(parent),
      code_(code),
      sourceIni_(sourceDescription),
      lastGenHadConflicts_(lastGenHadConflicts) {
    setWindowTitle("Install into Slippi");
    setMinimumWidth(640);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSpacing(2);

    auto* header = new QWidget(this);
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    auto* titleLabel = new QLabel("Install Gecko Code", header);
    titleLabel->setObjectName("sectionTitle");
    auto* descLabel = new QLabel("Choose the Dolphin INI file to install into.", header);
    descLabel->setObjectName("sectionDesc");
    headerLayout->addWidget(titleLabel);
    headerLayout->addWidget(descLabel);
    layout->addWidget(header);

    auto* targetRow = new QHBoxLayout();
    targetRow->setSpacing(8);
    targetEdit_ = new QLineEdit(defaultTarget);
    targetEdit_->setReadOnly(true);
    targetRow->addWidget(targetEdit_, 1);
    auto* browse = new QPushButton("Select");
    browse->setObjectName("selectBtn");
    targetRow->addWidget(browse);
    layout->addLayout(targetRow);

    layout->addSpacing(12);

    summary_ = new QLabel();
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    summary_->setStyleSheet("QLabel { background: transparent; color: #E8E6EA; }");
    layout->addWidget(summary_);

    layout->addSpacing(8);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    auto* install = new QPushButton("Back Up and Install");
    install->setObjectName("accentBtn");
    install->setDefault(true);
    install->setMinimumHeight(44);
    auto* cancel = new QPushButton("Cancel");
    cancel->setObjectName("selectBtn");
    cancel->setMinimumHeight(44);
    buttonRow->setSpacing(8);
    buttonRow->addWidget(install);
    buttonRow->addWidget(cancel);
    layout->addLayout(buttonRow);

    connect(install, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    connect(browse, &QPushButton::clicked, this, [this] {
        QString chosen = QFileDialog::getSaveFileName(this, "Dolphin user ini", targetEdit_->text(),
            "Dolphin ini (*.ini);;All files (*)", nullptr, QFileDialog::DontConfirmOverwrite);
        if (!chosen.isEmpty()) {
            targetEdit_->setText(chosen);
        }
    });

    connect(targetEdit_, &QLineEdit::textChanged, this, &InstallDialog::refreshSummary);
    refreshSummary();
}

QString InstallDialog::targetPath() const { return targetEdit_->text().trimmed(); }

void InstallDialog::refreshSummary() {
    QString target = targetPath();
    QFileInfo info(target);
    std::string existing;
    if (info.exists()) {
        try {
            existing = Decomp2Gecko::read_file_bytes(target.toStdString());
        } catch (const std::exception&) {
            existing.clear();
        }
    }

    InstallPlan plan = Decomp2Gecko::userini::plan_install(existing, code_);

    QString verb = plan.replaces_existing ? "Replace" : "Add";
    QString html = QStringLiteral("<p>%1 <b>%2</b> (%3 lines) under [Gecko].</p>")
                       .arg(verb, QString::fromStdString(code_.name).toHtmlEscaped())
                       .arg(code_.lines.size());
    if (plan.creates_enabled_section) {
        html += "<p>Create [Gecko_Enabled] and enable this code.</p>";
    } else if (!plan.already_enabled) {
        html += "<p>Enable this code in [Gecko_Enabled].</p>";
    }
    if (plan.removes_from_disabled) {
        html += "<p>Remove this code from [Gecko_Disabled].</p>";
    }
    if (info.exists()) {
        html += "<p>The existing file will be backed up first.</p>";
    }
    if (lastGenHadConflicts_) {
        html += "<p style='color: #E09030;'>The last generation reported Slippi conflicts. "
                "These codes may break Slippi's own hooks.</p>";
    }
    html += "<p style='color: #6B5F73;'>For netplay, both players need the same codes.</p>";

    summary_->setText(html);
}
