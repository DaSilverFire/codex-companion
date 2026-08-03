#pragma once

#include "codex/models/BridgeModels.h"
#include "codex/models/CodexModels.h"
#include "codex/models/ThreadRuntimeStatus.h"
#include "codex/state/SidebarOrderingSnapshot.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct TaskProjectionContext final {
    QHash<QString, QString> sessionNames;
    QSet<QString> pendingApprovalThreadIds;
    QSet<QString> attentionPromotedThreadIds;
    SidebarOrderingSnapshot sidebarOrdering;
    QDateTime now;
    QHash<QString, ThreadRuntimeStatus>
        runtimeStatuses;
};

class TaskProjector final {
public:
    static BridgeTask project(
        const CodexThreadRecord& thread,
        const RolloutSnapshot& rollout,
        const std::optional<BridgeGoal>& goal,
        const TaskProjectionContext& context);

    static BridgeTask applyingGoal(
        BridgeTask task,
        const std::optional<BridgeGoal>& goal,
        const std::optional<ThreadRuntimeStatus>&
            runtimeStatus,
        const QDateTime& now);

    static QVector<BridgeTask> projectAll(
        const CodexStateSnapshot& snapshot,
        const QHash<QString, RolloutSnapshot>& rollouts,
        const QHash<QString, BridgeGoal>& goals,
        const TaskProjectionContext& context);
};

} // namespace companion
