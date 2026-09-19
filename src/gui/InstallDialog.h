#pragma once

#include <QDialog>
#include <QString>

#include "Decomp2Gecko/userini.h"

class QLabel;
class QLineEdit;

class InstallDialog : public QDialog {
    Q_OBJECT

public:
    InstallDialog(const Decomp2Gecko::userini::GeneratedCode& code, const QString& sourceDescription,
        const QString& defaultTarget, bool lastGenHadConflicts, QWidget* parent = nullptr);

    QString targetPath() const;

private:
    void refreshSummary();

    Decomp2Gecko::userini::GeneratedCode code_;
    QString sourceIni_;
    bool lastGenHadConflicts_;
    QLineEdit* targetEdit_ = nullptr;
    QLabel* summary_ = nullptr;
};
