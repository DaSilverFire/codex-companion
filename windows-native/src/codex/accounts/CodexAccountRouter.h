#pragma once

#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "core/Result.h"

#include <QProcessEnvironment>
#include <QStringView>

#include <optional>

namespace companion {

struct CodexAccountRoute final {
    std::optional<QUuid> profileId;
    QProcessEnvironment environment;
};

class CodexAccountRouter final {
public:
    CodexAccountRouter(
        QProcessEnvironment
            baseEnvironment,
        CodexAccountProfileStore&
            profileStore,
        const CodexAccountRuntime&
            profileRuntime,
        CodexThreadAccountBindingStore&
            bindingStore);

    CodexAccountRoute routeNewWork()
        const;
    CodexAccountRoute routeThread(
        QStringView threadId) const;
    CodexAccountRoute routeProfile(
        const QUuid& profileId) const;

    Result<void> bindNewThread(
        QString threadId,
        std::optional<QUuid>
            profileId);
    Result<bool> removeProfile(
        const QUuid& profileId);

private:
    CodexAccountRoute route(
        std::optional<
            CodexAccountProfile>
            profile) const;

    QProcessEnvironment
        baseEnvironment_;
    CodexAccountProfileStore*
        profileStore_ = nullptr;
    const CodexAccountRuntime*
        profileRuntime_ = nullptr;
    CodexThreadAccountBindingStore*
        bindingStore_ = nullptr;
};

} // namespace companion
