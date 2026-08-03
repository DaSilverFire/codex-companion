#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "codex/ipc/FollowerRequestFactory.h"

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace companion {

struct TaskProjectionState final {
    QHash<QString, PendingApproval> pendingApprovals;
    QSet<QString> pendingApprovalThreadIds;
    QSet<QString> attentionPromotedThreadIds;
};

class ApprovalPromotionTracker final {
public:
    explicit ApprovalPromotionTracker(
        std::chrono::milliseconds holdDuration =
            std::chrono::seconds(10));

    QSet<QString> promotedThreadIds(
        const QSet<QString>& pendingThreadIds,
        const QDateTime& now);

private:
    std::mutex mutex_;
    std::chrono::milliseconds holdDuration_;
    std::optional<QSet<QString>> previousPendingThreadIds_;
    QHash<QString, QDateTime> holdUntilByThreadId_;
};

class DesktopApprovalStateStore final {
public:
    explicit DesktopApprovalStateStore(
        const CodexEnvironment& environment);

    DesktopApprovalStateStore(
        QStringList logRoots,
        std::chrono::milliseconds maximumFileAge);

    ~DesktopApprovalStateStore();

    DesktopApprovalStateStore(
        const DesktopApprovalStateStore&) = delete;
    DesktopApprovalStateStore& operator=(
        const DesktopApprovalStateStore&) = delete;

    TaskProjectionState snapshot(
        const QDateTime& now);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace companion
