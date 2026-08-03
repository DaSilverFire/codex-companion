#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "codex/runtime/CodexProcessSnapshot.h"
#include "codex/state/DesktopApprovalState.h"
#include "core/Result.h"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <stop_token>

namespace companion {

namespace detail {
struct TaskSnapshotLoaderTestAccess;
}

using TaskProjectionStateProvider =
    std::function<TaskProjectionState(const QDateTime&)>;
using TaskNowProvider = std::function<QDateTime()>;
using ThreadRuntimeStatusProvider =
    std::function<Result<ThreadRuntimeSnapshot>(
        std::stop_token)>;

class TaskSnapshotLoader final {
public:
    explicit TaskSnapshotLoader(
        CodexEnvironment environment,
        TaskNowProvider nowProvider = {});

    TaskSnapshotLoader(
        CodexEnvironment environment,
        TaskProjectionStateProvider projectionStateProvider,
        TaskNowProvider nowProvider = {},
        ThreadRuntimeStatusProvider
            runtimeStatusProvider = {});

    Result<CodexProcessSnapshot> load(
        const QHash<QString, BridgeGoal>& cachedGoals,
        std::stop_token stopToken = {}) const;

private:
    enum class LoadPhase {
        AfterNow,
        AfterProjectionState,
        AfterThreads,
        AfterSessionNames,
        AfterSidebar,
        BeforeRollout,
        AfterRollout,
        AfterProjection,
        BeforeJobs,
        AfterJobs,
    };

    using LoadPhaseProbe =
        std::function<void(LoadPhase, qsizetype)>;

    TaskSnapshotLoader(
        CodexEnvironment environment,
        TaskProjectionStateProvider projectionStateProvider,
        TaskNowProvider nowProvider,
        ThreadRuntimeStatusProvider
            runtimeStatusProvider,
        LoadPhaseProbe loadPhaseProbe);

    void probeLoadPhase(
        LoadPhase phase,
        qsizetype index) const noexcept;

    friend struct detail::TaskSnapshotLoaderTestAccess;

    CodexEnvironment environment_;
    TaskProjectionStateProvider projectionStateProvider_;
    TaskNowProvider nowProvider_;
    ThreadRuntimeStatusProvider
        runtimeStatusProvider_;
    LoadPhaseProbe loadPhaseProbe_;
    std::shared_ptr<DesktopApprovalStateStore>
        approvalStateStore_;
};

} // namespace companion
