#include "codex/state/SubagentReader.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>

using namespace companion;

namespace {

template <typename T>
QString resultErrorMessage(const Result<T>& result)
{
    return result.hasValue() ? QString() : result.error().message;
}

class TestDatabase final {
public:
    explicit TestDatabase(const QString& path)
        : name_(
              QStringLiteral("subagent-reader-test-")
              + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          database_(QSqlDatabase::addDatabase(
              QStringLiteral("QSQLITE"), name_))
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
        QSqlDatabase::removeDatabase(name_);
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

    void insertThread(
        const QString& id,
        const QString& title,
        const QString& source,
        qint64 updatedAtMilliseconds,
        qint64 recencyAtMilliseconds,
        bool archived = false)
    {
        QSqlQuery query(database_);
        if (!query.prepare(QStringLiteral(
                "insert into threads "
                "(id, title, source, updated_at, updated_at_ms, "
                "recency_at_ms, archived) "
                "values (?, ?, ?, ?, ?, ?, ?)"))) {
            qFatal(
                "could not prepare fixture insert: %s",
                qPrintable(query.lastError().text()));
        }
        query.addBindValue(id);
        query.addBindValue(title);
        query.addBindValue(source);
        query.addBindValue(updatedAtMilliseconds / 1000);
        query.addBindValue(updatedAtMilliseconds);
        query.addBindValue(recencyAtMilliseconds);
        query.addBindValue(archived ? 1 : 0);
        if (!query.exec()) {
            qFatal(
                "fixture insert failed: %s",
                qPrintable(query.lastError().text()));
        }
    }

private:
    QString name_;
    QSqlDatabase database_;
};

QString subagentSource(
    const QString& parentId,
    const QString& nickname,
    const QString& role,
    const QString& privateInstruction = {})
{
    QJsonObject spawn{
        {QStringLiteral("parent_thread_id"), parentId},
        {QStringLiteral("depth"), 1},
        {QStringLiteral("agent_nickname"), nickname},
        {QStringLiteral("agent_role"), role},
    };
    if (!privateInstruction.isEmpty()) {
        spawn.insert(
            QStringLiteral("private_instruction"),
            privateInstruction);
    }
    return QString::fromUtf8(
        QJsonDocument(QJsonObject{
            {QStringLiteral("subagent"),
             QJsonObject{
                 {QStringLiteral("thread_spawn"), spawn},
             }},
        }).toJson(QJsonDocument::Compact));
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("could not read fixture database");
    }
    return QCryptographicHash::hash(
        file.readAll(), QCryptographicHash::Sha256);
}

double bridgeSeconds(qint64 unixMilliseconds)
{
    constexpr qint64 kReferenceDateUnixMilliseconds =
        978307200000LL;
    return static_cast<double>(
               unixMilliseconds -
               kReferenceDateUnixMilliseconds) /
        1000.0;
}

} // namespace

class SubagentReaderTests final : public QObject {
    Q_OBJECT

private slots:
    void projectsSelectedParentWithStatusPrivacyAndRecencyOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("state.sqlite"));
        const QDateTime now = QDateTime::fromString(
            QStringLiteral("2026-07-22T02:00:00.000Z"),
            Qt::ISODateWithMs);
        QVERIFY(now.isValid());
        const qint64 nowMs = now.toMSecsSinceEpoch();

        {
            TestDatabase database(path);
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    title text not null,
                    source text not null,
                    updated_at integer not null,
                    updated_at_ms integer,
                    recency_at_ms integer,
                    archived integer not null
                )
            )SQL"));
            database.insertThread(
                QStringLiteral("parent"),
                QStringLiteral("Parent task"),
                QStringLiteral("user"),
                nowMs,
                100);
            database.insertThread(
                QStringLiteral("agent-new"),
                QStringLiteral("Build mobile timeline"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Noether"),
                    QStringLiteral("worker"),
                    QStringLiteral("secretImplementation")),
                nowMs - 60'000,
                500);
            database.insertThread(
                QStringLiteral("agent-wait"),
                QStringLiteral("Approve transport"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Turing"),
                    QStringLiteral("reviewer")),
                nowMs - 600'000,
                450);
            database.insertThread(
                QStringLiteral("agent-boundary"),
                QStringLiteral("Check boundary"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Hopper"),
                    QStringLiteral("explorer")),
                nowMs - 180'000,
                400);
            database.insertThread(
                QStringLiteral("agent-default"),
                QStringLiteral("   "),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("   "),
                    QStringLiteral("   ")),
                nowMs - 181'000,
                350);
            database.insertThread(
                QStringLiteral("agent-old"),
                QStringLiteral("Inspect transport"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Curie"),
                    QStringLiteral("explorer")),
                nowMs - 600'000,
                300);
            database.insertThread(
                QStringLiteral("agent-other"),
                QStringLiteral("Unrelated agent"),
                subagentSource(
                    QStringLiteral("other"),
                    QStringLiteral("Hume"),
                    QStringLiteral("explorer")),
                nowMs - 10'000,
                700);
            database.insertThread(
                QStringLiteral("agent-archived"),
                QStringLiteral("Archived agent"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Ada"),
                    QStringLiteral("worker")),
                nowMs - 10'000,
                650,
                true);
            database.insertThread(
                QStringLiteral("agent-malformed"),
                QStringLiteral("Malformed source"),
                QStringLiteral(R"({"subagent":{"other":{}}})"),
                nowMs - 10'000,
                600);
        }

        const QByteArray beforeHash = fileHash(path);
        QStringList connectionsBefore = QSqlDatabase::connectionNames();
        connectionsBefore.sort();
        const Result<QVector<BridgeSubagent>> result =
            SubagentReader::read(
                path,
                QStringLiteral("  parent  "),
                QSet<QString>{QStringLiteral("agent-wait")},
                now,
                8);

        QVERIFY2(result.hasValue(), qPrintable(resultErrorMessage(result)));
        const QVector<BridgeSubagent>& agents = result.value();
        QCOMPARE(
            QVector<QString>({
                agents.at(0).id,
                agents.at(1).id,
                agents.at(2).id,
                agents.at(3).id,
                agents.at(4).id,
            }),
            QVector<QString>({
                QStringLiteral("agent-new"),
                QStringLiteral("agent-wait"),
                QStringLiteral("agent-boundary"),
                QStringLiteral("agent-default"),
                QStringLiteral("agent-old"),
            }));

        QCOMPARE(agents.at(0).name, QStringLiteral("Noether"));
        QCOMPARE(
            agents.at(0).title,
            QStringLiteral("Build mobile timeline"));
        QCOMPARE(
            agents.at(0).role,
            std::optional<QString>(QStringLiteral("worker")));
        QCOMPARE(agents.at(0).status, TaskStatus::Running);
        QCOMPARE(agents.at(0).needsApproval, std::optional<bool>(false));
        QCOMPARE(
            agents.at(0).updatedAt.secondsSinceReferenceDate,
            bridgeSeconds(nowMs - 60'000));

        QCOMPARE(agents.at(1).status, TaskStatus::Waiting);
        QCOMPARE(agents.at(1).needsApproval, std::optional<bool>(true));
        QCOMPARE(
            agents.at(2).status,
            TaskStatus::Completed);
        QCOMPARE(agents.at(3).name, QStringLiteral("Subagent"));
        QCOMPARE(
            agents.at(3).title,
            QStringLiteral("Subagent task"));
        QVERIFY(!agents.at(3).role.has_value());
        QCOMPARE(agents.at(4).name, QStringLiteral("Curie"));
        QCOMPARE(agents.at(4).status, TaskStatus::Completed);

        QStringList visible;
        for (const BridgeSubagent& agent : agents) {
            visible.append(agent.id);
            visible.append(agent.name);
            visible.append(agent.title);
            if (agent.role.has_value()) {
                visible.append(*agent.role);
            }
        }
        QVERIFY(!visible.join(QLatin1Char('\n')).contains(
            QStringLiteral("secretImplementation")));

        QCOMPARE(fileHash(path), beforeHash);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral("-wal")));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral("-shm")));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral("-journal")));
        QStringList connectionsAfter = QSqlDatabase::connectionNames();
        connectionsAfter.sort();
        QCOMPARE(connectionsAfter, connectionsBefore);
    }

    void clampsLimitAndUsesStableInputErrors()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("state.sqlite"));
        const QDateTime now = QDateTime::fromString(
            QStringLiteral("2026-07-22T02:00:00.000Z"),
            Qt::ISODateWithMs);
        const qint64 nowMs = now.toMSecsSinceEpoch();

        {
            TestDatabase database(path);
            database.execute(QString::fromUtf8(R"SQL(
                create table threads (
                    id text primary key,
                    title text not null,
                    source text not null,
                    updated_at integer not null,
                    updated_at_ms integer,
                    recency_at_ms integer,
                    archived integer not null
                )
            )SQL"));
            database.insertThread(
                QStringLiteral("agent-1"),
                QStringLiteral("Newest"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("One"),
                    QStringLiteral("worker")),
                nowMs - 10'000,
                200);
            database.insertThread(
                QStringLiteral("agent-2"),
                QStringLiteral("Older"),
                subagentSource(
                    QStringLiteral("parent"),
                    QStringLiteral("Two"),
                    QStringLiteral("worker")),
                nowMs - 20'000,
                100);
        }

        const auto clamped = SubagentReader::read(
            path, QStringLiteral("parent"), {}, now, 0);
        QVERIFY2(
            clamped.hasValue(),
            qPrintable(resultErrorMessage(clamped)));
        QCOMPARE(clamped.value().size(), 1);
        QCOMPARE(
            clamped.value().first().id,
            QStringLiteral("agent-1"));

        const auto invalidParent = SubagentReader::read(
            path, QStringLiteral("  "), {}, now, 8);
        QVERIFY(!invalidParent.hasValue());
        QCOMPARE(
            invalidParent.error().code,
            QStringLiteral("codex.invalid_thread_id"));

        const auto missing = SubagentReader::read(
            directory.filePath(QStringLiteral("missing.sqlite")),
            QStringLiteral("parent"),
            {},
            now,
            8);
        QVERIFY(!missing.hasValue());
        QCOMPARE(
            missing.error().code,
            QStringLiteral("codex.state_missing"));
    }
};

QTEST_GUILESS_MAIN(SubagentReaderTests)
#include "SubagentReaderTests.moc"
