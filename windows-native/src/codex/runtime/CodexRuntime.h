#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/BridgeModels.h"
#include "codex/runtime/CodexRuntimeCommandState.h"
#include "codex/runtime/CodexRuntimeOperations.h"
#include "codex/runtime/CodexProcessSnapshot.h"
#include "codex/state/HistoryCoordinator.h"
#include "core/Result.h"

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class QTimer;

namespace companion {

class CompanionCommandBus;
class CompanionState;
class CodexRuntimeOperationRegistry;
class CodexRuntimeOperationState;
class RuntimeContinuationHost;
struct CodexRuntimeHistoryEntry;
struct CodexRuntimeCapabilityEntry;
struct CodexRuntimeCapabilityStopLink;
struct CodexRuntimeUsageEntry;

namespace detail {
struct CompanionCommandBusDeliveryState;
struct CompanionStateAccessState;
struct CodexRuntimeTestAccess;
}

struct CodexRuntimeCadence final {
    int activeRefreshMilliseconds = 30'000;
    int passiveRefreshMilliseconds = 20'000;
    int settleRefreshMilliseconds = 140;
    int taskStaleMilliseconds = 8'000;
    int goalStaleMilliseconds = 60'000;
};

using RuntimeTaskLoader =
    std::function<Result<CodexProcessSnapshot>(
        const QHash<QString, BridgeGoal>&,
        std::stop_token)>;
using RuntimeGoalLoader =
    std::function<Result<
        QHash<QString, std::optional<BridgeGoal>>>(
        const QVector<QString>&,
        std::stop_token)>;
using RuntimeExecutor =
    std::function<void(std::function<void()>)>;
using RuntimeNowProvider = std::function<QDateTime()>;
using RuntimeHistoryLoader =
    std::function<Result<HistorySnapshot>(
        const HistoryKey&,
        const QSet<QString>&,
        const QDateTime&,
        std::stop_token)>;
using RuntimeCapabilityLoader =
    std::function<Result<BridgeCapabilities>(
        const QString&,
        std::stop_token)>;
using RuntimeUsageReadStarter =
    std::function<
        QFuture<Result<BridgeUsageSnapshot>>()>;

struct CodexRuntimeHistoryDependencies final {
    RuntimeHistoryLoader historyLoader;
    std::shared_ptr<HistoryCoordinator>
        historyCoordinator;
};

struct CodexRuntimeReadDependencies final {
    RuntimeCapabilityLoader capabilityLoader;
    RuntimeUsageReadStarter usageReadStarter;
};

struct CodexRuntimeDependencies final {
    RuntimeTaskLoader taskLoader;
    RuntimeGoalLoader goalLoader;
    RuntimeExecutor executor;
    RuntimeNowProvider nowProvider;
    std::optional<
        CodexRuntimeHistoryDependencies>
        history;
    std::optional<CodexRuntimeReadDependencies>
        reads;
    std::optional<
        CodexRuntimeMutationDependencies>
        mutations;
};

class CodexRuntime final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        bool running
        READ running
        NOTIFY runningChanged)
    Q_PROPERTY(
        bool loading
        READ loading
        NOTIFY loadingChanged)
    Q_PROPERTY(
        QString errorCode
        READ errorCode
        NOTIFY statusChanged)
    Q_PROPERTY(
        QString errorMessage
        READ errorMessage
        NOTIFY statusChanged)
    Q_PROPERTY(
        bool historyLoading
        READ historyLoading
        NOTIFY historyChanged)
    Q_PROPERTY(
        QString historyErrorCode
        READ historyErrorCode
        NOTIFY historyChanged)
    Q_PROPERTY(
        QString historyErrorMessage
        READ historyErrorMessage
        NOTIFY historyChanged)
    Q_PROPERTY(
        QString selectedHistoryThreadId
        READ selectedHistoryThreadId
        NOTIFY historyChanged)
    Q_PROPERTY(
        bool capabilitiesLoading
        READ capabilitiesLoading
        NOTIFY capabilitiesChanged)
    Q_PROPERTY(
        QString capabilitiesErrorCode
        READ capabilitiesErrorCode
        NOTIFY capabilitiesChanged)
    Q_PROPERTY(
        QString capabilitiesErrorMessage
        READ capabilitiesErrorMessage
        NOTIFY capabilitiesChanged)
    Q_PROPERTY(
        bool usageLoading
        READ usageLoading
        NOTIFY usageChanged)
    Q_PROPERTY(
        QString usageErrorCode
        READ usageErrorCode
        NOTIFY usageChanged)
    Q_PROPERTY(
        QString usageErrorMessage
        READ usageErrorMessage
        NOTIFY usageChanged)

public:
    CodexRuntime(
        CompanionState& state,
        CompanionCommandBus& commandBus,
        const CodexEnvironment& environment,
        QObject* parent = nullptr);

    CodexRuntime(
        CompanionState& state,
        CompanionCommandBus& commandBus,
        CodexRuntimeDependencies dependencies,
        std::shared_ptr<
            RuntimeContinuationHost>
            continuationHost,
        CodexRuntimeCadence cadence = {},
        QObject* parent = nullptr);

    CodexRuntime(
        CompanionState& state,
        CompanionCommandBus& commandBus,
        CodexRuntimeDependencies dependencies,
        std::shared_ptr<
            RuntimeContinuationHost>
            continuationHost,
        CodexRuntimeMode mode,
        CodexRuntimeCadence cadence = {},
        QObject* parent = nullptr);

    CodexRuntime(
        CompanionState& state,
        CompanionCommandBus& commandBus,
        RuntimeTaskLoader taskLoader,
        RuntimeGoalLoader goalLoader,
        RuntimeExecutor executor,
        RuntimeNowProvider nowProvider,
        CodexRuntimeCadence cadence = {},
        QObject* parent = nullptr);

    ~CodexRuntime() override;

    Result<void> start();
    void stop();
    Q_INVOKABLE void refreshNow();
    Q_INVOKABLE void setProcessSurfaceVisible(
        bool visible);
    Q_INVOKABLE void requestOperationStop(
        QString operationKey);
    Q_INVOKABLE void
    invalidateChatCapabilities();

    bool running() const noexcept;
    bool loading() const noexcept;
    QString errorCode() const;
    QString errorMessage() const;
    const CodexProcessSnapshot&
    processSnapshot() const noexcept;
    bool historyLoading() const noexcept;
    QString historyErrorCode() const;
    QString historyErrorMessage() const;
    QString selectedHistoryThreadId() const;
    const std::optional<
        CodexHistoryPublication>&
    historyPublication() const noexcept;
    bool capabilitiesLoading() const noexcept;
    QString capabilitiesErrorCode() const;
    QString capabilitiesErrorMessage() const;
    const std::optional<BridgeCapabilities>&
    capabilities() const noexcept;
    bool usageLoading() const noexcept;
    QString usageErrorCode() const;
    QString usageErrorMessage() const;
    const std::optional<BridgeUsageSnapshot>&
    usageSnapshot() const noexcept;
    bool chatCapabilitiesValid() const noexcept;
    quint64 capabilityRevision() const noexcept;

signals:
    void runningChanged();
    void loadingChanged();
    void statusChanged();
    void processSnapshotChanged();
    void historyChanged();
    void capabilitiesChanged();
    void usageChanged();
    void taskCreated(const QString& threadId);
    void chatMessageReceived(
        const BridgeMessage& message);
    void goalChanged(const BridgeGoal& goal);
    void usageResetFinished(
        UsageResetOutcome outcome);

private:
    using GoalResultMap =
        QHash<QString, std::optional<BridgeGoal>>;

    enum class CollaboratorStatus {
        Ready,
        Unavailable,
        ThreadMismatch,
    };

    class TransitionGuard final {
    public:
        explicit TransitionGuard(
            CodexRuntime& runtime);
        ~TransitionGuard();

        TransitionGuard(
            const TransitionGuard&) = delete;
        TransitionGuard& operator=(
            const TransitionGuard&) = delete;

    private:
        CodexRuntime& runtime_;
    };

    CodexRuntime(
        CompanionState& state,
        CompanionCommandBus& commandBus,
        CodexRuntimeDependencies dependencies,
        std::shared_ptr<
            RuntimeContinuationHost>
            continuationHost,
        CodexRuntimeCadence cadence,
        bool historyCommandsEnabled,
        bool readCommandsEnabled,
        CodexRuntimeMode mode,
        QObject* parent);

    Result<void> startOnOwnerThread();
    Result<void> startTaskRefresh(bool startup);
    Result<void> startGoalRefresh();
    void applyTaskResult(
        std::uint64_t generation,
        Result<CodexProcessSnapshot> result);
    void applyGoalResult(
        std::uint64_t generation,
        std::uint64_t goalRevision,
        Result<GoalResultMap> result);
    void evaluateGoalStaleness();
    void evaluateGoalStaleness(
        const QDateTime& now);
    Result<void> bindRuntimeCommandGroup();
    void startPassiveTimer();
    void startActiveTimer();
    void scheduleSettleTimer();
    void stopTimers();
    void stopActiveAndSettleTimers();
    void handlePassiveTimer();
    void handleActiveTimer();
    void handleSettleTimer();
    void stopOnOwnerThread();
    Result<void> requestRefreshOnOwnerThread();
    Result<void> refreshNowOnOwnerThread();
    void setProcessSurfaceVisibleOnOwnerThread(
        bool visible);
    void beginTransition();
    void endTransition();
    void drainDeferredOwnerActions();
    void requestActiveStops() noexcept;
    void requestHistoryStops() noexcept;
    void stopHistoryOperations();
    CollaboratorStatus
    collaboratorStatus() const noexcept;
    static CompanionError collaboratorError(
        CollaboratorStatus status);
    void stopForRuntimeFailure(
        const CompanionError& error);
    void publishTaskRefreshStarted();
    void publishStoppedFailure(
        const CompanionError& error);
    void setLoading(bool loading);
    void setError(const CompanionError& error);
    void clearError();

    static void postTaskResult(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>& deliveryState,
        std::uint64_t generation,
        Result<CodexProcessSnapshot> result);
    static void postGoalResult(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>& deliveryState,
        std::uint64_t generation,
        std::uint64_t goalRevision,
        Result<GoalResultMap> result);
    static void dispatchRefreshCommand(
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    static void dispatchHistoryCommand(
        QVariantMap arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    void requestHistoryOnOwnerThread(
        const QVariantMap& arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    void applyHistoryResult(
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeHistoryEntry>& entry);
    static void postHistoryResult(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>& deliveryState,
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeHistoryEntry>& entry);
    static void dispatchCapabilitiesCommand(
        QVariantMap arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    static void dispatchUsageCommand(
        QVariantMap arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    static void dispatchMutationCommand(
        QString command,
        QVariantMap arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    void requestCapabilitiesOnOwnerThread(
        const QVariantMap& arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    void requestUsageOnOwnerThread(
        const QVariantMap& arguments,
        const std::shared_ptr<
            CodexRuntimeCommandInvocationState>&
            invocation);
    void enqueueCapabilityRequest(
        QString cwd,
        std::shared_ptr<
            CodexRuntimeOperationState> operation,
        std::shared_ptr<
            CodexRuntimeCapabilityStopLink>
            stopLink,
        bool forceFresh);
    void startCapabilityEntry(
        const std::shared_ptr<
            CodexRuntimeCapabilityEntry>& entry);
    void applyCapabilityResult(
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeCapabilityEntry>& entry);
    static void postCapabilityResult(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeCapabilityEntry>& entry);
    void startUsageEntry(
        const std::shared_ptr<
            CodexRuntimeUsageEntry>& entry);
    void applyUsageResult(
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeUsageEntry>& entry);
    static void postUsageResult(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        const std::shared_ptr<
            CodexRuntimeUsageEntry>& entry);
    static void postTaskMutationSuccess(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration);
    static void postTaskCreated(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        QString threadId);
    static void postChatMessageSuccess(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        ChatResult result);
    static void postGoalMutationSuccess(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        BridgeGoal goal);
    static void postUsageResetSuccess(
        const std::weak_ptr<
            CodexRuntimeDeliveryState>&
            deliveryState,
        std::uint64_t runtimeGeneration,
        UsageResetOutcome outcome);
    void scheduleUsageRefreshAfterMutation();
    void invalidateChatCapabilitiesOnOwnerThread();
    void scheduleInvalidatedCapabilityReload();
    void requestReadStops() noexcept;
    void stopReadOperations();

    friend struct detail::CodexRuntimeTestAccess;

    QPointer<CompanionState> state_;
    std::shared_ptr<
        detail::CompanionStateAccessState>
        stateAccessState_;
    std::shared_ptr<
        detail::CompanionCommandBusDeliveryState>
        commandBusDeliveryState_;
    std::shared_ptr<RuntimeContinuationHost>
        continuationHost_;
    std::shared_ptr<
        CodexRuntimeOperationRegistry>
        operationRegistry_;
    RuntimeTaskLoader taskLoader_;
    RuntimeGoalLoader goalLoader_;
    RuntimeExecutor executor_;
    RuntimeNowProvider nowProvider_;
    RuntimeHistoryLoader historyLoader_;
    std::shared_ptr<HistoryCoordinator>
        historyCoordinator_;
    RuntimeCapabilityLoader capabilityLoader_;
    RuntimeUsageReadStarter usageReadStarter_;
    RuntimeSendMutationStarter
        sendMutationStarter_;
    RuntimeApprovalMutationStarter
        approvalMutationStarter_;
    RuntimeTaskCreateMutationStarter
        taskCreateMutationStarter_;
    RuntimeChatMutationStarter
        chatMutationStarter_;
    RuntimeGoalMutationStarter
        goalMutationStarter_;
    RuntimeUsageResetMutationStarter
        usageResetMutationStarter_;
    CodexRuntimeCadence cadence_;
    CodexRuntimeMode mode_ =
        CodexRuntimeMode::Interactive;
    std::shared_ptr<CodexRuntimeDeliveryState>
        deliveryState_;
    CodexProcessSnapshot processSnapshot_;
    QHash<QString, BridgeGoal> cachedGoals_;
    std::optional<std::stop_source> taskStopSource_;
    std::optional<std::stop_source> goalStopSource_;
    std::optional<QDateTime> lastTaskRefreshFinishedAt_;
    std::optional<QDateTime> lastGoalRefreshFinishedAt_;
    QTimer* passiveTimer_ = nullptr;
    QTimer* activeTimer_ = nullptr;
    QTimer* settleTimer_ = nullptr;
    std::uint64_t generation_ = 0;
    std::uint64_t historySelectionGeneration_ = 0;
    std::uint64_t nextOperationGeneration_ = 0;
    std::uint64_t nextReadEntryGeneration_ = 0;
    std::uint64_t capabilityRevision_ = 0;
    std::uint64_t goalRevision_ = 0;
    bool running_ = false;
    bool loading_ = false;
    bool taskRefreshActive_ = false;
    bool goalRefreshActive_ = false;
    bool taskFollowUpRequested_ = false;
    bool processSurfaceVisible_ = false;
    int transitionDepth_ = 0;
    bool drainingDeferredActions_ = false;
    bool deferredStop_ = false;
    bool deferredRefresh_ = false;
    std::optional<bool>
        deferredProcessSurfaceVisible_;
    bool destroying_ = false;
    bool historyCommandsEnabled_ = false;
    bool readCommandsEnabled_ = false;
    bool mutationCommandsEnabled_ = false;
    bool historyLoading_ = false;
    QString errorCode_;
    QString errorMessage_;
    QString historyErrorCode_;
    QString historyErrorMessage_;
    QString selectedHistoryThreadId_;
    std::optional<CodexHistoryPublication>
        historyPublication_;
    QHash<
        HistoryKey,
        std::shared_ptr<
            CodexRuntimeHistoryEntry>>
        historyEntries_;
    bool capabilitiesLoading_ = false;
    QString capabilitiesErrorCode_;
    QString capabilitiesErrorMessage_;
    std::optional<BridgeCapabilities>
        capabilities_;
    std::optional<QString>
        latestCapabilityCwd_;
    std::optional<QString>
        publishedCapabilityCwd_;
    std::optional<std::uint64_t>
        publishedCapabilityRevision_;
    bool chatCapabilityValid_ = false;
    std::shared_ptr<
        CodexRuntimeCapabilityEntry>
        activeCapabilityEntry_;
    std::shared_ptr<
        CodexRuntimeCapabilityEntry>
        queuedCapabilityEntry_;
    bool usageLoading_ = false;
    QString usageErrorCode_;
    QString usageErrorMessage_;
    std::optional<BridgeUsageSnapshot>
        usageSnapshot_;
    std::shared_ptr<CodexRuntimeUsageEntry>
        activeUsageEntry_;
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        queuedUsageWaiters_;
    bool usageFollowUpRequested_ = false;
};

} // namespace companion
