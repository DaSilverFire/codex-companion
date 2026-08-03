#pragma once

#include "codex/accounts/CodexAccountRouter.h"
#include "codex/appserver/AppServerRpcClient.h"
#include "codex/models/ThreadRuntimeStatus.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <functional>
#include <stop_token>

namespace companion {

struct CodexThreadAccountHandoffResult final {
    QString threadId;
    QString rolloutPath;
    QUuid profileId;

    friend bool operator==(
        const CodexThreadAccountHandoffResult&,
        const CodexThreadAccountHandoffResult&) =
        default;
};

using CodexThreadAccountHandoffPerformer =
    std::function<Result<QHash<int, RpcResponse>>(
        const CodexAccountRoute&,
        const QVector<RpcRequest>&,
        std::stop_token)>;

class CodexThreadAccountHandoffService final {
public:
    CodexThreadAccountHandoffService(
        const CodexEnvironment& environment,
        CodexAccountRouter& router);
    CodexThreadAccountHandoffService(
        CodexAccountRouter& router,
        CodexThreadAccountHandoffPerformer
            performer);

    Result<CodexThreadAccountHandoffResult>
    handoff(
        QString threadId,
        QString rolloutPath,
        ThreadRuntimeStatus runtimeStatus,
        const QUuid& destinationProfileId,
        std::stop_token stopToken = {}) const;

    static bool canHandoff(
        ThreadRuntimeStatus runtimeStatus)
        noexcept;

private:
    CodexAccountRouter* router_ = nullptr;
    CodexThreadAccountHandoffPerformer
        performer_;
};

} // namespace companion
