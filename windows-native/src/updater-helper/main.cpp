#include "updater-helper/UpdateInstallResult.h"
#include "updater-helper/UpdateInstallRequest.h"
#include "updater-helper/UpdateInstallTransaction.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace {

constexpr int kInvalidArgumentsExit = 2;
constexpr int kInvalidRequestExit = 3;
constexpr int kTransactionFailedExit = 4;
constexpr int kResultWriteFailedExit = 5;

int writeResult(
    QStringView transactionRoot,
    const companion::
        UpdateInstallResultRecord& record,
    int intendedExitCode)
{
    const QString resultPath =
        QDir(transactionRoot.toString())
            .filePath(
                QStringLiteral(
                    "result.json"));
    return record.write(resultPath)
                   .hasValue()
        ? intendedExitCode
        : kResultWriteFailedExit;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(
        argc,
        argv);
    QCoreApplication::setOrganizationName(
        QStringLiteral("DaSilverFire"));
    QCoreApplication::setApplicationName(
        QStringLiteral(
            "Codex Companion Updater"));

    const QStringList arguments =
        application.arguments();
    if (arguments.size() != 3
        || arguments.at(1)
            != QStringLiteral(
                "--request")) {
        return kInvalidArgumentsExit;
    }

    const QString transactionRoot =
        QCoreApplication::
            applicationDirPath();
    const QString installerLogPath =
        QDir(transactionRoot)
            .filePath(
                QStringLiteral(
                    "installer.log"));
    const auto request =
        companion::UpdateInstallRequest::
            load(arguments.at(2));
    if (!request.hasValue()) {
        companion::
            UpdateInstallResultRecord record;
        record.success = false;
        record.errorCode =
            request.error().code;
        record.message =
            request.error().message;
        record.completedAtUtc =
            QDateTime::
                currentDateTimeUtc()
                    .toString(
                        Qt::ISODateWithMs);
        record.installerLogPath =
            installerLogPath;
        record.context =
            request.error().context;
        return writeResult(
            transactionRoot,
            record,
            kInvalidRequestExit);
    }

    auto transaction =
        companion::
            UpdateInstallTransaction::
                createProduction(
                    transactionRoot);
    const auto installed =
        transaction.run(
            request.value());

    companion::
        UpdateInstallResultRecord record;
    record.requestId =
        request.value().requestId;
    record.success =
        installed.hasValue();
    record.completedAtUtc =
        QDateTime::
            currentDateTimeUtc()
                .toString(
                    Qt::ISODateWithMs);
    record.installerLogPath =
        installerLogPath;
    if (installed.hasValue()) {
        record.message =
            QStringLiteral(
                "The update installed successfully.");
    } else {
        record.errorCode =
            installed.error().code;
        record.message =
            installed.error().message;
        record.context =
            installed.error().context;
    }
    return writeResult(
        transactionRoot,
        record,
        installed.hasValue()
            ? 0
            : kTransactionFailedExit);
}
