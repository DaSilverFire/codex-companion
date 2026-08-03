#include "core/CompanionInstallationIdentityStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <optional>
#include <utility>

namespace companion {
namespace {

CompanionError identityError(
    const QString& path)
{
    return {
        QStringLiteral(
            "mobile.installation_identity_unavailable"),
        QStringLiteral(
            "The Companion installation identity could not be loaded."),
        false,
        {
            {
                QStringLiteral("path"),
                path,
            },
        },
    };
}

std::optional<QString> normalizedIdentity(
    QString value)
{
    const QUuid parsed(
        value.trimmed());
    if (parsed.isNull()) {
        return std::nullopt;
    }
    return parsed.toString(
                     QUuid::WithoutBraces)
        .toUpper();
}

} // namespace

CompanionInstallationIdentityStore::
CompanionInstallationIdentityStore(
    QString filePath)
    : filePath_(
          filePath.trimmed().isEmpty()
              ? QString()
              : QFileInfo(
                    std::move(filePath))
                    .absoluteFilePath())
{
}

QString CompanionInstallationIdentityStore::
defaultFilePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::
                AppDataLocation);
    if (root.isEmpty()) {
        return {};
    }
    return QDir(root).filePath(
        QStringLiteral(
            "Security/installation-id"));
}

Result<QString>
CompanionInstallationIdentityStore::
loadOrCreate()
{
    QMutexLocker locker(&mutex_);
    if (!cached_.isEmpty()) {
        return Result<QString>::success(
            cached_);
    }
    if (filePath_.isEmpty()) {
        return Result<QString>::failure(
            identityError(filePath_));
    }

    const QFileInfo information(
        filePath_);
    if (information.exists()) {
        if (!information.isFile()
            || information.isSymLink()
            || information.size() > 1'024) {
            return Result<QString>::failure(
                identityError(filePath_));
        }
        QFile file(filePath_);
        if (!file.open(
                QIODevice::ReadOnly)) {
            return Result<QString>::failure(
                identityError(filePath_));
        }
        const auto existing =
            normalizedIdentity(
                QString::fromUtf8(
                    file.readAll()));
        if (existing.has_value()) {
            cached_ = *existing;
            return Result<QString>::success(
                cached_);
        }
    }

    QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<QString>::failure(
            identityError(filePath_));
    }

    const QString created =
        QUuid::createUuid()
            .toString(
                QUuid::WithoutBraces)
            .toUpper();
    const QByteArray encoded =
        created.toUtf8();
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(
            QIODevice::WriteOnly)
        || file.write(encoded)
            != encoded.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<QString>::failure(
            identityError(filePath_));
    }

    cached_ = created;
    return Result<QString>::success(
        cached_);
}

const QString&
CompanionInstallationIdentityStore::
filePath() const noexcept
{
    return filePath_;
}

} // namespace companion
