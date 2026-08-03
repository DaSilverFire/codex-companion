#include "codex/accounts/CodexThreadAccountHandoffService.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <utility>

namespace companion {
namespace {

CompanionError handoffError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

Result<QString> normalizedThreadId(
    QString threadId)
{
    threadId = threadId.trimmed();
    if (threadId.isEmpty()) {
        return Result<QString>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_thread_invalid"),
                QStringLiteral(
                    "The Codex thread ID is empty.")));
    }
    return Result<QString>::success(
        std::move(threadId));
}

Result<QString> normalizedRolloutPath(
    QString rolloutPath)
{
    rolloutPath = rolloutPath.trimmed();
    if (rolloutPath.isEmpty()) {
        return Result<QString>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_path_invalid"),
                QStringLiteral(
                    "The Codex rollout path is empty.")));
    }
    const QString absolute =
        QDir::cleanPath(
            QFileInfo(rolloutPath)
                .absoluteFilePath());
    if (absolute.isEmpty()) {
        return Result<QString>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_path_invalid"),
                QStringLiteral(
                    "The Codex rollout path is invalid.")));
    }
    return Result<QString>::success(
        absolute);
}

bool pathsEqual(
    const QString& first,
    const QString& second)
{
    const auto normalizedFirst =
        normalizedRolloutPath(first);
    const auto normalizedSecond =
        normalizedRolloutPath(second);
    if (!normalizedFirst.hasValue()
        || !normalizedSecond.hasValue()) {
        return false;
    }
#ifdef Q_OS_WIN
    return normalizedFirst.value().compare(
               normalizedSecond.value(),
               Qt::CaseInsensitive)
        == 0;
#else
    return normalizedFirst.value()
        == normalizedSecond.value();
#endif
}

Result<QPair<QString, QString>>
resumedThread(const QJsonValue& value)
{
    if (!value.isObject()) {
        return Result<
            QPair<QString, QString>>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_response_invalid"),
                QStringLiteral(
                    "Codex returned an unreadable handoff response.")));
    }
    const QJsonValue threadValue =
        value.toObject().value(
            QStringLiteral("thread"));
    if (!threadValue.isObject()) {
        return Result<
            QPair<QString, QString>>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_response_invalid"),
                QStringLiteral(
                    "Codex returned an unreadable handoff response.")));
    }
    const QJsonObject thread =
        threadValue.toObject();
    const QString threadId =
        thread.value(QStringLiteral("id"))
            .toString()
            .trimmed();
    const QString rolloutPath =
        thread.value(QStringLiteral("path"))
            .toString()
            .trimmed();
    if (threadId.isEmpty()
        || rolloutPath.isEmpty()) {
        return Result<
            QPair<QString, QString>>::failure(
            handoffError(
                QStringLiteral(
                    "codex.account_handoff_response_invalid"),
                QStringLiteral(
                    "Codex returned an unreadable handoff response.")));
    }
    return Result<
        QPair<QString, QString>>::success(
        {threadId, rolloutPath});
}

} // namespace

CodexThreadAccountHandoffService::
    CodexThreadAccountHandoffService(
        const CodexEnvironment& environment,
        CodexAccountRouter& router)
    : CodexThreadAccountHandoffService(
          router,
          [environment](
              const CodexAccountRoute& route,
              const QVector<RpcRequest>&
                  requests,
              std::stop_token stopToken) {
              AppServerRpcClient client(
                  environment,
                  route.environment);
              return client.perform(
                  requests,
                  stopToken);
          })
{
}

CodexThreadAccountHandoffService::
    CodexThreadAccountHandoffService(
        CodexAccountRouter& router,
        CodexThreadAccountHandoffPerformer
            performer)
    : router_(&router),
      performer_(std::move(performer))
{
}

Result<CodexThreadAccountHandoffResult>
CodexThreadAccountHandoffService::handoff(
    QString threadId,
    QString rolloutPath,
    ThreadRuntimeStatus runtimeStatus,
    const QUuid& destinationProfileId,
    std::stop_token stopToken) const
{
    if (router_ == nullptr || !performer_) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_unavailable"),
                    QStringLiteral(
                        "Codex account handoff is unavailable.")));
    }
    if (stopToken.stop_requested()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.operation_canceled"),
                    QStringLiteral(
                        "The Codex account handoff was canceled.")));
    }
    if (!canHandoff(runtimeStatus)) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_unsafe"),
                    QStringLiteral(
                        "An active or attention-pending Codex task cannot move between account profiles.")));
    }

    const auto normalizedThread =
        normalizedThreadId(
            std::move(threadId));
    if (!normalizedThread.hasValue()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                normalizedThread.error());
    }
    const auto normalizedPath =
        normalizedRolloutPath(
            std::move(rolloutPath));
    if (!normalizedPath.hasValue()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                normalizedPath.error());
    }
    if (destinationProfileId.isNull()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_profile_invalid"),
                    QStringLiteral(
                        "The destination Codex account profile is invalid.")));
    }

    const CodexAccountRoute route =
        router_->routeProfile(
            destinationProfileId);
    if (route.profileId
        != std::optional<QUuid>(
            destinationProfileId)) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_profile_missing"),
                    QStringLiteral(
                        "The destination Codex account profile does not exist.")));
    }

    const RpcRequest request{
        2,
        QStringLiteral("thread/resume"),
        {
            {
                QStringLiteral("threadId"),
                normalizedThread.value(),
            },
            {
                QStringLiteral("path"),
                normalizedPath.value(),
            },
        },
    };
    Result<QHash<int, RpcResponse>>
        responses =
            Result<
                QHash<
                    int,
                    RpcResponse>>::failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_failed"),
                    QStringLiteral(
                        "Codex account handoff failed.")));
    try {
        responses =
            performer_(
                route,
                {request},
                stopToken);
    } catch (...) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_failed"),
                    QStringLiteral(
                        "Codex account handoff failed.")));
    }
    if (!responses.hasValue()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                responses.error());
    }
    const auto response =
        responses.value().constFind(
            request.id);
    if (response
        == responses.value().cend()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_response_missing"),
                    QStringLiteral(
                        "Codex app-server did not return a handoff response.")));
    }
    if (response->isError) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_failed"),
                    response->error
                            .trimmed()
                            .isEmpty()
                        ? QStringLiteral(
                              "Codex account handoff failed.")
                        : response->error));
    }
    const auto resumed =
        resumedThread(response->result);
    if (!resumed.hasValue()
        || resumed.value().first
            != normalizedThread.value()
        || !pathsEqual(
            resumed.value().second,
            normalizedPath.value())) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                handoffError(
                    QStringLiteral(
                        "codex.account_handoff_mismatch"),
                    QStringLiteral(
                        "Codex resumed a different or unreadable task; the account binding was not changed.")));
    }

    const auto bound =
        router_->bindNewThread(
            normalizedThread.value(),
            destinationProfileId);
    if (!bound.hasValue()) {
        return Result<
            CodexThreadAccountHandoffResult>::
            failure(
                bound.error());
    }
    return Result<
        CodexThreadAccountHandoffResult>::
        success({
            normalizedThread.value(),
            normalizedPath.value(),
            destinationProfileId,
        });
}

bool CodexThreadAccountHandoffService::
    canHandoff(
        ThreadRuntimeStatus runtimeStatus)
        noexcept
{
    return runtimeStatus
            == ThreadRuntimeStatus::Idle
        || runtimeStatus
            == ThreadRuntimeStatus::NotLoaded
        || runtimeStatus
            == ThreadRuntimeStatus::SystemError;
}

} // namespace companion
