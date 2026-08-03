#include "updater-helper/UpdateInstallResult.h"

#include <utility>

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace companion {
namespace {

CompanionError resultError(
    QString code,
    QString message,
    QStringView path)
{
    QVariantMap context;
    if (!path.isEmpty()) {
        context.insert(
            QStringLiteral("path"),
            path.toString());
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

} // namespace

QByteArray UpdateInstallResultRecord::
toJson() const
{
    const QJsonObject object{
        {
            QStringLiteral("schema"),
            1,
        },
        {
            QStringLiteral("requestId"),
            requestId,
        },
        {
            QStringLiteral("success"),
            success,
        },
        {
            QStringLiteral("errorCode"),
            errorCode,
        },
        {
            QStringLiteral("message"),
            message,
        },
        {
            QStringLiteral(
                "completedAtUtc"),
            completedAtUtc,
        },
        {
            QStringLiteral(
                "installerLogPath"),
            installerLogPath,
        },
        {
            QStringLiteral("context"),
            QJsonObject::fromVariantMap(
                context),
        },
    };
    QByteArray encoded =
        QJsonDocument(object)
            .toJson(
                QJsonDocument::Compact);
    encoded.append('\n');
    return encoded;
}

Result<void> UpdateInstallResultRecord::
write(QStringView filePath) const
{
    const QString path =
        filePath.toString();
    if (path.isEmpty()
        || !QFileInfo(path).isAbsolute()) {
        return Result<void>::failure(
            resultError(
                QStringLiteral(
                    "update.result_path_invalid"),
                QStringLiteral(
                    "The updater result path is invalid."),
                filePath));
    }

    QSaveFile file(path);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return Result<void>::failure(
            resultError(
                QStringLiteral(
                    "update.result_open_failed"),
                QStringLiteral(
                    "The updater result file could not be opened."),
                filePath));
    }

    const QByteArray encoded =
        toJson();
    if (file.write(encoded)
        != encoded.size()) {
        file.cancelWriting();
        return Result<void>::failure(
            resultError(
                QStringLiteral(
                    "update.result_write_failed"),
                QStringLiteral(
                    "The updater result file could not be written."),
                filePath));
    }
    if (!file.commit()) {
        return Result<void>::failure(
            resultError(
                QStringLiteral(
                    "update.result_commit_failed"),
                QStringLiteral(
                    "The updater result file could not be committed."),
                filePath));
    }
    return Result<void>::success();
}

} // namespace companion
