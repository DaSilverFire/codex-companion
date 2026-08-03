#include "codex/appserver/AppServerRpcClient.h"

#include "codex/appserver/AppServerProcess.h"
#include "codex/discovery/CodexInstallationDiscovery.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSet>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace companion {

namespace {

CompanionError invalidRequestError(
    const QString& message,
    int id)
{
    return {
        QStringLiteral(
            "codex.app_server_invalid_request"),
        message,
        false,
        {{QStringLiteral("id"), id}},
    };
}

CompanionError operationCanceledError()
{
    return {
        QStringLiteral(
            "codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

Result<void> initialize(
    appserver_detail::ProcessSession& session)
{
    const Result<void> sent = session.send(
        appserver_detail::initializeRequest(
            QStringLiteral("codex-companion"),
            QStringLiteral("Codex Companion"),
            appserver_detail::
                windowsClientVersion(
                    QCoreApplication::
                        applicationVersion())));
    if (!sent.hasValue()) {
        return sent;
    }
    const Result<RpcResponse> response =
        session.response(1);
    if (!response.hasValue()) {
        return Result<void>::failure(response.error());
    }
    if (response.value().isError) {
        return Result<void>::failure({
            QStringLiteral(
                "codex.app_server_initialize_failed"),
            response.value().error,
            false,
            {
                {
                    QStringLiteral("serverMessage"),
                    response.value().error,
                },
            },
        });
    }
    return session.send(
        appserver_detail::initializedNotification());
}

} // namespace

QJsonObject RpcRequest::jsonObject() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
}

AppServerRpcClient::AppServerRpcClient(
    const CodexEnvironment& environment,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds,
    AppServerTransportMode transportMode)
    : AppServerRpcClient(
          [environment] {
              return CodexInstallationDiscovery::
                  trustedAppServerCandidates(environment);
          },
          std::move(processEnvironment),
          timeoutMilliseconds,
          transportMode)
{
}

AppServerRpcClient::AppServerRpcClient(
    CodexExecutableCandidateProvider candidateProvider,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds,
    AppServerTransportMode transportMode)
    : candidateProvider_(std::move(candidateProvider)),
      processEnvironment_(std::move(processEnvironment)),
      timeoutMilliseconds_(
          std::max(1, timeoutMilliseconds)),
      transportMode_(transportMode)
{
}

Result<QHash<int, RpcResponse>>
AppServerRpcClient::perform(
    const QVector<RpcRequest>& requests) const
{
    return perform(requests, {});
}

Result<QHash<int, RpcResponse>>
AppServerRpcClient::perform(
    const QVector<RpcRequest>& requests,
    std::stop_token stopToken) const
{
    QSet<int> ids;
    for (const RpcRequest& request : requests) {
        if (request.id <= 1) {
            return Result<QHash<int, RpcResponse>>::failure(
                invalidRequestError(
                    QStringLiteral(
                        "Codex app-server request IDs must begin at 2."),
                    request.id));
        }
        if (ids.contains(request.id)) {
            return Result<QHash<int, RpcResponse>>::failure(
                invalidRequestError(
                    QStringLiteral(
                        "Codex app-server request IDs must be unique."),
                    request.id));
        }
        if (request.method.trimmed().isEmpty()) {
            return Result<QHash<int, RpcResponse>>::failure(
                invalidRequestError(
                    QStringLiteral(
                        "Codex app-server request method is required."),
                    request.id));
        }
        ids.insert(request.id);
    }

    if (stopToken.stop_requested()) {
        return Result<
            QHash<int, RpcResponse>>::failure(
            operationCanceledError());
    }

    const QVector<QString> candidates =
        candidateProvider_
        ? candidateProvider_()
        : QVector<QString>{};
    const bool usesSharedProxy =
        transportMode_
        == AppServerTransportMode::
            SharedDaemonProxy;
    const QStringList arguments =
        usesSharedProxy
        ? QStringList{
              QStringLiteral("app-server"),
              QStringLiteral("proxy"),
          }
        : QStringList{
              QStringLiteral("app-server"),
              QStringLiteral("--listen"),
              QStringLiteral("stdio://"),
          };
    const appserver_detail::Transport transport =
        usesSharedProxy
        ? appserver_detail::Transport::
              ProxyWebSocket
        : appserver_detail::Transport::
              StdioJsonLines;
    const bool hasCandidate = std::any_of(
        candidates.cbegin(),
        candidates.cend(),
        [](const QString& candidate) {
            return !candidate.trimmed().isEmpty();
        });
    if (!hasCandidate) {
        auto missing =
            appserver_detail::ProcessSession::start(
                {},
                arguments,
                processEnvironment_,
                transport,
                timeoutMilliseconds_,
                stopToken);
        return Result<QHash<int, RpcResponse>>::failure(
            missing.error());
    }

    QDeadlineTimer deadline(timeoutMilliseconds_);
    std::optional<CompanionError> lastError;
    std::unique_ptr<
        appserver_detail::ProcessSession> session;
    qsizetype remainingAttempts = std::count_if(
        candidates.cbegin(),
        candidates.cend(),
        [](const QString& candidate) {
            return !candidate.trimmed().isEmpty();
        });
    for (const QString& candidate : candidates) {
        if (candidate.trimmed().isEmpty()) {
            continue;
        }
        const int attemptTimeout =
            appserver_detail::
                retryAttemptTimeoutMilliseconds(
                    deadline,
                    remainingAttempts);
        --remainingAttempts;
        if (attemptTimeout <= 0) {
            break;
        }
        auto started =
            appserver_detail::ProcessSession::start(
                {candidate},
                arguments,
                processEnvironment_,
                transport,
                attemptTimeout,
                stopToken);
        if (!started.hasValue()) {
            if (started.error().code
                == QStringLiteral(
                    "codex.operation_canceled")) {
                return Result<
                    QHash<int, RpcResponse>>::failure(
                    started.error());
            }
            lastError = started.error();
            continue;
        }

        std::unique_ptr<
            appserver_detail::ProcessSession> candidateSession =
            std::move(started.value());
        const Result<void> initialized =
            initialize(*candidateSession);
        if (initialized.hasValue()) {
            session = std::move(candidateSession);
            break;
        }
        if (initialized.error().code
            == QStringLiteral(
                "codex.operation_canceled")) {
            return Result<
                QHash<int, RpcResponse>>::failure(
                initialized.error());
        }
        lastError = initialized.error();
        candidateSession->close();
    }
    if (!session) {
        return Result<QHash<int, RpcResponse>>::failure(
            lastError.has_value()
                ? std::move(*lastError)
                : CompanionError{
                      QStringLiteral(
                          "codex.executable_not_found"),
                      QStringLiteral(
                          "Could not find an installed Codex executable."),
                      false,
                      {},
                   });
    }
    const qint64 requestBudget =
        deadline.remainingTime();
    if (requestBudget <= 0) {
        session->close();
        return Result<QHash<int, RpcResponse>>::failure({
            QStringLiteral(
                "codex.app_server_timed_out"),
            QStringLiteral(
                "Codex app-server did not respond in time."),
            true,
            {},
        });
    }
    session->resetDeadline(
        static_cast<int>(
            std::min<qint64>(
                requestBudget,
                std::numeric_limits<int>::max())));

    for (const RpcRequest& request : requests) {
        const Result<void> sent =
            session->send(request.jsonObject());
        if (!sent.hasValue()) {
            return Result<QHash<int, RpcResponse>>::failure(
                sent.error());
        }
    }

    QHash<int, RpcResponse> responses;
    for (const RpcRequest& request : requests) {
        const Result<RpcResponse> response =
            session->response(request.id);
        if (!response.hasValue()) {
            return Result<QHash<int, RpcResponse>>::failure(
                response.error());
        }
        responses.insert(request.id, response.value());
    }
    session->close();
    return Result<QHash<int, RpcResponse>>::success(
        std::move(responses));
}

} // namespace companion
