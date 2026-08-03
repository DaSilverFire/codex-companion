#include "codex/commands/TaskCommandService.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

namespace companion {

namespace {

Result<void> commandFailure(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return Result<void>::failure({
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    });
}

QThreadPool* orchestrationPool()
{
    static QThreadPool* pool = [] {
        auto* result = new QThreadPool();
        result->setMaxThreadCount(
            std::max(2, QThread::idealThreadCount()));
        return result;
    }();
    return pool;
}

std::shared_ptr<QObject> retainedOwnerThreadDispatcher(
    QThread* ownerThread)
{
    auto deleter = [](QObject* object) {
        if (object == nullptr) {
            return;
        }
        if (object->thread() == QThread::currentThread()) {
            object->deleteLater();
            return;
        }
        const bool queued = QMetaObject::invokeMethod(
            object,
            [object] {
                object->deleteLater();
            },
            Qt::QueuedConnection);
        Q_UNUSED(queued);
    };
    std::shared_ptr<QObject> dispatcher(
        new QObject(),
        std::move(deleter));
    if (ownerThread != nullptr
        && dispatcher->thread() != ownerThread) {
        dispatcher->moveToThread(ownerThread);
    }
    return dispatcher;
}

Result<void> commandExceptionFailure(
    const QString& threadId,
    const QString& method)
{
    QVariantMap context{
        {QStringLiteral("threadId"), threadId},
    };
    if (!method.isEmpty()) {
        context.insert(QStringLiteral("method"), method);
    }
    return commandFailure(
        QStringLiteral("codex.send_failed"),
        QStringLiteral("Codex could not accept the message."),
        std::move(context));
}

Result<void> attachmentBoundaryFailure(
    const QString& threadId)
{
    return commandFailure(
        QStringLiteral("codex.attachments_unavailable"),
        QStringLiteral(
            "Codex command attachments are unavailable."),
        {{QStringLiteral("threadId"), threadId}});
}

enum class FollowerWaitStatus {
    Outcome,
    Malformed,
};

struct FollowerWaitResult final {
    FollowerWaitStatus status = FollowerWaitStatus::Malformed;
    FollowerSendOutcome outcome = FollowerSendOutcome::Failed;
};

FollowerWaitResult waitForFollower(
    QFuture<FollowerSendOutcome>& future)
{
    if (!future.isValid()) {
        return {FollowerWaitStatus::Malformed};
    }
    while (!future.isFinished() && !future.isCanceled()) {
        QThread::msleep(1);
    }
    if (future.isCanceled() || future.resultCount() < 1) {
        return {FollowerWaitStatus::Malformed};
    }
    try {
        return {
            FollowerWaitStatus::Outcome,
            future.result(),
        };
    } catch (...) {
        return {FollowerWaitStatus::Malformed};
    }
}

QString outcomeText(FollowerSendOutcome outcome)
{
    switch (outcome) {
    case FollowerSendOutcome::Sent:
        return QStringLiteral("sent");
    case FollowerSendOutcome::SharedDaemonUnavailable:
        return QStringLiteral("sharedDaemonUnavailable");
    case FollowerSendOutcome::TimedOut:
        return QStringLiteral("timedOut");
    case FollowerSendOutcome::ThreadNotLoaded:
        return QStringLiteral("threadNotLoaded");
    case FollowerSendOutcome::NoActiveTurn:
        return QStringLiteral("noActiveTurn");
    case FollowerSendOutcome::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

Result<void> resultForOutcome(
    FollowerSendOutcome outcome,
    const QString& threadId,
    const QString& method)
{
    switch (outcome) {
    case FollowerSendOutcome::Sent:
        return Result<void>::success();
    case FollowerSendOutcome::SharedDaemonUnavailable:
        return commandFailure(
            QStringLiteral("codex.shared_daemon_unavailable"),
            QStringLiteral("Codex shared daemon is unavailable."),
            {
                {QStringLiteral("threadId"), threadId},
                {QStringLiteral("method"), method},
            });
    case FollowerSendOutcome::TimedOut:
        return commandFailure(
            QStringLiteral("codex.send_timed_out"),
            QStringLiteral("Codex did not acknowledge the message in time."),
            {
                {QStringLiteral("threadId"), threadId},
                {QStringLiteral("method"), method},
            });
    case FollowerSendOutcome::ThreadNotLoaded:
        return commandFailure(
            QStringLiteral("codex.thread_not_loaded"),
            QStringLiteral("The Codex task is not loaded in the shared daemon."),
            {
                {QStringLiteral("threadId"), threadId},
                {QStringLiteral("method"), method},
            });
    case FollowerSendOutcome::NoActiveTurn:
        return commandFailure(
            QStringLiteral("codex.no_active_turn"),
            QStringLiteral("This task has no active turn to steer."),
            {
                {QStringLiteral("threadId"), threadId},
                {QStringLiteral("method"), method},
            });
    case FollowerSendOutcome::Failed:
        return commandFailure(
            QStringLiteral("codex.send_failed"),
            QStringLiteral("Codex could not accept the message."),
            {
                {QStringLiteral("threadId"), threadId},
                {QStringLiteral("method"), method},
            });
    }
    return commandFailure(
        QStringLiteral("codex.send_failed"),
        QStringLiteral("Codex could not accept the message."),
        {
            {QStringLiteral("threadId"), threadId},
            {QStringLiteral("method"), method},
        });
}

QUuid stagingUuidFor(const QString& clientMessageId)
{
    const QUuid parsed(clientMessageId);
    if (!parsed.isNull()) {
        return parsed;
    }
    return QUuid::createUuid();
}

QString normalizedClientMessageId(QString value)
{
    value = value.trimmed();
    if (!value.isEmpty()) {
        return value;
    }
    return QUuid::createUuid().toString(
        QUuid::WithoutBraces);
}

} // namespace

CommandOwnedAttachmentStager makeRetainedLegacyStager(
    CommandAttachmentStager legacy)
{
    if (!legacy) {
        return {};
    }
    return [
        legacy = std::move(legacy)](
        const QVector<BridgeAttachment>& attachments,
        const QUuid& requestId) {
        Result<QVector<StagedAttachment>> staged =
            legacy(
                attachments,
                requestId.toString(
                    QUuid::WithoutBraces));
        if (!staged.hasValue()) {
            return Result<
                StagedAttachmentBatch>::failure(
                staged.error());
        }
        auto cleanupLease =
            StagedAttachmentCleanupLease::
                retainedInert();
        if (cleanupLease == nullptr) {
            return Result<
                StagedAttachmentBatch>::failure({
                QStringLiteral(
                    "attachment.storage_failed"),
                QStringLiteral(
                    "Codex Companion could not retain "
                    "attachment cleanup state."),
                false,
                {},
            });
        }
        return Result<
            StagedAttachmentBatch>::success({
            std::move(staged.value()),
            std::move(cleanupLease),
        });
    };
}

struct TaskCommandService::ReplyAffinityState final {
    std::atomic<quint64> epoch = 0;
};

struct TaskCommandService::State final {
    State(
        CommandAttachmentValidator requestedValidator,
        CommandOwnedAttachmentStager requestedStager,
        CommandSettingsSender requestedSettingsSender,
        CommandSubmitter requestedSubmitter,
        CommandQueuedReplySender requestedQueuedReplySender,
        CommandDiagnosticSink requestedDiagnosticSink,
        CommandCompletionHook requestedCompletionHook)
        : validator(std::move(requestedValidator)),
          stager(std::move(requestedStager)),
          settingsSender(std::move(requestedSettingsSender)),
          submitter(std::move(requestedSubmitter)),
          queuedReplySender(
              std::move(requestedQueuedReplySender)),
          diagnosticSink(std::move(requestedDiagnosticSink)),
          completionHook(std::move(requestedCompletionHook))
    {
    }

    CommandAttachmentValidator validator;
    CommandOwnedAttachmentStager stager;
    CommandSettingsSender settingsSender;
    CommandSubmitter submitter;
    CommandQueuedReplySender queuedReplySender;
    CommandDiagnosticSink diagnosticSink;
    CommandCompletionHook completionHook;
    std::mutex mutex;
    QSet<QString> inFlight;
};

TaskCommandService::TaskCommandService(
    const CodexEnvironment& environment,
    QObject* parent)
    : TaskCommandService(
          [](const QVector<BridgeAttachment>& attachments) {
              return AttachmentStore::validate(
                  attachments);
          },
          [store = AttachmentStore()](
              const QVector<BridgeAttachment>& attachments,
              const QUuid& requestId) {
              return store.stageOwned(
                  attachments,
                  requestId);
          },
          [client = std::make_shared<FollowerClient>(
               environment)](
              QString threadId,
              QString model,
              QString effort) {
              return client->updateThreadSettings(
                  std::move(threadId),
                  std::move(model),
                  std::move(effort));
          },
          [client = std::make_shared<FollowerClient>(
               environment)](
              QString prompt,
              QString threadId,
              SendAction action,
              QString clientMessageId,
              QString cwd,
              QVector<StagedAttachment> attachments) {
              return client->submit(
                  std::move(prompt),
                  std::move(threadId),
                  action,
                  std::move(clientMessageId),
                  std::move(cwd),
                  std::move(attachments));
          },
          [client = std::make_shared<FollowerClient>(
               environment)](
              QString prompt,
              QString threadId,
              QString clientMessageId,
              QString cwd,
              QVector<StagedAttachment> attachments) {
              return client->queueReply(
                  std::move(prompt),
                  std::move(threadId),
                  std::move(clientMessageId),
                  std::move(cwd),
                  std::move(attachments));
          },
          {},
          parent)
{
}

TaskCommandService::TaskCommandService(
    CommandAttachmentValidator validator,
    CommandAttachmentStager stager,
    CommandSettingsSender settingsSender,
    CommandSubmitter submitter,
    CommandQueuedReplySender queuedReplySender,
    CommandDiagnosticSink diagnosticSink,
    QObject* parent)
    : TaskCommandService(
          std::move(validator),
          makeRetainedLegacyStager(
              std::move(stager)),
          std::move(settingsSender),
          std::move(submitter),
          std::move(queuedReplySender),
          std::move(diagnosticSink),
          {},
          parent)
{
}

TaskCommandService::TaskCommandService(
    CommandAttachmentValidator validator,
    CommandAttachmentStager stager,
    CommandSettingsSender settingsSender,
    CommandSubmitter submitter,
    CommandQueuedReplySender queuedReplySender,
    CommandDiagnosticSink diagnosticSink,
    CommandCompletionHook completionHook,
    QObject* parent)
    : QObject(parent),
      replyAffinityState_(std::make_shared<ReplyAffinityState>()),
      state_(std::make_shared<State>(
          std::move(validator),
          makeRetainedLegacyStager(
              std::move(stager)),
          std::move(settingsSender),
          std::move(submitter),
          std::move(queuedReplySender),
          std::move(diagnosticSink),
          std::move(completionHook)))
{
}

TaskCommandService::TaskCommandService(
    CommandAttachmentValidator validator,
    CommandOwnedAttachmentStager stager,
    CommandSettingsSender settingsSender,
    CommandSubmitter submitter,
    CommandQueuedReplySender queuedReplySender,
    CommandDiagnosticSink diagnosticSink,
    QObject* parent)
    : TaskCommandService(
          std::move(validator),
          std::move(stager),
          std::move(settingsSender),
          std::move(submitter),
          std::move(queuedReplySender),
          std::move(diagnosticSink),
          {},
          parent)
{
}

TaskCommandService::TaskCommandService(
    CommandAttachmentValidator validator,
    CommandOwnedAttachmentStager stager,
    CommandSettingsSender settingsSender,
    CommandSubmitter submitter,
    CommandQueuedReplySender queuedReplySender,
    CommandDiagnosticSink diagnosticSink,
    CommandCompletionHook completionHook,
    QObject* parent)
    : QObject(parent),
      replyAffinityState_(std::make_shared<ReplyAffinityState>()),
      state_(std::make_shared<State>(
          std::move(validator),
          std::move(stager),
          std::move(settingsSender),
          std::move(submitter),
          std::move(queuedReplySender),
          std::move(diagnosticSink),
          std::move(completionHook)))
{
}

TaskCommandService::~TaskCommandService() = default;

bool TaskCommandService::event(QEvent* event)
{
    if (event != nullptr
        && event->type() == QEvent::ThreadChange
        && replyAffinityState_ != nullptr) {
        replyAffinityState_->epoch.fetch_add(
            1,
            std::memory_order_acq_rel);
    }
    return QObject::event(event);
}

CommitAwareMutationHandle<void>
TaskCommandService::sendMutation(
    const SendRequest& request)
{
    if (request.executionState) {
        request.executionState
            ->retainedCurrentSettings
            .store(
                false,
                std::memory_order_release);
    }
    auto mutation =
        CommitAwareMutation<void>::create();
    CommitAwareMutationHandle<void> handle =
        mutation->handle();
    const QString prompt = request.prompt.trimmed();
    if (prompt.isEmpty()) {
        mutation->finish(commandFailure(
            QStringLiteral("codex.prompt_empty"),
            QStringLiteral("Enter a message first.")));
        return handle;
    }
    const QString threadId = request.threadId.trimmed();
    if (threadId.isEmpty()) {
        mutation->finish(commandFailure(
            QStringLiteral("codex.thread_id_empty"),
            QStringLiteral("Choose a Codex task first.")));
        return handle;
    }
    if (request.action == SendAction::Steer
        && request.expectedTurnId.trimmed().isEmpty()) {
        mutation->finish(commandFailure(
            QStringLiteral("codex.no_active_turn"),
            QStringLiteral("This task has no active turn to steer."),
            {{QStringLiteral("threadId"), threadId}}));
        return handle;
    }

    const std::shared_ptr<State> state = state_;
    {
        const std::scoped_lock lock(state->mutex);
        if (state->inFlight.contains(threadId)) {
            mutation->finish(commandFailure(
                QStringLiteral("codex.send_in_flight"),
                QStringLiteral(
                    "A Codex message is already being sent to this task."),
                {{QStringLiteral("threadId"), threadId}}));
            return handle;
        }
        state->inFlight.insert(threadId);
    }

    try {
        auto servicePointer =
            std::make_shared<
                QPointer<TaskCommandService>>(this);
        std::shared_ptr<QObject> replyDispatcher =
            retainedOwnerThreadDispatcher(thread());
        const std::shared_ptr<
            ReplyAffinityState> replyAffinityState =
            replyAffinityState_;
        const quint64 replyAffinityEpoch =
            replyAffinityState != nullptr
            ? replyAffinityState->epoch.load(
                  std::memory_order_acquire)
            : 0;
        orchestrationPool()->start(
        [state,
         servicePointer,
         replyDispatcher,
         replyAffinityState,
         replyAffinityEpoch,
         request,
         prompt,
         threadId,
         mutation] {
            struct InFlightGuard final {
                InFlightGuard(
                    std::shared_ptr<State> requestedState,
                    QString requestedThreadId)
                    : state(std::move(requestedState)),
                      threadId(std::move(requestedThreadId))
                {
                }

                InFlightGuard(const InFlightGuard&) = delete;
                InFlightGuard& operator=(const InFlightGuard&) = delete;

                std::shared_ptr<State> state;
                QString threadId;
                bool active = true;

                ~InFlightGuard()
                {
                    clear();
                }

                void clear()
                {
                    const std::scoped_lock lock(
                        state->mutex);
                    if (active) {
                        state->inFlight.remove(threadId);
                        active = false;
                    }
                }
            };
            auto guard =
                std::make_shared<InFlightGuard>(
                    state,
                    threadId);
            auto totalTimer =
                std::make_shared<QElapsedTimer>();
            totalTimer->start();
            QString currentMethod;
            const auto record =
                [state,
                 threadId,
                 totalTimer](
                    const QString& method,
                    const QString& outcome,
                    qsizetype attachmentCount) noexcept {
                if (!state->diagnosticSink) {
                    return;
                }
                try {
                    state->diagnosticSink({
                        threadId,
                        method,
                        outcome,
                        attachmentCount,
                        totalTimer->elapsed(),
                    });
                } catch (...) {
                }
            };
            const auto recordOutcome =
                [&record](
                    const QString& method,
                    FollowerSendOutcome outcome,
                    qsizetype attachmentCount) noexcept {
                record(
                    method,
                    outcomeText(outcome),
                    attachmentCount);
            };
            const auto notifyCompletion =
                [state](
                    CommandCompletionPhase phase) noexcept {
                if (!state->completionHook) {
                    return;
                }
                try {
                    state->completionHook(phase);
                } catch (...) {
                }
            };
            const auto complete =
                [guard,
                 mutation,
                 notifyCompletion](Result<void> result) {
                guard->clear();
                return mutation->finishWithCallbacks(
                    std::move(result),
                    [notifyCompletion] {
                        notifyCompletion(
                            CommandCompletionPhase::
                                BeforeAddResult);
                    },
                    [notifyCompletion] {
                        notifyCompletion(
                            CommandCompletionPhase::
                                AfterAddResultBeforeFinish);
                    });
            };

            try {
                const QString clientMessageId =
                    normalizedClientMessageId(
                        request.clientMessageId);
                const QUuid requestId =
                    stagingUuidFor(clientMessageId);
                const QString cwd = request.cwd.trimmed();

                if (!state->validator || !state->stager) {
                    complete(attachmentBoundaryFailure(threadId));
                    return;
                }

                const Result<void> valid =
                    state->validator(request.attachments);
                if (!valid.hasValue()) {
                    complete(attachmentBoundaryFailure(threadId));
                    return;
                }
                Result<StagedAttachmentBatch> staged =
                    state->stager(
                        request.attachments,
                        requestId);
                if (!staged.hasValue()) {
                    complete(attachmentBoundaryFailure(threadId));
                    return;
                }
                StagedAttachmentBatch batch =
                    std::move(staged.value());
                if (batch.cleanupLease == nullptr) {
                    complete(attachmentBoundaryFailure(threadId));
                    return;
                }
                const QVector<StagedAttachment> attachments =
                    std::move(batch.attachments);
                const auto claimCommit =
                    [mutation,
                     cleanupLease = batch.cleanupLease,
                     record](
                        const QString& method,
                        qsizetype attachmentCount) {
                    record(
                        method,
                        QStringLiteral("commitPending"),
                        attachmentCount);
                    record(
                        method,
                        QStringLiteral("claimEstablished"),
                        attachmentCount);
                    if (!mutation->tryCommit()) {
                        return false;
                    }
                    cleanupLease
                        ->retainForCommittedUse();
                    record(
                        method,
                        QStringLiteral("committed"),
                        attachmentCount);
                    return true;
                };

                const bool settingsRequested =
                    !request.model.trimmed().isEmpty()
                    || !request.reasoningEffort
                            .trimmed()
                            .isEmpty();
                if (settingsRequested) {
                    currentMethod = QStringLiteral("settings");
                    FollowerSendOutcome settingsOutcome =
                        FollowerSendOutcome::Failed;
                    if (state->settingsSender) {
                        if (!claimCommit(
                                QStringLiteral("settings"),
                                attachments.size())) {
                            guard->clear();
                            return;
                        }
                        try {
                            QFuture<FollowerSendOutcome> settings =
                                state->settingsSender(
                                    threadId,
                                    request.model,
                                    request.reasoningEffort);
                            const FollowerWaitResult settingsWait =
                                waitForFollower(settings);
                            if (settingsWait.status
                                == FollowerWaitStatus::Outcome) {
                                settingsOutcome =
                                    settingsWait.outcome;
                            }
                        } catch (...) {
                            settingsOutcome =
                                FollowerSendOutcome::Failed;
                        }
                    }
                    recordOutcome(
                        QStringLiteral("settings"),
                        settingsOutcome,
                        attachments.size());
                    if (request.executionState) {
                        request.executionState
                            ->retainedCurrentSettings
                            .store(
                                settingsOutcome
                                    != FollowerSendOutcome::
                                        Sent,
                                std::memory_order_release);
                    }
                }

                const QString method =
                    request.action == SendAction::Reply
                    ? QStringLiteral("reply")
                    : QStringLiteral("steer");
                currentMethod = method;
                if ((request.action == SendAction::Reply
                     && !state->queuedReplySender)
                    || (request.action == SendAction::Steer
                        && !state->submitter)) {
                    complete(
                        commandFailure(
                        QStringLiteral("codex.send_failed"),
                        QStringLiteral(
                            "Codex command transport is unavailable."),
                        {
                            {QStringLiteral("threadId"), threadId},
                            {QStringLiteral("method"), method},
                        }));
                    return;
                }
                if (!claimCommit(
                        method,
                        attachments.size())) {
                    guard->clear();
                    return;
                }
                QFuture<FollowerSendOutcome> sent =
                    request.action == SendAction::Reply
                    ? state->queuedReplySender(
                          prompt,
                          threadId,
                          clientMessageId,
                          cwd,
                          attachments)
                    : state->submitter(
                          prompt,
                          threadId,
                          request.action,
                          clientMessageId,
                          cwd,
                          attachments);
                const FollowerWaitResult sentWait =
                    waitForFollower(sent);
                if (sentWait.status == FollowerWaitStatus::Malformed) {
                    complete(
                        commandExceptionFailure(
                            threadId,
                            method));
                    return;
                }
                const FollowerSendOutcome outcome =
                    sentWait.outcome;
                recordOutcome(
                    method,
                    outcome,
                    attachments.size());
                if (request.action == SendAction::Reply
                    && outcome == FollowerSendOutcome::Sent
                    && replyDispatcher != nullptr) {
                    currentMethod =
                        QStringLiteral("replyQueued");
                    const qsizetype attachmentCount =
                        attachments.size();
                    record(
                        QStringLiteral("replyQueuedDispatch"),
                        QStringLiteral("postPending"),
                        attachmentCount);
                    record(
                        QStringLiteral("replyQueuedDispatch"),
                        QStringLiteral("postDeferred"),
                        attachmentCount);
                    const bool completed =
                        complete(
                        resultForOutcome(
                            FollowerSendOutcome::Sent,
                            threadId,
                            method));
                    if (!completed) {
                        return;
                    }
                    const bool queued =
                        QMetaObject::invokeMethod(
                            replyDispatcher.get(),
                            [replyDispatcher,
                             servicePointer,
                             replyAffinityState,
                             replyAffinityEpoch,
                             threadId,
                             clientMessageId] {
                                try {
                                    Q_UNUSED(replyDispatcher);
                                    if (replyAffinityState == nullptr
                                        || replyAffinityState->epoch.load(
                                               std::memory_order_acquire)
                                            != replyAffinityEpoch) {
                                        return;
                                    }
                                    TaskCommandService* service =
                                        servicePointer->data();
                                    if (service == nullptr) {
                                        return;
                                    }
                                    emit service->replyQueued(
                                        threadId,
                                        clientMessageId);
                                } catch (...) {
                                }
                            },
                            Qt::QueuedConnection);
                    Q_UNUSED(queued);
                    return;
                }
                complete(
                    resultForOutcome(
                        outcome,
                        threadId,
                        method));
            } catch (...) {
                complete(
                    commandExceptionFailure(
                        threadId,
                        currentMethod));
            }
        });
    } catch (...) {
        {
            const std::scoped_lock lock(state->mutex);
            state->inFlight.remove(threadId);
        }
        mutation->finish(
            commandExceptionFailure(
                threadId,
                {}));
    }
    return handle;
}

QFuture<Result<void>> TaskCommandService::send(
    const SendRequest& request)
{
    return cancellationDetachedMutationFuture(
        sendMutation(request).terminalFuture);
}

} // namespace companion
