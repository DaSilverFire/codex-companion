#include "codex/accounts/CodexAccountRuntime.h"

#include <QDir>
#include <QFileInfo>

#include <utility>

namespace companion {
namespace {

QString absoluteDirectoryPath(
    QString path)
{
    return QDir::cleanPath(
        QFileInfo(std::move(path))
            .absoluteFilePath());
}

CompanionError runtimeError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

} // namespace

CodexAccountRuntime::CodexAccountRuntime(
    QString profilesRoot,
    QString sharedSqliteHome)
    : profilesRoot_(
          absoluteDirectoryPath(
              std::move(
                  profilesRoot))),
      sharedSqliteHome_(
          absoluteDirectoryPath(
              std::move(
                  sharedSqliteHome)))
{
}

Result<QProcessEnvironment>
CodexAccountRuntime::forProfile(
    const QProcessEnvironment&
        baseEnvironment,
    const CodexAccountProfile&
        profile) const
{
    if (profile.id.isNull()) {
        return Result<
            QProcessEnvironment>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.account_profile_id_invalid"),
                QStringLiteral(
                    "The Codex account profile ID is empty.")));
    }
    if (profilesRoot_.isEmpty()
        || sharedSqliteHome_.isEmpty()) {
        return Result<
            QProcessEnvironment>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.account_runtime_invalid"),
                QStringLiteral(
                    "The Codex account profile runtime paths are unavailable.")));
    }

    QProcessEnvironment environment =
        baseEnvironment;
    environment.insert(
        QStringLiteral("CODEX_HOME"),
        QDir(profilesRoot_).filePath(
            codexAccountProfileDirectoryName(
                profile.id)));
    environment.insert(
        QStringLiteral(
            "CODEX_SQLITE_HOME"),
        sharedSqliteHome_);
    return Result<
        QProcessEnvironment>::success(
            std::move(environment));
}

Result<void>
CodexAccountRuntime::prepareProfileHome(
    const CodexAccountProfile&
        profile) const
{
    const auto environment =
        forProfile(
            QProcessEnvironment{},
            profile);
    if (!environment.hasValue()) {
        return Result<void>::failure(
            environment.error());
    }
    const QString profileHome =
        environment.value().value(
            QStringLiteral("CODEX_HOME"));
    if (!QDir().mkpath(profileHome)
        || !QFileInfo(profileHome).isDir()) {
        return Result<void>::failure(
            runtimeError(
                QStringLiteral(
                    "codex.account_profile_home_unavailable"),
                QStringLiteral(
                    "The Codex account profile directory could not be prepared.")));
    }
    return Result<void>::success();
}

const QString&
CodexAccountRuntime::profilesRoot()
    const noexcept
{
    return profilesRoot_;
}

const QString&
CodexAccountRuntime::sharedSqliteHome()
    const noexcept
{
    return sharedSqliteHome_;
}

} // namespace companion
