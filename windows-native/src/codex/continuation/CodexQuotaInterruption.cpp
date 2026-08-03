#include "codex/continuation/CodexQuotaInterruption.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>

#include <cmath>
#include <utility>

namespace companion {
namespace {

CompanionError quotaError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

QString normalized(QString value)
{
    return value.trimmed();
}

} // namespace

QString CodexQuotaInterruption::eventKey()
    const
{
    return threadId + QLatin1Char('|')
        + turnId;
}

Result<RpcRequest>
CodexQuotaInterruptionProtocol::
    latestTurnRequest(
        int id,
        QString threadId)
{
    threadId =
        normalized(std::move(threadId));
    if (id <= 0 || threadId.isEmpty()) {
        return Result<RpcRequest>::failure(
            quotaError(
                QStringLiteral(
                    "codex.quota_request_invalid"),
                QStringLiteral(
                    "The Codex latest-turn request is invalid.")));
    }
    return Result<RpcRequest>::success({
        id,
        QStringLiteral(
            "thread/turns/list"),
        {
            {
                QStringLiteral("threadId"),
                threadId,
            },
            {
                QStringLiteral("limit"),
                1,
            },
            {
                QStringLiteral(
                    "sortDirection"),
                QStringLiteral("desc"),
            },
            {
                QStringLiteral("itemsView"),
                QStringLiteral("notLoaded"),
            },
        },
    });
}

Result<
    std::optional<
        CodexQuotaInterruption>>
CodexQuotaInterruptionProtocol::parse(
    QString threadId,
    const RpcResponse& response)
{
    threadId =
        normalized(std::move(threadId));
    if (threadId.isEmpty()) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            failure(
                quotaError(
                    QStringLiteral(
                        "codex.quota_thread_invalid"),
                    QStringLiteral(
                        "The Codex thread ID is empty.")));
    }
    if (response.isError) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            failure(
                quotaError(
                    QStringLiteral(
                        "codex.quota_read_failed"),
                    QStringLiteral(
                        "Codex could not read the latest turn.")));
    }
    if (!response.result.isObject()) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            failure(
                quotaError(
                    QStringLiteral(
                        "codex.quota_response_invalid"),
                    QStringLiteral(
                        "Codex returned an unreadable latest-turn response.")));
    }
    const QJsonValue dataValue =
        response.result.toObject().value(
            QStringLiteral("data"));
    if (!dataValue.isArray()) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            failure(
                quotaError(
                    QStringLiteral(
                        "codex.quota_response_invalid"),
                    QStringLiteral(
                        "Codex returned an unreadable latest-turn response.")));
    }
    const QJsonArray turns =
        dataValue.toArray();
    if (turns.isEmpty()) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            success(std::nullopt);
    }
    if (!turns.at(0).isObject()) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            success(std::nullopt);
    }

    const QJsonObject turn =
        turns.at(0).toObject();
    const QString status =
        turn.value(
                QStringLiteral("status"))
            .toString();
    const QString turnId =
        turn.value(
                QStringLiteral("id"))
            .toString()
            .trimmed();
    const QJsonValue errorValue =
        turn.value(
            QStringLiteral("error"));
    if (status
            != QStringLiteral("failed")
        || turnId.isEmpty()
        || !errorValue.isObject()
        || errorValue.toObject()
                   .value(
                       QStringLiteral(
                           "codexErrorInfo"))
                   .toString()
            != QStringLiteral(
                "usageLimitExceeded")) {
        return Result<
            std::optional<
                CodexQuotaInterruption>>::
            success(std::nullopt);
    }

    std::optional<QDateTime>
        completedAt;
    const QJsonValue completedValue =
        turn.value(
            QStringLiteral("completedAt"));
    if (completedValue.isDouble()
        && std::isfinite(
            completedValue.toDouble())) {
        completedAt =
            QDateTime::
                fromMSecsSinceEpoch(
                    qRound64(
                        completedValue
                            .toDouble()
                        * 1000.0),
                    QTimeZone::UTC);
    }
    return Result<
        std::optional<
            CodexQuotaInterruption>>::
        success(
            CodexQuotaInterruption{
                threadId,
                turnId,
                completedAt,
            });
}

} // namespace companion
