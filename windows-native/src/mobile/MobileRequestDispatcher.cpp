#include "mobile/MobileRequestDispatcher.h"

#include "codex/chat/ChatCatalog.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"

#include <QDateTime>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <utility>

namespace companion {

namespace {

constexpr qint64 kAppleReferenceEpochSeconds =
    978'307'200;

CompanionError dependencyUnavailable()
{
    return {
        QStringLiteral(
            "mobile.dependency_unavailable"),
        QStringLiteral(
            "The Companion mobile dependency is unavailable."),
        false,
        {},
    };
}

template <typename T>
Result<T> waitForResult(
    QFuture<Result<T>> future)
{
    if (!future.isValid()) {
        return Result<T>::failure(
            dependencyUnavailable());
    }
    try {
        future.waitForFinished();
        if (future.isCanceled()
            || future.resultCount() != 1) {
            return Result<T>::failure(
                dependencyUnavailable());
        }
        return future.result();
    } catch (const std::exception& error) {
        CompanionError mapped =
            dependencyUnavailable();
        const QString message =
            QString::fromUtf8(error.what()).trimmed();
        if (!message.isEmpty()) {
            mapped.message = message;
        }
        return Result<T>::failure(
            std::move(mapped));
    } catch (...) {
        return Result<T>::failure(
            dependencyUnavailable());
    }
}

template <typename T, typename Callback, typename... Arguments>
Result<T> invoke(
    const Callback& callback,
    Arguments&&... arguments)
{
    if (!callback) {
        return Result<T>::failure(
            dependencyUnavailable());
    }
    try {
        return waitForResult<T>(
            callback(
                std::forward<Arguments>(
                    arguments)...));
    } catch (const std::exception& error) {
        CompanionError mapped =
            dependencyUnavailable();
        const QString message =
            QString::fromUtf8(error.what()).trimmed();
        if (!message.isEmpty()) {
            mapped.message = message;
        }
        return Result<T>::failure(
            std::move(mapped));
    } catch (...) {
        return Result<T>::failure(
            dependencyUnavailable());
    }
}

QThreadPool* dispatcherPool()
{
    static QThreadPool* pool = [] {
        auto* result = new QThreadPool();
        result->setMaxThreadCount(
            std::max(2, QThread::idealThreadCount()));
        return result;
    }();
    return pool;
}

QString nonempty(
    const std::optional<QString>& value)
{
    return value.has_value()
        ? value->trimmed()
        : QString();
}

qint64 boundedLimit(
    const std::optional<qint64>& requested,
    qint64 fallback)
{
    return std::clamp(
        requested.value_or(fallback),
        qint64(1),
        kMaximumPageSize);
}

QString clientMessageId(
    const BridgeRequest& request)
{
    return request.id.toString(
        QUuid::WithoutBraces).toUpper();
}

BridgeDate currentBridgeDate()
{
    return {
        (QDateTime::currentMSecsSinceEpoch()
         / 1'000.0)
            - static_cast<double>(
                kAppleReferenceEpochSeconds),
    };
}

BridgeResponse success(
    const BridgeRequest& request)
{
    BridgeResponse response;
    response.id = request.id;
    response.protocolVersion =
        kBridgeProtocolVersion;
    response.operation = request.operation;
    response.succeeded = true;
    return response;
}

BridgeResponse failure(
    const BridgeRequest& request,
    QString code,
    QString message)
{
    BridgeResponse response =
        success(request);
    response.succeeded = false;
    response.errorCode = std::move(code);
    response.message = std::move(message);
    return response;
}

BridgeResponse archiveFailure(
    const BridgeRequest& request,
    const CompanionError& error)
{
    return failure(
        request,
        QStringLiteral("archive_error"),
        error.message);
}

QVector<BridgeTask> attachGoals(
    QVector<BridgeTask> tasks,
    const MobileGoalMap& goals)
{
    for (BridgeTask& task : tasks) {
        const auto found =
            goals.constFind(task.id);
        if (found != goals.cend()
            && found.value().has_value()) {
            task.goal = *found.value();
        }
    }
    return tasks;
}

std::optional<BridgeTask> findTask(
    const MobileRequestReadDependencies& reads,
    const QString& threadId)
{
    if (!reads.taskPageLoader) {
        return std::nullopt;
    }
    const Result<MobileTaskPage> page =
        invoke<MobileTaskPage>(
            reads.taskPageLoader,
            std::optional<QString>{},
            kMaximumPageSize);
    if (!page.hasValue()) {
        return std::nullopt;
    }
    const auto found = std::find_if(
        page.value().tasks.cbegin(),
        page.value().tasks.cend(),
        [&threadId](const BridgeTask& task) {
            return task.id == threadId;
        });
    return found != page.value().tasks.cend()
        ? std::optional<BridgeTask>(*found)
        : std::nullopt;
}

BridgeResponse sendFailure(
    const BridgeRequest& request,
    const CompanionError& error)
{
    if (error.code
        == QStringLiteral(
            "codex.no_active_turn")) {
        return failure(
            request,
            QStringLiteral("no_active_turn"),
            QStringLiteral(
                "This task is not currently running, so it cannot be steered."));
    }
    if (error.code
        == QStringLiteral(
            "codex.thread_not_loaded")) {
        return failure(
            request,
            QStringLiteral(
                "thread_not_loaded"),
            QStringLiteral(
                "The Windows PC could not load this task in the background. Your message was not sent."));
    }
    if (error.code
        == QStringLiteral(
            "codex.shared_daemon_unavailable")) {
        return failure(
            request,
            QStringLiteral(
                "native_transport_unavailable"),
            QStringLiteral(
                "ChatGPT's local task connection is unavailable. Your message was not lost."));
    }
    if (error.code
        == QStringLiteral(
            "codex.send_timed_out")) {
        return failure(
            request,
            QStringLiteral("timed_out"),
            QStringLiteral(
                "Codex did not confirm the message in time."));
    }
    return failure(
        request,
        QStringLiteral("send_failed"),
        QStringLiteral(
            "Codex did not accept the message."));
}

BridgeResponse approvalFailure(
    const BridgeRequest& request,
    const CompanionError& error)
{
    if (error.code
        == QStringLiteral(
            "approval.request_not_found")) {
        return failure(
            request,
            QStringLiteral("approval_gone"),
            QStringLiteral(
                "That approval request is no longer active."));
    }
    if (error.code
        == QStringLiteral(
            "approval.shared_daemon_unavailable")) {
        return failure(
            request,
            QStringLiteral(
                "native_transport_unavailable"),
            QStringLiteral(
                "ChatGPT's native approval connection is unavailable. Refresh the request, then retry."));
    }
    if (error.code
        == QStringLiteral(
            "approval.timed_out")) {
        return failure(
            request,
            QStringLiteral(
                "approval_timed_out"),
            QStringLiteral(
                "The approval response could not be confirmed."));
    }
    return failure(
        request,
        QStringLiteral("approval_failed"),
        QStringLiteral(
            "Codex did not accept the approval response."));
}

bool taskCreationNeedsSetup(
    const QString& code)
{
    return code
            == QStringLiteral(
                "codex.shared_daemon_unavailable")
        || code
            == QStringLiteral(
                "codex.task_worker_unavailable")
        || code
            == QStringLiteral(
                "codex.executable_not_found")
        || code
            == QStringLiteral(
                "codex.app_server_launch_failed")
        || code
            == QStringLiteral(
                "codex.app_server_process_exited");
}

bool taskCreationTimedOut(
    const CompanionError& error)
{
    if (error.code
            == QStringLiteral(
                "codex.task_create_ambiguous")
        || error.code.contains(
            QStringLiteral("timed_out"),
            Qt::CaseInsensitive)
        || error.code.contains(
            QStringLiteral("timeout"),
            Qt::CaseInsensitive)) {
        return true;
    }
    const QString cause =
        error.context
            .value(QStringLiteral("causeCode"))
            .toString();
    return cause.contains(
               QStringLiteral("timed_out"),
               Qt::CaseInsensitive)
        || cause.contains(
            QStringLiteral("timeout"),
            Qt::CaseInsensitive);
}

BridgeResponse createTaskFailure(
    const BridgeRequest& request,
    const CompanionError& error)
{
    if (taskCreationNeedsSetup(error.code)) {
        return failure(
            request,
            QStringLiteral(
                "native_transport_setup_required"),
            QStringLiteral(
                "Restart ChatGPT once after native Companion transport is enabled. The task was not started."));
    }
    if (taskCreationTimedOut(error)) {
        return failure(
            request,
            QStringLiteral("timed_out"),
            QStringLiteral(
                "Codex did not confirm the new task in time."));
    }
    return failure(
        request,
        QStringLiteral("create_failed"),
        QStringLiteral(
            "Codex did not start the new task."));
}

QString resetMessage(
    UsageResetOutcome outcome)
{
    switch (outcome) {
    case UsageResetOutcome::Reset:
        return QStringLiteral(
            "Codex usage reset applied.");
    case UsageResetOutcome::NothingToReset:
        return QStringLiteral(
            "There is currently no Codex limit to reset.");
    case UsageResetOutcome::NoCredit:
        return QStringLiteral(
            "That Codex reset is no longer available.");
    case UsageResetOutcome::AlreadyRedeemed:
        return QStringLiteral(
            "That Codex reset was already used.");
    }
    return QStringLiteral(
        "Codex reset request completed.");
}

} // namespace

struct MobileRequestDispatcher::State final {
    MobileRequestReadDependencies reads;
    MobileRequestMutationDependencies mutations;
    MobileRequestDispatcherConfiguration
        configuration;

    BridgeResponse dispatch(
        const BridgeRequest& request) const
    {
        if (request.protocolVersion
            != kBridgeProtocolVersion) {
            return failure(
                request,
                QStringLiteral(
                    "protocol_mismatch"),
                QStringLiteral(
                    "Update Codex Companion on the Windows PC and iPhone."));
        }

        switch (request.operation) {
        case BridgeOperation::Handshake:
            return handshake(request);
        case BridgeOperation::ListTasks:
            return listTasks(request);
        case BridgeOperation::LoadMessages:
            return loadMessages(request);
        case BridgeOperation::SendMessage:
            return sendMessage(request);
        case BridgeOperation::RespondToApproval:
            return respondToApproval(request);
        case BridgeOperation::CreateTask:
            return createTask(request);
        case BridgeOperation::LoadCapabilities:
            return loadCapabilities(request);
        case BridgeOperation::SendCasualChat:
            return sendCasualChat(request);
        case BridgeOperation::LoadUsage:
            return loadUsage(request);
        case BridgeOperation::ConsumeUsageReset:
            return consumeUsageReset(request);
        case BridgeOperation::CreateGoal:
            return mutateGoal(
                request,
                RuntimeGoalMutationKind::Create);
        case BridgeOperation::ResumeGoal:
            return mutateGoal(
                request,
                RuntimeGoalMutationKind::Resume);
        case BridgeOperation::UpdateGoal:
            return mutateGoal(
                request,
                RuntimeGoalMutationKind::Update);
        case BridgeOperation::
            LoadPresencePetManifest:
            return loadPresencePetManifest(
                request);
        case BridgeOperation::
            LoadPresencePetChunk:
            return loadPresencePetChunk(
                request);
        }
        return failure(
            request,
            QStringLiteral("archive_error"),
            QStringLiteral(
                "The Companion request operation is unsupported."));
    }

private:
    BridgeResponse handshake(
        const BridgeRequest& request) const
    {
        BridgeResponse response =
            success(request);
        response.macName =
            configuration.hostName.trimmed();
        response.macDeviceId =
            configuration.hostDeviceId.trimmed();
        if (configuration.relayUrlProvider) {
            std::optional<QString> relay =
                configuration.relayUrlProvider();
            if (relay.has_value()) {
                const QString normalized =
                    relay->trimmed();
                if (!normalized.isEmpty()) {
                    response.relayUrlString =
                        normalized;
                }
            }
        }
        if (configuration
                .presencePetCatalogService) {
            response.features =
                QVector<BridgeFeature>{
                    BridgeFeature::
                        PresencePetPackageV1,
                };
            const auto presentation =
                configuration
                    .presencePetCatalogService
                    ->presentation();
            response.selectedDesktopPetId =
                presentation
                    .selectedDesktopPetId;
            response.presencePetCatalog =
                presentation.catalog;
        }
        return response;
    }

    BridgeResponse
    loadPresencePetManifest(
        const BridgeRequest& request) const
    {
        const QString packageId =
            nonempty(
                request
                    .presencePetPackageId);
        const QString contentHash =
            nonempty(
                request
                    .presencePetContentHash);
        if (packageId.isEmpty()
            || contentHash.isEmpty()) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_presence_pet_request"),
                QStringLiteral(
                    "Choose a valid Companion pet package first."));
        }
        if (!configuration
                 .presencePetCatalogService) {
            return failure(
                request,
                QStringLiteral(
                    "presence_pet_unavailable"),
                QStringLiteral(
                    "Companion pet packages are unavailable on this PC."));
        }

        const auto loaded =
            configuration
                .presencePetCatalogService
                ->manifest(
                    packageId,
                    contentHash);
        if (!loaded.hasValue()) {
            return failure(
                request,
                loaded.error().code,
                loaded.error().message);
        }
        BridgeResponse response =
            success(request);
        response.presencePetManifest =
            loaded.value();
        return response;
    }

    BridgeResponse loadPresencePetChunk(
        const BridgeRequest& request) const
    {
        const QString packageId =
            nonempty(
                request
                    .presencePetPackageId);
        const QString contentHash =
            nonempty(
                request
                    .presencePetContentHash);
        const QString fileName =
            nonempty(
                request
                    .presencePetFileName);
        const qint64 offset =
            request.presencePetOffset
                .value_or(-1);
        const qint64 length =
            request.presencePetLength
                .value_or(-1);
        if (packageId.isEmpty()
            || contentHash.isEmpty()
            || fileName.isEmpty()
            || offset < 0
            || length <= 0
            || length
                > MobilePresencePetCatalogService::
                      kMaximumChunkLength) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_presence_pet_request"),
                QStringLiteral(
                    "The Companion pet file request is invalid."));
        }
        if (!configuration
                 .presencePetCatalogService) {
            return failure(
                request,
                QStringLiteral(
                    "presence_pet_unavailable"),
                QStringLiteral(
                    "Companion pet packages are unavailable on this PC."));
        }

        const auto loaded =
            configuration
                .presencePetCatalogService
                ->chunk(
                    packageId,
                    contentHash,
                    fileName,
                    offset,
                    length);
        if (!loaded.hasValue()) {
            return failure(
                request,
                loaded.error().code,
                loaded.error().message);
        }
        BridgeResponse response =
            success(request);
        response.presencePetChunk =
            loaded.value();
        return response;
    }

    BridgeResponse listTasks(
        const BridgeRequest& request) const
    {
        const Result<MobileTaskPage> page =
            invoke<MobileTaskPage>(
                reads.taskPageLoader,
                request.cursor,
                boundedLimit(
                    request.limit,
                    kDefaultTaskPageSize));
        if (!page.hasValue()) {
            return archiveFailure(
                request,
                page.error());
        }

        QVector<BridgeTask> tasks =
            page.value().tasks;
        if (reads.goalLoader
            && !tasks.isEmpty()) {
            QVector<QString> threadIds;
            threadIds.reserve(tasks.size());
            for (const BridgeTask& task : tasks) {
                threadIds.push_back(task.id);
            }
            const Result<MobileGoalMap> goals =
                invoke<MobileGoalMap>(
                    reads.goalLoader,
                    std::move(threadIds));
            if (goals.hasValue()) {
                tasks = attachGoals(
                    std::move(tasks),
                    goals.value());
            }
        }

        BridgeResponse response =
            success(request);
        response.tasks = std::move(tasks);
        response.nextCursor =
            page.value().nextCursor;
        return response;
    }

    BridgeResponse loadMessages(
        const BridgeRequest& request) const
    {
        const QString threadId =
            nonempty(request.threadId);
        if (threadId.isEmpty()) {
            return failure(
                request,
                QStringLiteral("missing_thread"),
                QStringLiteral(
                    "Choose a task first."));
        }
        const Result<HistorySnapshot> loaded =
            invoke<HistorySnapshot>(
                reads.historyLoader,
                MobileHistoryKey{
                    threadId,
                    request.cursor,
                    static_cast<int>(
                        boundedLimit(
                            request.limit,
                            kDefaultMessagePageSize)),
                });
        if (!loaded.hasValue()) {
            return archiveFailure(
                request,
                loaded.error());
        }

        const HistorySnapshot& snapshot =
            loaded.value();
        BridgeResponse response =
            success(request);
        response.messages = snapshot.messages;
        response.nextCursor =
            snapshot.nextCursor;
        response.threadId = threadId;
        response.timelineItems =
            snapshot.timelineItems;
        response.revision =
            snapshot.revision;
        response.timelineNextCursor =
            snapshot.timelineNextCursor;
        response.subagents =
            snapshot.subagents;
        response.contextUsage =
            snapshot.contextUsage;
        return response;
    }

    BridgeResponse sendMessage(
        const BridgeRequest& request) const
    {
        const QString threadId =
            nonempty(request.threadId);
        const QString text =
            nonempty(request.text);
        if (threadId.isEmpty()
            || text.isEmpty()) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_message"),
                QStringLiteral(
                    "Enter a message first."));
        }

        const std::optional<BridgeTask> task =
            findTask(reads, threadId);
        SendRequest send;
        send.prompt = text;
        send.threadId = threadId;
        send.cwd = nonempty(request.cwd);
        if (send.cwd.isEmpty()
            && task.has_value()
            && task->cwd.has_value()) {
            send.cwd = task->cwd->trimmed();
        }
        send.action =
            request.sendAction
                    == SendAction::Steer
            ? SendAction::Steer
            : SendAction::Reply;
        if (task.has_value()
            && task->activeTurnId.has_value()) {
            send.expectedTurnId =
                task->activeTurnId->trimmed();
        }
        send.clientMessageId =
            clientMessageId(request);
        send.model =
            nonempty(request.model);
        send.reasoningEffort =
            nonempty(
                request.reasoningEffort);
        if (request.attachments.has_value()) {
            send.attachments =
                *request.attachments;
        }
        send.executionState =
            std::make_shared<
                SendExecutionState>();
        const std::shared_ptr<
            SendExecutionState> executionState =
                send.executionState;

        const Result<void> sent =
            invoke<void>(
                mutations.sendMessage,
                std::move(send));
        if (!sent.hasValue()) {
            return sendFailure(
                request,
                sent.error());
        }
        BridgeResponse response =
            success(request);
        const bool retainedCurrentSettings =
            executionState
                ->retainedCurrentSettings
                .load(
                    std::memory_order_acquire);
        response.message =
            request.sendAction
                    == SendAction::Steer
            ? retainedCurrentSettings
                ? QStringLiteral(
                      "Steered task using its current model.")
                : QStringLiteral(
                      "Steered task.")
            : retainedCurrentSettings
                ? QStringLiteral(
                      "Reply sent using the task's current model.")
                : QStringLiteral(
                      "Reply sent.");
        return response;
    }

    BridgeResponse respondToApproval(
        const BridgeRequest& request) const
    {
        const QString threadId =
            nonempty(request.threadId);
        if (threadId.isEmpty()
            || !request.approvalDecision
                    .has_value()) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_approval"),
                QStringLiteral(
                    "That approval request is unavailable."));
        }

        const ApprovalDecision decision =
            *request.approvalDecision;
        const Result<void> responded =
            invoke<void>(
                mutations.respondToApproval,
                threadId,
                decision);
        if (!responded.hasValue()) {
            return approvalFailure(
                request,
                responded.error());
        }
        BridgeResponse response =
            success(request);
        response.message =
            decision == ApprovalDecision::Decline
            ? QStringLiteral(
                  "Request declined.")
            : QStringLiteral(
                  "Approval sent.");
        return response;
    }

    BridgeResponse createTask(
        const BridgeRequest& request) const
    {
        const QString prompt =
            nonempty(request.text);
        if (prompt.isEmpty()) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_message"),
                QStringLiteral(
                    "Describe the new task first."));
        }

        RuntimeTaskCreateRequest create;
        create.text = prompt;
        create.cwd = nonempty(request.cwd);
        create.clientMessageId =
            clientMessageId(request);
        create.model =
            nonempty(request.model);
        create.reasoningEffort =
            nonempty(
                request.reasoningEffort);
        create.skillName =
            nonempty(request.skillName);
        create.skillPath =
            nonempty(request.skillPath);
        if (request.attachments.has_value()) {
            create.attachments =
                *request.attachments;
        }
        const Result<QString> created =
            invoke<QString>(
                mutations.createTask,
                std::move(create));
        if (!created.hasValue()) {
            return createTaskFailure(
                request,
                created.error());
        }
        const QString threadId =
            created.value().trimmed();
        if (threadId.isEmpty()) {
            return failure(
                request,
                QStringLiteral("create_failed"),
                QStringLiteral(
                    "Codex did not start the new task."));
        }

        BridgeResponse response =
            success(request);
        response.message =
            QStringLiteral(
                "New Codex task started.");
        response.threadId = threadId;
        return response;
    }

    BridgeResponse loadCapabilities(
        const BridgeRequest& request) const
    {
        const Result<BridgeCapabilities> loaded =
            invoke<BridgeCapabilities>(
                reads.capabilityLoader,
                nonempty(request.cwd));
        if (!loaded.hasValue()) {
            return archiveFailure(
                request,
                loaded.error());
        }
        BridgeResponse response =
            success(request);
        response.capabilities =
            loaded.value();
        return response;
    }

    BridgeResponse sendCasualChat(
        const BridgeRequest& request) const
    {
        const QString text =
            nonempty(request.text);
        const bool hasAttachments =
            request.attachments.has_value()
            && !request.attachments->isEmpty();
        if (text.isEmpty() && !hasAttachments) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_message"),
                QStringLiteral(
                    "Enter a message first."));
        }

        const ResolvedChatAgent agent =
            ChatCatalog::resolveAgent(
                nonempty(
                    request.chatAgentId));
        ChatRequest chat;
        chat.provider =
            request.chatProvider.value_or(
                ChatProvider::OnDevice);
        chat.modelId =
            nonempty(request.chatModelId);
        chat.prompt =
            QStringLiteral(
                "Mode: %1\n%2\n\nUser request:\n%3")
                .arg(agent.agent.name)
                .arg(agent.promptInstruction)
                .arg(text);
        if (request.attachments.has_value()) {
            chat.attachments =
                *request.attachments;
        }
        const Result<ChatResult> answered =
            invoke<ChatResult>(
                mutations.sendCasualChat,
                std::move(chat));
        if (!answered.hasValue()) {
            QString errorCode;
            switch (chat.provider) {
            case ChatProvider::OnDevice:
                errorCode =
                    QStringLiteral(
                        "on_device_chat_unavailable");
                break;
            case ChatProvider::OpenAIAPI:
                errorCode =
                    QStringLiteral(
                        "openai_chat_unavailable");
                break;
            case ChatProvider::LumoAPI:
                errorCode =
                    QStringLiteral(
                        "lumo_chat_unavailable");
                break;
            }
            return failure(
                request,
                std::move(errorCode),
                answered.error().message);
        }

        BridgeMessage message;
        message.id =
            QUuid::createUuid()
                .toString(
                    QUuid::WithoutBraces)
                .toUpper();
        message.role =
            MessageRole::Assistant;
        message.text =
            answered.value().text;
        message.createdAt =
            configuration.nowProvider
            ? configuration.nowProvider()
            : currentBridgeDate();

        BridgeResponse response =
            success(request);
        response.chatMessage =
            std::move(message);
        return response;
    }

    BridgeResponse loadUsage(
        const BridgeRequest& request) const
    {
        const Result<BridgeUsageSnapshot> loaded =
            invoke<BridgeUsageSnapshot>(
                reads.usageLoader);
        if (!loaded.hasValue()) {
            return failure(
                request,
                QStringLiteral(
                    "usage_unavailable"),
                loaded.error().message);
        }
        BridgeResponse response =
            success(request);
        response.usageSnapshot =
            loaded.value();
        return response;
    }

    BridgeResponse consumeUsageReset(
        const BridgeRequest& request) const
    {
        const QString creditId =
            nonempty(request.resetCreditId);
        if (creditId.isEmpty()
            || !request.idempotencyKey
                    .has_value()) {
            return failure(
                request,
                QStringLiteral(
                    "invalid_reset"),
                QStringLiteral(
                    "Choose an available Codex reset first."));
        }
        const Result<UsageResetOutcome> consumed =
            invoke<UsageResetOutcome>(
                mutations.consumeUsageReset,
                creditId,
                *request.idempotencyKey);
        if (!consumed.hasValue()) {
            return failure(
                request,
                QStringLiteral(
                    "reset_failed"),
                consumed.error().message);
        }

        BridgeResponse response =
            success(request);
        response.message =
            resetMessage(
                consumed.value());
        if (reads.usageLoader) {
            const Result<BridgeUsageSnapshot>
                refreshed =
                    invoke<BridgeUsageSnapshot>(
                        reads.usageLoader);
            if (refreshed.hasValue()) {
                response.usageSnapshot =
                    refreshed.value();
            }
        }
        return response;
    }

    BridgeResponse mutateGoal(
        const BridgeRequest& request,
        RuntimeGoalMutationKind kind) const
    {
        const QString threadId =
            nonempty(request.threadId);
        const QString objective =
            nonempty(
                request.goalObjective);
        if (kind
                == RuntimeGoalMutationKind::Create
            && (threadId.isEmpty()
                || objective.isEmpty()
                || (request.goalTokenBudget
                        .has_value()
                    && *request.goalTokenBudget
                        <= 0))) {
            return failure(
                request,
                QStringLiteral("invalid_goal"),
                QStringLiteral(
                    "Choose a task and enter a valid goal objective first."));
        }
        if (kind
                == RuntimeGoalMutationKind::Resume
            && threadId.isEmpty()) {
            return failure(
                request,
                QStringLiteral("invalid_goal"),
                QStringLiteral(
                    "Choose a goal first."));
        }
        if (kind
                == RuntimeGoalMutationKind::Update
            && (threadId.isEmpty()
                || objective.isEmpty())) {
            return failure(
                request,
                QStringLiteral("invalid_goal"),
                QStringLiteral(
                    "Choose a goal and enter its updated objective first."));
        }

        RuntimeGoalMutationRequest mutation;
        mutation.kind = kind;
        mutation.threadId = threadId;
        if (kind
                == RuntimeGoalMutationKind::Create
            || kind
                == RuntimeGoalMutationKind::Update) {
            mutation.objective =
                objective;
        }
        if (kind
            == RuntimeGoalMutationKind::Create) {
            mutation.tokenBudget =
                request.goalTokenBudget;
        }

        const Result<BridgeGoal> changed =
            invoke<BridgeGoal>(
                mutations.mutateGoal,
                std::move(mutation));
        if (!changed.hasValue()) {
            QString code;
            if (kind
                == RuntimeGoalMutationKind::Create) {
                code = QStringLiteral(
                    "goal_create_failed");
            } else if (
                kind
                == RuntimeGoalMutationKind::Resume) {
                code = QStringLiteral(
                    "goal_resume_failed");
            } else {
                code = QStringLiteral(
                    "goal_update_failed");
            }
            return failure(
                request,
                std::move(code),
                changed.error().message);
        }

        BridgeResponse response =
            success(request);
        if (kind
            == RuntimeGoalMutationKind::Create) {
            response.message =
                QStringLiteral(
                    "Goal created.");
        } else if (
            kind
            == RuntimeGoalMutationKind::Resume) {
            response.message =
                QStringLiteral(
                    "Goal resumed.");
        } else {
            response.message =
                QStringLiteral(
                    "Goal updated.");
        }
        response.goal =
            changed.value();
        return response;
    }
};

MobileRequestDispatcher::MobileRequestDispatcher(
    MobileRequestReadDependencies reads,
    MobileRequestMutationDependencies mutations,
    MobileRequestDispatcherConfiguration
        configuration)
    : state_(std::make_shared<State>(
          State{
              std::move(reads),
              std::move(mutations),
              std::move(configuration),
          }))
{
}

QFuture<BridgeResponse>
MobileRequestDispatcher::handle(
    BridgeRequest request) const
{
    const std::shared_ptr<const State> state =
        state_;
    return QtConcurrent::run(
        dispatcherPool(),
        [state, request = std::move(request)] {
            try {
                return state->dispatch(request);
            } catch (const std::exception& error) {
                const QString message =
                    QString::fromUtf8(
                        error.what()).trimmed();
                return failure(
                    request,
                    QStringLiteral(
                        "archive_error"),
                    message.isEmpty()
                        ? QStringLiteral(
                              "The Companion request failed.")
                        : message);
            } catch (...) {
                return failure(
                    request,
                    QStringLiteral(
                        "archive_error"),
                    QStringLiteral(
                        "The Companion request failed."));
            }
        });
}

} // namespace companion
