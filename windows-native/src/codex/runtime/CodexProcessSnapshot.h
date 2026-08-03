#pragma once

#include "codex/ipc/FollowerRequestFactory.h"
#include "codex/models/BridgeModels.h"
#include "codex/models/CodexModels.h"
#include "codex/models/ThreadRuntimeStatus.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace companion {

struct CodexProcessSnapshot final {
    QVector<BridgeTask> tasks;
    QVector<CodexJobRecord> jobs;
    QHash<QString, PendingApproval> pendingApprovals;
    QHash<QString, ThreadRuntimeStatus>
        runtimeStatuses;
    QVector<QString> goalCandidateThreadIds;
    QSet<QString> attentionPromotedThreadIds;

    friend bool operator==(
        const CodexProcessSnapshot&,
        const CodexProcessSnapshot&) = default;
};

} // namespace companion
