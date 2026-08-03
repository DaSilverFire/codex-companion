#include "codex/state/CodexStateDatabaseReader.h"

#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace companion {

namespace {

class ScopedSqlConnection final {
public:
    ScopedSqlConnection()
        : name_(
              QStringLiteral("codex-state-")
              + QUuid::createUuid().toString(QUuid::WithoutBraces)),
          database_(QSqlDatabase::addDatabase(
              QStringLiteral("QSQLITE"), name_))
    {
    }

    ~ScopedSqlConnection()
    {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(name_);
    }

    ScopedSqlConnection(const ScopedSqlConnection&) = delete;
    ScopedSqlConnection& operator=(const ScopedSqlConnection&) = delete;

    QSqlDatabase& database() noexcept
    {
        return database_;
    }

private:
    QString name_;
    QSqlDatabase database_;
};

CompanionError stateError(
    QString code,
    QString message,
    const QString& databasePath,
    const QString& detail = {})
{
    QVariantMap context{{QStringLiteral("path"), databasePath}};
    if (!detail.isEmpty()) {
        context.insert(QStringLiteral("detail"), detail);
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

QString databaseUri(const QString& databasePath)
{
    QUrl uri = QUrl::fromLocalFile(
        QFileInfo(databasePath).absoluteFilePath());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("mode"), QStringLiteral("ro"));
    uri.setQuery(query);
    return uri.toString(QUrl::FullyEncoded);
}

Result<void> setQueryOnly(
    QSqlDatabase& database,
    const QString& databasePath)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("pragma query_only = on"))) {
        return Result<void>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not make the Codex state connection query-only."),
            databasePath,
            query.lastError().text()));
    }
    return Result<void>::success();
}

Result<QSet<QString>> tableNames(
    QSqlDatabase& database,
    const QString& databasePath)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "select name from sqlite_master "
            "where type = 'table' order by name"))) {
        return Result<QSet<QString>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not inspect the Codex state schema."),
            databasePath,
            query.lastError().text()));
    }

    QSet<QString> names;
    while (query.next()) {
        names.insert(query.value(0).toString());
    }
    return Result<QSet<QString>>::success(std::move(names));
}

Result<QSet<QString>> columnNames(
    QSqlDatabase& database,
    const QString& tableName,
    const QString& databasePath)
{
    QSqlQuery query(database);
    const QString sql = QStringLiteral("pragma table_info(\"%1\")")
        .arg(tableName);
    if (!query.exec(sql)) {
        return Result<QSet<QString>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not inspect a Codex state table."),
            databasePath,
            query.lastError().text()));
    }

    QSet<QString> names;
    while (query.next()) {
        names.insert(query.value(1).toString());
    }
    return Result<QSet<QString>>::success(std::move(names));
}

bool hasRequiredColumns(
    const QSet<QString>& columns,
    std::initializer_list<QStringView> required)
{
    return std::all_of(
        required.begin(),
        required.end(),
        [&columns](QStringView name) {
            return columns.contains(name.toString());
        });
}

QString optionalExpression(
    const QSet<QString>& columns,
    QStringView name)
{
    return columns.contains(name.toString())
        ? name.toString()
        : QStringLiteral("null");
}

std::optional<QString> nonemptyString(const QVariant& value)
{
    if (value.isNull()) {
        return std::nullopt;
    }
    const QString trimmed = value.toString().trimmed();
    return trimmed.isEmpty()
        ? std::nullopt
        : std::optional<QString>(trimmed);
}

std::optional<QDateTime> numericTimestamp(double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }

    constexpr double kMillisecondThreshold = 100'000'000'000.0;
    const double milliseconds = value >= kMillisecondThreshold
        ? value
        : value * 1000.0;
    if (milliseconds >
        static_cast<double>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }

    const QDateTime date = QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(milliseconds), QTimeZone::UTC);
    return date.isValid()
        ? std::optional<QDateTime>(date)
        : std::nullopt;
}

std::optional<QDateTime> timestamp(const QVariant& value)
{
    if (value.isNull()) {
        return std::nullopt;
    }

    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return numericTimestamp(value.toDouble());
    default:
        break;
    }

    const QString text = value.toString().trimmed();
    if (text.isEmpty()) {
        return std::nullopt;
    }

    bool numeric = false;
    const double number = text.toDouble(&numeric);
    if (numeric) {
        return numericTimestamp(number);
    }

    QDateTime parsed =
        QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(text, Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return std::nullopt;
    }
    return parsed.toUTC();
}

std::optional<qint64> integer(const QVariant& value)
{
    if (value.isNull()) {
        return std::nullopt;
    }
    bool valid = false;
    const qint64 parsed = value.toLongLong(&valid);
    return valid
        ? std::optional<qint64>(parsed)
        : std::nullopt;
}

std::optional<GoalStatus> goalStatus(const QVariant& value)
{
    QString normalized =
        value.toString().trimmed().toLower();
    normalized.remove(QLatin1Char('_'));
    normalized.remove(QLatin1Char('-'));
    if (normalized == QStringLiteral("active")) {
        return GoalStatus::Active;
    }
    if (normalized == QStringLiteral("paused")) {
        return GoalStatus::Paused;
    }
    if (normalized == QStringLiteral("blocked")) {
        return GoalStatus::Blocked;
    }
    if (normalized == QStringLiteral("usagelimited")) {
        return GoalStatus::UsageLimited;
    }
    if (normalized == QStringLiteral("budgetlimited")) {
        return GoalStatus::BudgetLimited;
    }
    if (normalized == QStringLiteral("complete")) {
        return GoalStatus::Complete;
    }
    return std::nullopt;
}

Result<QVector<CodexThreadRecord>> readThreadsFromConnection(
    QSqlDatabase& database,
    const QSet<QString>& tables,
    const QString& databasePath)
{
    if (!tables.contains(QStringLiteral("threads"))) {
        return Result<QVector<CodexThreadRecord>>::failure(stateError(
            QStringLiteral("codex.state_invalid"),
            QStringLiteral("The Codex state database is missing the threads table."),
            databasePath));
    }

    const auto columnsResult =
        columnNames(database, QStringLiteral("threads"), databasePath);
    if (!columnsResult.hasValue()) {
        return Result<QVector<CodexThreadRecord>>::failure(
            columnsResult.error());
    }
    const QSet<QString>& columns = columnsResult.value();
    if (!hasRequiredColumns(
            columns,
            {
                u"id",
                u"title",
                u"cwd",
                u"first_user_message",
                u"rollout_path",
                u"updated_at",
                u"archived",
                u"source",
            })) {
        return Result<QVector<CodexThreadRecord>>::failure(stateError(
            QStringLiteral("codex.state_invalid"),
            QStringLiteral("The Codex threads table is missing required fields."),
            databasePath));
    }

    const QString sql = QStringLiteral(
        "select id, title, cwd, first_user_message, rollout_path, "
        "%1, %2, %3, updated_at, %4, %5 "
        "from threads "
        "where archived = 0 "
        "and source not like :subagent")
        .arg(
            optionalExpression(columns, u"preview"),
            optionalExpression(columns, u"model"),
            optionalExpression(columns, u"reasoning_effort"),
            optionalExpression(columns, u"updated_at_ms"),
            optionalExpression(columns, u"recency_at_ms"));

    QSqlQuery query(database);
    if (!query.prepare(sql)) {
        return Result<QVector<CodexThreadRecord>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not prepare the Codex thread query."),
            databasePath,
            query.lastError().text()));
    }
    query.bindValue(
        QStringLiteral(":subagent"),
        QStringLiteral("{\"subagent\":%"));
    if (!query.exec()) {
        return Result<QVector<CodexThreadRecord>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not read Codex threads."),
            databasePath,
            query.lastError().text()));
    }

    QVector<CodexThreadRecord> records;
    while (query.next()) {
        const QString id = query.value(0).toString().trimmed();
        const QString rolloutPath =
            query.value(4).toString().trimmed();
        auto updatedAt = timestamp(query.value(9));
        if (!updatedAt.has_value()) {
            updatedAt = timestamp(query.value(8));
        }
        if (id.isEmpty() || rolloutPath.isEmpty() ||
            !updatedAt.has_value()) {
            return Result<QVector<CodexThreadRecord>>::failure(stateError(
                QStringLiteral("codex.state_invalid"),
                QStringLiteral("A Codex thread contains malformed required data."),
                databasePath,
                id));
        }

        const QDateTime recencyAt =
            timestamp(query.value(10)).value_or(*updatedAt);
        records.append(CodexThreadRecord{
            id,
            query.value(1).toString().trimmed(),
            query.value(2).toString().trimmed(),
            query.value(3).toString().trimmed(),
            rolloutPath,
            query.value(5).toString().trimmed(),
            nonemptyString(query.value(6)),
            nonemptyString(query.value(7)),
            *updatedAt,
            recencyAt,
        });
    }

    std::sort(
        records.begin(),
        records.end(),
        [](const CodexThreadRecord& left, const CodexThreadRecord& right) {
            if (left.recencyAt != right.recencyAt) {
                return left.recencyAt > right.recencyAt;
            }
            if (left.updatedAt != right.updatedAt) {
                return left.updatedAt > right.updatedAt;
            }
            return left.id < right.id;
        });
    return Result<QVector<CodexThreadRecord>>::success(
        std::move(records));
}

Result<QVector<CodexGoalCandidateRecord>>
readGoalCandidatesFromConnection(
    QSqlDatabase& database,
    const QSet<QString>& tables,
    const QString& databasePath,
    const QDateTime& now)
{
    if (!tables.contains(
            QStringLiteral("thread_goals"))) {
        return Result<
            QVector<CodexGoalCandidateRecord>>::success({});
    }

    const auto goalColumnsResult =
        columnNames(
            database,
            QStringLiteral("thread_goals"),
            databasePath);
    if (!goalColumnsResult.hasValue()) {
        return Result<
            QVector<CodexGoalCandidateRecord>>::failure(
            goalColumnsResult.error());
    }
    const QSet<QString>& goalColumns =
        goalColumnsResult.value();
    if (!hasRequiredColumns(
            goalColumns,
            {u"thread_id", u"status"})) {
        return Result<
            QVector<CodexGoalCandidateRecord>>::failure(
            stateError(
                QStringLiteral("codex.state_invalid"),
                QStringLiteral(
                    "The Codex goal state is missing required fields."),
                databasePath));
    }

    bool joinsThreads =
        tables.contains(QStringLiteral("threads"));
    QSet<QString> threadColumns;
    if (joinsThreads) {
        const auto threadColumnsResult =
            columnNames(
                database,
                QStringLiteral("threads"),
                databasePath);
        if (!threadColumnsResult.hasValue()) {
            return Result<
                QVector<CodexGoalCandidateRecord>>::failure(
                threadColumnsResult.error());
        }
        threadColumns = threadColumnsResult.value();
        if (!hasRequiredColumns(
                threadColumns,
                {u"id", u"updated_at", u"archived", u"source"})) {
            return Result<
                QVector<CodexGoalCandidateRecord>>::failure(
                stateError(
                    QStringLiteral("codex.state_invalid"),
                    QStringLiteral(
                        "The Codex goal state is missing required thread fields."),
                    databasePath));
        }
    }

    const bool hasGoalUpdatedAt =
        goalColumns.contains(
            QStringLiteral("updated_at_ms"));
    const QString goalUpdatedAtMilliseconds =
        hasGoalUpdatedAt
        ? QStringLiteral(
              "coalesce(g.updated_at_ms, 0)")
        : QStringLiteral("0");
    const QString goalUpdatedAtSeconds =
        QStringLiteral("%1 / 1000")
            .arg(goalUpdatedAtMilliseconds);
    const QString normalizedGoalStatus =
        QStringLiteral(
            "lower(trim(coalesce(g.status, '')))");
    QString goalEligibility = QStringLiteral(
        "%1 in ("
        "'active', 'paused', 'blocked', "
        "'usage_limited', 'usagelimited', "
        "'budget_limited', 'budgetlimited')")
        .arg(normalizedGoalStatus);
    if (hasGoalUpdatedAt) {
        goalEligibility += QStringLiteral(
            " or (%1 = 'complete' "
            "and %2 >= :recentCompleteCutoff)")
            .arg(
                normalizedGoalStatus,
                goalUpdatedAtMilliseconds);
    }
    const auto goalExpression =
        [&goalColumns](QStringView column) {
            return goalColumns.contains(
                       column.toString())
                ? QStringLiteral("g.%1")
                      .arg(column)
                : QStringLiteral("null");
        };
    const QString activityExpression =
        joinsThreads
        ? QStringLiteral("max(t.updated_at, %1)")
              .arg(goalUpdatedAtSeconds)
        : goalUpdatedAtSeconds;
    const QString fromAndFilter =
        joinsThreads
        ? QStringLiteral(
              "from threads t "
              "inner join thread_goals g "
              "on g.thread_id = t.id "
              "where t.archived = 0 "
              "and t.source not like :subagent "
              "and (%1)")
              .arg(goalEligibility)
        : QStringLiteral(
              "from thread_goals g "
              "where (%1)")
              .arg(goalEligibility);
    const QString sql = QStringLiteral(
        "select g.thread_id, g.status, "
        "%1 as activity_at, "
        "%2, %3, %4, %5, %6, %7 "
        "%8 "
        "order by activity_at desc, g.thread_id asc "
        "limit 50")
        .arg(
            activityExpression,
            goalExpression(u"objective"),
            goalExpression(u"token_budget"),
            goalExpression(u"tokens_used"),
            goalExpression(u"time_used_seconds"),
            goalExpression(u"created_at_ms"),
            goalExpression(u"updated_at_ms"),
            fromAndFilter);

    QSqlQuery query(database);
    if (!query.prepare(sql)) {
        return Result<
            QVector<CodexGoalCandidateRecord>>::failure(
            stateError(
                QStringLiteral("codex.state_read_failed"),
                QStringLiteral(
                    "Could not prepare the Codex goal-candidate query."),
                databasePath,
                query.lastError().text()));
    }
    if (joinsThreads) {
        query.bindValue(
            QStringLiteral(":subagent"),
            QStringLiteral("{\"subagent\":%"));
    }
    if (hasGoalUpdatedAt) {
        query.bindValue(
            QStringLiteral(
                ":recentCompleteCutoff"),
            now.addSecs(-30 * 60)
                .toMSecsSinceEpoch());
    }
    if (!query.exec()) {
        return Result<
            QVector<CodexGoalCandidateRecord>>::failure(
            stateError(
                QStringLiteral("codex.state_read_failed"),
                QStringLiteral(
                    "Could not read Codex goal candidates."),
                databasePath,
                query.lastError().text()));
    }

    QVector<CodexGoalCandidateRecord> records;
    QSet<QString> seenThreadIds;
    while (query.next()) {
        const QString threadId =
            query.value(0).toString().trimmed();
        const auto status =
            goalStatus(query.value(1));
        if (threadId.isEmpty()
            || !status.has_value()
            || seenThreadIds.contains(threadId)) {
            continue;
        }
        seenThreadIds.insert(threadId);

        const qint64 fallbackTimestamp =
            now.toMSecsSinceEpoch();
        const qint64 updatedAt =
            integer(query.value(8))
                .value_or(fallbackTimestamp);
        const qint64 createdAt =
            integer(query.value(7))
                .value_or(updatedAt);
        std::optional<qint64> tokenBudget;
        if (!query.value(4).isNull()) {
            tokenBudget = integer(
                query.value(4));
        }
        BridgeGoal goal{
            threadId,
            query.value(3).toString().trimmed(),
            *status,
            tokenBudget,
            integer(query.value(5)).value_or(0),
            integer(query.value(6)).value_or(0),
            createdAt,
            updatedAt,
        };
        const QDateTime activityAt =
            timestamp(query.value(2))
                .value_or(
                    QDateTime::fromMSecsSinceEpoch(
                        updatedAt,
                        QTimeZone::UTC));
        records.append({
            std::move(goal),
            activityAt,
        });
    }
    return Result<
        QVector<CodexGoalCandidateRecord>>::success(
        std::move(records));
}

Result<std::optional<CodexHistoryThreadRecord>>
readThreadByIdFromConnection(
    QSqlDatabase& database,
    const QSet<QString>& tables,
    const QString& databasePath,
    const QString& requestedThreadId)
{
    if (!tables.contains(QStringLiteral("threads"))) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_invalid"),
                QStringLiteral(
                    "The Codex state database is missing the threads table."),
                databasePath));
    }

    const auto columnsResult =
        columnNames(
            database,
            QStringLiteral("threads"),
            databasePath);
    if (!columnsResult.hasValue()) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            columnsResult.error());
    }
    if (!hasRequiredColumns(
            columnsResult.value(),
            {
                u"id",
                u"rollout_path",
            })) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_invalid"),
                QStringLiteral(
                    "The Codex threads table is missing required history fields."),
                databasePath));
    }

    const QString threadId =
        requestedThreadId.trimmed();
    if (threadId.isEmpty()) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_invalid"),
                QStringLiteral(
                    "The selected Codex thread is invalid."),
                databasePath));
    }

    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral(
            "select id, rollout_path from threads "
            "where id = :threadId limit 1"))) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_read_failed"),
                QStringLiteral(
                    "Could not prepare the Codex history thread query."),
                databasePath,
                query.lastError().text()));
    }
    query.bindValue(
        QStringLiteral(":threadId"),
        threadId);
    if (!query.exec()) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_read_failed"),
                QStringLiteral(
                    "Could not read the selected Codex thread."),
                databasePath,
                query.lastError().text()));
    }
    if (!query.next()) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::success(
            std::nullopt);
    }

    const QString selectedId =
        query.value(0).toString().trimmed();
    const QString rolloutPath =
        query.value(1).toString().trimmed();
    if (selectedId.isEmpty()
        || rolloutPath.isEmpty()) {
        return Result<
            std::optional<
                CodexHistoryThreadRecord>>::failure(
            stateError(
                QStringLiteral(
                    "codex.state_invalid"),
                QStringLiteral(
                    "The selected Codex thread contains malformed history data."),
                databasePath,
                selectedId));
    }

    return Result<
        std::optional<
            CodexHistoryThreadRecord>>::success(
        CodexHistoryThreadRecord{
            selectedId,
            rolloutPath,
        });
}

Result<QVector<CodexJobRecord>> readJobsFromConnection(
    QSqlDatabase& database,
    const QSet<QString>& tables,
    const QString& databasePath)
{
    if (!tables.contains(QStringLiteral("agent_jobs"))) {
        return Result<QVector<CodexJobRecord>>::success({});
    }

    const auto columnsResult =
        columnNames(database, QStringLiteral("agent_jobs"), databasePath);
    if (!columnsResult.hasValue()) {
        return Result<QVector<CodexJobRecord>>::failure(
            columnsResult.error());
    }
    const QSet<QString>& columns = columnsResult.value();
    if (!hasRequiredColumns(
            columns,
            {
                u"id",
                u"name",
                u"status",
                u"instruction",
                u"created_at",
                u"updated_at",
            })) {
        return Result<QVector<CodexJobRecord>>::failure(stateError(
            QStringLiteral("codex.state_invalid"),
            QStringLiteral("The Codex jobs table is missing required fields."),
            databasePath));
    }

    QString assignedThreadExpression = QStringLiteral("null");
    if (tables.contains(QStringLiteral("agent_job_items"))) {
        const auto itemColumnsResult = columnNames(
            database,
            QStringLiteral("agent_job_items"),
            databasePath);
        if (!itemColumnsResult.hasValue()) {
            return Result<QVector<CodexJobRecord>>::failure(
                itemColumnsResult.error());
        }
        const QSet<QString>& itemColumns = itemColumnsResult.value();
        if (!hasRequiredColumns(
                itemColumns,
                {u"job_id", u"assigned_thread_id"})) {
            return Result<QVector<CodexJobRecord>>::failure(stateError(
                QStringLiteral("codex.state_invalid"),
                QStringLiteral(
                    "The Codex job-items table is missing required fields."),
                databasePath));
        }
        const QString ordering = itemColumns.contains(
            QStringLiteral("updated_at"))
            ? QStringLiteral("updated_at desc")
            : QStringLiteral("rowid desc");
        assignedThreadExpression = QStringLiteral(
            "(select assigned_thread_id from agent_job_items "
            "where job_id = agent_jobs.id "
            "and assigned_thread_id is not null "
            "and assigned_thread_id != '' "
            "order by %1 limit 1)")
            .arg(ordering);
    }

    const QString sql = QStringLiteral(
        "select id, name, status, instruction, %1, created_at, "
        "updated_at, %2, %3 from agent_jobs")
        .arg(
            optionalExpression(columns, u"last_error"),
            optionalExpression(columns, u"started_at"),
            assignedThreadExpression);

    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return Result<QVector<CodexJobRecord>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not read Codex jobs."),
            databasePath,
            query.lastError().text()));
    }

    QVector<CodexJobRecord> records;
    while (query.next()) {
        const QString id = query.value(0).toString().trimmed();
        const QString name = query.value(1).toString().trimmed();
        const QString status = query.value(2).toString().trimmed();
        const QString instruction =
            query.value(3).toString().trimmed();
        const auto createdAt = timestamp(query.value(5));
        const auto updatedAt = timestamp(query.value(6));
        if (id.isEmpty() || status.isEmpty() ||
            !createdAt.has_value() || !updatedAt.has_value()) {
            return Result<QVector<CodexJobRecord>>::failure(stateError(
                QStringLiteral("codex.state_invalid"),
                QStringLiteral("A Codex job contains malformed required data."),
                databasePath,
                id));
        }

        auto startedAt = timestamp(query.value(7));
        if (!startedAt.has_value()) {
            startedAt = createdAt;
        }
        records.append(CodexJobRecord{
            id,
            name,
            status,
            instruction,
            nonemptyString(query.value(4)),
            nonemptyString(query.value(8)),
            *updatedAt,
            startedAt,
        });
    }

    std::sort(
        records.begin(),
        records.end(),
        [](const CodexJobRecord& left, const CodexJobRecord& right) {
            if (left.updatedAt != right.updatedAt) {
                return left.updatedAt > right.updatedAt;
            }
            return left.id < right.id;
        });
    return Result<QVector<CodexJobRecord>>::success(std::move(records));
}

template <typename Value, typename Reader>
Result<Value> readDatabase(
    const QString& databasePath,
    Reader&& reader)
{
    const QFileInfo databaseFile(databasePath);
    if (!databaseFile.isFile()) {
        return Result<Value>::failure(stateError(
            QStringLiteral("codex.state_missing"),
            QStringLiteral("The Codex state database was not found."),
            databasePath));
    }

    ScopedSqlConnection connection;
    QSqlDatabase& database = connection.database();
    database.setConnectOptions(
        QStringLiteral(
            "QSQLITE_OPEN_READONLY;"
            "QSQLITE_OPEN_URI;"
            "QSQLITE_BUSY_TIMEOUT=1000"));
    database.setDatabaseName(databaseUri(databasePath));
    if (!database.open()) {
        return Result<Value>::failure(stateError(
            QStringLiteral("codex.state_open_failed"),
            QStringLiteral("Could not open the Codex state database."),
            databasePath,
            database.lastError().text()));
    }

    const auto queryOnly = setQueryOnly(database, databasePath);
    if (!queryOnly.hasValue()) {
        return Result<Value>::failure(queryOnly.error());
    }

    const auto tables = tableNames(database, databasePath);
    if (!tables.hasValue()) {
        return Result<Value>::failure(tables.error());
    }

    return reader(database, tables.value(), databasePath);
}

} // namespace

Result<std::optional<CodexHistoryThreadRecord>>
CodexStateDatabaseReader::readThreadById(
    const QString& databasePath,
    const QString& threadId)
{
    return readDatabase<
        std::optional<
            CodexHistoryThreadRecord>>(
        databasePath,
        [&threadId](
            QSqlDatabase& database,
            const QSet<QString>& tables,
            const QString& path) {
            return readThreadByIdFromConnection(
                database,
                tables,
                path,
                threadId);
        });
}

Result<QVector<CodexThreadRecord>>
CodexStateDatabaseReader::readThreads(
    const QString& databasePath)
{
    return readDatabase<QVector<CodexThreadRecord>>(
        databasePath,
        [](QSqlDatabase& database,
           const QSet<QString>& tables,
           const QString& path) {
            return readThreadsFromConnection(
                database, tables, path);
        });
}

Result<QVector<CodexGoalCandidateRecord>>
CodexStateDatabaseReader::readGoalCandidates(
    const QString& databasePath)
{
    return readGoalCandidates(
        databasePath,
        QDateTime::currentDateTimeUtc());
}

Result<QVector<CodexGoalCandidateRecord>>
CodexStateDatabaseReader::readGoalCandidates(
    const QString& databasePath,
    const QDateTime& now)
{
    const QDateTime effectiveNow =
        now.isValid()
        ? now.toUTC()
        : QDateTime::currentDateTimeUtc();
    return readDatabase<
        QVector<CodexGoalCandidateRecord>>(
        databasePath,
        [effectiveNow](
            QSqlDatabase& database,
            const QSet<QString>& tables,
            const QString& path) {
            return readGoalCandidatesFromConnection(
                database,
                tables,
                path,
                effectiveNow);
        });
}

Result<QVector<QString>>
CodexStateDatabaseReader::
readGoalCandidateThreadIds(
    const QString& databasePath)
{
    return readGoalCandidateThreadIds(
        databasePath,
        QDateTime::currentDateTimeUtc());
}

Result<QVector<QString>>
CodexStateDatabaseReader::
readGoalCandidateThreadIds(
    const QString& databasePath,
    const QDateTime& now)
{
    auto candidates =
        readGoalCandidates(databasePath, now);
    if (!candidates.hasValue()) {
        return Result<QVector<QString>>::failure(
            candidates.error());
    }
    QVector<QString> threadIds;
    threadIds.reserve(
        candidates.value().size());
    for (const CodexGoalCandidateRecord& candidate :
         candidates.value()) {
        threadIds.append(
            candidate.goal.threadId);
    }
    return Result<QVector<QString>>::success(
        std::move(threadIds));
}

Result<QVector<CodexJobRecord>>
CodexStateDatabaseReader::readJobs(
    const QString& databasePath)
{
    return readDatabase<QVector<CodexJobRecord>>(
        databasePath,
        [](QSqlDatabase& database,
           const QSet<QString>& tables,
           const QString& path) {
            return readJobsFromConnection(
                database, tables, path);
        });
}

Result<CodexStateSnapshot> CodexStateDatabaseReader::readSnapshot(
    const QString& databasePath)
{
    return readDatabase<CodexStateSnapshot>(
        databasePath,
        [](QSqlDatabase& database,
           const QSet<QString>& tables,
           const QString& path) {
            auto threads = readThreadsFromConnection(
                database, tables, path);
            if (!threads.hasValue()) {
                return Result<CodexStateSnapshot>::failure(
                    threads.error());
            }
            auto jobs = readJobsFromConnection(
                database, tables, path);
            if (!jobs.hasValue()) {
                return Result<CodexStateSnapshot>::failure(
                    jobs.error());
            }
            return Result<CodexStateSnapshot>::success({
                std::move(threads.value()),
                std::move(jobs.value()),
            });
        });
}

} // namespace companion
