#include "app/CompanionApplication.h"
#include "update/UpdateBuildConfiguration.h"

#include <QCoreApplication>
#include <QGuiApplication>

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DaSilverFire"));
    QCoreApplication::setApplicationName(QStringLiteral("Codex Companion"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(
            COMPANION_WINDOWS_VERSION));
    application.setQuitOnLastWindowClosed(false);

    companion::CompanionApplication::
        configureStandardPathsForLaunch();
    companion::CompanionApplication companionApp(application);
    const auto started = companionApp.start();
    if (!started.hasValue()) {
        return started.error().code == QStringLiteral("app.already-running")
            ? 0
            : 1;
    }
    return application.exec();
}
