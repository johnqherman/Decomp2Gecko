#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

#include "MainWindow.h"
#include "Decomp2Gecko/version.h"

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("Decomp2Gecko");
    QCoreApplication::setApplicationName("Decomp2Gecko-gui");
    QCoreApplication::setApplicationVersion(Decomp2Gecko::VERSION_STRING);

    QCommandLineParser parser;
    parser.setApplicationDescription("Relink a decomp source mod into Gecko codes for the vanilla DOL.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(application);

    QGuiApplication::setDesktopFileName("Decomp2Gecko");
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/icons/%1.png").arg(size));
    }
    application.setWindowIcon(icon);

    MainWindow window;
    window.show();
    return application.exec();
}
