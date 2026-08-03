#include "codex/appserver/TaskCreator.h"

#include "codex/appserver/AppServerProcess.h"
#include "codex/discovery/CodexInstallationDiscovery.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace companion {

namespace {

constexpr qsizetype kMaximumHostedSessions = 8;

CompanionError taskCreationError(
    const QString& message,
    const QString& serverMessage = {})
{
    QVariantMap context;
    if (!serverMessage.isEmpty()) {
        context.insert(
            QStringLiteral("serverMessage"),
            serverMessage);
    }
    return {
        QStringLiteral("codex.task_create_failed"),
        message,
        false,
        std::move(context),
    };
}

bool writeMayHaveStarted(
    const CompanionError& error)
{
    const auto value = error.context.constFind(
        QStringLiteral("writeMayHaveStarted"));
    return value == error.context.cend()
        || value.value().toBool();
}

CompanionError knownUnsentTaskError(
    const CompanionError& cause,
    const QString& phase,
    const QString& clientMessageId,
    const QString& threadId = {})
{
    CompanionError error = cause;
    error.context.insert(
        QStringLiteral("phase"), phase);
    error.context.insert(
        QStringLiteral("clientMessageId"),
        clientMessageId);
    if (!threadId.isEmpty()) {
        error.context.insert(
            QStringLiteral("threadId"), threadId);
    }
    return error;
}

CompanionError mapSessionError(
    const CompanionError& error)
{
    if (error.code
        == QStringLiteral(
            "codex.app_server_process_exited")
        || error.code
            == QStringLiteral(
                "codex.app_server_launch_failed")) {
        CompanionError mapped = error;
        mapped.code = QStringLiteral(
            "codex.shared_daemon_unavailable");
        mapped.message = QStringLiteral(
            "Codex app-server is unavailable.");
        return mapped;
    }
    return error;
}

CompanionError ambiguousTaskCreationError(
    const CompanionError& cause,
    const QString& phase,
    const QString& clientMessageId,
    const QString& threadId = {})
{
    QVariantMap context = cause.context;
    if (!threadId.isEmpty()) {
        context.insert(
            QStringLiteral("threadId"), threadId);
    }
    context.insert(
        QStringLiteral("clientMessageId"),
        clientMessageId);
    context.insert(
        QStringLiteral("phase"),
        phase);
    context.insert(
        QStringLiteral("causeCode"), cause.code);
    context.insert(
        QStringLiteral("causeMessage"),
        cause.message);
    return {
        QStringLiteral("codex.task_create_ambiguous"),
        QStringLiteral(
            "Codex may have started this task, but Companion did not receive the turn acknowledgement."),
        false,
        std::move(context),
    };
}

QJsonObject threadStartRequest(
    const CreateTaskRequest& request,
    int requestId)
{
    QJsonObject params{
        {QStringLiteral("ephemeral"), false},
        {
            QStringLiteral("serviceName"),
            QStringLiteral("codex-companion-mobile"),
        },
    };
    const QString cwd = request.cwd.trimmed();
    if (!cwd.isEmpty()) {
        params.insert(QStringLiteral("cwd"), cwd);
    }
    const QString model = request.model.trimmed();
    if (!model.isEmpty()) {
        params.insert(QStringLiteral("model"), model);
    }
    return {
        {QStringLiteral("id"), requestId},
        {
            QStringLiteral("method"),
            QStringLiteral("thread/start"),
        },
        {QStringLiteral("params"), params},
    };
}

QJsonObject turnStartRequest(
    const CreateTaskRequest& request,
    int requestId,
    const QString& threadId,
    const QString& prompt,
    const QString& clientMessageId)
{
    QJsonArray input{
        QJsonObject{
            {
                QStringLiteral("type"),
                QStringLiteral("text"),
            },
            {QStringLiteral("text"), prompt},
            {
                QStringLiteral("text_elements"),
                QJsonArray{},
            },
        },
    };
    const QString skillName =
        request.skillName.trimmed();
    const QString skillPath =
        request.skillPath.trimmed();
    if (!skillName.isEmpty() && !skillPath.isEmpty()) {
        input.append(QJsonObject{
            {
                QStringLiteral("type"),
                QStringLiteral("skill"),
            },
            {QStringLiteral("name"), skillName},
            {QStringLiteral("path"), skillPath},
        });
    }
    for (const StagedAttachment& attachment :
         request.attachments) {
        input.append(attachment.appServerInput());
    }

    QJsonObject params{
        {QStringLiteral("threadId"), threadId},
        {QStringLiteral("input"), input},
        {
            QStringLiteral("clientUserMessageId"),
            clientMessageId,
        },
    };
    const QString model = request.model.trimmed();
    if (!model.isEmpty()) {
        params.insert(QStringLiteral("model"), model);
    }
    const QString effort =
        request.reasoningEffort.trimmed();
    if (!effort.isEmpty()) {
        params.insert(QStringLiteral("effort"), effort);
    }
    return {
        {QStringLiteral("id"), requestId},
        {
            QStringLiteral("method"),
            QStringLiteral("turn/start"),
        },
        {QStringLiteral("params"), params},
    };
}

Result<void> initialize(
    appserver_detail::ProcessSession& session)
{
    const Result<void> sent = session.send(
        appserver_detail::initializeRequest(
            QStringLiteral("codex-companion-mobile"),
            QStringLiteral("Codex Companion Mobile"),
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
        return Result<void>::failure(
            mapSessionError(response.error()));
    }
    if (response.value().isError) {
        return Result<void>::failure(
            taskCreationError(
                response.value().error,
                response.value().error));
    }
    return session.send(
        appserver_detail::initializedNotification());
}

Result<QString> threadIdFrom(
    const RpcResponse& response)
{
    if (response.isError) {
        return Result<QString>::failure(
            taskCreationError(
                response.error,
                response.error));
    }
    const QString threadId =
        response.result
            .toObject()
            .value(QStringLiteral("thread"))
            .toObject()
            .value(QStringLiteral("id"))
            .toString()
            .trimmed();
    if (threadId.isEmpty()) {
        return Result<QString>::failure(
            {
                QStringLiteral(
                    "codex.app_server_invalid_response"),
                QStringLiteral(
                    "Codex app-server did not return a created thread ID."),
                false,
                {},
            });
    }
    return Result<QString>::success(threadId);
}

} // namespace

class TaskCreatorWorker final : public QObject {
    Q_OBJECT

public:
    TaskCreatorWorker(
        CodexExecutableCandidateProvider candidateProvider,
        SharedDaemonAvailabilityProbe sharedDaemonProbe,
        QProcessEnvironment processEnvironment,
        int timeoutMilliseconds)
        : candidateProvider_(std::move(candidateProvider)),
          sharedDaemonProbe_(
              std::move(sharedDaemonProbe)),
          processEnvironment_(
              std::move(processEnvironment)),
          timeoutMilliseconds_(
              std::max(1, timeoutMilliseconds))
    {
    }

    Result<QString> create(
        const CreateTaskRequest& request)
    {
        const QString prompt = request.prompt.trimmed();
        if (prompt.isEmpty()) {
            return Result<QString>::failure({
                QStringLiteral("codex.task_prompt_empty"),
                QStringLiteral(
                    "Enter a task prompt first."),
                false,
                {},
            });
        }

        const Result<void> connected = ensureConnected();
        if (!connected.hasValue()) {
            return Result<QString>::failure(
                mapSessionError(connected.error()));
        }
        session_->resetDeadline(timeoutMilliseconds_);

        if (nextRequestId_
            > std::numeric_limits<int>::max() - 1) {
            return Result<QString>::failure({
                QStringLiteral(
                    "codex.app_server_request_id_exhausted"),
                QStringLiteral(
                    "Codex app-server request IDs are exhausted."),
                false,
                {},
            });
        }
        const int threadRequestId = nextRequestId_;
        const int turnRequestId = nextRequestId_ + 1;
        nextRequestId_ += 2;

        QString clientMessageId =
            request.clientMessageId.trimmed();
        if (clientMessageId.isEmpty()) {
            clientMessageId =
                QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
        }

        const Result<void> threadSent =
            session_->send(
                threadStartRequest(
                    request, threadRequestId));
        if (!threadSent.hasValue()) {
            if (!writeMayHaveStarted(
                    threadSent.error())) {
                const CompanionError error =
                    knownUnsentTaskError(
                        threadSent.error(),
                        QStringLiteral("thread/start"),
                        clientMessageId);
                if (!session_->isRunning()) {
                    session_.reset();
                    activeHostId_ = 0;
                }
                return Result<QString>::failure(
                    error);
            }
            const CompanionError error =
                ambiguousTaskCreationError(
                    threadSent.error(),
                    QStringLiteral("thread/start"),
                    clientMessageId);
            quarantineSession();
            return Result<QString>::failure(
                error);
        }
        const Result<RpcResponse> threadResponse =
            session_->response(threadRequestId);
        if (!threadResponse.hasValue()) {
            const CompanionError error =
                ambiguousTaskCreationError(
                    threadResponse.error(),
                    QStringLiteral("thread/start"),
                    clientMessageId);
            quarantineSession();
            return Result<QString>::failure(
                error);
        }
        const Result<QString> threadId =
            threadIdFrom(threadResponse.value());
        if (!threadId.hasValue()) {
            if (threadResponse.value().isError) {
                return threadId;
            }
            const CompanionError error =
                ambiguousTaskCreationError(
                    threadId.error(),
                    QStringLiteral("thread/start"),
                    clientMessageId);
            quarantineSession();
            return Result<QString>::failure(error);
        }
        const Result<void> turnSent = session_->send(
            turnStartRequest(
                request,
                turnRequestId,
                threadId.value(),
                prompt,
                clientMessageId));
        if (!turnSent.hasValue()) {
            if (!writeMayHaveStarted(
                    turnSent.error())) {
                const CompanionError error =
                    knownUnsentTaskError(
                        turnSent.error(),
                        QStringLiteral("turn/start"),
                        clientMessageId,
                        threadId.value());
                if (!session_->isRunning()) {
                    session_.reset();
                    activeHostId_ = 0;
                }
                return Result<QString>::failure(
                    error);
            }
            const CompanionError error =
                ambiguousTaskCreationError(
                    turnSent.error(),
                    QStringLiteral("turn/start"),
                    clientMessageId,
                    threadId.value());
            quarantineSession();
            return Result<QString>::failure(
                error);
        }
        const Result<RpcResponse> turnResponse =
            session_->response(turnRequestId);
        if (!turnResponse.hasValue()) {
            const CompanionError error =
                ambiguousTaskCreationError(
                    turnResponse.error(),
                    QStringLiteral("turn/start"),
                    clientMessageId,
                    threadId.value());
            quarantineSession();
            return Result<QString>::failure(
                error);
        }
        if (turnResponse.value().isError) {
            return Result<QString>::failure(
                taskCreationError(
                    turnResponse.value().error,
                    turnResponse.value().error));
        }

        return Result<QString>::success(
            threadId.value());
    }

    void shutdown()
    {
        if (session_) {
            session_->close();
            session_.reset();
        }
        for (auto& hosted :
             quarantinedSessions_) {
            hosted.session->close();
        }
        quarantinedSessions_.clear();
    }

    Result<QVector<PendingAppServerRequest>>
    takePendingServerRequests()
    {
        pruneUnavailableSessions();

        struct PendingBatch final {
            appserver_detail::ProcessSession* session =
                nullptr;
            qsizetype count = 0;
        };

        QVector<PendingAppServerRequest> pending;
        QVector<PendingBatch> batches;
        const auto appendRequests =
            [&pending, &batches](
                quint64 hostId,
                appserver_detail::ProcessSession& session)
            -> Result<void> {
            const auto requests =
                session.pendingServerRequests();
            if (!requests.hasValue()) {
                return Result<void>::failure(
                    requests.error());
            }
            for (const QJsonObject& request :
                 requests.value()) {
                pending.append({
                    hostId,
                    request.value(
                        QStringLiteral("id")),
                    request
                        .value(QStringLiteral("method"))
                        .toString(),
                    request
                        .value(QStringLiteral("params"))
                        .toObject(),
                });
            }
            batches.append({
                &session,
                requests.value().size(),
            });
            return Result<void>::success();
        };

        if (session_) {
            const Result<void> appended =
                appendRequests(
                    activeHostId_, *session_);
            if (!appended.hasValue()) {
                session_->close();
                session_.reset();
                activeHostId_ = 0;
            }
        }
        for (auto iterator =
                 quarantinedSessions_.begin();
             iterator
             != quarantinedSessions_.end();) {
            const Result<void> appended =
                appendRequests(
                    iterator->hostId,
                    *iterator->session);
            if (!appended.hasValue()) {
                iterator->session->close();
                iterator =
                    quarantinedSessions_.erase(
                        iterator);
                continue;
            }
            ++iterator;
        }
        for (const PendingBatch& batch : batches) {
            batch.session->consumeServerRequests(
                batch.count);
        }
        return Result<
            QVector<PendingAppServerRequest>>::success(
            std::move(pending));
    }

    Result<void> respondToServerRequest(
        quint64 hostId,
        const QJsonValue& requestId,
        const QJsonValue& result)
    {
        pruneUnavailableSessions();
        if (session_ && activeHostId_ == hostId) {
            session_->resetDeadline(
                timeoutMilliseconds_);
            return session_->sendResponse(
                requestId, result);
        }
        for (auto& hosted :
             quarantinedSessions_) {
            if (hosted.hostId == hostId) {
                hosted.session->resetDeadline(
                    timeoutMilliseconds_);
                return hosted.session->sendResponse(
                    requestId, result);
            }
        }
        return Result<void>::failure({
            QStringLiteral(
                "codex.app_server_request_not_found"),
            QStringLiteral(
                "The Codex app-server request host is no longer available."),
            false,
            {
                {
                    QStringLiteral("hostId"),
                    QVariant::fromValue(hostId),
                },
            },
        });
    }

private:
    struct HostedSession final {
        quint64 hostId = 0;
        std::unique_ptr<
            appserver_detail::ProcessSession> session;
    };

    void pruneUnavailableSessions()
    {
        if (session_ && !session_->isRunning()) {
            session_.reset();
            activeHostId_ = 0;
        }
        for (auto iterator =
                 quarantinedSessions_.begin();
             iterator
             != quarantinedSessions_.end();) {
            if (!iterator->session->isRunning()) {
                iterator =
                    quarantinedSessions_.erase(
                        iterator);
                continue;
            }
            ++iterator;
        }
    }

    void quarantineSession()
    {
        if (!session_) {
            return;
        }
        if (session_->isRunning()) {
            quarantinedSessions_.push_back({
                activeHostId_,
                std::move(session_),
            });
            activeHostId_ = 0;
            return;
        }
        session_.reset();
        activeHostId_ = 0;
    }

    Result<void> ensureConnected()
    {
        pruneUnavailableSessions();
        if (session_ && session_->isRunning()) {
            return Result<void>::success();
        }
        session_.reset();
        activeHostId_ = 0;

        if (quarantinedSessions_.size()
            >= kMaximumHostedSessions) {
            return Result<void>::failure({
                QStringLiteral(
                    "codex.app_server_quarantine_limit"),
                QStringLiteral(
                    "Too many uncertain Codex task hosts are still running."),
                true,
                {
                    {
                        QStringLiteral("hostCount"),
                        static_cast<qlonglong>(
                            quarantinedSessions_.size()),
                    },
                    {
                        QStringLiteral("maximumHosts"),
                        static_cast<qlonglong>(
                            kMaximumHostedSessions),
                    },
                },
            });
        }

        const bool useProxy =
            sharedDaemonProbe_
            && sharedDaemonProbe_();
        struct ConnectionAttempt final {
            QStringList arguments;
            appserver_detail::Transport transport =
                appserver_detail::Transport::StdioJsonLines;
        };
        QVector<ConnectionAttempt> attempts;
        if (useProxy) {
            attempts.append({
                {
                    QStringLiteral("app-server"),
                    QStringLiteral("proxy"),
                },
                appserver_detail::Transport::ProxyWebSocket,
            });
        }
        attempts.append({
            {
                QStringLiteral("app-server"),
                QStringLiteral("--listen"),
                QStringLiteral("stdio://"),
            },
            appserver_detail::Transport::StdioJsonLines,
        });
        const QVector<QString> candidates =
            candidateProvider_
            ? candidateProvider_()
            : QVector<QString>{};
        if (candidates.isEmpty()) {
            const ConnectionAttempt& attempt =
                attempts.last();
            auto missing =
                appserver_detail::ProcessSession::start(
                    {},
                    attempt.arguments,
                    processEnvironment_,
                    attempt.transport,
                    timeoutMilliseconds_);
            return Result<void>::failure(
                missing.error());
        }

        QDeadlineTimer deadline(timeoutMilliseconds_);
        std::optional<CompanionError> lastError;
        const qsizetype candidateCount =
            std::count_if(
                candidates.cbegin(),
                candidates.cend(),
                [](const QString& candidate) {
                    return !candidate.trimmed().isEmpty();
                });
        qsizetype remainingAttempts =
            candidateCount * attempts.size();
        for (const ConnectionAttempt& attempt :
             attempts) {
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
                        attempt.arguments,
                        processEnvironment_,
                        attempt.transport,
                        attemptTimeout);
                if (!started.hasValue()) {
                    lastError = started.error();
                    continue;
                }

                std::unique_ptr<
                    appserver_detail::ProcessSession>
                    candidateSession =
                        std::move(started.value());
                const Result<void> initialized =
                    initialize(*candidateSession);
                if (initialized.hasValue()) {
                    session_ =
                        std::move(candidateSession);
                    activeHostId_ = nextHostId_++;
                    return Result<void>::success();
                }
                lastError = initialized.error();
                candidateSession->close();
            }
        }

        if (lastError.has_value()) {
            return Result<void>::failure(
                std::move(*lastError));
        }
        return Result<void>::failure({
            QStringLiteral("codex.executable_not_found"),
            QStringLiteral(
                "Could not find an installed Codex executable."),
            false,
            {},
        });
    }

    CodexExecutableCandidateProvider candidateProvider_;
    SharedDaemonAvailabilityProbe sharedDaemonProbe_;
    QProcessEnvironment processEnvironment_;
    int timeoutMilliseconds_ =
        TaskCreator::kDefaultTimeoutMilliseconds;
    int nextRequestId_ = 2;
    quint64 activeHostId_ = 0;
    quint64 nextHostId_ = 1;
    std::unique_ptr<
        appserver_detail::ProcessSession> session_;
    std::vector<HostedSession>
        quarantinedSessions_;
};

class TaskCreator::State final {
public:
    State(
        CodexExecutableCandidateProvider candidateProvider,
        SharedDaemonAvailabilityProbe sharedDaemonProbe,
        QProcessEnvironment processEnvironment,
        int timeoutMilliseconds)
        : worker_(new TaskCreatorWorker(
              std::move(candidateProvider),
              std::move(sharedDaemonProbe),
              std::move(processEnvironment),
              timeoutMilliseconds))
    {
        worker_->moveToThread(&thread_);
        QObject::connect(
            &thread_,
            &QThread::finished,
            worker_,
            &QObject::deleteLater);
        thread_.start();
    }

    ~State()
    {
        if (thread_.isRunning()) {
            QMetaObject::invokeMethod(
                worker_,
                [this] { worker_->shutdown(); },
                Qt::BlockingQueuedConnection);
            thread_.quit();
            thread_.wait();
        }
    }

    Result<QString> create(
        const CreateTaskRequest& request) const
    {
        std::optional<Result<QString>> result;
        const bool invoked = QMetaObject::invokeMethod(
            worker_,
            [this, &request, &result] {
                result.emplace(
                    worker_->create(request));
            },
            Qt::BlockingQueuedConnection);
        if (!invoked || !result.has_value()) {
            return Result<QString>::failure({
                QStringLiteral(
                    "codex.task_worker_unavailable"),
                QStringLiteral(
                    "Codex task worker is unavailable."),
                true,
                {},
            });
        }
        return std::move(*result);
    }

    Result<QVector<PendingAppServerRequest>>
    takePendingServerRequests() const
    {
        std::optional<Result<
            QVector<PendingAppServerRequest>>> result;
        const bool invoked = QMetaObject::invokeMethod(
            worker_,
            [this, &result] {
                result.emplace(
                    worker_
                        ->takePendingServerRequests());
            },
            Qt::BlockingQueuedConnection);
        if (!invoked || !result.has_value()) {
            return Result<
                QVector<PendingAppServerRequest>>::failure({
                QStringLiteral(
                    "codex.task_worker_unavailable"),
                QStringLiteral(
                    "Codex task worker is unavailable."),
                true,
                {},
            });
        }
        return std::move(*result);
    }

    Result<void> respondToServerRequest(
        quint64 hostId,
        const QJsonValue& requestId,
        const QJsonValue& result) const
    {
        std::optional<Result<void>> outcome;
        const bool invoked = QMetaObject::invokeMethod(
            worker_,
            [this,
             hostId,
             requestId,
             result,
             &outcome] {
                outcome.emplace(
                    worker_
                        ->respondToServerRequest(
                            hostId,
                            requestId,
                            result));
            },
            Qt::BlockingQueuedConnection);
        if (!invoked || !outcome.has_value()) {
            return Result<void>::failure({
                QStringLiteral(
                    "codex.task_worker_unavailable"),
                QStringLiteral(
                    "Codex task worker is unavailable."),
                true,
                {},
            });
        }
        return std::move(*outcome);
    }

private:
    mutable QThread thread_;
    TaskCreatorWorker* worker_ = nullptr;
};

TaskCreator::TaskCreator(
    const CodexEnvironment& environment,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds)
    : TaskCreator(
          [environment] {
              return CodexInstallationDiscovery::
                  trustedAppServerCandidates(environment);
          },
          [environment] {
              return QFileInfo::exists(
                  QDir(environment.codexHome).filePath(
                      QStringLiteral(
                          "app-server-control/app-server-control.sock")));
          },
          std::move(processEnvironment),
          timeoutMilliseconds)
{
}

TaskCreator::TaskCreator(
    CodexExecutableCandidateProvider candidateProvider,
    SharedDaemonAvailabilityProbe sharedDaemonProbe,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds)
    : state_(std::make_unique<State>(
          std::move(candidateProvider),
          std::move(sharedDaemonProbe),
          std::move(processEnvironment),
          timeoutMilliseconds))
{
}

TaskCreator::~TaskCreator() = default;

Result<QString> TaskCreator::create(
    const CreateTaskRequest& request) const
{
    return state_->create(request);
}

Result<QVector<PendingAppServerRequest>>
TaskCreator::takePendingServerRequests() const
{
    return state_->takePendingServerRequests();
}

Result<void> TaskCreator::respondToServerRequest(
    quint64 hostId,
    const QJsonValue& requestId,
    const QJsonValue& result) const
{
    return state_->respondToServerRequest(
        hostId, requestId, result);
}

} // namespace companion

#include "TaskCreator.moc"
