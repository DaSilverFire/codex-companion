#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "codex/commands/CommitAwareMutation.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QFuture>
#include <QProcessEnvironment>
#include <QUuid>

#include <functional>

namespace companion {

enum class UsageResetOutcome {
    Reset,
    NothingToReset,
    NoCredit,
    AlreadyRedeemed,
};

using UsageRpcPerformer =
    std::function<Result<QHash<int, RpcResponse>>(
        const QVector<RpcRequest>&)>;
using UsageClock = std::function<BridgeDate()>;
using UsageCommitProbe = std::function<void(QString)>;

class UsageService final {
public:
    explicit UsageService(
        const CodexEnvironment& environment,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            AppServerRpcClient::kDefaultTimeoutMilliseconds);

    explicit UsageService(
        UsageRpcPerformer performer,
        UsageClock clock = {},
        UsageCommitProbe consumeCommitProbe = {});

    QFuture<Result<BridgeUsageSnapshot>> read() const;

    CommitAwareMutationHandle<UsageResetOutcome>
    consumeResetMutation(
        const QString& creditId,
        const QUuid& idempotencyKey) const;

    QFuture<Result<UsageResetOutcome>> consumeReset(
        const QString& creditId,
        const QUuid& idempotencyKey) const;

private:
    UsageRpcPerformer performer_;
    UsageClock clock_;
    UsageCommitProbe consumeCommitProbe_;
};

} // namespace companion
