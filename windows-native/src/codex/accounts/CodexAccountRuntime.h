#pragma once

#include "codex/accounts/CodexAccountProfile.h"
#include "core/Result.h"

#include <QProcessEnvironment>
#include <QString>

namespace companion {

class CodexAccountRuntime final {
public:
    CodexAccountRuntime(
        QString profilesRoot,
        QString sharedSqliteHome);

    Result<QProcessEnvironment>
    forProfile(
        const QProcessEnvironment&
            baseEnvironment,
        const CodexAccountProfile&
            profile) const;

    Result<void> prepareProfileHome(
        const CodexAccountProfile&
            profile) const;

    const QString& profilesRoot()
        const noexcept;
    const QString& sharedSqliteHome()
        const noexcept;

private:
    QString profilesRoot_;
    QString sharedSqliteHome_;
};

} // namespace companion
