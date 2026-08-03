#include "codex/runtime/TaskSnapshotLoader.h"
#include "codex/state/RolloutReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>

#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

using namespace companion;

namespace companion::detail {
struct TaskSnapshotLoaderTestAccess;
}

template <typename Type, typename = void>
struct HasProductionTaskSnapshotLoaderTestFactory
    : std::false_type {
};

template <typename Type>
struct HasProductionTaskSnapshotLoaderTestFactory<
    Type,
    std::void_t<decltype(
        Type::create(
            std::declval<CodexEnvironment>(),
            std::declval<TaskProjectionStateProvider>(),
            std::declval<TaskNowProvider>(),
            std::declval<std::function<void(int, qsizetype)>>()))>>
    : std::true_type {
};

static_assert(
    !HasProductionTaskSnapshotLoaderTestFactory<
        companion::detail::TaskSnapshotLoaderTestAccess>::value,
    "The production task-snapshot header must not expose "
    "a callable test-access factory.");

namespace companion::detail {

struct TaskSnapshotLoaderTestAccess final {
    using Phase = TaskSnapshotLoader::LoadPhase;
    using Probe = TaskSnapshotLoader::LoadPhaseProbe;

    static std::unique_ptr<TaskSnapshotLoader> create(
        CodexEnvironment environment,
        TaskProjectionStateProvider projectionStateProvider,
        TaskNowProvider nowProvider,
        Probe probe)
    {
        return std::unique_ptr<TaskSnapshotLoader>(
            new TaskSnapshotLoader(
                std::move(environment),
                std::move(projectionStateProvider),
                std::move(nowProvider),
                {},
                std::move(probe)));
    }
};

} // namespace companion::detail

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

QString connectionName(QStringView prefix)
{
    return prefix.toString()
        + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

class TestDatabase final {
public:
    TestDatabase(const QString& path, QStringView prefix)
        : connectionName_(connectionName(prefix)),
          database_(QSqlDatabase::addDatabase(
              QStringLiteral("QSQLITE"), connectionName_))
    {
        database_.setDatabaseName(path);
        if (!database_.open()) {
            qFatal(
                "could not open fixture database: %s",
                qPrintable(database_.lastError().text()));
        }
    }

    ~TestDatabase()
    {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(connectionName_);
    }

    TestDatabase(const TestDatabase&) = delete;
    TestDatabase& operator=(const TestDatabase&) = delete;

    void execute(const QString& sql)
    {
        QSqlQuery query(database_);
        if (!query.exec(sql)) {
            qFatal(
                "fixture SQL failed: %s",
                qPrintable(query.lastError().text()));
        }
    }

    void begin()
    {
        execute(QStringLiteral("begin"));
    }

    void commit()
    {
        execute(QStringLiteral("commit"));
    }

    void insertThread(
        const QString& id,
        const QString& rolloutPath,
        const QDateTime& updatedAt,
        const QDateTime& recencyAt,
        const QString& title = QStringLiteral("Stored title"),
        const QString& cwd = QStringLiteral("C:\\Work\\Project"),
        const QString& firstMessage = QStringLiteral("First request"),
        const QString& preview = QStringLiteral("Stored preview"))
    {
        QSqlQuery query(database_);
        query.prepare(QString::fromUtf8(R"SQL(
            insert into threads (
                id, rollout_path, updated_at, updated_at_ms, recency_at_ms,
                source, cwd, title, first_user_message, archived, preview,
                model, reasoning_effort
            ) values (
                :id, :rollout, :updatedSeconds, :updatedMilliseconds,
                :recencyMilliseconds, 'desktop', :cwd, :title, :first,
                0, :preview, 'gpt-5.6', 'high'
            )
        )SQL"));
        query.bindValue(QStringLiteral(":id"), id);
        query.bindValue(QStringLiteral(":rollout"), rolloutPath);
        query.bindValue(
            QStringLiteral(":updatedSeconds"),
            updatedAt.toSecsSinceEpoch());
        query.bindValue(
            QStringLiteral(":updatedMilliseconds"),
            updatedAt.toMSecsSinceEpoch());
        query.bindValue(
            QStringLiteral(":recencyMilliseconds"),
            recencyAt.toMSecsSinceEpoch());
        query.bindValue(QStringLiteral(":cwd"), cwd);
        query.bindValue(QStringLiteral(":title"), title);
        query.bindValue(QStringLiteral(":first"), firstMessage);
        query.bindValue(QStringLiteral(":preview"), preview);
        if (!query.exec()) {
            qFatal(
                "thread fixture insert failed: %s",
                qPrintable(query.lastError().text()));
        }
    }

    void insertJob(
        const QString& id,
        const QString& threadId,
        const QDateTime& now)
    {
        QSqlQuery query(database_);
        query.prepare(QString::fromUtf8(R"SQL(
            insert into agent_jobs (
                id, name, status, instruction, created_at, updated_at,
                started_at, last_error
            ) values (
                :id, 'Sprite work', 'running', 'Build sprites',
                :created, :updated, :started, 'Needs review'
            )
        )SQL"));
        query.bindValue(QStringLiteral(":id"), id);
        query.bindValue(
            QStringLiteral(":created"),
            now.addSecs(-120).toSecsSinceEpoch());
        query.bindValue(
            QStringLiteral(":updated"),
            now.toSecsSinceEpoch());
        query.bindValue(
            QStringLiteral(":started"),
            now.addSecs(-60).toSecsSinceEpoch());
        if (!query.exec()) {
            qFatal(
                "job fixture insert failed: %s",
                qPrintable(query.lastError().text()));
        }

        query.prepare(QString::fromUtf8(R"SQL(
            insert into agent_job_items (
                job_id, item_id, assigned_thread_id, updated_at
            ) values (:job, 'item-1', :thread, :updated)
        )SQL"));
        query.bindValue(QStringLiteral(":job"), id);
        query.bindValue(QStringLiteral(":thread"), threadId);
        query.bindValue(
            QStringLiteral(":updated"),
            now.toSecsSinceEpoch());
        if (!query.exec()) {
            qFatal(
                "job-item fixture insert failed: %s",
            qPrintable(query.lastError().text()));
        }
    }

private:
    QString connectionName_;
    QSqlDatabase database_;
};

void createThreadSchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table threads (
            id text primary key,
            rollout_path text not null,
            updated_at integer not null,
            updated_at_ms integer,
            recency_at_ms integer,
            source text not null,
            cwd text not null,
            title text not null,
            first_user_message text not null,
            archived integer not null,
            preview text not null,
            model text,
            reasoning_effort text
        )
    )SQL"));
}

void createGoalSchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table thread_goals (
            thread_id text not null,
            status text not null,
            updated_at_ms integer
        )
    )SQL"));
}

void createFullGoalSchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table thread_goals (
            thread_id text primary key,
            goal_id text not null,
            objective text not null,
            status text not null,
            token_budget integer,
            tokens_used integer not null default 0,
            time_used_seconds integer not null default 0,
            created_at_ms integer not null,
            updated_at_ms integer not null
        )
    )SQL"));
}

void createJobSchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table agent_jobs (
            id text primary key,
            name text not null,
            status text not null,
            instruction text not null,
            created_at integer not null,
            updated_at integer not null,
            started_at integer,
            last_error text
        )
    )SQL"));
    database.execute(QString::fromUtf8(R"SQL(
        create table agent_job_items (
            job_id text not null,
            item_id text not null,
            assigned_thread_id text,
            updated_at integer not null,
            primary key (job_id, item_id)
        )
    )SQL"));
}

void createMalformedJobSchema(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        create table agent_jobs (
            id text primary key,
            name text not null,
            status text not null,
            created_at integer not null,
            updated_at integer not null
        )
    )SQL"));
}

class Fixture final {
public:
    Fixture()
    {
        if (!directory.isValid()) {
            qFatal("could not create task snapshot fixture");
        }

        environment.homeDirectory =
            directory.filePath(QStringLiteral("home"));
        environment.localAppData =
            directory.filePath(QStringLiteral("local"));
        environment.codexHome =
            directory.filePath(QStringLiteral("codex"));
        environment.stateDatabase =
            QDir(environment.codexHome).filePath(
                QStringLiteral("state_5.sqlite"));
        environment.goalDatabase =
            QDir(environment.codexHome).filePath(
                QStringLiteral("goals_1.sqlite"));
        environment.sessionIndex =
            QDir(environment.codexHome).filePath(
                QStringLiteral("session_index.jsonl"));
        environment.rolloutRoot =
            QDir(environment.codexHome).filePath(
                QStringLiteral("sessions"));
        environment.configToml =
            QDir(environment.codexHome).filePath(
                QStringLiteral("config.toml"));
        environment.petRoot =
            directory.filePath(QStringLiteral("pet"));
        environment.codexBinRoot =
            directory.filePath(QStringLiteral("bin"));

        for (const QString& path : {
                 environment.homeDirectory,
                 environment.localAppData,
                 environment.codexHome,
                 environment.rolloutRoot,
             }) {
            if (!QDir().mkpath(path)) {
                qFatal("could not create task snapshot fixture directory");
            }
        }
    }

    QTemporaryDir directory;
    CodexEnvironment environment;
};

void writeFile(
    const QString& path,
    QByteArrayView bytes,
    std::optional<QDateTime> modifiedAt = std::nullopt)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        qFatal("could not create fixture file directory");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("could not open fixture file");
    }
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
        qFatal("could not finish fixture file");
    }
    if (!file.flush()) {
        qFatal("could not flush fixture file");
    }
    if (modifiedAt.has_value()
        && !file.setFileTime(
            *modifiedAt,
            QFileDevice::FileModificationTime)) {
        qFatal("could not set fixture file time");
    }
}

void writeJson(const QString& path, const QJsonObject& object)
{
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    writeFile(path, bytes);
}

QByteArray showApprovalLine(
    const QString& timestamp,
    const QString& threadId,
    qint64 requestId,
    const QString& kind)
{
    return QStringLiteral(
               "%1 [desktop-notifications] show approval "
               "conversationId=%2 requestId=%3 kind=%4\n")
        .arg(
            timestamp,
            threadId,
            QString::number(requestId),
            kind)
        .toUtf8();
}

QString stableLogsRoot(const QString& localAppData)
{
    return QDir(localAppData).filePath(QStringLiteral(
        "Packages/OpenAI.Codex_2p2nqsd0c76g0/"
        "LocalCache/Local/Codex/Logs"));
}

QByteArray rolloutMessage(
    const QString& role,
    const QString& fragmentType,
    const QString& text)
{
    return QJsonDocument(QJsonObject{
        {
            QStringLiteral("timestamp"),
            QStringLiteral("2026-07-22T11:59:00.000Z"),
        },
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {
            QStringLiteral("payload"),
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("message")},
                {QStringLiteral("role"), role},
                {
                    QStringLiteral("content"),
                    QJsonArray{
                        QJsonObject{
                            {QStringLiteral("type"), fragmentType},
                            {QStringLiteral("text"), text},
                        },
                    },
                },
            },
        },
    }).toJson(QJsonDocument::Compact);
}

QByteArray lifecycleLine(
    const QString& type,
    const QString& turnId = {})
{
    QJsonObject payload{{QStringLiteral("type"), type}};
    if (!turnId.isEmpty()) {
        payload.insert(QStringLiteral("turn_id"), turnId);
    }
    return QJsonDocument(QJsonObject{
        {
            QStringLiteral("timestamp"),
            QStringLiteral("2026-07-22T11:59:00.000Z"),
        },
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"), payload},
    }).toJson(QJsonDocument::Compact);
}

void writeRollout(
    const QString& path,
    const QString& assistantText,
    const QString& lifecycleType = QStringLiteral("task_completed"))
{
    QByteArray bytes;
    bytes.append(
        rolloutMessage(
            QStringLiteral("user"),
            QStringLiteral("input_text"),
            QStringLiteral("User request")));
    bytes.append('\n');
    bytes.append(
        rolloutMessage(
            QStringLiteral("assistant"),
            QStringLiteral("output_text"),
            assistantText));
    bytes.append('\n');
    bytes.append(
        lifecycleLine(
            lifecycleType,
            lifecycleType.contains(QStringLiteral("started"))
                ? QStringLiteral("turn-live")
                : QString()));
    bytes.append('\n');
    writeFile(path, bytes);
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

const BridgeTask& taskFor(
    const CodexProcessSnapshot& snapshot,
    QStringView threadId)
{
    for (const BridgeTask& task : snapshot.tasks) {
        if (task.id == threadId) {
            return task;
        }
    }
    qFatal("missing expected task");
    Q_UNREACHABLE_RETURN(tasks.first());
}

void verifyCanceled(const CompanionError& error)
{
    QCOMPARE(
        error.code,
        QStringLiteral("codex.operation_canceled"));
    QCOMPARE(
        error.message,
        QStringLiteral("The Codex operation was canceled."));
    QVERIFY(!error.retryable);
    QVERIFY(error.context.isEmpty());
}

void verifyUnavailable(const CompanionError& error)
{
    QCOMPARE(
        error.code,
        QStringLiteral("codex.task_snapshot_unavailable"));
    QCOMPARE(
        error.message,
        QStringLiteral("Could not read Codex task state."));
    QVERIFY(error.retryable);
    QVERIFY(error.context.isEmpty());
}

} // namespace

class TaskSnapshotLoaderTests final : public QObject {
    Q_OBJECT

private slots:
    void productionLoaderUsesApprovalStateAndSourceOrdering()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"production-loader");
            createThreadSchema(database);
            createJobSchema(database);
            database.insertThread(
                QStringLiteral("thread-pending"),
                QStringLiteral("sessions/pending.jsonl"),
                now.addSecs(-600),
                now.addSecs(-300),
                QStringLiteral("Pending stored"));
            database.insertThread(
                QStringLiteral("thread-pinned"),
                QStringLiteral("sessions/pinned.jsonl"),
                now.addSecs(-420),
                now.addSecs(-200),
                QStringLiteral("Pinned stored"));
            database.insertThread(
                QStringLiteral("thread-fresh"),
                QStringLiteral("sessions/fresh.jsonl"),
                now.addSecs(-10),
                now.addSecs(-100),
                QStringLiteral("Fresh stored"));
            database.insertJob(
                QStringLiteral("job-1"),
                QStringLiteral("thread-pending"),
                now);
        }

        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/pending.jsonl")),
            QStringLiteral("Pending assistant"));
        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/pinned.jsonl")),
            QStringLiteral("Pinned assistant"));
        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/fresh.jsonl")),
            QStringLiteral("Fresh assistant"));
        writeFile(
            fixture.environment.sessionIndex,
            QByteArrayLiteral(
                "{\"id\":\"thread-pending\","
                "\"thread_name\":\"Pending indexed\"}\n"));
        writeJson(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral(".codex-global-state.json")),
            {
                {
                    QStringLiteral("pinned-thread-ids"),
                    QJsonArray{QStringLiteral("thread-pinned")},
                },
            });
        writeFile(
            QDir(stableLogsRoot(fixture.environment.localAppData))
                .filePath(
                    QStringLiteral("session-a-t0-main.log")),
            showApprovalLine(
                QStringLiteral("2026-07-22T11:59:00.000Z"),
                QStringLiteral("thread-pending"),
                42,
                QStringLiteral("fileChange")),
            now.addSecs(-1));

        TaskSnapshotLoader loader(
            fixture.environment,
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(
            taskIds(result.value().tasks),
            QVector<QString>({
                QStringLiteral("thread-pending"),
                QStringLiteral("thread-pinned"),
                QStringLiteral("thread-fresh"),
            }));
        QCOMPARE(
            taskFor(result.value(), u"thread-pending").title,
            QStringLiteral("Pending indexed"));
        QVERIFY(
            taskFor(result.value(), u"thread-pending").needsApproval);
        QCOMPARE(result.value().pendingApprovals.size(), 1);
        const PendingApproval pending =
            result.value().pendingApprovals.value(
                QStringLiteral("thread-pending"));
        QCOMPARE(pending.threadId, QStringLiteral("thread-pending"));
        QCOMPARE(pending.requestId, qint64(42));
        QCOMPARE(
            pending.method,
            PendingApprovalMethod::FileChange);
        QVERIFY(!pending.proposedExecpolicyAmendment.has_value());
        QCOMPARE(result.value().jobs.size(), 1);
        QCOMPARE(result.value().jobs.first().id, QStringLiteral("job-1"));
        QCOMPARE(
            result.value().jobs.first().threadId.value(),
            QStringLiteral("thread-pending"));
    }

    void malformedJobsDoNotHideTasks()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"malformed-jobs");
            createThreadSchema(database);
            createMalformedJobSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/a.jsonl"),
                now.addSecs(-20),
                now.addSecs(-20));
        }
        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/a.jsonl")),
            QStringLiteral("Assistant"));

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().id,
            QStringLiteral("thread-a"));
        QVERIFY(result.value().jobs.isEmpty());
    }

    void staleMalformedJobDoesNotHideCurrentJobs()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"stale-malformed-job");
            createThreadSchema(database);
            database.execute(QString::fromUtf8(R"SQL(
                create table agent_jobs (
                    id text primary key,
                    name text,
                    status text not null,
                    instruction text not null,
                    created_at integer not null,
                    updated_at integer not null
                )
            )SQL"));
            database.execute(QStringLiteral(
                "insert into agent_jobs values "
                "('current', 'Current work', 'running', 'Continue work', "
                "%1, %2), "
                "('stale', '', 'completed', 'Old work', %3, %4)")
                .arg(
                    now.addSecs(-120).toSecsSinceEpoch())
                .arg(now.toSecsSinceEpoch())
                .arg(
                    now.addSecs(-1200).toSecsSinceEpoch())
                .arg(
                    now.addSecs(-600).toSecsSinceEpoch()));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        bool foundCurrent = false;
        for (const CodexJobRecord& job :
             result.value().jobs) {
            foundCurrent =
                foundCurrent
                || job.id == QStringLiteral("current");
        }
        QVERIFY(foundCurrent);
    }

    void cachedGoalsAttachWithoutChangingProjectedOrder()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"cached-goals");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-new"),
                QStringLiteral("sessions/new.jsonl"),
                now.addSecs(-20),
                now.addSecs(-20));
            database.insertThread(
                QStringLiteral("thread-old"),
                QStringLiteral("sessions/old.jsonl"),
                now.addSecs(-600),
                now.addSecs(-600));
        }

        const BridgeGoal goal{
            QStringLiteral("thread-old"),
            QStringLiteral("Finish parity"),
            GoalStatus::Paused,
            50'000,
            42,
            90,
            100,
            200,
        };
        TaskProjectionState projection;
        projection.pendingApprovalThreadIds.insert(
            QStringLiteral("thread-old"));
        projection.attentionPromotedThreadIds.insert(
            QStringLiteral("thread-old"));

        TaskSnapshotLoader loader(
            fixture.environment,
            [projection](const QDateTime&) {
                return projection;
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({{goal.threadId, goal}});

        QVERIFY(result.hasValue());
        QCOMPARE(
            taskIds(result.value().tasks),
            QVector<QString>({
                QStringLiteral("thread-old"),
                QStringLiteral("thread-new"),
            }));
        QVERIFY(
            result.value()
                .attentionPromotedThreadIds
                .contains(
                    QStringLiteral("thread-old")));
        QVERIFY(result.value().tasks.first().goal.has_value());
        QVERIFY(result.value().tasks.first().goal.value() == goal);
    }

    void staleIncompleteGoalIsRetainedBeforeAuthoritativeRefresh()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"stale-goal-candidate");
            createThreadSchema(database);
            createGoalSchema(database);
            database.insertThread(
                QStringLiteral("thread-goal"),
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
            database.execute(QString::fromUtf8(R"SQL(
                insert into thread_goals values
                  ('thread-goal', 'active', 1784721600000)
            )SQL"));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().id,
            QStringLiteral("thread-goal"));
        QVERIFY(
            result.value().tasks.first().goal
                .has_value());
        QCOMPARE(
            result.value().tasks.first().goal->status,
            GoalStatus::Active);
        QCOMPARE(
            result.value().goalCandidateThreadIds,
            QVector<QString>({
                QStringLiteral("thread-goal"),
            }));
    }

    void recentCompletedGoalIsRetainedBeforeAuthoritativeRefresh()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"recent-complete-goal-candidate");
            createThreadSchema(database);
            createGoalSchema(database);
            database.insertThread(
                QStringLiteral("thread-goal"),
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
            database.execute(QStringLiteral(
                "insert into thread_goals values "
                "('thread-goal', 'complete', %1)")
                .arg(
                    now.addSecs(-10 * 60)
                        .toMSecsSinceEpoch()));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QVERIFY(
            result.value().tasks.first().goal
                .has_value());
        QCOMPARE(
            result.value().tasks.first().goal->status,
            GoalStatus::Complete);
        QCOMPARE(
            result.value().goalCandidateThreadIds,
            QVector<QString>({
                QStringLiteral("thread-goal"),
            }));
    }

    void staleIncompleteGoalRemainsVisibleWithoutRuntimeActivity()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"stale-incomplete-goal");
            createThreadSchema(database);
            createGoalSchema(database);
            database.insertThread(
                QStringLiteral("thread-goal"),
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
            database.execute(QStringLiteral(
                "insert into thread_goals values "
                "('thread-goal', 'paused', %1)")
                .arg(
                    now.addSecs(-2 * 60 * 60)
                        .toMSecsSinceEpoch()));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().id,
            QStringLiteral("thread-goal"));
        QVERIFY(
            result.value().tasks.first().goal
                .has_value());
        QCOMPARE(
            result.value().tasks.first().goal->status,
            GoalStatus::Paused);
        QCOMPARE(
            result.value().goalCandidateThreadIds,
            QVector<QString>({
                QStringLiteral("thread-goal"),
            }));
    }

    void newerPersistedGoalWinsOverStaleCachedGoal()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        const qint64 persistedUpdatedAt =
            now.addSecs(-5 * 60).toMSecsSinceEpoch();
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"newer-persisted-goal");
            createThreadSchema(database);
            createFullGoalSchema(database);
            database.insertThread(
                QStringLiteral("thread-goal"),
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
            database.execute(QStringLiteral(
                "insert into thread_goals values "
                "('thread-goal', 'persisted-goal', "
                "'Continue Windows parity', 'active', "
                "500000, 12345, 900, %1, %2)")
                .arg(
                    now.addSecs(-2 * 60 * 60)
                        .toMSecsSinceEpoch())
                .arg(persistedUpdatedAt));
        }

        const BridgeGoal staleCachedGoal{
            QStringLiteral("thread-goal"),
            QStringLiteral("Stale cached completion"),
            GoalStatus::Complete,
            500'000,
            12'345,
            900,
            now.addSecs(-2 * 60 * 60)
                .toMSecsSinceEpoch(),
            now.addSecs(-60 * 60)
                .toMSecsSinceEpoch(),
        };
        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({
                {
                    staleCachedGoal.threadId,
                    staleCachedGoal,
                },
            });

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QVERIFY(
            result.value().tasks.first().goal
                .has_value());
        QCOMPARE(
            result.value().tasks.first().goal->status,
            GoalStatus::Active);
        QCOMPARE(
            result.value().tasks.first().goal->objective,
            QStringLiteral("Continue Windows parity"));
        QCOMPARE(
            result.value().tasks.first().goal->updatedAt,
            persistedUpdatedAt);
    }

    void splitGoalDatabaseHydratesGoalOnFirstPublication()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 24, 18, 0, 0);
        {
            TestDatabase stateDatabase(
                fixture.environment.stateDatabase,
                u"split-goal-state");
            createThreadSchema(stateDatabase);
            stateDatabase.insertThread(
                QStringLiteral("thread-goal"),
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
        }
        {
            TestDatabase goalDatabase(
                fixture.environment.goalDatabase,
                u"split-goal-data");
            createFullGoalSchema(goalDatabase);
            goalDatabase.execute(QStringLiteral(
                "insert into thread_goals values "
                "('thread-goal', 'goal-id', 'Finish Windows parity', "
                "'paused', 500000, 12345, 900, %1, %2)")
                .arg(
                    now.addSecs(-3600)
                        .toMSecsSinceEpoch())
                .arg(now.toMSecsSinceEpoch()));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        const BridgeTask& task =
            result.value().tasks.first();
        QVERIFY(task.goal.has_value());
        QCOMPARE(
            task.goal->objective,
            QStringLiteral("Finish Windows parity"));
        QCOMPARE(
            task.goal->status,
            GoalStatus::Paused);
        QCOMPARE(
            task.goal->tokenBudget,
            std::optional<qint64>(500000));
        QCOMPARE(task.goal->tokensUsed, 12345);
        QCOMPARE(task.goal->elapsedSeconds, 900);
        QCOMPARE(
            result.value().goalCandidateThreadIds,
            QVector<QString>({
                QStringLiteral("thread-goal"),
            }));
    }

    void unreadableSessionAndRolloutInputsDegradeToEmptyOptionalData()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"unreadable-optionals");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/unreadable.jsonl"),
                now.addSecs(-420),
                now.addSecs(-420),
                QStringLiteral("Stored title"),
                QStringLiteral("C:\\Work\\Project"),
                QStringLiteral("First request"),
                QStringLiteral("Stored preview"));
        }
        QVERIFY(QDir().mkpath(fixture.environment.sessionIndex));
        QVERIFY(QDir().mkpath(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/unreadable.jsonl"))));

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        const BridgeTask& task = result.value().tasks.first();
        QCOMPARE(task.title, QStringLiteral("Stored title"));
        QCOMPARE(task.preview, QStringLiteral("Stored preview"));
        QCOMPARE(task.status, TaskStatus::Completed);
        QVERIFY(!task.activeTurnId.has_value());
    }

    void productionLoaderUsesMobileRolloutProfile()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"mobile-rollout-profile");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/mobile.jsonl"),
                now,
                now,
                QStringLiteral("Stored title"),
                QStringLiteral("C:\\Work\\Project"),
                QStringLiteral("First request"),
                QStringLiteral("Stored preview"));
        }
        const QString mobileAssistant(
            static_cast<qsizetype>(
                RolloutReader::kMaximumPreviewLineBytes)
                + 64 * 1024,
            QLatin1Char('m'));
        QByteArray bytes = rolloutMessage(
            QStringLiteral("assistant"),
            QStringLiteral("input_text"),
            mobileAssistant);
        QVERIFY(
            bytes.size()
            > RolloutReader::kMaximumPreviewLineBytes);
        QVERIFY(
            bytes.size()
            < RolloutReader::kMobileTaskTailBytes);
        bytes.append('\n');
        bytes.append(
            lifecycleLine(
                QStringLiteral("task_completed")));
        bytes.append('\n');
        writeFile(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/mobile.jsonl")),
            bytes);

        TaskSnapshotLoader loader(
            fixture.environment,
            TaskProjectionStateProvider{},
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().preview,
            mobileAssistant);
    }

    void throwingProjectionProviderYieldsEmptyState()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"throwing-projection");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/a.jsonl"),
                now.addSecs(-420),
                now.addSecs(-420));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) -> TaskProjectionState {
                throw std::runtime_error(
                    "SECRET_APPROVAL_LOG_CONTENT");
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QVERIFY(result.value().pendingApprovals.isEmpty());
        QVERIFY(!result.value().tasks.first().needsApproval);
    }

    void emptyProjectionProviderYieldsEmptyState()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"empty-projection");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/a.jsonl"),
                now.addSecs(-420),
                now.addSecs(-420));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            TaskProjectionStateProvider{},
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QVERIFY(result.value().pendingApprovals.isEmpty());
        QVERIFY(!result.value().tasks.first().needsApproval);
    }

    void authoritativeRuntimeStatusRetainsAndOverridesAnOldThread()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"runtime-active");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-runtime"),
                QStringLiteral(
                    "sessions/runtime.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; },
            [](std::stop_token) {
                return Result<
                    ThreadRuntimeSnapshot>::
                    success({
                        {
                            {
                                QStringLiteral(
                                    "thread-runtime"),
                                ThreadRuntimeStatus::
                                    Active,
                            },
                        },
                        true,
                    });
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().status,
            TaskStatus::Running);
        QCOMPARE(
            result.value().runtimeStatuses.value(
                QStringLiteral("thread-runtime")),
            ThreadRuntimeStatus::Active);
    }

    void inactiveRuntimeStatusesDoNotRetainOldThreads()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"runtime-inactive");
            createThreadSchema(database);
            for (const QString& threadId : {
                     QStringLiteral("thread-idle"),
                     QStringLiteral("thread-not-loaded"),
                     QStringLiteral("thread-active"),
                     QStringLiteral("thread-error"),
                 }) {
                database.insertThread(
                    threadId,
                    QStringLiteral("sessions/%1.jsonl")
                        .arg(threadId),
                    now.addSecs(-24 * 60 * 60),
                    now.addSecs(-24 * 60 * 60));
            }
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; },
            [](std::stop_token) {
                return Result<
                    ThreadRuntimeSnapshot>::
                    success({
                        {
                            {
                                QStringLiteral(
                                    "thread-idle"),
                                ThreadRuntimeStatus::
                                    Idle,
                            },
                            {
                                QStringLiteral(
                                    "thread-not-loaded"),
                                ThreadRuntimeStatus::
                                    NotLoaded,
                            },
                            {
                                QStringLiteral(
                                    "thread-active"),
                                ThreadRuntimeStatus::
                                    Active,
                            },
                            {
                                QStringLiteral(
                                    "thread-error"),
                                ThreadRuntimeStatus::
                                    SystemError,
                            },
                        },
                        true,
                    });
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 2);
        const QStringList ids =
            taskIds(result.value().tasks);
        QVERIFY(ids.contains(
            QStringLiteral("thread-active")));
        QVERIFY(ids.contains(
            QStringLiteral("thread-error")));
        QVERIFY(!ids.contains(
            QStringLiteral("thread-idle")));
        QVERIFY(!ids.contains(
            QStringLiteral("thread-not-loaded")));
    }

    void unavailableRuntimeFallsBackToPendingApprovals()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"runtime-fallback");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-pending"),
                QStringLiteral(
                    "sessions/pending.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
        }
        TaskProjectionState projection;
        projection.pendingApprovalThreadIds.insert(
            QStringLiteral("thread-pending"));
        projection.pendingApprovals.insert(
            QStringLiteral("thread-pending"),
            PendingApproval{});

        TaskSnapshotLoader loader(
            fixture.environment,
            [projection](const QDateTime&) {
                return projection;
            },
            [now] { return now; },
            [](std::stop_token) {
                return Result<
                    ThreadRuntimeSnapshot>::
                    success({{}, false});
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QVERIFY(
            result.value().tasks.first()
                .needsApproval);
        QCOMPARE(
            result.value().runtimeStatuses.value(
                QStringLiteral("thread-pending")),
            ThreadRuntimeStatus::
                WaitingOnApproval);
    }

    void authoritativeEmptyRuntimeDoesNotUseAStaleApproval()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"runtime-authoritative-empty");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-stale"),
                QStringLiteral(
                    "sessions/stale.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
        }
        TaskProjectionState projection;
        projection.pendingApprovalThreadIds.insert(
            QStringLiteral("thread-stale"));
        projection.pendingApprovals.insert(
            QStringLiteral("thread-stale"),
            PendingApproval{});

        TaskSnapshotLoader loader(
            fixture.environment,
            [projection](const QDateTime&) {
                return projection;
            },
            [now] { return now; },
            [](std::stop_token) {
                return Result<
                    ThreadRuntimeSnapshot>::
                    success({{}, true});
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QVERIFY(result.value().tasks.isEmpty());
        QVERIFY(
            result.value().runtimeStatuses.isEmpty());
        QCOMPARE(
            result.value().pendingApprovals.size(),
            1);
    }

    void productionSidebarPathIsCodexHomeGlobalState()
    {
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"sidebar-path");
            createThreadSchema(database);
            database.insertThread(
                QStringLiteral("thread-pinned"),
                QStringLiteral("sessions/pinned.jsonl"),
                now.addSecs(-420),
                now.addSecs(-600));
            database.insertThread(
                QStringLiteral("thread-fresh"),
                QStringLiteral("sessions/fresh.jsonl"),
                now.addSecs(-10),
                now.addSecs(-10));
        }
        writeJson(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral(".codex-global-state.json")),
            {
                {
                    QStringLiteral("pinned-thread-ids"),
                    QJsonArray{QStringLiteral("thread-pinned")},
                },
            });
        writeJson(
            QDir(fixture.environment.homeDirectory).filePath(
                QStringLiteral(".codex-global-state.json")),
            {
                {
                    QStringLiteral("pinned-thread-ids"),
                    QJsonArray{QStringLiteral("thread-fresh")},
                },
            });

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(
            taskIds(result.value().tasks),
            QVector<QString>({
                QStringLiteral("thread-pinned"),
                QStringLiteral("thread-fresh"),
            }));
    }

    void activityCapPrecedesSidebarPinning()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        const QString oldestId =
            QStringLiteral("thread-50");
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"activity-cap");
            createThreadSchema(database);
            for (int index = 0; index < 51; ++index) {
                const QString id =
                    QStringLiteral("thread-%1")
                        .arg(index, 2, 10, QLatin1Char('0'));
                database.insertThread(
                    id,
                    QStringLiteral("sessions/%1.jsonl")
                        .arg(id),
                    now.addSecs(-index),
                    now.addSecs(-index));
            }
        }
        writeJson(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral(
                    ".codex-global-state.json")),
            {
                {
                    QStringLiteral("pinned-thread-ids"),
                    QJsonArray{oldestId},
                },
            });

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 50);
        QVERIFY(
            !taskIds(result.value().tasks)
                 .contains(oldestId));
        QCOMPARE(
            result.value().tasks.first().id,
            QStringLiteral("thread-00"));
    }

    void staleThreadsDoNotCrowdOutRuntimeActivityBeforeCap()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        const QString runtimeThreadId =
            QStringLiteral("thread-runtime");
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"runtime-activity-cap");
            createThreadSchema(database);
            for (int index = 0; index < 50; ++index) {
                const QString id =
                    QStringLiteral("thread-stale-%1")
                        .arg(index, 2, 10, QLatin1Char('0'));
                database.insertThread(
                    id,
                    QStringLiteral("sessions/%1.jsonl")
                        .arg(id),
                    now.addSecs(-(60 * 60 + index)),
                    now.addSecs(-(60 * 60 + index)));
            }
            database.insertThread(
                runtimeThreadId,
                QStringLiteral("sessions/runtime.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; },
            [runtimeThreadId](std::stop_token) {
                return Result<ThreadRuntimeSnapshot>::success({
                    {
                        {
                            runtimeThreadId,
                            ThreadRuntimeStatus::Active,
                        },
                    },
                    true,
                });
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 1);
        QCOMPARE(
            result.value().tasks.first().id,
            runtimeThreadId);
        QCOMPARE(
            result.value().tasks.first().status,
            TaskStatus::Running);
    }

    void goalActivityParticipatesInPreSidebarCap()
    {
        Fixture fixture;
        const QDateTime now =
            utcDate(2026, 7, 22, 12, 0, 0);
        const QString goalThreadId =
            QStringLiteral("goal-active");
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"goal-activity-cap");
            createThreadSchema(database);
            createGoalSchema(database);
            for (int index = 0; index < 50; ++index) {
                const QString id =
                    QStringLiteral("thread-%1")
                        .arg(index, 2, 10, QLatin1Char('0'));
                database.insertThread(
                    id,
                    QStringLiteral("sessions/%1.jsonl")
                        .arg(id),
                    now.addSecs(-index),
                    now.addSecs(-index));
            }
            database.insertThread(
                goalThreadId,
                QStringLiteral("sessions/goal.jsonl"),
                now.addSecs(-24 * 60 * 60),
                now.addSecs(-24 * 60 * 60));
            database.execute(QStringLiteral(
                "insert into thread_goals values "
                "('%1', 'active', %2)")
                .arg(goalThreadId)
                .arg(now.toMSecsSinceEpoch()));
        }
        const BridgeGoal goal{
            goalThreadId,
            QStringLiteral("Finish Windows parity"),
            GoalStatus::Active,
            std::nullopt,
            100,
            60,
            now.addSecs(-60)
                .toMSecsSinceEpoch(),
            now.toMSecsSinceEpoch(),
        };

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [now] { return now; });
        const Result<CodexProcessSnapshot> result =
            loader.load({{goalThreadId, goal}});

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().tasks.size(), 50);
        QVERIFY(
            taskIds(result.value().tasks)
                .contains(goalThreadId));
        QVERIFY(
            !taskIds(result.value().tasks)
                 .contains(
                     QStringLiteral("thread-49")));
    }

    void validNowProviderIsCalledOnceAndConvertedToUtc()
    {
        Fixture fixture;
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"now-provider");
            createThreadSchema(database);
        }

        int nowCalls = 0;
        QDateTime observedNow;
        const QDateTime localNow(
            QDate(2026, 7, 22),
            QTime(17, 0, 0),
            QTimeZone::fromSecondsAheadOfUtc(5 * 60 * 60));
        TaskSnapshotLoader loader(
            fixture.environment,
            [&observedNow](const QDateTime& now) {
                observedNow = now;
                return TaskProjectionState{};
            },
            [&nowCalls, localNow] {
                ++nowCalls;
                return localNow;
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QCOMPARE(nowCalls, 1);
        QCOMPARE(observedNow.timeSpec(), Qt::UTC);
        QCOMPARE(observedNow, localNow.toUTC());
    }

    void emptyNowProviderUsesCurrentUtcTime()
    {
        Fixture fixture;
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"default-now");
            createThreadSchema(database);
        }

        bool observedValidUtc = false;
        TaskSnapshotLoader loader(
            fixture.environment,
            [&observedValidUtc](const QDateTime& now) {
                observedValidUtc =
                    now.isValid()
                    && now.timeSpec() == Qt::UTC;
                return TaskProjectionState{};
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(result.hasValue());
        QVERIFY(observedValidUtc);
    }

    void invalidNowProviderReturnsSanitizedUnavailable()
    {
        Fixture fixture;
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"invalid-now");
            createThreadSchema(database);
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [] { return QDateTime{}; });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(!result.hasValue());
        verifyUnavailable(result.error());
    }

    void throwingNowProviderReturnsSanitizedUnavailable()
    {
        Fixture fixture;
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"throwing-now");
            createThreadSchema(database);
        }

        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            []() -> QDateTime {
                throw std::runtime_error(
                    "SECRET_CLOCK_FAILURE");
            });
        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(!result.hasValue());
        verifyUnavailable(result.error());
        const QString errorBlob =
            result.error().code
            + result.error().message
            + QString::fromUtf8(
                QJsonDocument(
                    QJsonObject::fromVariantMap(
                        result.error().context))
                    .toJson(QJsonDocument::Compact));
        QVERIFY(!errorBlob.contains(
            QStringLiteral("SECRET_CLOCK_FAILURE")));
    }

    void cancellationAfterInvalidNowProviderWins()
    {
        Fixture fixture;
        std::stop_source stopSource;
        TaskSnapshotLoader loader(
            fixture.environment,
            TaskProjectionStateProvider{},
            [&stopSource] {
                stopSource.request_stop();
                return QDateTime{};
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({}, stopSource.get_token());

        QVERIFY(stopSource.stop_requested());
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void cancellationFromThrowingNowProviderWins()
    {
        Fixture fixture;
        std::stop_source stopSource;
        TaskSnapshotLoader loader(
            fixture.environment,
            TaskProjectionStateProvider{},
            [&stopSource]() -> QDateTime {
                stopSource.request_stop();
                throw std::runtime_error(
                    "SECRET_THROWING_NOW");
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({}, stopSource.get_token());

        QVERIFY(stopSource.stop_requested());
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void threadReadFailureRemainsFatal()
    {
        Fixture fixture;
        TaskSnapshotLoader loader(
            fixture.environment,
            [](const QDateTime&) {
                return TaskProjectionState{};
            },
            [] {
                return utcDate(2026, 7, 22, 12, 0, 0);
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({});

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.state_missing"));
    }

    void loadPhaseCancellationIsExact_data()
    {
        using Phase =
            companion::detail::TaskSnapshotLoaderTestAccess::Phase;
        QTest::addColumn<int>("phase");
        QTest::addColumn<qlonglong>("targetIndex");

        QTest::newRow("after-now")
            << static_cast<int>(Phase::AfterNow)
            << qlonglong(-1);
        QTest::newRow("before-database")
            << static_cast<int>(Phase::AfterProjectionState)
            << qlonglong(-1);
        QTest::newRow("after-threads")
            << static_cast<int>(Phase::AfterThreads)
            << qlonglong(-1);
        QTest::newRow("after-session-names")
            << static_cast<int>(Phase::AfterSessionNames)
            << qlonglong(-1);
        QTest::newRow("after-sidebar")
            << static_cast<int>(Phase::AfterSidebar)
            << qlonglong(-1);
        QTest::newRow("before-first-rollout")
            << static_cast<int>(Phase::BeforeRollout)
            << qlonglong(0);
        QTest::newRow("between-rollout-rows")
            << static_cast<int>(Phase::AfterRollout)
            << qlonglong(0);
        QTest::newRow("after-last-rollout")
            << static_cast<int>(Phase::AfterRollout)
            << qlonglong(1);
        QTest::newRow("after-projection")
            << static_cast<int>(Phase::AfterProjection)
            << qlonglong(-1);
        QTest::newRow("before-jobs")
            << static_cast<int>(Phase::BeforeJobs)
            << qlonglong(-1);
        QTest::newRow("after-jobs")
            << static_cast<int>(Phase::AfterJobs)
            << qlonglong(-1);
    }

    void loadPhaseCancellationIsExact()
    {
        using Access =
            companion::detail::TaskSnapshotLoaderTestAccess;
        using Phase = Access::Phase;
        QFETCH(int, phase);
        QFETCH(qlonglong, targetIndex);

        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"phase-cancellation");
            createThreadSchema(database);
            createJobSchema(database);
            database.insertThread(
                QStringLiteral("thread-a"),
                QStringLiteral("sessions/a.jsonl"),
                now,
                now);
            database.insertThread(
                QStringLiteral("thread-b"),
                QStringLiteral("sessions/b.jsonl"),
                now.addSecs(-1),
                now.addSecs(-1));
            database.insertJob(
                QStringLiteral("job-1"),
                QStringLiteral("thread-a"),
                now);
        }
        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/a.jsonl")),
            QStringLiteral("Assistant A"));
        writeRollout(
            QDir(fixture.environment.codexHome).filePath(
                QStringLiteral("sessions/b.jsonl")),
            QStringLiteral("Assistant B"));

        std::stop_source stopSource;
        bool targetReached = false;
        bool phaseObservedAfterStop = false;
        QVector<QPair<int, qlonglong>> observed;
        std::unique_ptr<TaskSnapshotLoader> loader =
            Access::create(
                fixture.environment,
                TaskProjectionStateProvider{},
                [now] { return now; },
                [&stopSource,
                 &targetReached,
                 &phaseObservedAfterStop,
                 &observed,
                 phase,
                 targetIndex](
                    Phase observedPhase,
                    qsizetype observedIndex) {
                    if (stopSource.stop_requested()) {
                        phaseObservedAfterStop = true;
                    }
                    observed.append({
                        static_cast<int>(observedPhase),
                        static_cast<qlonglong>(observedIndex),
                    });
                    if (static_cast<int>(observedPhase) == phase
                        && observedIndex == targetIndex) {
                        targetReached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<CodexProcessSnapshot> result =
            loader->load({}, stopSource.get_token());

        QVERIFY(targetReached);
        QVERIFY(!phaseObservedAfterStop);
        QVERIFY(!observed.isEmpty());
        QCOMPARE(observed.last().first, phase);
        QCOMPARE(observed.last().second, targetIndex);
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void cancellationDuringFailingThreadReadWins()
    {
        using Access =
            companion::detail::TaskSnapshotLoaderTestAccess;
        using Phase = Access::Phase;
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        std::stop_source stopSource;
        bool reached = false;
        std::unique_ptr<TaskSnapshotLoader> loader =
            Access::create(
                fixture.environment,
                TaskProjectionStateProvider{},
                [now] { return now; },
                [&stopSource,
                 &reached](
                    Phase phase,
                    qsizetype index) {
                    if (phase == Phase::AfterThreads
                        && index == -1) {
                        reached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<CodexProcessSnapshot> result =
            loader->load({}, stopSource.get_token());

        QVERIFY(reached);
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void cancellationDuringFailingJobReadWins()
    {
        using Access =
            companion::detail::TaskSnapshotLoaderTestAccess;
        using Phase = Access::Phase;
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"cancel-failing-jobs");
            createThreadSchema(database);
            createMalformedJobSchema(database);
        }
        std::stop_source stopSource;
        bool reached = false;
        std::unique_ptr<TaskSnapshotLoader> loader =
            Access::create(
                fixture.environment,
                TaskProjectionStateProvider{},
                [now] { return now; },
                [&stopSource,
                 &reached](
                    Phase phase,
                    qsizetype index) {
                    if (phase == Phase::AfterJobs
                        && index == -1) {
                        reached = true;
                        stopSource.request_stop();
                    }
                });

        const Result<CodexProcessSnapshot> result =
            loader->load({}, stopSource.get_token());

        QVERIFY(reached);
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void cancellationFromThrowingProjectionProviderWins()
    {
        Fixture fixture;
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"cancel-throwing-projection");
            createThreadSchema(database);
        }
        std::stop_source stopSource;
        TaskSnapshotLoader loader(
            fixture.environment,
            [&stopSource](const QDateTime&)
                -> TaskProjectionState {
                stopSource.request_stop();
                throw std::runtime_error(
                    "SECRET_THROWING_PROJECTION");
            },
            [] {
                return utcDate(2026, 7, 22, 12, 0, 0);
            });

        const Result<CodexProcessSnapshot> result =
            loader.load({}, stopSource.get_token());

        QVERIFY(stopSource.stop_requested());
        QVERIFY(!result.hasValue());
        verifyCanceled(result.error());
    }

    void throwingLoadPhaseProbeDoesNotEscape()
    {
        using Access =
            companion::detail::TaskSnapshotLoaderTestAccess;
        using Phase = Access::Phase;
        Fixture fixture;
        const QDateTime now = utcDate(2026, 7, 22, 12, 0, 0);
        {
            TestDatabase database(
                fixture.environment.stateDatabase,
                u"throwing-load-probe");
            createThreadSchema(database);
        }
        int calls = 0;
        std::unique_ptr<TaskSnapshotLoader> loader =
            Access::create(
                fixture.environment,
                TaskProjectionStateProvider{},
                [now] { return now; },
                [&calls](Phase, qsizetype) {
                    ++calls;
                    throw std::runtime_error(
                        "SECRET_LOAD_PROBE");
                });

        const Result<CodexProcessSnapshot> result =
            loader->load({});

        QVERIFY(result.hasValue());
        QVERIFY(calls > 0);
    }
};

QTEST_GUILESS_MAIN(TaskSnapshotLoaderTests)

#include "TaskSnapshotLoaderTests.moc"
