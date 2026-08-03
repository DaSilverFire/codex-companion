#include "codex/runtime/TaskSnapshotLoader.h"

#include "codex/runtime/ThreadRuntimeStatusReader.h"
#include "codex/state/CodexStateDatabaseReader.h"
#include "codex/state/RolloutReader.h"
#include "codex/state/SessionIndexReader.h"
#include "codex/state/SidebarOrderingSnapshot.h"
#include "codex/state/TaskProjector.h"

#include <QDir>

#include <algorithm>
#include <optional>
#include <utility>

namespace companion {

namespace {

constexpr qsizetype kMaximumProcessThreadCandidates = 50;
constexpr qint64 kCurrentThreadWindowSeconds =
    3 * 60 + 5 * 60;
constexpr qint64 kRecentGoalCompletionWindowSeconds =
    30 * 60;

QDateTime unixTimestamp(qint64 value)
{
    constexpr qint64 secondsMagnitudeThreshold =
        100000000000LL;
    if (value > -secondsMagnitudeThreshold
        && value < secondsMagnitudeThreshold) {
        value *= 1000;
    }
    return QDateTime::fromMSecsSinceEpoch(
        value,
        QTimeZone::UTC);
}

bool goalKeepsThreadCurrent(
    const BridgeGoal& goal,
    const QDateTime& now)
{
    if (goal.status != GoalStatus::Complete) {
        return true;
    }
    const QDateTime updatedAt =
        unixTimestamp(goal.updatedAt);
    return updatedAt.isValid()
        && updatedAt
            >= now.addSecs(
                -kRecentGoalCompletionWindowSeconds);
}

QDateTime processCandidateActivity(
    const CodexThreadRecord& thread,
    const QHash<QString, BridgeGoal>& cachedGoals,
    const QHash<QString, QDateTime>& goalActivity,
    const QDateTime& now)
{
    QDateTime activityAt = thread.updatedAt;
    const auto persisted =
        goalActivity.constFind(thread.id);
    if (persisted != goalActivity.constEnd()
        && persisted.value().isValid()
        && persisted.value() > activityAt) {
        activityAt = persisted.value();
    }
    const auto goal = cachedGoals.constFind(thread.id);
    if (goal != cachedGoals.constEnd()
        && goalKeepsThreadCurrent(goal.value(), now)) {
        const QDateTime goalUpdatedAt =
            unixTimestamp(goal->updatedAt);
        if (goalUpdatedAt.isValid()
            && goalUpdatedAt > activityAt) {
            activityAt = goalUpdatedAt;
        }
    }
    return activityAt;
}

bool isCurrentProcessCandidate(
    const CodexThreadRecord& thread,
    const QHash<QString, BridgeGoal>& cachedGoals,
    const QSet<QString>& goalCandidateThreadIds,
    const QHash<QString, ThreadRuntimeStatus>&
        runtimeStatuses,
    const QDateTime& now)
{
    if (thread.updatedAt
        >= now.addSecs(
            -kCurrentThreadWindowSeconds)) {
        return true;
    }
    const auto goal = cachedGoals.constFind(thread.id);
    return (goal != cachedGoals.constEnd()
            && goalKeepsThreadCurrent(
                goal.value(),
                now))
        || goalCandidateThreadIds.contains(
            thread.id)
        || runtimeStatuses.contains(thread.id);
}

CompanionError canceledError()
{
    return {
        QStringLiteral("codex.operation_canceled"),
        QStringLiteral("The Codex operation was canceled."),
        false,
        {},
    };
}

CompanionError unavailableError()
{
    return {
        QStringLiteral("codex.task_snapshot_unavailable"),
        QStringLiteral("Could not read Codex task state."),
        true,
        {},
    };
}

std::optional<CompanionError> cancellation(
    std::stop_token stopToken)
{
    return stopToken.stop_requested()
        ? std::optional<CompanionError>(canceledError())
        : std::nullopt;
}

} // namespace

TaskSnapshotLoader::TaskSnapshotLoader(
    CodexEnvironment environment,
    TaskNowProvider nowProvider)
    : environment_(std::move(environment)),
      nowProvider_(std::move(nowProvider)),
      approvalStateStore_(
          std::make_shared<DesktopApprovalStateStore>(
              environment_))
{
    projectionStateProvider_ =
        [store = approvalStateStore_](
            const QDateTime& now) {
            return store->snapshot(now);
        };
    const auto runtimeReader =
        std::make_shared<
            ThreadRuntimeStatusReader>(
            environment_);
    runtimeStatusProvider_ =
        [runtimeReader](
            std::stop_token stopToken) {
            return runtimeReader->read(
                stopToken);
        };
}

TaskSnapshotLoader::TaskSnapshotLoader(
    CodexEnvironment environment,
    TaskProjectionStateProvider projectionStateProvider,
    TaskNowProvider nowProvider,
    ThreadRuntimeStatusProvider
        runtimeStatusProvider)
    : TaskSnapshotLoader(
          std::move(environment),
          std::move(projectionStateProvider),
          std::move(nowProvider),
          std::move(runtimeStatusProvider),
          {})
{
}

TaskSnapshotLoader::TaskSnapshotLoader(
    CodexEnvironment environment,
    TaskProjectionStateProvider projectionStateProvider,
    TaskNowProvider nowProvider,
    ThreadRuntimeStatusProvider
        runtimeStatusProvider,
    LoadPhaseProbe loadPhaseProbe)
    : environment_(std::move(environment)),
      projectionStateProvider_(
          std::move(projectionStateProvider)),
      nowProvider_(std::move(nowProvider)),
      runtimeStatusProvider_(
          std::move(runtimeStatusProvider)),
      loadPhaseProbe_(std::move(loadPhaseProbe))
{
}

void TaskSnapshotLoader::probeLoadPhase(
    LoadPhase phase,
    qsizetype index) const noexcept
{
    if (!loadPhaseProbe_) {
        return;
    }
    try {
        loadPhaseProbe_(phase, index);
    } catch (...) {
    }
}

Result<CodexProcessSnapshot>
TaskSnapshotLoader::load(
    const QHash<QString, BridgeGoal>& cachedGoals,
    std::stop_token stopToken) const
{
    try {
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }

        QDateTime now;
        try {
            now = nowProvider_
                ? nowProvider_()
                : QDateTime::currentDateTimeUtc();
        } catch (...) {
            probeLoadPhase(LoadPhase::AfterNow, -1);
            if (const auto stopped =
                    cancellation(stopToken);
                stopped.has_value()) {
                return Result<CodexProcessSnapshot>::failure(
                    *stopped);
            }
            return Result<CodexProcessSnapshot>::failure(
                unavailableError());
        }
        probeLoadPhase(LoadPhase::AfterNow, -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        if (!now.isValid()) {
            return Result<CodexProcessSnapshot>::failure(
                unavailableError());
        }
        now = now.toUTC();

        TaskProjectionState projectionState;
        if (projectionStateProvider_) {
            try {
                projectionState =
                    projectionStateProvider_(now);
            } catch (...) {
                projectionState = {};
            }
        }
        probeLoadPhase(
            LoadPhase::AfterProjectionState,
            -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }

        ThreadRuntimeSnapshot runtimeSnapshot;
        if (runtimeStatusProvider_) {
            try {
                auto runtimeResult =
                    runtimeStatusProvider_(
                        stopToken);
                if (runtimeResult.hasValue()) {
                    runtimeSnapshot =
                        std::move(
                            runtimeResult.value());
                }
            } catch (...) {
                runtimeSnapshot = {};
            }
        }
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        if (!runtimeSnapshot.authoritative) {
            for (const QString& threadId :
                 projectionState
                     .pendingApprovalThreadIds) {
                runtimeSnapshot.statuses.insert(
                    threadId,
                    ThreadRuntimeStatus::
                        WaitingOnApproval);
            }
        }

        auto threads =
            CodexStateDatabaseReader::readThreads(
                environment_.stateDatabase);
        probeLoadPhase(LoadPhase::AfterThreads, -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        if (!threads.hasValue()) {
            return Result<CodexProcessSnapshot>::failure(
                threads.error());
        }
        QVector<CodexThreadRecord> processThreads =
            std::move(threads.value());

        QHash<QString, CodexGoalCandidateRecord>
            persistedGoalCandidates;
        const auto mergeGoalCandidates =
            [&persistedGoalCandidates](
                QVector<CodexGoalCandidateRecord>
                    candidates) {
                for (CodexGoalCandidateRecord&
                         candidate : candidates) {
                    const QString threadId =
                        candidate.goal.threadId.trimmed();
                    if (threadId.isEmpty()) {
                        continue;
                    }
                    auto existing =
                        persistedGoalCandidates.find(
                            threadId);
                    if (existing
                            == persistedGoalCandidates.end()
                        || (!existing->activityAt.isValid()
                            && candidate.activityAt.isValid())
                        || (candidate.activityAt.isValid()
                            && candidate.activityAt
                                > existing->activityAt)) {
                        persistedGoalCandidates.insert(
                            threadId,
                            std::move(candidate));
                    }
                }
            };
        auto stateGoals =
            CodexStateDatabaseReader::
                readGoalCandidates(
                    environment_.stateDatabase,
                    now);
        if (stateGoals.hasValue()) {
            mergeGoalCandidates(
                std::move(stateGoals.value()));
        }
        const QString goalDatabase =
            environment_.goalDatabase.trimmed();
        if (!goalDatabase.isEmpty()
            && QDir::cleanPath(goalDatabase)
                .compare(
                    QDir::cleanPath(
                        environment_.stateDatabase),
                    Qt::CaseInsensitive)
                != 0) {
            auto separateGoals =
                CodexStateDatabaseReader::
                    readGoalCandidates(
                        goalDatabase,
                        now);
            if (separateGoals.hasValue()) {
                mergeGoalCandidates(
                    std::move(
                        separateGoals.value()));
            }
        }

        QSet<QString> knownThreadIds;
        knownThreadIds.reserve(
            processThreads.size());
        for (const CodexThreadRecord& thread :
             processThreads) {
            knownThreadIds.insert(thread.id);
        }
        auto persisted =
            persistedGoalCandidates.begin();
        while (persisted
               != persistedGoalCandidates.end()) {
            if (!knownThreadIds.contains(
                    persisted.key())) {
                persisted =
                    persistedGoalCandidates.erase(
                        persisted);
            } else {
                ++persisted;
            }
        }

        QHash<QString, BridgeGoal> projectionGoals;
        QHash<QString, QDateTime> goalActivity;
        QSet<QString> persistedGoalThreadIds;
        projectionGoals.reserve(
            persistedGoalCandidates.size()
            + cachedGoals.size());
        goalActivity.reserve(
            persistedGoalCandidates.size());
        persistedGoalThreadIds.reserve(
            persistedGoalCandidates.size());
        for (auto candidate =
                 persistedGoalCandidates.cbegin();
             candidate
                 != persistedGoalCandidates.cend();
             ++candidate) {
            projectionGoals.insert(
                candidate.key(),
                candidate->goal);
            goalActivity.insert(
                candidate.key(),
                candidate->activityAt);
            if (goalKeepsThreadCurrent(
                    candidate->goal,
                    now)) {
                persistedGoalThreadIds.insert(
                    candidate.key());
            }
        }
        for (auto cached = cachedGoals.cbegin();
             cached != cachedGoals.cend();
             ++cached) {
            const auto persistedGoal =
                projectionGoals.constFind(cached.key());
            if (persistedGoal
                    == projectionGoals.constEnd()
                || cached->updatedAt
                    > persistedGoal->updatedAt) {
                projectionGoals.insert(
                    cached.key(),
                    cached.value());
            }
        }

        processThreads.erase(
            std::remove_if(
                processThreads.begin(),
                processThreads.end(),
                [&projectionGoals,
                 &persistedGoalThreadIds,
                 &runtimeSnapshot,
                 &now](
                    const CodexThreadRecord& thread) {
                    return !isCurrentProcessCandidate(
                        thread,
                        projectionGoals,
                        persistedGoalThreadIds,
                        runtimeSnapshot.statuses,
                        now);
                }),
            processThreads.end());
        std::sort(
            processThreads.begin(),
            processThreads.end(),
            [&projectionGoals,
             &goalActivity,
             &now](
                const CodexThreadRecord& left,
                const CodexThreadRecord& right) {
                const QDateTime leftActivity =
                    processCandidateActivity(
                        left,
                        projectionGoals,
                        goalActivity,
                        now);
                const QDateTime rightActivity =
                    processCandidateActivity(
                        right,
                        projectionGoals,
                        goalActivity,
                        now);
                if (leftActivity != rightActivity) {
                    return leftActivity > rightActivity;
                }
                if (left.updatedAt
                    != right.updatedAt) {
                    return left.updatedAt
                        > right.updatedAt;
                }
                return left.id < right.id;
            });
        if (processThreads.size()
            > kMaximumProcessThreadCandidates) {
            processThreads.resize(
                kMaximumProcessThreadCandidates);
        }

        QVector<QString> goalCandidateThreadIds;
        for (const CodexThreadRecord& thread :
             processThreads) {
            if (persistedGoalThreadIds.contains(
                    thread.id)) {
                goalCandidateThreadIds.append(
                    thread.id);
            }
        }
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }

        auto names = SessionIndexReader::readNames(
            environment_.sessionIndex);
        probeLoadPhase(
            LoadPhase::AfterSessionNames,
            -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        QHash<QString, QString> sessionNames;
        if (names.hasValue()) {
            sessionNames = std::move(names.value());
        }

        const SidebarOrderingSnapshot sidebarOrdering =
            SidebarOrderingSnapshot::read(
                QDir(environment_.codexHome).filePath(
                    QStringLiteral(
                        ".codex-global-state.json")));
        probeLoadPhase(LoadPhase::AfterSidebar, -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }

        QHash<QString, RolloutSnapshot> rollouts;
        rollouts.reserve(processThreads.size());
        for (qsizetype index = 0;
             index < processThreads.size();
             ++index) {
            if (const auto stopped =
                    cancellation(stopToken);
                stopped.has_value()) {
                return Result<CodexProcessSnapshot>::failure(
                    *stopped);
            }
            probeLoadPhase(
                LoadPhase::BeforeRollout,
                index);
            if (const auto stopped =
                    cancellation(stopToken);
                stopped.has_value()) {
                return Result<CodexProcessSnapshot>::failure(
                    *stopped);
            }

            const CodexThreadRecord& thread =
                processThreads.at(index);
            auto rollout =
                RolloutReader::readMobileTaskTail(
                thread.rolloutPath,
                environment_.codexHome);
            probeLoadPhase(
                LoadPhase::AfterRollout,
                index);
            if (const auto stopped =
                    cancellation(stopToken);
                stopped.has_value()) {
                return Result<CodexProcessSnapshot>::failure(
                    *stopped);
            }
            if (rollout.hasValue()) {
                rollouts.insert(
                    thread.id,
                    std::move(rollout.value()));
            }
        }

        TaskProjectionContext projectionContext{
            std::move(sessionNames),
            runtimeSnapshot.authoritative
                ? QSet<QString>()
                : projectionState
                      .pendingApprovalThreadIds,
            projectionState.attentionPromotedThreadIds,
            sidebarOrdering,
            now,
            runtimeSnapshot.statuses,
        };
        CodexStateSnapshot threadSnapshot{
            std::move(processThreads),
            {},
        };
        QVector<BridgeTask> tasks =
            TaskProjector::projectAll(
                threadSnapshot,
                rollouts,
                projectionGoals,
                projectionContext);
        probeLoadPhase(
            LoadPhase::AfterProjection,
            -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }

        probeLoadPhase(LoadPhase::BeforeJobs, -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        auto readJobs =
            CodexStateDatabaseReader::readJobs(
                environment_.stateDatabase);
        probeLoadPhase(LoadPhase::AfterJobs, -1);
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        QVector<CodexJobRecord> jobs;
        if (readJobs.hasValue()) {
            jobs = std::move(readJobs.value());
        }

        return Result<CodexProcessSnapshot>::success({
            std::move(tasks),
            std::move(jobs),
            std::move(projectionState.pendingApprovals),
            std::move(runtimeSnapshot.statuses),
            std::move(goalCandidateThreadIds),
            std::move(
                projectionState
                    .attentionPromotedThreadIds),
        });
    } catch (...) {
        if (const auto stopped = cancellation(stopToken);
            stopped.has_value()) {
            return Result<CodexProcessSnapshot>::failure(
                *stopped);
        }
        return Result<CodexProcessSnapshot>::failure(
            unavailableError());
    }
}

} // namespace companion
