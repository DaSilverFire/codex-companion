#include "codex/state/SubagentReader.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
#include <optional>
#include <utility>

namespace companion {

namespace {

inline constexpr qint64 kReferenceDateUnixMilliseconds =
    978307200000LL;
inline constexpr qint64 kRecentSubagentMilliseconds =
    3 * 60 * 1000;

class ScopedSqlConnection final {
public:
    ScopedSqlConnection()
        : name_(
              QStringLiteral("codex-subagents-")
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

std::optional<QString> nonempty(QString value)
{
    value = value.trimmed();
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(std::move(value));
}

Result<QSet<QString>> threadColumns(
    QSqlDatabase& database,
    const QString& databasePath)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "pragma table_info(\"threads\")"))) {
        return Result<QSet<QString>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not inspect the Codex threads table."),
            databasePath,
            query.lastError().text()));
    }

    QSet<QString> columns;
    while (query.next()) {
        columns.insert(query.value(1).toString());
    }
    if (columns.isEmpty()) {
        return Result<QSet<QString>>::failure(stateError(
            QStringLiteral("codex.state_invalid"),
            QStringLiteral(
                "The Codex state database is missing the threads table."),
            databasePath));
    }
    return Result<QSet<QString>>::success(std::move(columns));
}

bool hasRequiredColumns(const QSet<QString>& columns)
{
    static const QVector<QString> required{
        QStringLiteral("id"),
        QStringLiteral("title"),
        QStringLiteral("source"),
        QStringLiteral("updated_at"),
        QStringLiteral("archived"),
    };
    return std::all_of(
        required.cbegin(),
        required.cend(),
        [&columns](const QString& column) {
            return columns.contains(column);
        });
}

QString updatedAtExpression(const QSet<QString>& columns)
{
    return columns.contains(QStringLiteral("updated_at_ms"))
        ? QStringLiteral(
              "coalesce(updated_at_ms, updated_at * 1000)")
        : QStringLiteral("updated_at * 1000");
}

QString recencyExpression(const QSet<QString>& columns)
{
    const QString updated = updatedAtExpression(columns);
    if (!columns.contains(QStringLiteral("recency_at_ms"))) {
        return updated;
    }
    return QStringLiteral(
               "coalesce(nullif(recency_at_ms, 0), %1)")
        .arg(updated);
}

std::optional<double> positiveNumber(const QVariant& value)
{
    bool valid = false;
    const double number = value.toDouble(&valid);
    if (!valid || !std::isfinite(number) || number <= 0.0) {
        return std::nullopt;
    }
    return number;
}

QDateTime distantPast()
{
    return QDateTime(
        QDate(1, 1, 1),
        QTime(0, 0),
        QTimeZone::UTC);
}

QDateTime dateFromMilliseconds(const QVariant& value)
{
    const auto milliseconds = positiveNumber(value);
    if (!milliseconds.has_value()) {
        return distantPast();
    }
    return QDateTime::fromMSecsSinceEpoch(
        static_cast<qint64>(*milliseconds),
        QTimeZone::UTC);
}

BridgeDate bridgeDate(const QDateTime& date)
{
    return {
        static_cast<double>(
            date.toMSecsSinceEpoch() -
            kReferenceDateUnixMilliseconds) /
        1000.0,
    };
}

std::optional<QJsonObject> spawnObject(const QString& source)
{
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(source.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    const QJsonValue subagent =
        document.object().value(QStringLiteral("subagent"));
    if (!subagent.isObject()) {
        return std::nullopt;
    }
    const QJsonValue spawn =
        subagent.toObject().value(QStringLiteral("thread_spawn"));
    return spawn.isObject()
        ? std::optional<QJsonObject>(spawn.toObject())
        : std::nullopt;
}

TaskStatus status(
    const QString& threadId,
    const QSet<QString>& pendingApprovalThreadIds,
    const QDateTime& updatedAt,
    const QDateTime& now)
{
    if (pendingApprovalThreadIds.contains(threadId)) {
        return TaskStatus::Waiting;
    }
    if (updatedAt.isValid() && now.isValid() &&
        updatedAt.msecsTo(now) < kRecentSubagentMilliseconds) {
        return TaskStatus::Running;
    }
    return TaskStatus::Completed;
}

struct ProjectedSubagent final {
    BridgeSubagent agent;
    double recencyMilliseconds = 0.0;
};

} // namespace

Result<QVector<BridgeSubagent>> SubagentReader::read(
    const QString& databasePath,
    const QString& parentThreadId,
    const QSet<QString>& pendingApprovalThreadIds,
    const QDateTime& now,
    int requestedLimit)
{
    const auto parent = nonempty(parentThreadId);
    if (!parent.has_value()) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.invalid_thread_id"),
            QStringLiteral("The parent thread ID is empty."),
            databasePath));
    }

    const QFileInfo databaseFile(databasePath);
    if (!databaseFile.isFile()) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
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
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.state_open_failed"),
            QStringLiteral("Could not open the Codex state database."),
            databasePath,
            database.lastError().text()));
    }

    QSqlQuery queryOnly(database);
    if (!queryOnly.exec(QStringLiteral("pragma query_only = on"))) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral(
                "Could not make the Codex state connection query-only."),
            databasePath,
            queryOnly.lastError().text()));
    }
    queryOnly.finish();

    const auto columnsResult =
        threadColumns(database, databasePath);
    if (!columnsResult.hasValue()) {
        return Result<QVector<BridgeSubagent>>::failure(
            columnsResult.error());
    }
    const QSet<QString>& columns = columnsResult.value();
    if (!hasRequiredColumns(columns)) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.state_invalid"),
            QStringLiteral(
                "The Codex threads table is missing required fields."),
            databasePath));
    }

    QSqlQuery query(database);
    const QString sql = QStringLiteral(
        "select id, title, source, %1, %2 "
        "from threads "
        "where archived = 0 "
        "and source like :subagent")
        .arg(
            updatedAtExpression(columns),
            recencyExpression(columns));
    if (!query.prepare(sql)) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not prepare the Codex subagent query."),
            databasePath,
            query.lastError().text()));
    }
    query.bindValue(
        QStringLiteral(":subagent"),
        QStringLiteral("{\"subagent\":%"));
    if (!query.exec()) {
        return Result<QVector<BridgeSubagent>>::failure(stateError(
            QStringLiteral("codex.state_read_failed"),
            QStringLiteral("Could not read Codex subagents."),
            databasePath,
            query.lastError().text()));
    }

    QVector<ProjectedSubagent> projected;
    while (query.next()) {
        const QString source = query.value(2).toString();
        const auto spawn = spawnObject(source);
        if (!spawn.has_value() ||
            spawn->value(QStringLiteral("parent_thread_id"))
                    .toString() != *parent) {
            continue;
        }

        const QString id = query.value(0).toString();
        const QDateTime updatedAt =
            dateFromMilliseconds(query.value(3));
        const bool needsApproval =
            pendingApprovalThreadIds.contains(id);
        const double recency = positiveNumber(query.value(4))
            .value_or(
                static_cast<double>(
                    updatedAt.toMSecsSinceEpoch()));
        projected.append({
            {
                id,
                nonempty(
                    spawn->value(
                        QStringLiteral("agent_nickname"))
                        .toString())
                    .value_or(QStringLiteral("Subagent")),
                nonempty(query.value(1).toString())
                    .value_or(QStringLiteral("Subagent task")),
                nonempty(
                    spawn->value(
                        QStringLiteral("agent_role"))
                        .toString()),
                bridgeDate(updatedAt),
                status(
                    id,
                    pendingApprovalThreadIds,
                    updatedAt,
                    now),
                needsApproval,
            },
            recency,
        });
    }

    std::sort(
        projected.begin(),
        projected.end(),
        [](const ProjectedSubagent& left,
           const ProjectedSubagent& right) {
            if (left.recencyMilliseconds !=
                right.recencyMilliseconds) {
                return left.recencyMilliseconds >
                    right.recencyMilliseconds;
            }
            return left.agent.id < right.agent.id;
        });

    const int limit = std::clamp(requestedLimit, 1, 50);
    const qsizetype resultSize = std::min<qsizetype>(
        limit, projected.size());
    QVector<BridgeSubagent> result;
    result.reserve(resultSize);
    for (qsizetype index = 0;
         index < resultSize;
         ++index) {
        result.append(std::move(projected[index].agent));
    }
    return Result<QVector<BridgeSubagent>>::success(
        std::move(result));
}

} // namespace companion
