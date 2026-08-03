#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/CodexModels.h"
#include "codex/state/CodexStateDatabaseReader.h"
#include "codex/state/RolloutReader.h"
#include "codex/state/SessionIndexReader.h"

#include <QCryptographicHash>
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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <optional>

using namespace companion;

namespace {

template <typename T>
QString resultErrorMessage(const Result<T>& result)
{
    return result.hasValue() ? QString() : result.error().message;
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

private:
    QString connectionName_;
    QSqlDatabase database_;
};

void createCurrentSchema(TestDatabase& database)
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

void insertCurrentRows(TestDatabase& database)
{
    database.execute(QString::fromUtf8(R"SQL(
        insert into threads values
          ('thread-primary', 'sessions/primary.jsonl', 1700000000,
           1700000000123, 1700000003123, 'desktop', 'C:\work\primary',
           'Primary title', 'First request', 0, 'Stored preview',
           'gpt-5.6', 'high'),
          ('thread-secondary', 'sessions/secondary.jsonl', 1700000005,
           1700000001123, 1700000002123, 'desktop', 'C:\work\secondary',
           'Secondary title', 'Second request', 0, '', null, null),
          ('archived', 'sessions/archived.jsonl', 1700000010,
           1700000010123, 1700000010123, 'desktop', 'C:\work\archived',
           'Archived', 'Ignore', 1, '', null, null),
          ('subagent', 'sessions/subagent.jsonl', 1700000020,
           1700000020123, 1700000020123, '{"subagent":"worker"}',
           'C:\work\subagent', 'Subagent', 'Ignore', 0, '', null, null)
    )SQL"));
    database.execute(QString::fromUtf8(R"SQL(
        insert into agent_jobs values
          ('job-1', 'Sprite work', 'running', 'Build sprites',
           1699999900, 1700000100, 1699999950, 'Needs review')
    )SQL"));
    database.execute(QString::fromUtf8(R"SQL(
        insert into agent_job_items values
          ('job-1', 'item-1', 'thread-primary', 1700000100)
    )SQL"));
}

QByteArray fileBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("could not read fixture file");
    }
    return file.readAll();
}

QByteArray fileHash(const QString& path)
{
    return QCryptographicHash::hash(
        fileBytes(path), QCryptographicHash::Sha256);
}

void writeFile(const QString& path, QByteArrayView bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("could not write fixture file");
    }
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
        qFatal("could not finish fixture file");
    }
}

void writeLines(
    const QString& path,
    const QVector<QByteArray>& lines,
    bool trailingNewline = true)
{
    QByteArray bytes;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (index > 0) {
            bytes.append('\n');
        }
        bytes.append(lines.at(index));
    }
    if (trailingNewline) {
        bytes.append('\n');
    }
    writeFile(path, bytes);
}

QByteArray compactJson(QJsonObject object)
{
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact);
}

QByteArray lifecycleEvent(
    const QString& type,
    const std::optional<QString>& turnId = std::nullopt,
    qsizetype paddingBytes = 0)
{
    QJsonObject payload{{QStringLiteral("type"), type}};
    if (turnId.has_value()) {
        payload.insert(QStringLiteral("turn_id"), *turnId);
    }
    if (paddingBytes > 0) {
        payload.insert(
            QStringLiteral("padding"),
            QString(paddingBytes, QLatin1Char('x')));
    }
    return compactJson({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-21T12:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"), payload},
    });
}

QByteArray responseMessage(
    const QString& role,
    const QString& text,
    const std::optional<QString>& directTurnId = std::nullopt,
    const std::optional<QString>& metadataTurnId = std::nullopt)
{
    const QString fragmentType = role == QStringLiteral("assistant")
        ? QStringLiteral("output_text")
        : QStringLiteral("input_text");
    QJsonObject payload{
        {QStringLiteral("type"), QStringLiteral("message")},
        {QStringLiteral("role"), role},
        {QStringLiteral("content"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("type"), fragmentType},
                 {QStringLiteral("text"), text},
             },
             QJsonObject{
                 {QStringLiteral("type"), QStringLiteral("ignored")},
                 {QStringLiteral("text"), QStringLiteral("hidden")},
             },
         }},
    };
    if (directTurnId.has_value()) {
        payload.insert(QStringLiteral("turn_id"), *directTurnId);
    }
    if (metadataTurnId.has_value()) {
        payload.insert(
            QStringLiteral("internal_chat_message_metadata_passthrough"),
            QJsonObject{{QStringLiteral("turn_id"), *metadataTurnId}});
    }
    return compactJson({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-21T12:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {QStringLiteral("payload"), payload},
    });
}

QByteArray responseMessageWithStringContent(
    const QString& role,
    const QString& text,
    const std::optional<QString>& turnId = std::nullopt)
{
    QJsonObject payload{
        {QStringLiteral("type"), QStringLiteral("message")},
        {QStringLiteral("role"), role},
        {QStringLiteral("content"), text},
    };
    if (turnId.has_value()) {
        payload.insert(QStringLiteral("turn_id"), *turnId);
    }
    return compactJson({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-21T12:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {QStringLiteral("payload"), payload},
    });
}

QByteArray responseMessageWithFragments(
    const QString& role,
    const QJsonArray& fragments,
    QJsonValue directTurnId = QJsonValue(QJsonValue::Undefined),
    QJsonValue metadataTurnId = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject payload{
        {QStringLiteral("type"), QStringLiteral("message")},
        {QStringLiteral("role"), role},
        {QStringLiteral("content"), fragments},
    };
    if (!directTurnId.isUndefined()) {
        payload.insert(
            QStringLiteral("turn_id"),
            std::move(directTurnId));
    }
    if (!metadataTurnId.isUndefined()) {
        payload.insert(
            QStringLiteral("internal_chat_message_metadata_passthrough"),
            QJsonObject{
                {
                    QStringLiteral("turn_id"),
                    std::move(metadataTurnId),
                },
            });
    }
    return compactJson({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-21T12:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("response_item")},
        {QStringLiteral("payload"), payload},
    });
}

QByteArray lifecycleEventWithTurnFields(
    const QString& type,
    QJsonValue directTurnId = QJsonValue(QJsonValue::Undefined),
    QJsonValue metadataTurnId = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject payload{{QStringLiteral("type"), type}};
    if (!directTurnId.isUndefined()) {
        payload.insert(
            QStringLiteral("turn_id"),
            std::move(directTurnId));
    }
    if (!metadataTurnId.isUndefined()) {
        payload.insert(
            QStringLiteral("internal_chat_message_metadata_passthrough"),
            QJsonObject{
                {
                    QStringLiteral("turn_id"),
                    std::move(metadataTurnId),
                },
            });
    }
    return compactJson({
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-07-21T12:00:00.000Z")},
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"), payload},
    });
}

QByteArray legacyAssistantMessage(
    const QString& text,
    const std::optional<QString>& turnId = std::nullopt)
{
    QJsonObject payload{
        {QStringLiteral("type"), QStringLiteral("agent_message")},
        {QStringLiteral("message"), text},
    };
    if (turnId.has_value()) {
        payload.insert(QStringLiteral("turn_id"), *turnId);
    }
    return compactJson({
        {QStringLiteral("type"), QStringLiteral("event_msg")},
        {QStringLiteral("payload"), payload},
    });
}

const CodexThreadRecord* findThread(
    const CodexStateSnapshot& snapshot,
    QStringView id)
{
    for (const CodexThreadRecord& thread : snapshot.threads) {
        if (thread.id == id) {
            return &thread;
        }
    }
    return nullptr;
}

} // namespace

class StateReaderTests final : public QObject {
    Q_OBJECT

private slots:
    void readsCurrentSchemaWithoutCreatingSidecars()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"current-schema");
            createCurrentSchema(database);
            insertCurrentRows(database);
        }

        const QByteArray beforeHash = fileHash(databasePath);
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-wal")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-shm")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-journal")));
        QStringList connectionsBefore = QSqlDatabase::connectionNames();
        connectionsBefore.sort();

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        const CodexStateSnapshot& snapshot = result.value();
        QCOMPARE(snapshot.threads.size(), 2);
        QCOMPARE(snapshot.threads.at(0).id, QStringLiteral("thread-primary"));
        QCOMPARE(snapshot.threads.at(1).id, QStringLiteral("thread-secondary"));

        const CodexThreadRecord& thread = snapshot.threads.at(0);
        QCOMPARE(thread.title, QStringLiteral("Primary title"));
        QCOMPARE(
            thread.workingDirectory,
            QStringLiteral("C:\\work\\primary"));
        QCOMPARE(thread.firstUserMessage, QStringLiteral("First request"));
        QCOMPARE(
            thread.rolloutPath,
            QStringLiteral("sessions/primary.jsonl"));
        QCOMPARE(thread.preview, QStringLiteral("Stored preview"));
        QCOMPARE(thread.model.value(), QStringLiteral("gpt-5.6"));
        QCOMPARE(thread.reasoningEffort.value(), QStringLiteral("high"));
        QCOMPARE(
            thread.updatedAt,
            QDateTime::fromMSecsSinceEpoch(
                1700000000123, QTimeZone::UTC));
        QCOMPARE(
            thread.recencyAt,
            QDateTime::fromMSecsSinceEpoch(
                1700000003123, QTimeZone::UTC));

        QCOMPARE(snapshot.jobs.size(), 1);
        const CodexJobRecord& job = snapshot.jobs.first();
        QCOMPARE(job.id, QStringLiteral("job-1"));
        QCOMPARE(job.name, QStringLiteral("Sprite work"));
        QCOMPARE(job.status, QStringLiteral("running"));
        QCOMPARE(job.instruction, QStringLiteral("Build sprites"));
        QCOMPARE(job.error.value(), QStringLiteral("Needs review"));
        QCOMPARE(job.threadId.value(), QStringLiteral("thread-primary"));
        QCOMPARE(
            job.updatedAt,
            QDateTime::fromSecsSinceEpoch(
                1700000100, QTimeZone::UTC));
        QCOMPARE(
            job.startedAt.value(),
            QDateTime::fromSecsSinceEpoch(
                1699999950, QTimeZone::UTC));

        QCOMPARE(fileHash(databasePath), beforeHash);
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-wal")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-shm")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-journal")));
        QStringList connectionsAfter = QSqlDatabase::connectionNames();
        connectionsAfter.sort();
        QCOMPARE(connectionsAfter, connectionsBefore);
    }

    void readsIncompleteGoalThreadIdsFromPersistedState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(
                databasePath,
                u"goal-candidates");
            createCurrentSchema(database);
            createGoalSchema(database);
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('active', 'sessions/active.jsonl', 1700000001,
                   1700000001000, 1700000001000, 'desktop', 'C:\work\active',
                   'Active', 'Active', 0, '', null, null),
                  ('paused', 'sessions/paused.jsonl', 1700000002,
                   1700000002000, 1700000002000, 'desktop', 'C:\work\paused',
                   'Paused', 'Paused', 0, '', null, null),
                  ('blocked', 'sessions/blocked.jsonl', 1700000003,
                   1700000003000, 1700000003000, 'desktop', 'C:\work\blocked',
                   'Blocked', 'Blocked', 0, '', null, null),
                  ('usage-limited', 'sessions/usage.jsonl', 1700000004,
                   1700000004000, 1700000004000, 'desktop', 'C:\work\usage',
                   'Usage', 'Usage', 0, '', null, null),
                  ('budget-limited', 'sessions/budget.jsonl', 1700000005,
                   1700000005000, 1700000005000, 'desktop', 'C:\work\budget',
                   'Budget', 'Budget', 0, '', null, null),
                  ('complete', 'sessions/complete.jsonl', 1700000006,
                   1700000006000, 1700000006000, 'desktop', 'C:\work\complete',
                   'Complete', 'Complete', 0, '', null, null),
                  ('archived', 'sessions/archived.jsonl', 1700000007,
                   1700000007000, 1700000007000, 'desktop', 'C:\work\archived',
                   'Archived', 'Archived', 1, '', null, null),
                  ('subagent', 'sessions/subagent.jsonl', 1700000008,
                   1700000008000, 1700000008000, '{"subagent":"worker"}',
                   'C:\work\subagent', 'Subagent', 'Subagent', 0, '', null, null)
            )SQL"));
            database.execute(QString::fromUtf8(R"SQL(
                insert into thread_goals values
                  ('active', 'active', 1700000011000),
                  ('paused', 'paused', 1700000012000),
                  ('blocked', 'blocked', 1700000013000),
                  ('usage-limited', 'usageLimited', 1700000014000),
                  ('budget-limited', 'budget_limited', 1700000015000),
                  ('complete', 'complete', 1700000016000),
                  ('archived', 'active', 1700000017000),
                  ('subagent', 'paused', 1700000018000)
            )SQL"));
        }

        const auto result =
            CodexStateDatabaseReader::
                readGoalCandidateThreadIds(
                    databasePath);

        QVERIFY2(
            result.hasValue(),
            qPrintable(resultErrorMessage(result)));
        QCOMPARE(
            result.value(),
            QVector<QString>({
                QStringLiteral("budget-limited"),
                QStringLiteral("usage-limited"),
                QStringLiteral("blocked"),
                QStringLiteral("paused"),
                QStringLiteral("active"),
            }));
    }

    void readsProjectedGoalsFromStandaloneGoalDatabase()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(
                QStringLiteral("goals.sqlite"));
        const QDateTime now =
            QDateTime(
                QDate(2026, 7, 24),
                QTime(18, 0),
                QTimeZone::UTC);
        {
            TestDatabase database(
                databasePath,
                u"standalone-goals");
            createFullGoalSchema(database);
            database.execute(QStringLiteral(
                "insert into thread_goals values "
                "('goal-thread', 'goal-id', 'Ship parity', "
                "'budgetLimited', 100000, 90000, 240, %1, %2)")
                .arg(
                    now.addSecs(-600)
                        .toMSecsSinceEpoch())
                .arg(now.toMSecsSinceEpoch()));
        }

        const auto result =
            CodexStateDatabaseReader::
                readGoalCandidates(
                    databasePath,
                    now);

        QVERIFY2(
            result.hasValue(),
            qPrintable(resultErrorMessage(result)));
        QCOMPARE(result.value().size(), 1);
        const CodexGoalCandidateRecord& record =
            result.value().first();
        QCOMPARE(
            record.goal.threadId,
            QStringLiteral("goal-thread"));
        QCOMPARE(
            record.goal.objective,
            QStringLiteral("Ship parity"));
        QCOMPARE(
            record.goal.status,
            GoalStatus::BudgetLimited);
        QCOMPARE(
            record.goal.tokenBudget,
            std::optional<qint64>(100000));
        QCOMPARE(record.goal.tokensUsed, 90000);
        QCOMPARE(record.goal.elapsedSeconds, 240);
        QCOMPARE(
            record.activityAt,
            now);
    }

    void readsUncheckpointedWalWithoutMutation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        QByteArray databaseHash;
        QByteArray walHash;

        {
            TestDatabase writer(databasePath, u"wal-writer");
            writer.execute(QStringLiteral("pragma journal_mode=wal"));
            writer.execute(QStringLiteral("pragma wal_autocheckpoint=0"));
            createCurrentSchema(writer);
            writer.execute(QStringLiteral("pragma wal_checkpoint(truncate)"));
            writer.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('wal-thread', 'sessions/wal.jsonl', 1700000200,
                   1700000200123, 1700000200123, 'desktop', 'C:\work\wal',
                   'WAL title', 'WAL request', 0, 'WAL preview',
                   'gpt-5.6', 'medium')
            )SQL"));

            const QString walPath = databasePath + QStringLiteral("-wal");
            const QString shmPath = databasePath + QStringLiteral("-shm");
            QVERIFY(QFileInfo::exists(walPath));
            QVERIFY(QFileInfo::exists(shmPath));
            databaseHash = fileHash(databasePath);
            walHash = fileHash(walPath);
            QStringList connectionsBefore = QSqlDatabase::connectionNames();
            connectionsBefore.sort();

            const auto result =
                CodexStateDatabaseReader::readSnapshot(databasePath);

            QVERIFY2(
                result.hasValue(), qPrintable(resultErrorMessage(result)));
            QVERIFY(findThread(result.value(), u"wal-thread") != nullptr);
            QCOMPARE(fileHash(databasePath), databaseHash);
            QCOMPARE(fileHash(walPath), walHash);
            QVERIFY(QFileInfo::exists(shmPath));
            QStringList connectionsAfter = QSqlDatabase::connectionNames();
            connectionsAfter.sort();
            QCOMPARE(connectionsAfter, connectionsBefore);
        }
    }

    void supportsOlderSchemaAndTimestampRepresentations()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"older-schema");
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    rollout_path text not null,
                    updated_at,
                    source text not null,
                    cwd text not null,
                    title text not null,
                    first_user_message text not null,
                    archived integer not null
                )
            )SQL"));
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('iso-thread', 'sessions/iso.jsonl',
                   '2026-07-20T12:34:56.789Z', 'desktop', 'C:\iso',
                   'ISO title', 'ISO request', 0),
                  ('millisecond-thread', 'sessions/ms.jsonl',
                   1784550000123, 'desktop', 'C:\ms',
                   'Millisecond title', 'Millisecond request', 0)
            )SQL"));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QCOMPARE(result.value().jobs.size(), 0);
        const CodexThreadRecord* iso =
            findThread(result.value(), u"iso-thread");
        const CodexThreadRecord* milliseconds =
            findThread(result.value(), u"millisecond-thread");
        QVERIFY(iso != nullptr);
        QVERIFY(milliseconds != nullptr);
        QCOMPARE(
            iso->updatedAt,
            QDateTime::fromString(
                QStringLiteral("2026-07-20T12:34:56.789Z"),
                Qt::ISODateWithMs));
        QCOMPARE(iso->recencyAt, iso->updatedAt);
        QVERIFY(iso->preview.isEmpty());
        QVERIFY(!iso->model.has_value());
        QVERIFY(!iso->reasoningEffort.has_value());
        QCOMPARE(
            milliseconds->updatedAt,
            QDateTime::fromMSecsSinceEpoch(
                1784550000123, QTimeZone::UTC));
    }

    void missingOptionalJobTablesReturnEmptyJobs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"empty-schema");
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    rollout_path text not null,
                    updated_at integer not null,
                    source text not null,
                    cwd text not null,
                    title text not null,
                    first_user_message text not null,
                    archived integer not null
                )
            )SQL"));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().threads.isEmpty());
        QVERIFY(result.value().jobs.isEmpty());
    }

    void missingThreadsTableReturnsTypedError()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"missing-threads");
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.state_invalid"));
    }

    void missingThreadFilterColumnsReturnTypedError_data()
    {
        QTest::addColumn<QString>("omittedColumn");
        QTest::newRow("missing-archived")
            << QStringLiteral("archived");
        QTest::newRow("missing-source")
            << QStringLiteral("source");
    }

    void missingThreadFilterColumnsReturnTypedError()
    {
        QFETCH(QString, omittedColumn);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"missing-filter-column");
            QStringList columns{
                QStringLiteral("id text primary key"),
                QStringLiteral("rollout_path text not null"),
                QStringLiteral("updated_at integer not null"),
                QStringLiteral("cwd text not null"),
                QStringLiteral("title text not null"),
                QStringLiteral("first_user_message text not null"),
            };
            if (omittedColumn != QStringLiteral("source")) {
                columns.append(QStringLiteral("source text not null"));
            }
            if (omittedColumn != QStringLiteral("archived")) {
                columns.append(QStringLiteral("archived integer not null"));
            }
            database.execute(
                QStringLiteral("create table threads (%1)")
                    .arg(columns.join(QStringLiteral(", "))));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.state_invalid"));
    }

    void nullThreadFilterValuesAreExcluded_data()
    {
        QTest::addColumn<QString>("archivedValue");
        QTest::addColumn<QString>("sourceValue");
        QTest::newRow("null-archived")
            << QStringLiteral("null")
            << QStringLiteral("'desktop'");
        QTest::newRow("null-source")
            << QStringLiteral("0")
            << QStringLiteral("null");
    }

    void nullThreadFilterValuesAreExcluded()
    {
        QFETCH(QString, archivedValue);
        QFETCH(QString, sourceValue);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"null-filter-value");
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    rollout_path text not null,
                    updated_at integer not null,
                    source text,
                    cwd text not null,
                    title text not null,
                    first_user_message text not null,
                    archived integer
                )
            )SQL"));
            database.execute(QStringLiteral(
                "insert into threads values "
                "('null-filter', 'sessions/null.jsonl', 1700000000, "
                "%1, 'C:\\work\\null', 'Null filter', 'Request', %2)")
                .arg(sourceValue, archivedValue));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(
            result.hasValue(),
            qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().threads.isEmpty());
    }

    void malformedArchivedThreadDoesNotHideValidActivity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(
                databasePath,
                u"irrelevant-malformed-thread");
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    rollout_path text,
                    updated_at,
                    source text,
                    cwd text not null,
                    title text not null,
                    first_user_message text not null,
                    archived integer
                )
            )SQL"));
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('valid', 'sessions/valid.jsonl', 1700000100, 'desktop',
                   'C:\work\valid', 'Valid', 'Request', 0),
                  ('malformed-archived', '', 'not-a-time', null,
                   'C:\work\archived', 'Archived', 'Ignore', 1)
            )SQL"));
        }

        const auto result =
            CodexStateDatabaseReader::readThreads(
                databasePath);

        QVERIFY2(
            result.hasValue(),
            qPrintable(resultErrorMessage(result)));
        QCOMPARE(result.value().size(), 1);
        QCOMPARE(
            result.value().first().id,
            QStringLiteral("valid"));
    }

    void malformedPresentJobItemsTableReturnsTypedError_data()
    {
        QTest::addColumn<QString>("omittedColumn");
        QTest::newRow("missing-job-id")
            << QStringLiteral("job_id");
        QTest::newRow("missing-assigned-thread-id")
            << QStringLiteral("assigned_thread_id");
    }

    void malformedPresentJobItemsTableReturnsTypedError()
    {
        QFETCH(QString, omittedColumn);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"malformed-job-items");
            createCurrentSchema(database);
            database.execute(QStringLiteral("drop table agent_job_items"));
            QStringList columns{
                QStringLiteral("item_id text not null"),
                QStringLiteral("updated_at integer not null"),
            };
            if (omittedColumn != QStringLiteral("job_id")) {
                columns.append(QStringLiteral("job_id text not null"));
            }
            if (omittedColumn != QStringLiteral("assigned_thread_id")) {
                columns.append(QStringLiteral("assigned_thread_id text"));
            }
            database.execute(
                QStringLiteral("create table agent_job_items (%1)")
                    .arg(columns.join(QStringLiteral(", "))));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.state_invalid"));
    }

    void splitReadsIsolateMalformedJobsWithoutMutation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"split-malformed-jobs");
            createCurrentSchema(database);
            insertCurrentRows(database);
            database.execute(QStringLiteral("drop table agent_jobs"));
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

        const QByteArray beforeHash = fileHash(databasePath);
        QStringList connectionsBefore = QSqlDatabase::connectionNames();
        connectionsBefore.sort();

        const auto threads =
            CodexStateDatabaseReader::readThreads(databasePath);
        const auto jobs =
            CodexStateDatabaseReader::readJobs(databasePath);
        const auto strict =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(
            threads.hasValue(),
            qPrintable(resultErrorMessage(threads)));
        QCOMPARE(threads.value().size(), 2);
        QVERIFY(!jobs.hasValue());
        QCOMPARE(
            jobs.error().code,
            QStringLiteral("codex.state_invalid"));
        QVERIFY(!strict.hasValue());
        QCOMPARE(
            strict.error().code,
            QStringLiteral("codex.state_invalid"));

        QCOMPARE(fileHash(databasePath), beforeHash);
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-wal")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-shm")));
        QVERIFY(!QFileInfo::exists(databasePath + QStringLiteral("-journal")));
        QStringList connectionsAfter = QSqlDatabase::connectionNames();
        connectionsAfter.sort();
        QCOMPARE(connectionsAfter, connectionsBefore);
    }

    void splitReadsMatchStrictSnapshot()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"split-compatible");
            createCurrentSchema(database);
            insertCurrentRows(database);
        }

        const auto threads =
            CodexStateDatabaseReader::readThreads(databasePath);
        const auto jobs =
            CodexStateDatabaseReader::readJobs(databasePath);
        const auto strict =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY2(
            threads.hasValue(),
            qPrintable(resultErrorMessage(threads)));
        QVERIFY2(jobs.hasValue(), qPrintable(resultErrorMessage(jobs)));
        QVERIFY2(
            strict.hasValue(),
            qPrintable(resultErrorMessage(strict)));
        QCOMPARE(threads.value(), strict.value().threads);
        QCOMPARE(jobs.value(), strict.value().jobs);
    }

    void malformedRequiredThreadDataReturnsTypedError()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(QStringLiteral("state.sqlite"));
        {
            TestDatabase database(databasePath, u"invalid-schema");
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    rollout_path text not null,
                    updated_at,
                    source text not null,
                    cwd text not null,
                    title text not null,
                    first_user_message text not null,
                    archived integer not null
                )
            )SQL"));
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('', 'sessions/invalid.jsonl', 'not-a-time', 'desktop',
                   'C:\invalid', 'Invalid', 'Invalid', 0)
            )SQL"));
        }

        const auto result =
            CodexStateDatabaseReader::readSnapshot(databasePath);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.state_invalid"));
    }

    void exactThreadLookupAllowsArchivedAndSubagentRows()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(
                QStringLiteral("state.sqlite"));
        {
            TestDatabase database(
                databasePath,
                u"exact-thread");
            createCurrentSchema(database);
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('archived-selected', 'sessions/archived.jsonl',
                   1700000000, null, null, 'desktop', 'C:\archived',
                   'Archived', 'Archived request', 1, '', null, null),
                  ('subagent-selected', 'sessions/subagent.jsonl',
                   1700000001, null, null, '{"subagent":"worker"}',
                   'C:\subagent', 'Subagent', 'Subagent request', 0,
                   '', null, null),
                  ('unrelated-malformed', '', 0, null, null, '',
                   '', '', '', 0, '', null, null)
            )SQL"));
        }

        const auto archived =
            CodexStateDatabaseReader::readThreadById(
                databasePath,
                QStringLiteral("archived-selected"));
        QVERIFY2(
            archived.hasValue(),
            qPrintable(resultErrorMessage(archived)));
        QVERIFY(archived.value().has_value());
        QCOMPARE(
            archived.value()->id,
            QStringLiteral("archived-selected"));
        QCOMPARE(
            archived.value()->rolloutPath,
            QStringLiteral(
                "sessions/archived.jsonl"));

        const auto subagent =
            CodexStateDatabaseReader::readThreadById(
                databasePath,
                QStringLiteral("subagent-selected"));
        QVERIFY2(
            subagent.hasValue(),
            qPrintable(resultErrorMessage(subagent)));
        QVERIFY(subagent.value().has_value());
        QCOMPARE(
            subagent.value()->id,
            QStringLiteral("subagent-selected"));
        QCOMPARE(
            subagent.value()->rolloutPath,
            QStringLiteral(
                "sessions/subagent.jsonl"));
    }

    void exactThreadLookupIsolatedAndValidatesSelectedRow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath =
            directory.filePath(
                QStringLiteral("state.sqlite"));
        {
            TestDatabase database(
                databasePath,
                u"exact-thread-invalid");
            createCurrentSchema(database);
            database.execute(QString::fromUtf8(R"SQL(
                insert into threads values
                  ('valid', 'sessions/valid.jsonl', 1700000000,
                   null, null, 'desktop', 'C:\valid', 'Valid',
                   'Valid request', 0, '', null, null),
                  ('blank-rollout', '   ', 1700000001, null, null,
                   'desktop', 'C:\blank', 'Blank', 'Blank request',
                   0, '', null, null),
                  ('unrelated-malformed', '', 0, null, null, '',
                   '', '', '', 0, '', null, null)
            )SQL"));
        }

        const auto valid =
            CodexStateDatabaseReader::readThreadById(
                databasePath,
                QStringLiteral("valid"));
        QVERIFY2(
            valid.hasValue(),
            qPrintable(resultErrorMessage(valid)));
        QVERIFY(valid.value().has_value());

        const auto missing =
            CodexStateDatabaseReader::readThreadById(
                databasePath,
                QStringLiteral("missing"));
        QVERIFY2(
            missing.hasValue(),
            qPrintable(resultErrorMessage(missing)));
        QVERIFY(!missing.value().has_value());

        const auto malformed =
            CodexStateDatabaseReader::readThreadById(
                databasePath,
                QStringLiteral("blank-rollout"));
        QVERIFY(!malformed.hasValue());
        QCOMPARE(
            malformed.error().code,
            QStringLiteral("codex.state_invalid"));
    }

    void sessionIndexUsesNewestValidName()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString indexPath =
            directory.filePath(QStringLiteral("session_index.jsonl"));
        writeLines(
            indexPath,
            {
                QByteArrayLiteral(
                    "{\"id\":\"thread-1\",\"thread_name\":\" Older \"}"),
                QByteArrayLiteral("{malformed"),
                QByteArrayLiteral(
                    "{\"id\":\"\",\"thread_name\":\"ignored\"}"),
                QByteArrayLiteral(
                    "{\"id\":\"thread-2\",\"thread_name\":\" Second \"}"),
                QByteArrayLiteral(
                    "{\"id\":\"thread-1\",\"thread_name\":\"Newest\"}"),
            });

        const auto names = SessionIndexReader::readNames(indexPath);
        const auto missing = SessionIndexReader::readNames(
            directory.filePath(QStringLiteral("missing.jsonl")));

        QVERIFY2(names.hasValue(), qPrintable(resultErrorMessage(names)));
        QCOMPARE(names.value().size(), 2);
        QCOMPARE(
            names.value().value(QStringLiteral("thread-1")),
            QStringLiteral("Newest"));
        QCOMPARE(
            names.value().value(QStringLiteral("thread-2")),
            QStringLiteral("Second"));
        QVERIFY2(
            missing.hasValue(), qPrintable(resultErrorMessage(missing)));
        QVERIFY(missing.value().isEmpty());
    }

    void parsesLifecycleAliases_data()
    {
        QTest::addColumn<QString>("eventType");
        QTest::addColumn<int>("expectedState");

        for (const char* type : {"task_started", "turn_started"}) {
            QTest::newRow(type)
                << QString::fromLatin1(type)
                << static_cast<int>(LifecycleState::Active);
        }
        for (const char* type : {
                 "task_complete",
                 "task_completed",
                 "turn_complete",
                 "turn_completed",
             }) {
            QTest::newRow(type)
                << QString::fromLatin1(type)
                << static_cast<int>(LifecycleState::Completed);
        }
        for (const char* type : {
                 "task_aborted",
                 "task_failed",
                 "turn_aborted",
                 "turn_failed",
             }) {
            QTest::newRow(type)
                << QString::fromLatin1(type)
                << static_cast<int>(LifecycleState::Failed);
        }
    }

    void parsesLifecycleAliases()
    {
        QFETCH(QString, eventType);
        QFETCH(int, expectedState);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        writeLines(
            rollout,
            {lifecycleEvent(
                eventType, QStringLiteral("turn-lifecycle"))});

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().lifecycle.has_value());
        QCOMPARE(
            static_cast<int>(result.value().lifecycle->state),
            expectedState);
        QCOMPARE(
            result.value().lifecycle->turnId.value(),
            QStringLiteral("turn-lifecycle"));
    }

    void acceptsLifecycleWithoutTurnId()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        writeLines(
            rollout,
            {lifecycleEvent(QStringLiteral("task_started"))});

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().lifecycle.has_value());
        QCOMPARE(
            result.value().lifecycle->state,
            LifecycleState::Active);
        QVERIFY(!result.value().lifecycle->turnId.has_value());
    }

    void readsNewestMessagesAndTurnIdsIndependently()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        writeLines(
            rollout,
            {
                responseMessage(
                    QStringLiteral("user"),
                    QStringLiteral("Older user"),
                    QStringLiteral("turn-old-user")),
                responseMessage(
                    QStringLiteral("assistant"),
                    QStringLiteral("Older assistant"),
                    QStringLiteral("turn-old-assistant")),
                QByteArrayLiteral("{malformed"),
                responseMessage(
                    QStringLiteral("user"),
                    QStringLiteral("  Latest user  "),
                    std::nullopt,
                    QStringLiteral("turn-user-metadata")),
                responseMessage(
                    QStringLiteral("assistant"),
                    QStringLiteral("  Latest assistant  "),
                    QStringLiteral("turn-assistant")),
                lifecycleEvent(
                    QStringLiteral("task_started"),
                    QStringLiteral("turn-active")),
            });

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().latestUserMessage.has_value());
        QCOMPARE(
            result.value().latestUserMessage->role,
            CodexMessageRole::User);
        QCOMPARE(
            result.value().latestUserMessage->text,
            QStringLiteral("Latest user"));
        QCOMPARE(
            result.value().latestUserMessage->turnId.value(),
            QStringLiteral("turn-user-metadata"));
        QVERIFY(result.value().latestAssistantMessage.has_value());
        QCOMPARE(
            result.value().latestAssistantMessage->role,
            CodexMessageRole::Assistant);
        QCOMPARE(
            result.value().latestAssistantMessage->text,
            QStringLiteral("Latest assistant"));
        QCOMPARE(
            result.value().latestAssistantMessage->turnId.value(),
            QStringLiteral("turn-assistant"));
        QVERIFY(result.value().lifecycle.has_value());
        QCOMPARE(
            result.value().lifecycle->turnId.value(),
            QStringLiteral("turn-active"));
    }

    void readsLegacyAssistantMessage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        writeLines(
            rollout,
            {legacyAssistantMessage(
                QStringLiteral("  Legacy assistant  "),
                QStringLiteral("turn-legacy"))});

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().latestAssistantMessage.has_value());
        QCOMPARE(
            result.value().latestAssistantMessage->text,
            QStringLiteral("Legacy assistant"));
        QCOMPARE(
            result.value().latestAssistantMessage->turnId.value(),
            QStringLiteral("turn-legacy"));
    }

    void mobileTaskTailUsesSourcePreviewProfile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("mobile-profile.jsonl"));
        const QString largeAssistant(
            static_cast<qsizetype>(
                RolloutReader::kMaximumPreviewLineBytes)
                + 64 * 1024,
            QLatin1Char('a'));
        const QByteArray largeLine = responseMessage(
            QStringLiteral("assistant"),
            largeAssistant,
            QStringLiteral("turn-mobile"));
        QVERIFY(
            largeLine.size()
            > RolloutReader::kMaximumPreviewLineBytes);
        QVERIFY(
            largeLine.size()
            < RolloutReader::kMobileTaskTailBytes);
        writeLines(
            rollout,
            {
                largeLine,
                responseMessage(
                    QStringLiteral("user"),
                    QStringLiteral("Newest user")),
                legacyAssistantMessage(
                    QStringLiteral("Legacy desktop assistant"),
                    QStringLiteral("turn-legacy")),
                lifecycleEvent(
                    QStringLiteral("task_started"),
                    QStringLiteral("turn-active")),
            });

        const auto mobile =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());
        const auto desktop =
            RolloutReader::readTail(
                rollout,
                directory.path());

        QVERIFY2(
            mobile.hasValue(),
            qPrintable(resultErrorMessage(mobile)));
        QVERIFY(mobile.value().latestAssistantMessage.has_value());
        QCOMPARE(
            mobile.value().latestAssistantMessage->text,
            largeAssistant);
        QCOMPARE(
            mobile.value().latestAssistantMessage->turnId.value(),
            QStringLiteral("turn-mobile"));
        QVERIFY(!mobile.value().latestUserMessage.has_value());
        QVERIFY(mobile.value().lifecycle.has_value());
        QCOMPARE(
            mobile.value().lifecycle->state,
            LifecycleState::Active);

        QVERIFY2(
            desktop.hasValue(),
            qPrintable(resultErrorMessage(desktop)));
        QVERIFY(desktop.value().latestAssistantMessage.has_value());
        QCOMPARE(
            desktop.value().latestAssistantMessage->text,
            QStringLiteral("Legacy desktop assistant"));
        QVERIFY(desktop.value().latestUserMessage.has_value());
    }

    void mobileTaskTailPreservesRawFragmentsAndDistinctTurnIds()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("mobile-turn-ids.jsonl"));
        const QJsonArray fragments{
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("input_text")},
                {QStringLiteral("text"), QStringLiteral("  first  ")},
            },
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("output_text")},
                {QStringLiteral("text"), QString()},
            },
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("output_text")},
                {QStringLiteral("text"), QStringLiteral(" second\tpart ")},
            },
            QJsonObject{
                {QStringLiteral("type"), QStringLiteral("ignored")},
                {QStringLiteral("text"), QStringLiteral("hidden")},
            },
        };
        writeLines(
            rollout,
            {
                responseMessageWithFragments(
                    QStringLiteral("assistant"),
                    fragments,
                    QString(),
                    QStringLiteral("metadata-message")),
                lifecycleEventWithTurnFields(
                    QStringLiteral("task_started"),
                    QJsonValue(QJsonValue::Undefined),
                    QStringLiteral("metadata-lifecycle")),
            });

        const auto mobile =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());
        const auto desktop =
            RolloutReader::readTail(
                rollout,
                directory.path());

        QVERIFY2(
            mobile.hasValue(),
            qPrintable(resultErrorMessage(mobile)));
        QVERIFY(mobile.value().latestAssistantMessage.has_value());
        QCOMPARE(
            mobile.value().latestAssistantMessage->text,
            QStringLiteral("first  \n\n second\tpart"));
        QVERIFY(
            mobile.value().latestAssistantMessage->turnId.has_value());
        QCOMPARE(
            mobile.value().latestAssistantMessage->turnId.value(),
            QString());
        QVERIFY(mobile.value().lifecycle.has_value());
        QVERIFY(!mobile.value().lifecycle->turnId.has_value());

        QVERIFY2(
            desktop.hasValue(),
            qPrintable(resultErrorMessage(desktop)));
        QVERIFY(desktop.value().latestAssistantMessage.has_value());
        QCOMPARE(
            desktop.value().latestAssistantMessage->text,
            QStringLiteral("second\tpart"));
        QCOMPARE(
            desktop.value().latestAssistantMessage->turnId.value(),
            QStringLiteral("metadata-message"));
        QVERIFY(desktop.value().lifecycle.has_value());
        QCOMPARE(
            desktop.value().lifecycle->turnId.value(),
            QStringLiteral("metadata-lifecycle"));

        writeLines(
            rollout,
            {
                lifecycleEventWithTurnFields(
                    QStringLiteral("task_started"),
                    QString(),
                    QStringLiteral("metadata-lifecycle")),
            });
        const auto directEmpty =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());
        const auto desktopDirectEmpty =
            RolloutReader::readTail(
                rollout,
                directory.path());

        QVERIFY2(
            directEmpty.hasValue(),
            qPrintable(resultErrorMessage(directEmpty)));
        QVERIFY(directEmpty.value().lifecycle.has_value());
        QVERIFY(
            directEmpty.value().lifecycle->turnId.has_value());
        QCOMPARE(
            directEmpty.value().lifecycle->turnId.value(),
            QString());
        QVERIFY(desktopDirectEmpty.value().lifecycle.has_value());
        QCOMPARE(
            desktopDirectEmpty.value().lifecycle->turnId.value(),
            QStringLiteral("metadata-lifecycle"));
    }

    void mobileTaskTailAcceptsStringContentAndMetadataTurnFallback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(
                QStringLiteral("mobile-string-content.jsonl"));
        const auto stringMessage =
            [](QJsonValue directTurnId) {
                QJsonObject payload{
                    {
                        QStringLiteral("type"),
                        QStringLiteral("message"),
                    },
                    {
                        QStringLiteral("role"),
                        QStringLiteral("assistant"),
                    },
                    {
                        QStringLiteral("content"),
                        QStringLiteral("  String assistant  "),
                    },
                    {
                        QStringLiteral(
                            "internal_chat_message_metadata_passthrough"),
                        QJsonObject{
                            {
                                QStringLiteral("turn_id"),
                                QStringLiteral("metadata-turn"),
                            },
                        },
                    },
                };
                if (!directTurnId.isUndefined()) {
                    payload.insert(
                        QStringLiteral("turn_id"),
                        std::move(directTurnId));
                }
                return compactJson({
                    {
                        QStringLiteral("timestamp"),
                        QStringLiteral(
                            "2026-07-21T12:00:00.000Z"),
                    },
                    {
                        QStringLiteral("type"),
                        QStringLiteral("response_item"),
                    },
                    {
                        QStringLiteral("payload"),
                        payload,
                    },
                });
            };

        writeLines(
            rollout,
            {
                stringMessage(
                    QJsonValue(QJsonValue::Undefined)),
            });
        const auto absentDirect =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());

        QVERIFY2(
            absentDirect.hasValue(),
            qPrintable(resultErrorMessage(absentDirect)));
        QVERIFY(
            absentDirect.value()
                .latestAssistantMessage.has_value());
        QCOMPARE(
            absentDirect.value()
                .latestAssistantMessage->text,
            QStringLiteral("String assistant"));
        QCOMPARE(
            absentDirect.value()
                .latestAssistantMessage->turnId.value(),
            QStringLiteral("metadata-turn"));

        writeLines(
            rollout,
            {
                stringMessage(42),
            });
        const auto nonStringDirect =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());

        QVERIFY2(
            nonStringDirect.hasValue(),
            qPrintable(resultErrorMessage(nonStringDirect)));
        QVERIFY(
            nonStringDirect.value()
                .latestAssistantMessage.has_value());
        QCOMPARE(
            nonStringDirect.value()
                .latestAssistantMessage->text,
            QStringLiteral("String assistant"));
        QCOMPARE(
            nonStringDirect.value()
                .latestAssistantMessage->turnId.value(),
            QStringLiteral("metadata-turn"));
    }

    void mobileTaskTailCapsAtOneMebibyte()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("mobile-cap.jsonl"));
        const QByteArray oldMessage = responseMessage(
            QStringLiteral("assistant"),
            QStringLiteral("Outside mobile tail"),
            QStringLiteral("turn-old"));
        QByteArray bytes = oldMessage + '\n';
        bytes.append(
            QByteArray(
                static_cast<qsizetype>(
                    RolloutReader::kMobileTaskTailBytes)
                    + 1024,
                'x'));
        writeFile(rollout, bytes);

        const auto mobile =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());
        const auto desktop =
            RolloutReader::readTail(
                rollout,
                directory.path());

        QVERIFY2(
            mobile.hasValue(),
            qPrintable(resultErrorMessage(mobile)));
        QVERIFY(!mobile.value().latestAssistantMessage.has_value());
        QVERIFY2(
            desktop.hasValue(),
            qPrintable(resultErrorMessage(desktop)));
        QVERIFY(desktop.value().latestAssistantMessage.has_value());
        QCOMPARE(
            desktop.value().latestAssistantMessage->text,
            QStringLiteral("Outside mobile tail"));
    }

    void mobileTaskTailPreservesExactTailBoundaryLine()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("mobile-boundary.jsonl"));
        const QByteArray assistant = responseMessage(
            QStringLiteral("assistant"),
            QStringLiteral("Boundary assistant"),
            QStringLiteral("turn-boundary"));
        const QByteArray lifecycle = lifecycleEvent(
            QStringLiteral("task_completed"),
            QStringLiteral("turn-complete"));
        QByteArray tail = assistant + '\n' + lifecycle + '\n';
        const qsizetype mobileTailBytes =
            static_cast<qsizetype>(
                RolloutReader::kMobileTaskTailBytes);
        QVERIFY(tail.size() < mobileTailBytes);
        tail.append(
            QByteArray(
                mobileTailBytes - tail.size(),
                'x'));
        const QByteArray prefix =
            QByteArrayLiteral("outside-tail-prefix");
        QByteArray bytes = prefix;
        bytes.append(tail);
        writeFile(rollout, bytes);
        QCOMPARE(
            bytes.size() - mobileTailBytes,
            prefix.size());

        const auto result =
            RolloutReader::readMobileTaskTail(
                rollout,
                directory.path());

        QVERIFY2(
            result.hasValue(),
            qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().latestAssistantMessage.has_value());
        QCOMPARE(
            result.value().latestAssistantMessage->text,
            QStringLiteral("Boundary assistant"));
        QVERIFY(result.value().lifecycle.has_value());
        QCOMPARE(
            result.value().lifecycle->state,
            LifecycleState::Completed);
    }

    void mobileTaskTailIoFailuresAreBestEffort()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString missing =
            directory.filePath(QStringLiteral("missing.jsonl"));
        const auto missingResult =
            RolloutReader::readMobileTaskTail(
                missing,
                directory.path());
        QVERIFY2(
            missingResult.hasValue(),
            qPrintable(resultErrorMessage(missingResult)));
        QCOMPARE(
            missingResult.value(),
            RolloutSnapshot{});

        const QString empty =
            directory.filePath(QStringLiteral("empty.jsonl"));
        writeFile(empty, {});
        const auto emptyResult =
            RolloutReader::readMobileTaskTail(
                empty,
                directory.path());
        QVERIFY2(
            emptyResult.hasValue(),
            qPrintable(resultErrorMessage(emptyResult)));
        QCOMPARE(
            emptyResult.value(),
            RolloutSnapshot{});

        const QString locked =
            directory.filePath(QStringLiteral("locked.jsonl"));
        writeLines(
            locked,
            {responseMessage(
                QStringLiteral("assistant"),
                QStringLiteral("Locked"))});
        const HANDLE lock = CreateFileW(
            reinterpret_cast<LPCWSTR>(locked.utf16()),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        const auto lockedResult =
            RolloutReader::readMobileTaskTail(
                locked,
                directory.path());
        CloseHandle(lock);

        QVERIFY2(
            lockedResult.hasValue(),
            qPrintable(resultErrorMessage(lockedResult)));
        QCOMPARE(
            lockedResult.value(),
            RolloutSnapshot{});
    }

    void sanitizesGeneratedContextBeforeSelectingVisibleMessages()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        writeLines(
            rollout,
            {
                responseMessageWithStringContent(
                    QStringLiteral("assistant"),
                    QStringLiteral("Older visible assistant"),
                    QStringLiteral("turn-older-assistant")),
                responseMessageWithStringContent(
                    QStringLiteral("user"),
                    QStringLiteral(
                        "<environment_context>\n"
                        "C:\\private\\workspace\n"
                        "</environment_context>\n"
                        "<in-app-browser-context "
                        "source=\"ambient-ui-state\">\n"
                        "https://private.example.test\n"
                        "</in-app-browser-context>\n"
                        "# Files mentioned by the user:\n\n"
                        "## My request for Codex:\n"
                        "<image name=[Image 1] "
                        "path=\"C:\\private\\capture.png\">\n"
                        "</image>\n"
                        "Visible request"),
                    QStringLiteral("turn-visible-user")),
                responseMessageWithStringContent(
                    QStringLiteral("assistant"),
                    QStringLiteral(
                        "<environment_context>"
                        "internal only"
                        "</environment_context>"),
                    QStringLiteral("turn-internal-assistant")),
            });

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().latestUserMessage.has_value());
        QCOMPARE(
            result.value().latestUserMessage->text,
            QStringLiteral("Visible request"));
        QCOMPARE(
            result.value().latestUserMessage->turnId.value(),
            QStringLiteral("turn-visible-user"));
        QVERIFY(result.value().latestAssistantMessage.has_value());
        QCOMPARE(
            result.value().latestAssistantMessage->text,
            QStringLiteral("Older visible assistant"));
        QCOMPARE(
            result.value().latestAssistantMessage->turnId.value(),
            QStringLiteral("turn-older-assistant"));
    }

    void enforcesSeparateMessageAndLifecycleLineLimits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        const qsizetype oversizedMessageText =
            static_cast<qsizetype>(
                RolloutReader::kMaximumPreviewLineBytes)
            + 1024;
        const qsizetype lifecyclePadding =
            static_cast<qsizetype>(
                RolloutReader::kMaximumPreviewLineBytes)
            + 1024;
        const qsizetype oversizedLifecyclePadding =
            static_cast<qsizetype>(
                RolloutReader::kMaximumLifecycleLineBytes)
            + 1024;
        writeLines(
            rollout,
            {
                responseMessage(
                    QStringLiteral("assistant"),
                    QStringLiteral("Visible assistant"),
                    QStringLiteral("turn-visible")),
                responseMessage(
                    QStringLiteral("assistant"),
                    QString(oversizedMessageText, QLatin1Char('x')),
                    QStringLiteral("turn-oversized")),
                lifecycleEvent(
                    QStringLiteral("task_started"),
                    QStringLiteral("turn-active"),
                    lifecyclePadding),
                lifecycleEvent(
                    QStringLiteral("task_failed"),
                    QStringLiteral("turn-too-large"),
                    oversizedLifecyclePadding),
            });

        const auto result =
            RolloutReader::readTail(rollout, directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(result.value().latestAssistantMessage.has_value());
        QCOMPARE(
            result.value().latestAssistantMessage->text,
            QStringLiteral("Visible assistant"));
        QVERIFY(result.value().lifecycle.has_value());
        QCOMPARE(
            result.value().lifecycle->state,
            LifecycleState::Active);
        QCOMPARE(
            result.value().lifecycle->turnId.value(),
            QStringLiteral("turn-active"));
    }

    void capsTailReadsAtEightMebibytes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rollout =
            directory.filePath(QStringLiteral("rollout.jsonl"));
        const QByteArray oldMessage = responseMessage(
            QStringLiteral("assistant"),
            QStringLiteral("Too old to scan"),
            QStringLiteral("turn-old"));
        QByteArray bytes = oldMessage + '\n';
        bytes.append(
            QByteArray(
                static_cast<qsizetype>(RolloutReader::kMaximumTailBytes)
                    + 1024,
                'x'));
        bytes.append('\n');
        writeFile(rollout, bytes);

        const auto result = RolloutReader::readTail(
            rollout,
            directory.path(),
            RolloutReader::kMaximumTailBytes * 2);

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(!result.value().latestUserMessage.has_value());
        QVERIFY(!result.value().latestAssistantMessage.has_value());
        QVERIFY(!result.value().lifecycle.has_value());
    }

    void missingRolloutReturnsEmptySnapshot()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const auto result = RolloutReader::readTail(
            QStringLiteral("sessions/missing.jsonl"),
            directory.path());

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        QVERIFY(!result.value().latestUserMessage.has_value());
        QVERIFY(!result.value().latestAssistantMessage.has_value());
        QVERIFY(!result.value().lifecycle.has_value());
    }

    void readsLiveStateWhenRequested()
    {
        if (!qEnvironmentVariableIsSet(
                "CODEX_COMPANION_VERIFY_LIVE_STATE")) {
            QSKIP("live Codex state read was not requested");
        }

        const auto environment = CodexEnvironment::discover();
        QVERIFY2(
            environment.hasValue(),
            qPrintable(resultErrorMessage(environment)));
        const auto snapshot = CodexStateDatabaseReader::readSnapshot(
            environment.value().stateDatabase);
        QVERIFY2(
            snapshot.hasValue(),
            qPrintable(resultErrorMessage(snapshot)));
        QVERIFY(!snapshot.value().threads.isEmpty());
    }
};

QTEST_GUILESS_MAIN(StateReaderTests)
#include "StateReaderTests.moc"
