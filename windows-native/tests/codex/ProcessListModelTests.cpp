#include "codex/runtime/ProcessListModel.h"

#include <QAbstractItemModel>
#include <QAbstractItemModelTester>
#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <optional>
#include <utility>

namespace {

using namespace companion;

constexpr double kSwiftReferenceDateUnixSeconds =
    978307200.0;

double bridgeSeconds(const QDateTime& date)
{
    return static_cast<double>(
               date.toMSecsSinceEpoch())
        / 1000.0
        - kSwiftReferenceDateUnixSeconds;
}

BridgeTask task(
    QString id,
    TaskStatus status,
    const QDateTime& updatedAt)
{
    BridgeTask result;
    result.id = std::move(id);
    result.title =
        QStringLiteral("Thread ") + result.id;
    result.preview =
        QStringLiteral("Thread preview ") + result.id;
    result.updatedAt.secondsSinceReferenceDate =
        bridgeSeconds(updatedAt);
    result.status = status;
    return result;
}

CodexJobRecord job(
    QString id,
    QString status,
    const QDateTime& updatedAt,
    std::optional<QString> threadId = std::nullopt)
{
    return {
        std::move(id),
        QStringLiteral("Job title"),
        std::move(status),
        QStringLiteral("Job instruction"),
        std::nullopt,
        std::move(threadId),
        updatedAt,
        updatedAt.addSecs(-30),
    };
}

QString processIdAt(
    const ProcessListModel& model,
    int row)
{
    return model.data(
                    model.index(row, 0),
                    ProcessListModel::ProcessIdRole)
        .toString();
}

int rowForProcessId(
    const ProcessListModel& model,
    const QString& processId)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        if (processIdAt(model, row) == processId) {
            return row;
        }
    }
    return -1;
}

} // namespace

class ProcessListModelTests final : public QObject {
    Q_OBJECT

private slots:
    void exposesThreadAndProcessIdentityRoles()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        BridgeTask projected =
            task(
                QStringLiteral("thread-a"),
                TaskStatus::Running,
                now);
        projected.rolloutPath =
            QStringLiteral(
                "C:/Codex/sessions/thread-a.jsonl");
        snapshot.tasks.append(
            projected);

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex index = model.index(0, 0);
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::IdRole)
                .toString(),
            QStringLiteral("thread-a"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ProcessIdRole)
                .toString(),
            QStringLiteral("thread-a"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ThreadIdRole)
                .toString(),
            QStringLiteral("thread-a"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::KindRole)
                .toString(),
            QStringLiteral("thread"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::
                         RolloutPathRole)
                .toString(),
            projected.rolloutPath);
    }

    void exposesAuthoritativeRuntimeStatus()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(
            task(
                QStringLiteral("thread-runtime"),
                TaskStatus::Running,
                now));
        snapshot.runtimeStatuses.insert(
            QStringLiteral("thread-runtime"),
            ThreadRuntimeStatus::Active);

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::RuntimeStatusRole)
                .toString(),
            QStringLiteral("active"));
    }

    void mapsJobsIntoDesktopProcessCards()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        CodexJobRecord current = job(
            QStringLiteral("build"),
            QStringLiteral("in_progress"),
            now.addSecs(-20),
            QStringLiteral("thread-build"));
        current.name = QStringLiteral("Build Windows Companion");
        current.instruction =
            QStringLiteral("Compile and verify the native app");
        snapshot.jobs.append(current);

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 1);
        const QModelIndex index = model.index(0, 0);
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ProcessIdRole)
                .toString(),
            QStringLiteral("job-build"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ThreadIdRole)
                .toString(),
            QStringLiteral("thread-build"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::KindRole)
                .toString(),
            QStringLiteral("job"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::TitleRole)
                .toString(),
            QStringLiteral("Build Windows Companion"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::PreviewRole)
                .toString(),
            QStringLiteral(
                "Compile and verify the native app"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("running"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::SourceStatusRole)
                .toString(),
            QStringLiteral("in_progress"));
    }

    void retainsOnlyCurrentCompletedJobs()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.jobs = {
            job(
                QStringLiteral("recent"),
                QStringLiteral("completed"),
                now.addSecs(-299)),
            job(
                QStringLiteral("boundary"),
                QStringLiteral("done"),
                now.addSecs(-300)),
            job(
                QStringLiteral("stale"),
                QStringLiteral("success"),
                now.addSecs(-600)),
            job(
                QStringLiteral("running"),
                QStringLiteral("running"),
                now.addSecs(-3600)),
            job(
                QStringLiteral("waiting-current"),
                QStringLiteral("mystery"),
                now.addSecs(-300)),
            job(
                QStringLiteral("waiting-expired"),
                QStringLiteral("mystery"),
                now.addSecs(-301)),
            job(
                QStringLiteral("failed"),
                QStringLiteral("failed"),
                now.addSecs(-3600)),
        };

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 4);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("job-recent"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("job-running"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral(
                    "job-waiting-current"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("job-failed"))
            >= 0);
        QCOMPARE(
            rowForProcessId(
                model,
                QStringLiteral("job-boundary")),
            -1);
        QCOMPARE(
            rowForProcessId(
                model,
                QStringLiteral("job-stale")),
            -1);
        QCOMPARE(
            rowForProcessId(
                model,
                QStringLiteral(
                    "job-waiting-expired")),
            -1);
    }

    void retainsOnlyCurrentCompletedThreads()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.tasks = {
            task(
                QStringLiteral("recent"),
                TaskStatus::Completed,
                now.addSecs(-479)),
            task(
                QStringLiteral("boundary"),
                TaskStatus::Completed,
                now.addSecs(-480)),
            task(
                QStringLiteral("stale"),
                TaskStatus::Completed,
                now.addSecs(-3600)),
            task(
                QStringLiteral("running"),
                TaskStatus::Running,
                now.addSecs(-60)),
            task(
                QStringLiteral("waiting"),
                TaskStatus::Waiting,
                now.addSecs(-60)),
            task(
                QStringLiteral("failed"),
                TaskStatus::Failed,
                now.addSecs(-60)),
        };

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 4);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("recent"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("running"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("waiting"))
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("failed"))
            >= 0);
        QCOMPARE(
            rowForProcessId(
                model,
                QStringLiteral("boundary")),
            -1);
        QCOMPARE(
            rowForProcessId(
                model,
                QStringLiteral("stale")),
            -1);
    }

    void rejectsStaleOrdinaryRunningAndWaitingThreads()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.tasks = {
            task(
                QStringLiteral("stale-running"),
                TaskStatus::Running,
                now.addDays(-1)),
            task(
                QStringLiteral("stale-waiting"),
                TaskStatus::Waiting,
                now.addDays(-1)),
            task(
                QStringLiteral("recent-completed"),
                TaskStatus::Completed,
                now.addSecs(-60)),
        };

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            processIdAt(model, 0),
            QStringLiteral("recent-completed"));
    }

    void staleThreadsRemainVisibleWhenPromotedByLiveState()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        const QDateTime staleAt = now.addDays(-1);
        BridgeTask runtime = task(
            QStringLiteral("stale-runtime"),
            TaskStatus::Running,
            staleAt);
        BridgeTask approval = task(
            QStringLiteral("stale-approval"),
            TaskStatus::Waiting,
            staleAt);
        BridgeTask attention = task(
            QStringLiteral("stale-attention"),
            TaskStatus::Waiting,
            staleAt);
        BridgeTask goalCandidate = task(
            QStringLiteral("stale-goal-candidate"),
            TaskStatus::Waiting,
            staleAt);

        CodexProcessSnapshot snapshot;
        snapshot.tasks = {
            runtime,
            approval,
            attention,
            goalCandidate,
        };
        snapshot.runtimeStatuses.insert(
            runtime.id,
            ThreadRuntimeStatus::Active);
        PendingApproval pending;
        pending.threadId = approval.id;
        snapshot.pendingApprovals.insert(
            approval.id,
            pending);
        snapshot.attentionPromotedThreadIds.insert(
            attention.id);
        snapshot.goalCandidateThreadIds.append(
            goalCandidate.id);

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 4);
        QVERIFY(rowForProcessId(model, runtime.id) >= 0);
        QVERIFY(rowForProcessId(model, approval.id) >= 0);
        QVERIFY(rowForProcessId(model, attention.id) >= 0);
        QVERIFY(
            rowForProcessId(model, goalCandidate.id)
            >= 0);
    }

    void recentlyCompletedGoalUsesThirtyMinuteWindow()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        BridgeTask visible = task(
            QStringLiteral("goal-visible"),
            TaskStatus::Completed,
            now.addSecs(-20 * 60));
        visible.goal = BridgeGoal{
            visible.id,
            QStringLiteral("Finish parity"),
            GoalStatus::Complete,
            std::nullopt,
            10,
            20 * 60,
            now.addSecs(-40 * 60)
                .toMSecsSinceEpoch(),
            now.addSecs(-20 * 60)
                .toMSecsSinceEpoch(),
        };
        BridgeTask expired = task(
            QStringLiteral("goal-expired"),
            TaskStatus::Completed,
            now.addSecs(-31 * 60));
        expired.goal = BridgeGoal{
            expired.id,
            QStringLiteral("Old completed goal"),
            GoalStatus::Complete,
            std::nullopt,
            10,
            31 * 60,
            now.addSecs(-60 * 60)
                .toMSecsSinceEpoch(),
            now.addSecs(-31 * 60)
                .toMSecsSinceEpoch(),
        };

        CodexProcessSnapshot snapshot;
        snapshot.tasks = {visible, expired};
        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            processIdAt(model, 0),
            QStringLiteral("goal-visible"));
    }

    void stalePausedGoalRemainsVisibleWithoutRuntimeActivity()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        BridgeTask stale = task(
            QStringLiteral("paused-stale"),
            TaskStatus::Waiting,
            now.addSecs(-24 * 60 * 60));
        stale.goal = BridgeGoal{
            stale.id,
            QStringLiteral("Old paused goal"),
            GoalStatus::Paused,
            std::nullopt,
            10,
            24 * 60 * 60,
            now.addSecs(-48 * 60 * 60)
                .toMSecsSinceEpoch(),
            now.addSecs(-24 * 60 * 60)
                .toMSecsSinceEpoch(),
        };
        BridgeTask recent = task(
            QStringLiteral("paused-recent"),
            TaskStatus::Waiting,
            now.addSecs(-(8 * 60 - 1)));
        recent.goal = stale.goal;
        recent.goal->threadId = recent.id;
        BridgeTask live = stale;
        live.id = QStringLiteral("paused-live");
        live.title = QStringLiteral("Thread paused-live");
        live.goal->threadId = live.id;
        BridgeTask active = stale;
        active.id = QStringLiteral("active-old");
        active.title = QStringLiteral("Thread active-old");
        active.status = TaskStatus::Running;
        active.goal->threadId = active.id;
        active.goal->status = GoalStatus::Active;

        CodexProcessSnapshot snapshot;
        snapshot.tasks = {stale, recent, live, active};
        snapshot.runtimeStatuses.insert(
            live.id,
            ThreadRuntimeStatus::WaitingOnUserInput);
        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 4);
        QVERIFY(
            rowForProcessId(model, stale.id)
            >= 0);
        QVERIFY(
            rowForProcessId(model, recent.id)
            >= 0);
        QVERIFY(
            rowForProcessId(model, live.id)
            >= 0);
        QVERIFY(
            rowForProcessId(model, active.id)
            >= 0);
    }

    void capsMergedFeedAfterOrdering()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        for (int index = 0; index < 10; ++index) {
            snapshot.tasks.append(
                task(
                    QStringLiteral("thread-%1")
                        .arg(index),
                    TaskStatus::Completed,
                    now.addSecs(-index)));
        }
        snapshot.jobs.append(
            job(
                QStringLiteral("active"),
                QStringLiteral("running"),
                now));

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 10);
        QCOMPARE(
            processIdAt(model, 0),
            QStringLiteral("job-active"));
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral("thread-9"))
            < 0);
    }

    void visibleGoalReplacesLowerPriorityNonGoalAtCap()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        for (int index = 0;
             index < 10;
             ++index) {
            snapshot.tasks.append(
                task(
                    QStringLiteral(
                        "thread-%1")
                        .arg(index),
                    TaskStatus::Completed,
                    now.addSecs(-index)));
        }
        BridgeTask goal = task(
            QStringLiteral(
                "goal-complete"),
            TaskStatus::Completed,
            now.addSecs(-20 * 60));
        goal.goal = BridgeGoal{
            goal.id,
            QStringLiteral(
                "Retain this goal"),
            GoalStatus::Complete,
            std::nullopt,
            10,
            20 * 60,
            now.addSecs(-30 * 60)
                .toMSecsSinceEpoch(),
            now.addSecs(-20 * 60)
                .toMSecsSinceEpoch(),
        };
        snapshot.tasks.append(goal);

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 10);
        QVERIFY(
            rowForProcessId(
                model,
                goal.id)
            >= 0);
        QVERIFY(
            rowForProcessId(
                model,
                QStringLiteral(
                    "thread-9"))
            < 0);
    }

    void assignedApprovalOnlyChangesTheThreadCard()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        BridgeTask assigned = task(
            QStringLiteral("thread-approval"),
            TaskStatus::Waiting,
            now);
        assigned.title =
            QStringLiteral("Approve package install");
        assigned.needsApproval = true;
        assigned.cwd = QStringLiteral("C:/worktree");
        assigned.activeTurnId =
            QStringLiteral("turn-approval");
        assigned.model = QStringLiteral("gpt-test");
        assigned.reasoningEffort =
            QStringLiteral("high");

        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(assigned);
        snapshot.jobs.append(
            job(
                QStringLiteral("approval"),
                QStringLiteral("running"),
                now,
                assigned.id));
        snapshot.pendingApprovals.insert(
            assigned.id,
            PendingApproval{});

        model.setSnapshot(snapshot, now);

        const int row = rowForProcessId(
            model,
            QStringLiteral("job-approval"));
        QVERIFY(row >= 0);
        const QModelIndex index = model.index(row, 0);
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::TitleRole)
                .toString(),
            assigned.title);
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("running"));
        QVERIFY(
            !model.data(
                      index,
                      ProcessListModel::
                          NeedsApprovalRole)
                 .toBool());
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::PreviewRole)
                .toString(),
            QStringLiteral("Job instruction"));

        const int threadRow =
            rowForProcessId(
                model,
                assigned.id);
        QVERIFY(threadRow >= 0);
        const QModelIndex threadIndex =
            model.index(threadRow, 0);
        QCOMPARE(
            model.data(
                     threadIndex,
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("waiting"));
        QVERIFY(
            model.data(
                     threadIndex,
                     ProcessListModel::
                         NeedsApprovalRole)
                .toBool());
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ActiveTurnIdRole)
                .toString(),
            QStringLiteral("turn-approval"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::ModelRole)
                .toString(),
            QStringLiteral("gpt-test"));
    }

    void heldApprovalPromotionOrdersTheFinalMergedFeed()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.tasks = {
            task(
                QStringLiteral("thread-newer"),
                TaskStatus::Waiting,
                now),
            task(
                QStringLiteral("thread-held"),
                TaskStatus::Waiting,
                now.addSecs(-60)),
        };
        snapshot.attentionPromotedThreadIds.insert(
            QStringLiteral("thread-held"));

        model.setSnapshot(snapshot, now);

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(
            processIdAt(model, 0),
            QStringLiteral("thread-held"));
        QCOMPARE(
            processIdAt(model, 1),
            QStringLiteral("thread-newer"));
    }

    void failedJobUsesTheRecordedError()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexJobRecord failed = job(
            QStringLiteral("failure"),
            QStringLiteral("error"),
            now);
        failed.error =
            QStringLiteral("The build process exited.");
        CodexProcessSnapshot snapshot;
        snapshot.jobs.append(failed);

        model.setSnapshot(snapshot, now);

        const QModelIndex index = model.index(0, 0);
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("failed"));
        QCOMPARE(
            model.data(
                     index,
                     ProcessListModel::PreviewRole)
                .toString(),
            QStringLiteral("The build process exited."));
    }

    void unresolvedFailureSurvivesMissingRefreshUntilNewActivity()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot failedSnapshot;
        failedSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Failed,
                now.addSecs(-30)));

        model.setSnapshot(failedSnapshot, now);
        QCOMPARE(model.rowCount(), 1);

        model.setSnapshot({}, now.addSecs(10));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            processIdAt(model, 0),
            QStringLiteral("thread-failure"));
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("failed"));

        CodexProcessSnapshot resumedSnapshot;
        resumedSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Running,
                now.addSecs(20)));
        model.setSnapshot(
            resumedSnapshot,
            now.addSecs(20));

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("running"));
    }

    void heuristicRunningDoesNotResolveAnUnchangedFailure()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot failedSnapshot;
        failedSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Failed,
                now));
        model.setSnapshot(failedSnapshot, now);

        CodexProcessSnapshot heuristicSnapshot;
        heuristicSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Running,
                now));
        model.setSnapshot(heuristicSnapshot, now);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("failed"));
    }

    void waitingForUserInputResolvesAnUnchangedFailure()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot failedSnapshot;
        failedSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Failed,
                now));
        model.setSnapshot(failedSnapshot, now);

        CodexProcessSnapshot waitingSnapshot;
        waitingSnapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Waiting,
                now));
        waitingSnapshot.runtimeStatuses.insert(
            QStringLiteral("thread-failure"),
            ThreadRuntimeStatus::
                WaitingOnUserInput);
        model.setSnapshot(waitingSnapshot, now);

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("waiting"));
    }

    void handledFailureStaysHiddenUntilANewerFailureAppears()
    {
        ProcessListModel model;
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(
            task(
                QStringLiteral("thread-failure"),
                TaskStatus::Failed,
                now));

        model.setSnapshot(snapshot, now);
        QCOMPARE(model.rowCount(), 1);

        model.markFailureHandled(
            QStringLiteral("thread-failure"),
            QStringLiteral("thread-failure"));
        QCOMPARE(model.rowCount(), 0);

        model.setSnapshot(snapshot, now.addSecs(10));
        QCOMPARE(model.rowCount(), 0);

        snapshot.tasks[0].updatedAt.secondsSinceReferenceDate =
            bridgeSeconds(now.addSecs(20));
        model.setSnapshot(
            snapshot,
            now.addSecs(20));

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::StatusRole)
                .toString(),
            QStringLiteral("failed"));
    }

    void handledAndUnresolvedFailuresPersistAcrossModelRestarts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath =
            directory.filePath(
                QStringLiteral(
                    "process-failures.v1.json"));
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot firstFailure;
        firstFailure.tasks.append(
            task(
                QStringLiteral("thread-unresolved"),
                TaskStatus::Failed,
                now));
        CodexProcessSnapshot handledFailure;
        handledFailure.tasks.append(
            task(
                QStringLiteral("thread-handled"),
                TaskStatus::Failed,
                now));

        {
            ProcessListModel model({}, statePath);
            model.setSnapshot(firstFailure, now);
            model.setSnapshot(handledFailure, now);
            model.markFailureHandled(
                QStringLiteral("thread-handled"),
                QStringLiteral("thread-handled"));
        }

        ProcessListModel restored({}, statePath);
        restored.setSnapshot({}, now.addSecs(10));

        QCOMPARE(restored.rowCount(), 1);
        QCOMPARE(
            processIdAt(restored, 0),
            QStringLiteral("thread-unresolved"));

        restored.setSnapshot(
            handledFailure,
            now.addSecs(10));
        QCOMPARE(
            rowForProcessId(
                restored,
                QStringLiteral("thread-handled")),
            -1);
        QVERIFY(
            rowForProcessId(
                restored,
                QStringLiteral("thread-unresolved"))
            >= 0);
    }

    void unresolvedFailureMetadataPersistsAcrossModelRestarts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath =
            directory.filePath(
                QStringLiteral(
                    "process-failures.v1.json"));
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        BridgeTask failed = task(
            QStringLiteral("thread-metadata"),
            TaskStatus::Failed,
            now);
        failed.taskGroup = BridgeTaskGroup{
            TaskGroupKind::Project,
            QStringLiteral("Companion"),
            QStringLiteral("C:/work/companion"),
        };
        failed.goal = BridgeGoal{
            failed.id,
            QStringLiteral("Finish Windows parity"),
            GoalStatus::Blocked,
            50'000,
            12'345,
            600,
            now.addSecs(-600)
                .toMSecsSinceEpoch(),
            now.toMSecsSinceEpoch(),
        };
        CodexProcessSnapshot snapshot;
        snapshot.tasks.append(failed);
        snapshot.runtimeStatuses.insert(
            failed.id,
            ThreadRuntimeStatus::SystemError);

        {
            ProcessListModel model({}, statePath);
            model.setSnapshot(snapshot, now);
            QCOMPARE(model.rowCount(), 1);
        }

        ProcessListModel restored({}, statePath);
        restored.setSnapshot({}, now.addSecs(10));

        QCOMPARE(restored.rowCount(), 1);
        const QModelIndex index = restored.index(0, 0);
        QCOMPARE(
            restored.data(
                        index,
                        ProcessListModel::GroupKindRole)
                .toString(),
            QStringLiteral("project"));
        QCOMPARE(
            restored.data(
                        index,
                        ProcessListModel::GroupTitleRole)
                .toString(),
            QStringLiteral("Companion"));
        const QVariantMap restoredGoal =
            restored.data(
                        index,
                        ProcessListModel::GoalRole)
                .toMap();
        QCOMPARE(
            restoredGoal.value(
                            QStringLiteral("objective"))
                .toString(),
            QStringLiteral("Finish Windows parity"));
        QCOMPARE(
            restoredGoal.value(
                            QStringLiteral("status"))
                .toString(),
            QStringLiteral("blocked"));
        QVERIFY(
            restored.snapshot().first().runtimeStatus
                == ThreadRuntimeStatus::SystemError);
    }

    void stableProcessIdsUpdateWithoutReset()
    {
        ProcessListModel model;
        QAbstractItemModelTester tester(
            &model,
            QAbstractItemModelTester::
                FailureReportingMode::QtTest);
        QSignalSpy resetSpy(
            &model,
            &QAbstractItemModel::modelReset);
        QSignalSpy changedSpy(
            &model,
            &QAbstractItemModel::dataChanged);
        const QDateTime now =
            QDateTime::fromSecsSinceEpoch(
                1'753'337'600,
                QTimeZone::UTC);
        CodexProcessSnapshot snapshot;
        snapshot.jobs.append(
            job(
                QStringLiteral("stable"),
                QStringLiteral("running"),
                now));
        model.setSnapshot(snapshot, now);

        snapshot.jobs[0].instruction =
            QStringLiteral("Updated instruction");
        model.setSnapshot(snapshot, now);

        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(changedSpy.count(), 1);
        QCOMPARE(
            model.data(
                     model.index(0, 0),
                     ProcessListModel::PreviewRole)
                .toString(),
            QStringLiteral("Updated instruction"));
    }
};

QTEST_MAIN(ProcessListModelTests)

#include "ProcessListModelTests.moc"
