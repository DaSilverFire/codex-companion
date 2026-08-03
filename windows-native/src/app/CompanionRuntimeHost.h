#pragma once

#include "codex/chat/ChatService.h"
#include "codex/chat/WindowsOnDeviceChatBackend.h"
#include "core/Result.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <memory>
#include <stop_token>

namespace companion {

class CompanionCommandBus;
class CompanionMobileHost;
class CompanionShellViewModel;
class CompanionState;
class CredentialStore;
class CodexAccountProfileStore;
class CodexAccountRouter;
class CodexAutomaticAccountContinuationCoordinator;
class CodexAutomaticContinuationJournal;
class CodexFailedTaskRetryJournal;
class CodexFailedTaskRetryService;
class CodexRuntime;
class CodexThreadAccountBindingStore;
class MobileRelayUrlState;
class MobilePresencePetCatalogService;
class PairingCoordinator;
class ProcessListModel;
class RelayPairingBootstrap;
class RuntimeContinuationHost;
class TaskListModel;
struct AppSettings;
struct CodexAutomaticContinuationOutcome;
struct BridgeGoal;
enum class UsageResetOutcome;

namespace detail {
class CompanionRuntimeHostTestAccess;
class RuntimeHostStatusDispatcher;
}

class CompanionRuntimeHost final : public QObject {
    Q_OBJECT

public:
    using ChatRequestSender =
        std::function<QFuture<
            Result<ChatResult>>(
            const ChatRequest&)>;

    static Result<std::unique_ptr<CompanionRuntimeHost>>
    createProduction(
        CompanionShellViewModel& shellViewModel,
        const AppSettings& settings,
        std::shared_ptr<CredentialStore>
            credentialStore,
        QObject* parent = nullptr,
        CodexAccountRouter*
            accountRouter = nullptr,
        CodexAccountProfileStore*
            accountProfileStore = nullptr,
        CodexThreadAccountBindingStore*
            accountBindingStore = nullptr,
        std::shared_ptr<
            MobilePresencePetCatalogService>
            presencePetCatalogService = {});

    ~CompanionRuntimeHost() override;

    Result<void> start();
    void setProcessSurfaceVisible(bool visible);
    Result<void> applyMobileSettings(
        const AppSettings& settings);
    void setAutomaticAccountContinuationEnabled(
        bool enabled);
    TaskListModel* taskModel() const noexcept;
    ProcessListModel*
    processModel() const noexcept;
    PairingCoordinator*
    mobilePairingCoordinator() noexcept;
    RelayPairingBootstrap*
    mobileRelayPairingBootstrap()
        noexcept;
    bool mobileNearbyAccessAvailable()
        const noexcept;
    QString mobileNearbyAccessStatusText()
        const;

public slots:
    void refreshChatAvailability();

signals:
    void runtimeErrorOccurred(CompanionError error);
    void petAnimationRequested(QString animation);
    void mobileNearbyAccessChanged(
        bool available,
        QString statusText);

private:
    CompanionRuntimeHost(
        CompanionShellViewModel& shellViewModel,
        std::unique_ptr<CompanionState> state,
        std::unique_ptr<CompanionCommandBus> commandBus,
        std::unique_ptr<CodexRuntime> runtime,
        std::shared_ptr<CredentialStore> credentialStore,
        std::shared_ptr<WindowsOnDeviceChatBackend> onDeviceBackend,
        ChatRequestSender chatSender,
        QObject* parent);
    CompanionRuntimeHost(
        CompanionShellViewModel& shellViewModel,
        std::unique_ptr<CompanionState> state,
        std::unique_ptr<CompanionCommandBus>
            commandBus,
        std::unique_ptr<CodexRuntime> runtime,
        QString sidebarStatePath,
        std::shared_ptr<
            RuntimeContinuationHost>
            continuationHost,
        std::shared_ptr<CredentialStore>
            credentialStore,
        std::shared_ptr<
            WindowsOnDeviceChatBackend>
            onDeviceBackend,
        ChatRequestSender chatSender,
        std::unique_ptr<
            CompanionMobileHost> mobileHost,
        std::shared_ptr<
            MobileRelayUrlState>
            mobileRelayUrlState,
        std::optional<CompanionError>
            mobileStartupError,
        QObject* parent);

    struct PendingProcessExecution;

    void refreshProcessStatus();
    void refreshProcessModel();
    void publishMobileNearbyAccess();
    void refreshUsageStatus();
    void requestUsage();
    void requestUsageReset(
        QString creditId,
        QString idempotencyKey);
    void sendProcessMessage(
        QString action,
        QString threadId,
        QString prompt,
        QString cwd,
        QString activeTurnId,
        QString model,
        QString reasoningEffort);
    void respondToProcessApproval(
        QString threadId,
        QString decision);
    void retryFailedProcess(
        QVariantMap process);
    void scheduleAutomaticAccountContinuations();
    void finishAutomaticAccountContinuations(
        QVector<
            CodexAutomaticContinuationOutcome>
            outcomes);
    void cancelPendingProcessCommand();
    bool executePendingProcessCommand(
        PendingProcessExecution& pending,
        QVariantMap arguments);
    static void clearPendingProcessCommand(
        PendingProcessExecution& pending);
    void handleProcessCommandFinished(
        const QString& command,
        quint64 executionId,
        bool succeeded,
        const QString& errorCode,
        const QString& message);
    void prepareOnDeviceChat();
    void sendLocalChat(QString prompt, QString modelId);
    void completeChatRequest();
    void completeOnDevicePreparation();
    void handleOnDeviceStatus(WindowsOnDeviceChatStatus status);
    void updateGoal(QString threadId, QString objective);
    void pauseGoal(QString threadId);
    void resumeGoal(QString threadId);
    void executeGoalMutation(
        QString command,
        QString threadId,
        QVariantMap arguments);
    void handleGoalCommandFinished(
        const QString& command,
        bool succeeded,
        const QString& errorCode,
        const QString& message);
    void handleUsageCommandFinished(
        const QString& command,
        bool succeeded,
        const QString& errorCode,
        const QString& message);
    void handleUsageResetFinished(
        UsageResetOutcome outcome);
    void handleGoalChanged(const BridgeGoal& goal);
    void refreshOpenGoalControls();
    ChatRequest chatRequest(QString prompt, const QString& modelId) const;
    void reportRuntimeError(const CompanionError& error);

    enum class PendingProcessCommand {
        None,
        Message,
        Approval,
        ApprovalFeedbackDecline,
        ApprovalFeedbackReply,
    };

    struct PendingProcessExecution {
        PendingProcessCommand kind =
            PendingProcessCommand::None;
        QString command;
        QVariantMap arguments;
        quint64 executionId = 0;
        QString operationKey;
        QString processId;

        bool active() const noexcept
        {
            return kind
                != PendingProcessCommand::None;
        }
    };

    CompanionShellViewModel& shellViewModel_;
    std::unique_ptr<CompanionState> state_;
    std::unique_ptr<CompanionCommandBus> commandBus_;
    std::unique_ptr<CodexRuntime> runtime_;
    std::unique_ptr<ProcessListModel>
        processListModel_;
    std::shared_ptr<
        RuntimeContinuationHost>
        continuationHost_;
    std::unique_ptr<
        CodexFailedTaskRetryJournal>
        failedTaskRetryJournal_;
    std::unique_ptr<
        CodexAutomaticContinuationJournal>
        automaticContinuationJournal_;
    std::unique_ptr<
        CodexFailedTaskRetryService>
        failedTaskRetryService_;
    std::unique_ptr<
        CodexAutomaticAccountContinuationCoordinator>
        automaticContinuationCoordinator_;
    std::shared_ptr<CredentialStore> credentialStore_;
    std::shared_ptr<WindowsOnDeviceChatBackend> onDeviceBackend_;
    std::shared_ptr<WindowsOnDeviceChatStatusSubscription>
        onDeviceStatusSubscription_;
    std::shared_ptr<
        detail::RuntimeHostStatusDispatcher>
        onDeviceStatusDispatcher_;
    ChatRequestSender chatSender_;
    std::unique_ptr<ChatService> chatService_;
    std::unique_ptr<
        CompanionMobileHost> mobileHost_;
    std::shared_ptr<
        MobileRelayUrlState>
        mobileRelayUrlState_;
    std::optional<CompanionError>
        mobileStartupError_;
    QFutureWatcher<Result<ChatResult>> chatWatcher_;
    QString pendingChatPrompt_;
    QString pendingChatTitle_;
    QString pendingChatModelId_;
    QFutureWatcher<Result<void>> preparationWatcher_;
    PendingProcessExecution
        pendingProcessMessage_;
    PendingProcessExecution
        pendingProcessApproval_;
    QString pendingGoalCommand_;
    QString pendingGoalThreadId_;
    bool automaticAccountContinuationEnabled_ =
        false;
    bool automaticAccountContinuationPending_ =
        false;
    std::stop_source
        automaticContinuationStopSource_;
    bool started_ = false;

    friend class detail::
        CompanionRuntimeHostTestAccess;
};

namespace detail {

class CompanionRuntimeHostTestAccess final {
public:
    static std::unique_ptr<CompanionRuntimeHost>
    create(
        CompanionShellViewModel& shellViewModel,
        std::unique_ptr<CompanionState> state,
        std::unique_ptr<CompanionCommandBus>
            commandBus,
        std::unique_ptr<CodexRuntime> runtime,
        std::shared_ptr<CredentialStore>
            credentialStore,
        std::shared_ptr<
            WindowsOnDeviceChatBackend>
            onDeviceBackend,
        CompanionRuntimeHost::
            ChatRequestSender chatSender,
        QObject* parent = nullptr);
};

} // namespace detail

} // namespace companion
