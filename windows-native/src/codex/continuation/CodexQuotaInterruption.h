#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "core/Result.h"

#include <QDateTime>
#include <QString>

#include <optional>

namespace companion {

struct CodexQuotaInterruption final {
    QString threadId;
    QString turnId;
    std::optional<QDateTime> completedAt;

    QString eventKey() const;

    friend bool operator==(
        const CodexQuotaInterruption&,
        const CodexQuotaInterruption&) =
        default;
};

class CodexQuotaInterruptionProtocol final {
public:
    static Result<RpcRequest>
    latestTurnRequest(
        int id,
        QString threadId);

    static Result<
        std::optional<
            CodexQuotaInterruption>>
    parse(
        QString threadId,
        const RpcResponse& response);
};

} // namespace companion
