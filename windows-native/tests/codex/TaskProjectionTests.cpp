#include "codex/models/BridgeModels.h"
#include "codex/models/CodexModels.h"
#include "codex/state/SidebarOrderingSnapshot.h"
#include "codex/state/TaskProjector.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <optional>
#include <utility>

using namespace companion;

namespace {

QDateTime utcDate(
    int year,
    int month,
    int day,
    int hour = 0,
    int minute = 0,
    int second = 0)
{
    return QDateTime(
        QDate(year, month, day),
        QTime(hour, minute, second),
        QTimeZone::UTC);
}

CodexThreadRecord threadRecord(
    QString id,
    const QDateTime& updatedAt,
    const QDateTime& recencyAt,
    QString title = QStringLiteral("Stored title"),
    QString cwd = QStringLiteral("C:\\Work\\Project"),
    QString firstMessage = QStringLiteral("First request"),
    QString preview = QStringLiteral("Stored preview"))
{
    return {
        std::move(id),
        std::move(title),
        std::move(cwd),
        std::move(firstMessage),
        QStringLiteral("sessions/thread.jsonl"),
        std::move(preview),
        QStringLiteral("gpt-5.6"),
        QStringLiteral("high"),
        updatedAt,
        recencyAt,
    };
}

RolloutSnapshot lifecycleSnapshot(
    LifecycleState state,
    std::optional<QString> turnId = std::nullopt)
{
    RolloutSnapshot snapshot;
    snapshot.lifecycle = TaskLifecycle{state, std::move(turnId)};
    return snapshot;
}

RolloutSnapshot assistantSnapshot(
    QString text,
    std::optional<TaskLifecycle> lifecycle = std::nullopt)
{
    RolloutSnapshot snapshot;
    snapshot.latestAssistantMessage = RolloutMessage{
        CodexMessageRole::Assistant,
        std::move(text),
        QStringLiteral("turn-assistant"),
        std::nullopt,
    };
    snapshot.lifecycle = std::move(lifecycle);
    return snapshot;
}

BridgeGoal goalRecord(
    QString threadId,
    GoalStatus status,
    const QDateTime& updatedAt)
{
    return {
        std::move(threadId),
        QStringLiteral("Finish parity"),
        status,
        std::nullopt,
        0,
        0,
        updatedAt.addSecs(-60).toMSecsSinceEpoch(),
        updatedAt.toMSecsSinceEpoch(),
    };
}

TaskProjectionContext contextAt(const QDateTime& now)
{
    TaskProjectionContext context;
    context.now = now;
    return context;
}

QString normalizedNativePath(QString path)
{
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(std::move(path))));
}

void writeJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("could not write sidebar-state fixture");
    }
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) {
        qFatal("could not finish sidebar-state fixture");
    }
}

QVector<QString> taskIds(const QVector<BridgeTask>& tasks)
{
    QVector<QString> ids;
    ids.reserve(tasks.size());
    for (const BridgeTask& task : tasks) {
        ids.append(task.id);
    }
    return ids;
}

} // namespace

class TaskProjectionTests final : public QObject {
    Q_OBJECT

private slots:
    void statusPrecedence_data()
    {
        QTest::addColumn<bool>("needsApproval");
        QTest::addColumn<int>("lifecycleState");
        QTest::addColumn<int>("ageSeconds");
        QTest::addColumn<int>("expectedStatus");
        QTest::addColumn<bool>("expectsActiveTurn");

        QTest::newRow("approval-before-active")
            << true
            << static_cast<int>(LifecycleState::Active)
            << 10
            << static_cast<int>(TaskStatus::Waiting)
            << true;
        QTest::newRow(
            "active-lifecycle-does-not-extend-recency")
            << false
            << static_cast<int>(LifecycleState::Active)
            << 600
            << static_cast<int>(TaskStatus::Completed)
            << true;
        QTest::newRow("completed-defers-to-recency")
            << false
            << static_cast<int>(LifecycleState::Completed)
            << 10
            << static_cast<int>(TaskStatus::Running)
            << false;
        QTest::newRow("failed-defers-to-recency")
            << false
            << static_cast<int>(LifecycleState::Failed)
            << 10
            << static_cast<int>(TaskStatus::Running)
            << false;
        QTest::newRow("fresh-without-lifecycle")
            << false
            << -1
            << 179
            << static_cast<int>(TaskStatus::Running)
            << false;
        QTest::newRow("stale-boundary-without-lifecycle")
            << false
            << -1
            << 180
            << static_cast<int>(TaskStatus::Completed)
            << false;
    }

    void statusPrecedence()
    {
        QFETCH(bool, needsApproval);
        QFETCH(int, lifecycleState);
        QFETCH(int, ageSeconds);
        QFETCH(int, expectedStatus);
        QFETCH(bool, expectsActiveTurn);

        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        const CodexThreadRecord thread = threadRecord(
            QStringLiteral("thread-status"),
            now.addSecs(-ageSeconds),
            now.addSecs(-ageSeconds));
        RolloutSnapshot rollout;
        if (lifecycleState >= 0) {
            rollout.lifecycle = TaskLifecycle{
                static_cast<LifecycleState>(lifecycleState),
                QStringLiteral("turn-active"),
            };
        }
        TaskProjectionContext context = contextAt(now);
        if (needsApproval) {
            context.pendingApprovalThreadIds.insert(thread.id);
        }

        const BridgeTask task = TaskProjector::project(
            thread, rollout, std::nullopt, context);

        QCOMPARE(static_cast<int>(task.status), expectedStatus);
        QCOMPARE(task.needsApproval, needsApproval);
        QCOMPARE(task.activeTurnId.has_value(), expectsActiveTurn);
        if (expectsActiveTurn) {
            QCOMPARE(
                task.activeTurnId.value(),
                QStringLiteral("turn-active"));
        }
    }

    void projectsTitlePreviewGoalAndMetadata()
    {
        const QDateTime referenceDate = utcDate(2001, 1, 1);
        CodexThreadRecord thread = threadRecord(
            QStringLiteral("thread-metadata"),
            referenceDate,
            referenceDate,
            QStringLiteral("Stored title"),
            QStringLiteral("C:\\Work\\Nested\\feature"),
            QStringLiteral("First request"),
            QStringLiteral("Stored preview"));
        const RolloutSnapshot rollout = assistantSnapshot(
            QStringLiteral("Assistant preview"),
            TaskLifecycle{
                LifecycleState::Active,
                QStringLiteral("turn-live"),
            });
        const BridgeGoal goal{
            thread.id,
            QStringLiteral("Finish parity"),
            GoalStatus::Paused,
            5000,
            125,
            42,
            1000,
            2000,
        };
        TaskProjectionContext context = contextAt(referenceDate);
        context.sessionNames.insert(
            thread.id, QStringLiteral("Indexed title"));
        context.pendingApprovalThreadIds.insert(thread.id);
        context.sidebarOrdering = SidebarOrderingSnapshot(
            {},
            {QStringLiteral("C:\\Work")},
            {},
            {{thread.id, QStringLiteral("C:\\Work")}},
            {{QStringLiteral("C:\\Work"), QStringLiteral("Work label")}});

        const BridgeTask task =
            TaskProjector::project(thread, rollout, goal, context);

        QCOMPARE(task.id, thread.id);
        QCOMPARE(task.title, QStringLiteral("Indexed title"));
        QCOMPARE(task.preview, QStringLiteral("Assistant preview"));
        QCOMPARE(task.updatedAt.secondsSinceReferenceDate, 0.0);
        QCOMPARE(
            task.cwd.value(),
            QStringLiteral("C:\\Work\\Nested\\feature"));
        QCOMPARE(task.status, TaskStatus::Waiting);
        QVERIFY(task.needsApproval);
        QCOMPARE(task.activeTurnId.value(), QStringLiteral("turn-live"));
        QCOMPARE(task.model.value(), QStringLiteral("gpt-5.6"));
        QCOMPARE(task.reasoningEffort.value(), QStringLiteral("high"));
        QCOMPARE(
            task.rolloutPath,
            thread.rolloutPath);
        QVERIFY(task.goal.has_value());
        QVERIFY(task.goal.value() == goal);
        QVERIFY(task.taskGroup.has_value());
        QCOMPARE(task.taskGroup->kind, TaskGroupKind::Project);
        QCOMPARE(task.taskGroup->title, QStringLiteral("Work label"));
        QCOMPARE(
            task.taskGroup->path.value(),
            normalizedNativePath(QStringLiteral("C:\\Work")));
    }

    void goalStatusDrivesProjectedStatus_data()
    {
        QTest::addColumn<int>("goalStatus");
        QTest::addColumn<int>("expectedStatus");

        QTest::newRow("active")
            << static_cast<int>(GoalStatus::Active)
            << static_cast<int>(TaskStatus::Running);
        QTest::newRow("paused")
            << static_cast<int>(GoalStatus::Paused)
            << static_cast<int>(TaskStatus::Waiting);
        QTest::newRow("blocked")
            << static_cast<int>(GoalStatus::Blocked)
            << static_cast<int>(TaskStatus::Waiting);
        QTest::newRow("usage-limited")
            << static_cast<int>(GoalStatus::UsageLimited)
            << static_cast<int>(TaskStatus::Waiting);
        QTest::newRow("budget-limited")
            << static_cast<int>(GoalStatus::BudgetLimited)
            << static_cast<int>(TaskStatus::Waiting);
        QTest::newRow("complete")
            << static_cast<int>(GoalStatus::Complete)
            << static_cast<int>(TaskStatus::Completed);
    }

    void goalStatusDrivesProjectedStatus()
    {
        QFETCH(int, goalStatus);
        QFETCH(int, expectedStatus);

        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        const CodexThreadRecord thread = threadRecord(
            QStringLiteral("goal-thread"),
            now.addSecs(-24 * 60 * 60),
            now.addSecs(-24 * 60 * 60));
        const BridgeGoal goal = goalRecord(
            thread.id,
            static_cast<GoalStatus>(goalStatus),
            now.addSecs(-60));

        const BridgeTask task = TaskProjector::project(
            thread,
            {},
            goal,
            contextAt(now));

        QCOMPARE(static_cast<int>(task.status), expectedStatus);
    }

    void sharedRuntimeStatusOverridesFreshnessAndGoalPresentation()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        const auto projected =
            [&now](
                ThreadRuntimeStatus runtimeStatus,
                const std::optional<BridgeGoal>& goal =
                    std::nullopt) {
                const CodexThreadRecord thread =
                    threadRecord(
                        QStringLiteral("runtime-thread"),
                        now.addSecs(-24 * 60 * 60),
                        now.addSecs(-24 * 60 * 60));
                TaskProjectionContext context =
                    contextAt(now);
                context.runtimeStatuses.insert(
                    thread.id,
                    runtimeStatus);
                return TaskProjector::project(
                    thread,
                    {},
                    goal,
                    context);
            };

        const BridgeTask active =
            projected(ThreadRuntimeStatus::Active);
        QCOMPARE(active.status, TaskStatus::Running);

        const BridgeTask approval =
            projected(
                ThreadRuntimeStatus::
                    WaitingOnApproval);
        QCOMPARE(approval.status, TaskStatus::Waiting);
        QVERIFY(approval.needsApproval);
        QCOMPARE(
            approval.preview,
            QStringLiteral(
                "This task is waiting for your approval."));

        const BridgeTask input =
            projected(
                ThreadRuntimeStatus::
                    WaitingOnUserInput);
        QCOMPARE(input.status, TaskStatus::Waiting);
        QVERIFY(!input.needsApproval);
        QCOMPARE(
            input.preview,
            QStringLiteral(
                "This task is waiting for your input."));

        const BridgeTask systemError =
            projected(ThreadRuntimeStatus::SystemError);
        QCOMPARE(systemError.status, TaskStatus::Failed);

        const BridgeGoal pausedGoal = goalRecord(
            QStringLiteral("runtime-thread"),
            GoalStatus::Paused,
            now.addSecs(-60));
        const BridgeTask activePausedGoal =
            projected(
                ThreadRuntimeStatus::Active,
                pausedGoal);
        QCOMPARE(
            activePausedGoal.status,
            TaskStatus::Running);
    }

    void appliesTitleFallbacksAndGraphemeBounds()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        TaskProjectionContext context = contextAt(now);
        const RolloutSnapshot rollout;

        const QString grapheme = QStringLiteral("e\u0301");
        const QString longIndexed = grapheme.repeated(100);
        CodexThreadRecord indexed = threadRecord(
            QStringLiteral("indexed"),
            now,
            now);
        context.sessionNames.insert(indexed.id, longIndexed);
        const BridgeTask indexedTask = TaskProjector::project(
            indexed, rollout, std::nullopt, context);
        QCOMPARE(
            indexedTask.title,
            grapheme.repeated(93) + QStringLiteral("..."));

        CodexThreadRecord stored = threadRecord(
            QStringLiteral("stored"),
            now,
            now,
            QString(100, QLatin1Char('S')));
        const BridgeTask storedTask = TaskProjector::project(
            stored, rollout, std::nullopt, context);
        QCOMPARE(
            storedTask.title,
            QString(93, QLatin1Char('S')) + QStringLiteral("..."));

        CodexThreadRecord folder = threadRecord(
            QStringLiteral("folder"),
            now,
            now,
            QStringLiteral("Same prompt"),
            QStringLiteral("C:\\Work\\my-feature"),
            QStringLiteral("Same prompt"));
        const BridgeTask folderTask = TaskProjector::project(
            folder, rollout, std::nullopt, context);
        QCOMPARE(folderTask.title, QStringLiteral("My Feature"));

        CodexThreadRecord first = threadRecord(
            QStringLiteral("first"),
            now,
            now,
            QString(),
            QString(),
            QString(70, QLatin1Char('F')));
        const BridgeTask firstTask = TaskProjector::project(
            first, rollout, std::nullopt, context);
        QCOMPARE(
            firstTask.title,
            QString(61, QLatin1Char('F')) + QStringLiteral("..."));

        CodexThreadRecord fallback = threadRecord(
            QStringLiteral("fallback"),
            now,
            now,
            QString(),
            QString(),
            QString(),
            QString());
        const BridgeTask fallbackTask = TaskProjector::project(
            fallback, rollout, std::nullopt, context);
        QCOMPARE(fallbackTask.title, QStringLiteral("Codex task"));
    }

    void appliesPreviewFallbacks()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        const TaskProjectionContext context = contextAt(now);

        CodexThreadRecord thread = threadRecord(
            QStringLiteral("preview"), now, now);
        const BridgeTask assistant = TaskProjector::project(
            thread,
            assistantSnapshot(QStringLiteral("Latest assistant")),
            std::nullopt,
            context);
        QCOMPARE(assistant.preview, QStringLiteral("Latest assistant"));

        const BridgeTask stored = TaskProjector::project(
            thread, {}, std::nullopt, context);
        QCOMPARE(stored.preview, QStringLiteral("Stored preview"));

        thread.preview.clear();
        const BridgeTask first = TaskProjector::project(
            thread, {}, std::nullopt, context);
        QCOMPARE(first.preview, QStringLiteral("First request"));

        thread.firstUserMessage.clear();
        const BridgeTask empty = TaskProjector::project(
            thread, {}, std::nullopt, context);
        QCOMPARE(empty.preview, QStringLiteral("No messages yet"));
    }

    void readsSidebarStateAndGroupsWindowsPaths()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString statePath =
            directory.filePath(QStringLiteral(".codex-global-state.json"));
        writeJson(
            statePath,
            {
                {QStringLiteral("pinned-thread-ids"),
                 QJsonArray{
                     QStringLiteral("pinned"),
                     QStringLiteral("pinned"),
                 }},
                {QStringLiteral("project-order"),
                 QJsonArray{
                     QStringLiteral("C:\\Work"),
                     QStringLiteral("C:/Work/Nested"),
                 }},
                {QStringLiteral("projectless-thread-ids"),
                 QJsonArray{QStringLiteral("chat-thread")}},
                {QStringLiteral("thread-workspace-root-hints"),
                 QJsonObject{
                     {QStringLiteral("hinted"),
                      QStringLiteral("c:/work/nested")},
                 }},
                {QStringLiteral("electron-workspace-root-labels"),
                 QJsonObject{
                     {QStringLiteral("C:\\WORK"),
                      QStringLiteral(" Work root ")},
                     {QStringLiteral("C:\\Work\\Nested"),
                      QStringLiteral("Nested label")},
                 }},
            });

        const SidebarOrderingSnapshot snapshot =
            SidebarOrderingSnapshot::read(statePath);

        QVERIFY(snapshot.isPinned(QStringLiteral("pinned")));
        const BridgeTaskGroup hinted = snapshot.taskGroup(
            QStringLiteral("hinted"),
            QStringLiteral("C:\\Work\\Nested\\child"));
        QCOMPARE(hinted.kind, TaskGroupKind::Project);
        QCOMPARE(hinted.title, QStringLiteral("Nested label"));
        QCOMPARE(
            hinted.path.value(),
            normalizedNativePath(QStringLiteral("C:\\Work\\Nested")));

        const BridgeTaskGroup projectless = snapshot.taskGroup(
            QStringLiteral("chat-thread"),
            QStringLiteral("C:\\Work\\Nested"));
        QCOMPARE(projectless.kind, TaskGroupKind::Chats);
        QVERIFY(!projectless.path.has_value());

        const BridgeTaskGroup codexDocuments = snapshot.taskGroup(
            QStringLiteral("documents"),
            QStringLiteral(
                "C:\\Users\\sidfi\\Documents\\Codex\\2026-07-21"));
        QCOMPARE(codexDocuments.kind, TaskGroupKind::Chats);

        const BridgeTaskGroup fallbackProject = snapshot.taskGroup(
            QStringLiteral("other"),
            QStringLiteral("C:\\Other\\my-project"));
        QCOMPARE(fallbackProject.kind, TaskGroupKind::Project);
        QCOMPARE(fallbackProject.title, QStringLiteral("my-project"));
        QCOMPARE(
            fallbackProject.path.value(),
            normalizedNativePath(QStringLiteral("C:\\Other\\my-project")));

        const BridgeTaskGroup noDirectory = snapshot.taskGroup(
            QStringLiteral("no-directory"), std::nullopt);
        QCOMPARE(noDirectory.kind, TaskGroupKind::Chats);
    }

    void missingOrMalformedSidebarStateUsesEmptySnapshot()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString missingPath =
            directory.filePath(QStringLiteral("missing.json"));
        const QString malformedPath =
            directory.filePath(QStringLiteral("malformed.json"));
        QFile malformed(malformedPath);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QCOMPARE(malformed.write("{malformed"), qint64(10));
        malformed.close();

        const SidebarOrderingSnapshot missing =
            SidebarOrderingSnapshot::read(missingPath);
        const SidebarOrderingSnapshot invalid =
            SidebarOrderingSnapshot::read(malformedPath);

        QVERIFY(!missing.isPinned(QStringLiteral("anything")));
        QVERIFY(!invalid.isPinned(QStringLiteral("anything")));
        QCOMPARE(
            missing.taskGroup(QStringLiteral("thread"), std::nullopt).kind,
            TaskGroupKind::Chats);
        QCOMPARE(
            invalid.taskGroup(QStringLiteral("thread"), std::nullopt).kind,
            TaskGroupKind::Chats);
    }

    void projectAllUsesDeterministicSidebarOrder()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        CodexStateSnapshot snapshot;
        const auto addThread =
            [&snapshot, &now](
                const QString& id,
                const QString& cwd,
                int recencyAge,
                int updatedAge = 100) {
                snapshot.threads.append(threadRecord(
                    id,
                    now.addSecs(-updatedAge),
                    now.addSecs(-recencyAge),
                    id,
                    cwd,
                    QStringLiteral("Request"),
                    QStringLiteral("Preview")));
            };

        addThread(QStringLiteral("attention"), QStringLiteral("C:\\B"), 900);
        addThread(QStringLiteral("pinned"), QStringLiteral("C:\\B"), 800);
        addThread(QStringLiteral("project-a"), QStringLiteral("C:\\A"), 700);
        addThread(QStringLiteral("b-waiting"), QStringLiteral("C:\\B"), 600);
        addThread(QStringLiteral("b-running"), QStringLiteral("C:\\B"), 500);
        addThread(QStringLiteral("b-failed"), QStringLiteral("C:\\B"), 400);
        addThread(
            QStringLiteral("b-completed-new"),
            QStringLiteral("C:\\B"),
            100,
            181);
        addThread(
            QStringLiteral("b-completed-alpha"),
            QStringLiteral("C:\\B"),
            200,
            182);
        addThread(
            QStringLiteral("b-completed-zeta"),
            QStringLiteral("C:\\B"),
            200,
            182);

        QHash<QString, RolloutSnapshot> rollouts;
        for (const CodexThreadRecord& thread : snapshot.threads) {
            rollouts.insert(
                thread.id,
                lifecycleSnapshot(LifecycleState::Completed));
        }
        rollouts.insert(
            QStringLiteral("b-running"),
            lifecycleSnapshot(
                LifecycleState::Active,
                QStringLiteral("turn-running")));
        rollouts.insert(
            QStringLiteral("b-failed"),
            lifecycleSnapshot(LifecycleState::Failed));

        const BridgeGoal waitingGoal{
            QStringLiteral("b-waiting"),
            QStringLiteral("Waiting goal"),
            GoalStatus::Active,
            std::nullopt,
            0,
            0,
            0,
            0,
        };
        QHash<QString, BridgeGoal> goals{
            {waitingGoal.threadId, waitingGoal},
        };

        TaskProjectionContext context = contextAt(now);
        context.pendingApprovalThreadIds.insert(
            QStringLiteral("b-waiting"));
        context.attentionPromotedThreadIds.insert(
            QStringLiteral("attention"));
        context.runtimeStatuses.insert(
            QStringLiteral("b-running"),
            ThreadRuntimeStatus::Active);
        context.runtimeStatuses.insert(
            QStringLiteral("b-failed"),
            ThreadRuntimeStatus::SystemError);
        context.sidebarOrdering = SidebarOrderingSnapshot(
            {QStringLiteral("pinned")},
            {QStringLiteral("C:\\A"), QStringLiteral("C:\\B")});

        const QVector<BridgeTask> tasks = TaskProjector::projectAll(
            snapshot, rollouts, goals, context);

        QCOMPARE(
            taskIds(tasks),
            QVector<QString>({
                QStringLiteral("attention"),
                QStringLiteral("pinned"),
                QStringLiteral("project-a"),
                QStringLiteral("b-waiting"),
                QStringLiteral("b-running"),
                QStringLiteral("b-failed"),
                QStringLiteral("b-completed-new"),
                QStringLiteral("b-completed-alpha"),
                QStringLiteral("b-completed-zeta"),
            }));
        QVERIFY(tasks.at(3).goal.has_value());
        QVERIFY(tasks.at(3).goal.value() == waitingGoal);
    }

    void projectAllOrdersByApprovalAndDatabaseFreshness()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        CodexStateSnapshot snapshot;
        snapshot.threads = {
            threadRecord(
                QStringLiteral("old-active"),
                now.addSecs(-600),
                now.addSecs(-1)),
            threadRecord(
                QStringLiteral("fresh-completed"),
                now.addSecs(-179),
                now.addSecs(-900)),
            threadRecord(
                QStringLiteral("boundary-active"),
                now.addSecs(-180),
                now),
            threadRecord(
                QStringLiteral("pending"),
                now.addSecs(-900),
                now.addSecs(-900)),
        };

        QHash<QString, RolloutSnapshot> rollouts{
            {QStringLiteral("old-active"),
             lifecycleSnapshot(
                 LifecycleState::Active,
                 QStringLiteral("turn-old"))},
            {QStringLiteral("fresh-completed"),
             lifecycleSnapshot(LifecycleState::Completed)},
            {QStringLiteral("boundary-active"),
             lifecycleSnapshot(
                 LifecycleState::Active,
                 QStringLiteral("turn-boundary"))},
            {QStringLiteral("pending"),
             lifecycleSnapshot(LifecycleState::Completed)},
        };

        TaskProjectionContext context = contextAt(now);
        context.pendingApprovalThreadIds.insert(
            QStringLiteral("pending"));
        context.runtimeStatuses.insert(
            QStringLiteral("old-active"),
            ThreadRuntimeStatus::Active);
        context.runtimeStatuses.insert(
            QStringLiteral("boundary-active"),
            ThreadRuntimeStatus::Active);

        const QVector<BridgeTask> tasks = TaskProjector::projectAll(
            snapshot, rollouts, {}, context);

        QCOMPARE(
            taskIds(tasks),
            QVector<QString>({
                QStringLiteral("pending"),
                QStringLiteral("boundary-active"),
                QStringLiteral("old-active"),
                QStringLiteral("fresh-completed"),
            }));
        QCOMPARE(tasks.at(1).status, TaskStatus::Running);
        QCOMPARE(tasks.at(2).status, TaskStatus::Running);
        QCOMPARE(tasks.at(3).status, TaskStatus::Running);
    }

    void projectAllRetainsOnlyCurrentProcessActivity()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        CodexStateSnapshot snapshot;
        const auto addThread =
            [&snapshot, &now](const QString& id, int ageSeconds) {
                snapshot.threads.append(threadRecord(
                    id,
                    now.addSecs(-ageSeconds),
                    now.addSecs(-ageSeconds)));
            };

        addThread(QStringLiteral("active-old"), 2 * 60 * 60);
        addThread(QStringLiteral("approval-old"), 2 * 60 * 60);
        addThread(QStringLiteral("failed-old"), 2 * 60 * 60);
        addThread(QStringLiteral("idle-history"), 24 * 60 * 60);
        addThread(QStringLiteral("not-loaded-history"), 24 * 60 * 60);
        addThread(QStringLiteral("completed-visible"), 8 * 60 - 1);
        addThread(QStringLiteral("completed-boundary"), 8 * 60);
        addThread(QStringLiteral("completed-expired"), 8 * 60 + 1);
        addThread(QStringLiteral("goal-active-old"), 2 * 60 * 60);
        addThread(QStringLiteral("goal-paused-old"), 2 * 60 * 60);
        addThread(QStringLiteral("goal-paused-recent"), 2 * 60 * 60);
        addThread(QStringLiteral("goal-complete-visible"), 2 * 60 * 60);
        addThread(QStringLiteral("goal-complete-boundary"), 2 * 60 * 60);

        QHash<QString, RolloutSnapshot> rollouts{
            {QStringLiteral("active-old"),
             lifecycleSnapshot(
                 LifecycleState::Active,
                 QStringLiteral("turn-active"))},
            {QStringLiteral("approval-old"),
             lifecycleSnapshot(LifecycleState::Completed)},
            {QStringLiteral("failed-old"),
             lifecycleSnapshot(LifecycleState::Failed)},
            {QStringLiteral("completed-visible"),
             lifecycleSnapshot(LifecycleState::Completed)},
            {QStringLiteral("completed-boundary"),
             lifecycleSnapshot(LifecycleState::Completed)},
            {QStringLiteral("completed-expired"),
             lifecycleSnapshot(LifecycleState::Completed)},
        };
        QHash<QString, BridgeGoal> goals{
            {QStringLiteral("goal-active-old"),
             goalRecord(
                 QStringLiteral("goal-active-old"),
                 GoalStatus::Active,
                 now.addSecs(-2 * 60 * 60))},
            {QStringLiteral("goal-paused-old"),
             goalRecord(
                 QStringLiteral("goal-paused-old"),
                 GoalStatus::Paused,
                 now.addSecs(-2 * 60 * 60))},
            {QStringLiteral("goal-paused-recent"),
             goalRecord(
                 QStringLiteral("goal-paused-recent"),
                 GoalStatus::Paused,
                 now.addSecs(-(8 * 60 - 1)))},
            {QStringLiteral("goal-complete-visible"),
             goalRecord(
                 QStringLiteral("goal-complete-visible"),
                 GoalStatus::Complete,
                 now.addSecs(-(30 * 60 - 1)))},
            {QStringLiteral("goal-complete-boundary"),
             goalRecord(
                 QStringLiteral("goal-complete-boundary"),
                 GoalStatus::Complete,
                 now.addSecs(-30 * 60))},
        };
        TaskProjectionContext context = contextAt(now);
        context.pendingApprovalThreadIds.insert(
            QStringLiteral("approval-old"));
        context.runtimeStatuses.insert(
            QStringLiteral("active-old"),
            ThreadRuntimeStatus::Active);
        context.runtimeStatuses.insert(
            QStringLiteral("failed-old"),
            ThreadRuntimeStatus::SystemError);
        context.runtimeStatuses.insert(
            QStringLiteral("idle-history"),
            ThreadRuntimeStatus::Idle);
        context.runtimeStatuses.insert(
            QStringLiteral("not-loaded-history"),
            ThreadRuntimeStatus::NotLoaded);

        const QVector<BridgeTask> tasks = TaskProjector::projectAll(
            snapshot, rollouts, goals, context);
        const QVector<QString> ids = taskIds(tasks);

        QCOMPARE(tasks.size(), 8);
        QVERIFY(ids.contains(QStringLiteral("active-old")));
        QVERIFY(ids.contains(QStringLiteral("approval-old")));
        QVERIFY(ids.contains(QStringLiteral("failed-old")));
        QVERIFY(ids.contains(QStringLiteral("completed-visible")));
        QVERIFY(ids.contains(QStringLiteral("goal-active-old")));
        QVERIFY(ids.contains(QStringLiteral("goal-paused-old")));
        QVERIFY(ids.contains(QStringLiteral("goal-paused-recent")));
        QVERIFY(ids.contains(QStringLiteral("goal-complete-visible")));
        QVERIFY(!ids.contains(QStringLiteral("completed-boundary")));
        QVERIFY(!ids.contains(QStringLiteral("completed-expired")));
        QVERIFY(!ids.contains(QStringLiteral("goal-complete-boundary")));
        QVERIFY(!ids.contains(QStringLiteral("idle-history")));
        QVERIFY(!ids.contains(QStringLiteral("not-loaded-history")));
    }

    void projectAllKeepsFiftyThreadsForTheMergedFeed()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        CodexStateSnapshot snapshot;
        QHash<QString, RolloutSnapshot> rollouts;
        for (int index = 0; index < 52; ++index) {
            const QString id = QStringLiteral("active-%1")
                                   .arg(index, 2, 10, QLatin1Char('0'));
            snapshot.threads.append(threadRecord(
                id,
                now.addSecs(-index),
                now.addSecs(-index)));
            rollouts.insert(
                id,
                lifecycleSnapshot(
                    LifecycleState::Active,
                    QStringLiteral("turn-%1").arg(index)));
        }

        const QVector<BridgeTask> tasks = TaskProjector::projectAll(
            snapshot, rollouts, {}, contextAt(now));

        QCOMPARE(tasks.size(), 50);
        QCOMPARE(tasks.first().id, QStringLiteral("active-00"));
        QCOMPARE(tasks.last().id, QStringLiteral("active-49"));
    }

    void duplicateProjectAliasesKeepLaterProjectsAheadOfUnlisted()
    {
        const QDateTime now = utcDate(2026, 7, 21, 20, 0, 0);
        CodexStateSnapshot snapshot;
        snapshot.threads = {
            threadRecord(
                QStringLiteral("project-b"),
                now.addSecs(-100),
                now.addSecs(-600),
                QStringLiteral("Project B"),
                QStringLiteral("C:\\B")),
            threadRecord(
                QStringLiteral("unlisted"),
                now.addSecs(-100),
                now.addSecs(-10),
                QStringLiteral("Unlisted"),
                QStringLiteral("C:\\Z")),
        };

        QHash<QString, RolloutSnapshot> rollouts{
            {QStringLiteral("project-b"),
             lifecycleSnapshot(LifecycleState::Completed)},
            {QStringLiteral("unlisted"),
             lifecycleSnapshot(LifecycleState::Completed)},
        };
        TaskProjectionContext context = contextAt(now);
        context.sidebarOrdering = SidebarOrderingSnapshot(
            {},
            {
                QStringLiteral("C:\\A"),
                QStringLiteral("c:/a"),
                QStringLiteral("C:\\B"),
            });

        const QVector<BridgeTask> tasks = TaskProjector::projectAll(
            snapshot, rollouts, {}, context);

        QCOMPARE(
            taskIds(tasks),
            QVector<QString>({
                QStringLiteral("project-b"),
                QStringLiteral("unlisted"),
            }));
    }
};

QTEST_GUILESS_MAIN(TaskProjectionTests)
#include "TaskProjectionTests.moc"
