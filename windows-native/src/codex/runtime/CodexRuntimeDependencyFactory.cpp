#include "codex/runtime/CodexRuntimeDependencyFactory.h"

#include "codex/runtime/CapabilityService.h"
#include "codex/runtime/HistorySnapshotLoader.h"
#include "codex/runtime/TaskSnapshotLoader.h"
#include "codex/state/HistoryCoordinator.h"

#include <QDateTime>
#include <QFuture>
#include <QThreadPool>
#include <QUuid>

#include <optional>
#include <utility>

namespace companion {

namespace {

CompanionError runtimeUnavailableError()
{
    return {
        QStringLiteral("codex.runtime_unavailable"),
        QStringLiteral(
            "Codex runtime is unavailable."),
        false,
        {},
    };
}

CompanionError taskCreateFailure()
{
    return {
        QStringLiteral(
            "task.create_failed"),
        QStringLiteral(
            "Task creation failed."),
        true,
        {},
    };
}

CompanionError chatFailure()
{
    return {
        QStringLiteral("chat.failed"),
        QStringLiteral("Chat failed."),
        true,
        {},
    };
}

CompanionError goalFailure()
{
    return {
        QStringLiteral("goal.invalid_request"),
        QStringLiteral(
            "Goal request is invalid."),
        false,
        {},
    };
}

bool validServices(
    const CodexRuntimeProductionServices&
        services)
{
    return services.attachmentStore != nullptr
        && services.taskCommandService
            != nullptr
        && services.approvalService != nullptr
        && (services.taskCreator != nullptr
            || services.taskCreatePerformer)
        && services.credentialStore != nullptr
        && services.openAITransport != nullptr
        && services.lumoTransport != nullptr
        && services.goalService != nullptr
        && services.usageService != nullptr;
}

RuntimeExecutor normalizedExecutor(
    RuntimeExecutor executor)
{
    if (executor) {
        return executor;
    }
    return [](std::function<void()> worker) {
        QThreadPool::globalInstance()->start(
            std::move(worker));
    };
}

RuntimeNowProvider normalizedNowProvider(
    RuntimeNowProvider nowProvider)
{
    if (nowProvider) {
        return nowProvider;
    }
    return [] {
        return QDateTime::currentDateTimeUtc();
    };
}

template <typename T>
CommitAwareMutationHandle<T>
readyMutationFailure(CompanionError error)
{
    const auto mutation =
        CommitAwareMutation<T>::create();
    CommitAwareMutationHandle<T> handle =
        mutation->handle();
    mutation->finish(
        Result<T>::failure(
            std::move(error)));
    return handle;
}

CommitAwareMutationHandle<QString>
startTaskCreateMutation(
    const std::shared_ptr<AttachmentStore>&
        attachmentStore,
    const RuntimeTaskCreatePerformer&
        taskCreatePerformer,
    const RuntimeExecutor& executor,
    RuntimeTaskCreateRequest request)
{
    const auto mutation =
        CommitAwareMutation<QString>::create();
    CommitAwareMutationHandle<QString> handle =
        mutation->handle();
    try {
        executor(
            [attachmentStore,
             taskCreatePerformer,
             mutation,
             request =
                 std::move(request)]() mutable {
                try {
                    const QUuid requestId(
                        request.clientMessageId);
                    if (requestId.isNull()) {
                        mutation->finish(
                            Result<QString>::failure(
                                taskCreateFailure()));
                        return;
                    }
                    Result<StagedAttachmentBatch>
                        staged =
                            attachmentStore
                                ->stageOwned(
                                    request
                                        .attachments,
                                    requestId);
                    if (!staged.hasValue()) {
                        mutation->finish(
                            Result<QString>::failure(
                                taskCreateFailure()));
                        return;
                    }
                    StagedAttachmentBatch batch =
                        std::move(staged.value());
                    if (batch.cleanupLease
                        == nullptr) {
                        mutation->finish(
                            Result<QString>::failure(
                                taskCreateFailure()));
                        return;
                    }
                    if (!mutation->tryCommit()) {
                        return;
                    }
                    batch.cleanupLease
                        ->retainForCommittedUse();

                    CreateTaskRequest create;
                    create.prompt =
                        std::move(request.text);
                    create.cwd =
                        std::move(request.cwd);
                    create.model =
                        std::move(request.model);
                    create.reasoningEffort =
                        std::move(
                            request.reasoningEffort);
                    create.skillName =
                        std::move(
                            request.skillName);
                    create.skillPath =
                        std::move(
                            request.skillPath);
                    create.attachments =
                        std::move(
                            batch.attachments);
                    create.clientMessageId =
                        std::move(
                            request.clientMessageId);
                    mutation->finish(
                        taskCreatePerformer(
                            create));
                } catch (...) {
                    mutation->finish(
                        Result<QString>::failure(
                            taskCreateFailure()));
                }
            });
    } catch (...) {
        mutation->finish(
            Result<QString>::failure(
                taskCreateFailure()));
    }
    return handle;
}

CommitAwareMutationHandle<ChatResult>
startChatMutation(
    const std::shared_ptr<ChatService>&
        chatService,
    const RuntimeExecutor& executor,
    ChatRequest request)
{
    const auto mutation =
        CommitAwareMutation<ChatResult>::create();
    CommitAwareMutationHandle<ChatResult> handle =
        mutation->handle();
    if (!mutation->tryCommit()) {
        return handle;
    }

    QFuture<Result<ChatResult>> future;
    try {
        future = chatService->send(request);
    } catch (...) {
        mutation->finish(
            Result<ChatResult>::failure(
                chatFailure()));
        return handle;
    }
    if (!future.isValid()) {
        mutation->finish(
            Result<ChatResult>::failure(
                chatFailure()));
        return handle;
    }

    try {
        executor(
            [future = std::move(future),
             mutation]() mutable {
                try {
                    future.waitForFinished();
                    if (!future.isCanceled()
                        && future.resultCount() == 1) {
                        mutation->finish(
                            future.result());
                        return;
                    }
                } catch (...) {
                }
                mutation->finish(
                    Result<ChatResult>::failure(
                        chatFailure()));
            });
    } catch (...) {
        mutation->finish(
            Result<ChatResult>::failure(
                chatFailure()));
    }
    return handle;
}

CommitAwareMutationHandle<BridgeGoal>
startGoalMutation(
    const std::shared_ptr<GoalService>&
        goalService,
    RuntimeGoalMutationRequest request)
{
    switch (request.kind) {
    case RuntimeGoalMutationKind::Create:
        if (!request.objective.has_value()) {
            return readyMutationFailure<
                BridgeGoal>(goalFailure());
        }
        return goalService->createMutation(
            request.threadId,
            *request.objective,
            request.tokenBudget);
    case RuntimeGoalMutationKind::Update:
        if (!request.objective.has_value()) {
            return readyMutationFailure<
                BridgeGoal>(goalFailure());
        }
        return goalService->updateMutation({
            request.threadId,
            *request.objective,
            request.tokenBudget,
        });
    case RuntimeGoalMutationKind::Pause:
        return goalService->pauseMutation(
            request.threadId);
    case RuntimeGoalMutationKind::Resume:
        return goalService->resumeMutation(
            request.threadId);
    }
    return readyMutationFailure<BridgeGoal>(
        goalFailure());
}

} // namespace

Result<CodexRuntimeProductionBundle>
CodexRuntimeDependencyFactory::build(
    const CodexEnvironment& environment,
    CodexRuntimeProductionServices services,
    RuntimeExecutor executor,
    RuntimeNowProvider nowProvider)
{
    if (!validServices(services)) {
        return Result<
            CodexRuntimeProductionBundle>::failure(
            runtimeUnavailableError());
    }

    try {
        RuntimeExecutor workerExecutor =
            normalizedExecutor(
                std::move(executor));
        RuntimeNowProvider clock =
            normalizedNowProvider(
                std::move(nowProvider));
        const auto taskLoader =
            std::make_shared<TaskSnapshotLoader>(
                environment,
                clock);
        const auto historyLoader =
            std::make_shared<
                HistorySnapshotLoader>(
                environment);
        const auto historyCoordinator =
            std::make_shared<
                HistoryCoordinator>();
        const auto capabilityService =
            std::make_shared<CapabilityService>(
                environment,
                [credentialStore =
                     services.credentialStore,
                 backend =
                     services.onDeviceBackend] {
                    ChatCatalogAvailability
                        availability;
                    if (backend != nullptr) {
                        try {
                            const auto status =
                                backend->status();
                            availability
                                .onDeviceAvailable =
                                status.available;
                            availability
                                .onDeviceSupportsAttachments =
                                status
                                    .supportsAttachments;
                        } catch (...) {
                        }
                    }
                    try {
                        availability
                            .hasOpenAIKey =
                            ChatService::
                                hasUsableCredential(
                                    *credentialStore,
                                    ChatProvider::
                                        OpenAIAPI);
                        availability.hasLumoKey =
                            ChatService::
                                hasUsableCredential(
                                    *credentialStore,
                                    ChatProvider::
                                        LumoAPI);
                    } catch (...) {
                    }
                    return availability;
                });
        const auto chatService =
            std::make_shared<ChatService>(
                services.credentialStore,
                services.openAITransport,
                services.lumoTransport,
                services.onDeviceBackend
                    ? OnDeviceChatSender(
                          [backend =
                               services
                                   .onDeviceBackend](
                              const ChatRequest&
                                  request) {
                              return backend->send(
                                  request);
                          })
                    : OnDeviceChatSender());
        RuntimeTaskCreatePerformer
            taskCreatePerformer =
                std::move(
                    services
                        .taskCreatePerformer);
        if (!taskCreatePerformer) {
            taskCreatePerformer =
                [taskCreator =
                     services.taskCreator](
                    const CreateTaskRequest&
                        request) {
                    return taskCreator->create(
                        request);
                };
        }

        CodexRuntimeDependencies dependencies;
        dependencies.taskLoader =
            [taskLoader](
                const QHash<QString, BridgeGoal>&
                    cachedGoals,
                std::stop_token stopToken) {
                return taskLoader->load(
                    cachedGoals,
                    stopToken);
            };
        dependencies.goalLoader =
            [goalService =
                 services.goalService](
                const QVector<QString>& threadIds,
                std::stop_token stopToken) {
                return goalService->readSync(
                    threadIds,
                    stopToken);
            };
        dependencies.executor = workerExecutor;
        dependencies.nowProvider = clock;
        dependencies.history =
            CodexRuntimeHistoryDependencies{
                [historyLoader](
                    const HistoryKey& key,
                    const QSet<QString>&
                        pendingApprovals,
                    const QDateTime& now,
                    std::stop_token stopToken) {
                    return historyLoader->load(
                        key,
                        pendingApprovals,
                        now,
                        stopToken);
                },
                historyCoordinator,
            };
        dependencies.reads =
            CodexRuntimeReadDependencies{
                [capabilityService](
                    const QString& cwd,
                    std::stop_token stopToken) {
                    return capabilityService
                        ->load(
                            cwd,
                            stopToken);
                },
                [usageService =
                     services.usageService] {
                    return usageService->read();
                },
            };
        dependencies.mutations =
            CodexRuntimeMutationDependencies{
                [taskCommandService =
                     services.taskCommandService](
                    SendRequest request) {
                    return taskCommandService
                        ->sendMutation(request);
                },
                [approvalService =
                     services.approvalService](
                    PendingApproval request,
                    ApprovalDecision decision) {
                    return approvalService
                        ->respondMutation(
                            request,
                            decision);
                },
                [attachmentStore =
                     services.attachmentStore,
                 taskCreatePerformer =
                     std::move(
                         taskCreatePerformer),
                 workerExecutor](
                    RuntimeTaskCreateRequest
                        request) {
                    return startTaskCreateMutation(
                        attachmentStore,
                        taskCreatePerformer,
                        workerExecutor,
                        std::move(request));
                },
                [chatService,
                 workerExecutor](
                    ChatRequest request) {
                    return startChatMutation(
                        chatService,
                        workerExecutor,
                        std::move(request));
                },
                [goalService =
                     services.goalService](
                    RuntimeGoalMutationRequest
                        request) {
                    return startGoalMutation(
                        goalService,
                        std::move(request));
                },
                [usageService =
                     services.usageService](
                    QString creditId,
                    QUuid idempotencyKey) {
                    return usageService
                        ->consumeResetMutation(
                            creditId,
                            idempotencyKey);
                },
            };

        return Result<
            CodexRuntimeProductionBundle>::success({
            std::move(dependencies),
            std::move(
                services.onDeviceBackend),
        });
    } catch (...) {
        return Result<
            CodexRuntimeProductionBundle>::failure(
            runtimeUnavailableError());
    }
}

Result<std::unique_ptr<CodexRuntimeLifetime>>
CodexRuntimeDependencyFactory::create(
    const CodexEnvironment& environment,
    CodexRuntimeProductionServices services,
    RuntimeExecutor executor,
    RuntimeNowProvider nowProvider,
    CodexRuntimeCadence cadence)
{
    Result<CodexRuntimeProductionBundle> built =
        build(
            environment,
            std::move(services),
            std::move(executor),
            std::move(nowProvider));
    if (!built.hasValue()) {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::failure(
            built.error());
    }
    CodexRuntimeProductionBundle bundle =
        std::move(built.value());
    return CodexRuntimeLifetime::
        createProduction(
            std::move(bundle.dependencies),
            std::move(
                bundle.onDeviceBackend),
            cadence);
}

} // namespace companion
