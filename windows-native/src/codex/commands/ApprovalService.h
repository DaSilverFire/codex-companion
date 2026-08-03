#pragma once

#include "codex/commands/CommitAwareMutation.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/ipc/FollowerClient.h"
#include "core/Result.h"

#include <QFuture>
#include <QJsonObject>

#include <functional>
#include <optional>

namespace companion {

using ApprovalResponder =
    std::function<QFuture<FollowerApprovalOutcome>(
        PendingApproval,
        ApprovalDecision)>;
using ApprovalRemoval =
    std::function<void(PendingApproval)>;
using ApprovalCommitProbe =
    std::function<void(QString)>;

class ApprovalService final {
public:
    explicit ApprovalService(
        const CodexEnvironment& environment);

    ApprovalService(
        ApprovalResponder responder,
        ApprovalRemoval removal = {},
        ApprovalCommitProbe commitProbe = {});

    CommitAwareMutationHandle<void> respondMutation(
        const PendingApproval& request,
        ApprovalDecision decision) const;

    QFuture<Result<void>> respond(
        const PendingApproval& request,
        ApprovalDecision decision) const;

    static std::optional<PendingApproval>
    pendingApprovalFromNotification(
        const QJsonObject& message);

private:
    ApprovalResponder responder_;
    ApprovalRemoval removal_;
    ApprovalCommitProbe commitProbe_;
};

} // namespace companion
