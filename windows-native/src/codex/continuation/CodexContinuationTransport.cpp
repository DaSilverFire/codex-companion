#include "codex/continuation/CodexContinuationTransport.h"

#include "codex/accounts/CodexThreadAccountHandoffService.h"
#include "codex/appserver/AppServerRpcClient.h"
#include "codex/commands/GoalService.h"
#include "codex/commands/UsageService.h"

#include <QJsonArray>
#include <QJsonObject>

#include <memory>
#include <utility>

namespace companion {
namespace {

CompanionError transportError(
    QString code,
    QString message,
    bool retryable = false)
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        {},
    };
}

template <typename T>
Result<T> waitForFuture(
    QFuture<Result<T>> future,
    std::stop_token stopToken,
    CompanionError unavailable)
{
    if (stopToken.stop_requested()) {
        return Result<T>::failure(
            transportError(
                QStringLiteral(
                    "codex.operation_canceled"),
                QStringLiteral(
                    "The Codex operation was canceled.")));
    }
    future.waitForFinished();
    if (stopToken.stop_requested()) {
        return Result<T>::failure(
            transportError(
                QStringLiteral(
                    "codex.operation_canceled"),
                QStringLiteral(
                    "The Codex operation was canceled.")));
    }
    if (future.isCanceled()
        || future.resultCount() != 1) {
        return Result<T>::failure(
            std::move(unavailable));
    }
    return future.result();
}

Result<std::optional<BridgeGoal>>
readGoal(
    const CodexEnvironment& environment,
    const CodexAccountRoute& route,
    QString threadId,
    std::stop_token stopToken)
{
    threadId = threadId.trimmed();
    if (threadId.isEmpty()) {
        return Result<
            std::optional<BridgeGoal>>::
            failure(
                transportError(
                    QStringLiteral(
                        "codex.continuation_thread_invalid"),
                    QStringLiteral(
                        "The Codex thread ID is empty.")));
    }
    GoalService service(
        environment,
        route.environment);
    const auto goals =
        service.readSync(
            {threadId},
            stopToken);
    if (!goals.hasValue()) {
        return Result<
            std::optional<BridgeGoal>>::
            failure(goals.error());
    }
    return Result<
        std::optional<BridgeGoal>>::
        success(
            goals.value().value(
                threadId,
                std::nullopt));
}

} // namespace

CodexContinuationCommands
createProductionCodexContinuationCommands(
    const CodexEnvironment& environment,
    CodexAccountRouter& router)
{
    const auto handoffService =
        std::make_shared<
            CodexThreadAccountHandoffService>(
            environment,
            router);
    CodexContinuationCommands commands;
    commands.readQuota =
        [environment](
            const CodexAccountRoute& route,
            QString threadId,
            std::stop_token stopToken) {
            const auto request =
                CodexQuotaInterruptionProtocol::
                    latestTurnRequest(
                        2,
                        threadId);
            if (!request.hasValue()) {
                return Result<
                    std::optional<
                        CodexQuotaInterruption>>::
                    failure(
                        request.error());
            }
            AppServerRpcClient client(
                environment,
                route.environment);
            const auto responses =
                client.perform(
                    {request.value()},
                    stopToken);
            if (!responses.hasValue()) {
                return Result<
                    std::optional<
                        CodexQuotaInterruption>>::
                    failure(
                        responses.error());
            }
            const auto response =
                responses.value()
                    .constFind(
                        request.value().id);
            if (response
                == responses.value()
                       .cend()) {
                return Result<
                    std::optional<
                        CodexQuotaInterruption>>::
                    failure(
                        transportError(
                            QStringLiteral(
                                "codex.quota_response_missing"),
                            QStringLiteral(
                                "Codex omitted the latest-turn response.")));
            }
            return CodexQuotaInterruptionProtocol::
                parse(
                    std::move(threadId),
                    response.value());
        };
    commands.readUsage =
        [environment](
            const CodexAccountRoute& route,
            std::stop_token stopToken) {
            UsageService service(
                environment,
                route.environment);
            return waitForFuture(
                service.read(),
                stopToken,
                transportError(
                    QStringLiteral(
                        "codex.continuation_usage_unavailable"),
                    QStringLiteral(
                        "Codex usage is unavailable."),
                    true));
        };
    commands.readGoal =
        [environment](
            const CodexAccountRoute& route,
            QString threadId,
            std::stop_token stopToken) {
            return readGoal(
                environment,
                route,
                std::move(threadId),
                stopToken);
        };
    commands.activateGoal =
        [environment](
            const CodexAccountRoute& route,
            QString threadId,
            std::stop_token stopToken) {
            GoalService service(
                environment,
                route.environment);
            auto mutation =
                service.resumeMutation(
                    threadId.trimmed());
            return waitForFuture(
                mutation.terminalFuture,
                stopToken,
                transportError(
                    QStringLiteral(
                        "codex.continuation_goal_unavailable"),
                    QStringLiteral(
                        "The Codex goal could not be resumed."),
                    true));
        };
    commands.handoff =
        [handoffService](
            QString threadId,
            QString rolloutPath,
            ThreadRuntimeStatus runtimeStatus,
            QUuid destinationProfileId,
            std::stop_token stopToken) {
            const auto result =
                handoffService->handoff(
                    std::move(threadId),
                    std::move(rolloutPath),
                    runtimeStatus,
                    destinationProfileId,
                    stopToken);
            return result.hasValue()
                ? Result<void>::success()
                : Result<void>::failure(
                      result.error());
        };
    commands.send =
        [environment](
            const CodexAccountRoute& route,
            QString threadId,
            QString text,
            QString clientMessageId,
            std::stop_token stopToken) {
            threadId = threadId.trimmed();
            text = text.trimmed();
            clientMessageId =
                clientMessageId.trimmed();
            if (threadId.isEmpty()
                || text.isEmpty()
                || clientMessageId.isEmpty()) {
                return Result<void>::failure(
                    transportError(
                        QStringLiteral(
                            "codex.continuation_send_invalid"),
                        QStringLiteral(
                            "The Codex continuation message is invalid.")));
            }
            const RpcRequest request{
                2,
                QStringLiteral("turn/start"),
                {
                    {
                        QStringLiteral(
                            "threadId"),
                        threadId,
                    },
                    {
                        QStringLiteral("input"),
                        QJsonArray{
                            QJsonObject{
                                {
                                    QStringLiteral(
                                        "type"),
                                    QStringLiteral(
                                        "text"),
                                },
                                {
                                    QStringLiteral(
                                        "text"),
                                    text,
                                },
                                {
                                    QStringLiteral(
                                        "text_elements"),
                                    QJsonArray{},
                                },
                            },
                        },
                    },
                    {
                        QStringLiteral(
                            "clientUserMessageId"),
                        clientMessageId,
                    },
                },
            };
            AppServerRpcClient client(
                environment,
                route.environment);
            const auto responses =
                client.perform(
                    {request},
                    stopToken);
            if (!responses.hasValue()) {
                return Result<void>::failure(
                    responses.error());
            }
            const auto response =
                responses.value()
                    .constFind(request.id);
            if (response
                == responses.value()
                       .cend()) {
                return Result<void>::failure(
                    transportError(
                        QStringLiteral(
                            "codex.continuation_response_missing"),
                        QStringLiteral(
                            "Codex omitted the continuation response.")));
            }
            if (response->isError
                || !response->result
                        .isObject()) {
                return Result<void>::failure(
                    transportError(
                        QStringLiteral(
                            "codex.continuation_send_failed"),
                        QStringLiteral(
                            "Codex could not continue the task."),
                        true));
            }
            return Result<void>::success();
        };
    return commands;
}

} // namespace companion
