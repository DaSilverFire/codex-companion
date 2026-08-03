#include "app/CompanionRuntimeHost.h"

#include "app/ChatCredentialAvailability.h"
#include "app/CompanionMobileHost.h"
#include "app/CompanionMobileRuntimeAdapter.h"
#include "app/RuntimeHostStatusDispatcher.h"
#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/accounts/ProfiledCodexControlService.h"
#include "codex/accounts/ProfiledTaskCreator.h"
#include "codex/appserver/TaskCreator.h"
#include "codex/attachments/AttachmentStore.h"
#include "codex/commands/ApprovalService.h"
#include "codex/commands/GoalService.h"
#include "codex/commands/TaskCommandService.h"
#include "codex/commands/UsageService.h"
#include "codex/continuation/CodexAutomaticAccountContinuationCoordinator.h"
#include "codex/continuation/CodexAutomaticContinuationJournal.h"
#include "codex/continuation/CodexContinuationTransport.h"
#include "codex/continuation/CodexFailedTaskRetryService.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/runtime/CodexRuntime.h"
#include "codex/runtime/CodexRuntimeDependencyFactory.h"
#include "codex/runtime/CodexRuntimeOperations.h"
#include "codex/runtime/ProcessListModel.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "core/AppSettings.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionInstallationIdentityStore.h"
#include "core/CompanionState.h"
#include "mobile/MobileRequestDispatcher.h"
#include "mobile/nearby/NearbyTransferAssembler.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"
#include "mobile/relay/RelaySettings.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/DpapiCredentialStore.h"
#include "platform/windows/mobile/WindowsTlsIdentityStore.h"
#include "ui/CompanionShellViewModel.h"

#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSysInfo>
#include <QUuid>

#include <cmath>
#include <utility>

namespace {

using companion::CompanionError;
using companion::GoalStatus;
using companion::WindowsOnDeviceChatPhase;
using companion::WindowsOnDeviceChatStatus;

constexpr double kSwiftReferenceDateUnixSeconds =
    978307200.0;

CompanionError runtimeHostError(QString code, QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

QString onDeviceStatusMessage(const WindowsOnDeviceChatStatus& status)
{
    switch (status.phase) {
    case WindowsOnDeviceChatPhase::ConsentRequired:
        return QStringLiteral("Download the Windows on-device chat model");
    case WindowsOnDeviceChatPhase::Idle:
        return QStringLiteral("Prepare the Windows on-device chat model");
    case WindowsOnDeviceChatPhase::DiscoveringExecutionProviders:
        return QStringLiteral("Checking Windows AI hardware");
    case WindowsOnDeviceChatPhase::DownloadingExecutionProviders:
        return QStringLiteral("Downloading Windows AI components - %1%")
            .arg(qRound(status.progressPercent));
    case WindowsOnDeviceChatPhase::ResolvingModel:
        return QStringLiteral("Selecting the on-device model");
    case WindowsOnDeviceChatPhase::DownloadingModel:
        return QStringLiteral("Downloading the on-device model - %1%")
            .arg(qRound(status.progressPercent));
    case WindowsOnDeviceChatPhase::LoadingModel:
        return QStringLiteral("Loading the on-device model");
    case WindowsOnDeviceChatPhase::Ready:
        return QStringLiteral("Ready on this Windows PC");
    case WindowsOnDeviceChatPhase::Failed:
        return QStringLiteral("On-device model setup failed. Retry preparation.");
    case WindowsOnDeviceChatPhase::Stopping:
        return QStringLiteral("Stopping the on-device model");
    }
    return QStringLiteral("On-device chat is unavailable");
}

bool isPreparing(const WindowsOnDeviceChatStatus& status)
{
    switch (status.phase) {
    case WindowsOnDeviceChatPhase::DiscoveringExecutionProviders:
    case WindowsOnDeviceChatPhase::DownloadingExecutionProviders:
    case WindowsOnDeviceChatPhase::ResolvingModel:
    case WindowsOnDeviceChatPhase::DownloadingModel:
    case WindowsOnDeviceChatPhase::LoadingModel:
        return true;
    default:
        return false;
    }
}

bool canPrepare(const WindowsOnDeviceChatStatus& status)
{
    return status.phase == WindowsOnDeviceChatPhase::ConsentRequired
        || status.phase == WindowsOnDeviceChatPhase::Idle
        || status.phase == WindowsOnDeviceChatPhase::Failed;
}

QString chatUsageSummary(
    const companion::ChatResult& result,
    bool onDevice)
{
    if (onDevice) {
        return QStringLiteral(
            "Private on-device response");
    }
    if (!result.inputTokens.has_value()
        || !result.outputTokens.has_value()) {
        return {};
    }
    return QStringLiteral("%1 in \u00b7 %2 out")
        .arg(*result.inputTokens)
        .arg(*result.outputTokens);
}

QString goalStatusText(GoalStatus status)
{
    switch (status) {
    case GoalStatus::Active:
        return QStringLiteral("active");
    case GoalStatus::Paused:
        return QStringLiteral("paused");
    case GoalStatus::Blocked:
        return QStringLiteral("blocked");
    case GoalStatus::UsageLimited:
        return QStringLiteral("usageLimited");
    case GoalStatus::BudgetLimited:
        return QStringLiteral("budgetLimited");
    case GoalStatus::Complete:
        return QStringLiteral("complete");
    }
    return {};
}

QVariantMap goalVariant(const companion::BridgeGoal& goal)
{
    QVariantMap result{
        {QStringLiteral("threadId"), goal.threadId},
        {QStringLiteral("objective"), goal.objective},
        {QStringLiteral("status"),
         goalStatusText(goal.status)},
        {QStringLiteral("tokensUsed"), goal.tokensUsed},
        {QStringLiteral("elapsedSeconds"),
         goal.elapsedSeconds},
        {QStringLiteral("createdAt"), goal.createdAt},
        {QStringLiteral("updatedAt"), goal.updatedAt},
    };
    if (goal.tokenBudget.has_value()) {
        result.insert(
            QStringLiteral("tokenBudget"),
            *goal.tokenBudget);
    }
    return result;
}

std::optional<companion::ThreadRuntimeStatus>
threadRuntimeStatus(QString value)
{
    value = value.trimmed();
    if (value == QStringLiteral("notLoaded")) {
        return companion::ThreadRuntimeStatus::
            NotLoaded;
    }
    if (value == QStringLiteral("idle")) {
        return companion::ThreadRuntimeStatus::Idle;
    }
    if (value == QStringLiteral("active")) {
        return companion::ThreadRuntimeStatus::
            Active;
    }
    if (value
        == QStringLiteral(
            "waitingOnApproval")) {
        return companion::ThreadRuntimeStatus::
            WaitingOnApproval;
    }
    if (value
        == QStringLiteral(
            "waitingOnUserInput")) {
        return companion::ThreadRuntimeStatus::
            WaitingOnUserInput;
    }
    if (value
        == QStringLiteral("systemError")) {
        return companion::ThreadRuntimeStatus::
            SystemError;
    }
    return std::nullopt;
}

std::optional<GoalStatus>
goalStatus(QString value)
{
    value = value.trimmed();
    if (value == QStringLiteral("active")) {
        return GoalStatus::Active;
    }
    if (value == QStringLiteral("paused")) {
        return GoalStatus::Paused;
    }
    if (value == QStringLiteral("blocked")) {
        return GoalStatus::Blocked;
    }
    if (value
        == QStringLiteral("usageLimited")) {
        return GoalStatus::UsageLimited;
    }
    if (value
        == QStringLiteral("budgetLimited")) {
        return GoalStatus::BudgetLimited;
    }
    if (value == QStringLiteral("complete")) {
        return GoalStatus::Complete;
    }
    return std::nullopt;
}

std::optional<companion::BridgeGoal>
goalFromVariant(const QVariantMap& value)
{
    if (value.isEmpty()) {
        return std::nullopt;
    }
    const QString threadId =
        value.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    const QString objective =
        value.value(QStringLiteral("objective"))
            .toString()
            .trimmed();
    const auto status =
        goalStatus(
            value.value(QStringLiteral("status"))
                .toString());
    if (threadId.isEmpty()
        || objective.isEmpty()
        || !status.has_value()) {
        return std::nullopt;
    }

    std::optional<qint64> tokenBudget;
    const QVariant budget =
        value.value(
            QStringLiteral("tokenBudget"));
    if (budget.isValid()
        && !budget.isNull()) {
        tokenBudget =
            budget.toLongLong();
    }
    return companion::BridgeGoal{
        threadId,
        objective,
        *status,
        tokenBudget,
        value.value(
                 QStringLiteral("tokensUsed"))
            .toLongLong(),
        value.value(
                 QStringLiteral(
                     "elapsedSeconds"))
            .toLongLong(),
        value.value(
                 QStringLiteral("createdAt"))
            .toLongLong(),
        value.value(
                 QStringLiteral("updatedAt"))
            .toLongLong(),
    };
}

qint64 processUpdatedAtMilliseconds(
    const QVariantMap& process)
{
    const double value =
        process.value(
                   QStringLiteral("updatedAt"))
            .toDouble();
    if (!std::isfinite(value)) {
        return 0;
    }
    return qRound64(
        (value
         + kSwiftReferenceDateUnixSeconds)
        * 1000.0);
}

qint64 bridgeDateMilliseconds(
    const companion::BridgeDate& date)
{
    return qRound64(
        (date.secondsSinceReferenceDate
         + kSwiftReferenceDateUnixSeconds)
        * 1000.0);
}

QVariantMap usageWindowVariant(
    const companion::BridgeUsageWindow& window)
{
    QVariantMap result{
        {
            QStringLiteral("remainingPercent"),
            window.remainingPercent,
        },
        {
            QStringLiteral("durationLabel"),
            window.durationLabel,
        },
    };
    if (window.resetsAt.has_value()) {
        result.insert(
            QStringLiteral("resetsAt"),
            bridgeDateMilliseconds(
                *window.resetsAt));
    }
    return result;
}

QVariantMap usageSnapshotVariant(
    const companion::BridgeUsageSnapshot& snapshot)
{
    QVariantList groups;
    groups.reserve(snapshot.groups.size());
    for (const auto& group : snapshot.groups) {
        QVariantMap row{
            {QStringLiteral("id"), group.id},
            {QStringLiteral("title"), group.title},
        };
        if (group.shortWindow.has_value()) {
            row.insert(
                QStringLiteral("shortWindow"),
                usageWindowVariant(
                    *group.shortWindow));
        }
        if (group.weeklyWindow.has_value()) {
            row.insert(
                QStringLiteral("weeklyWindow"),
                usageWindowVariant(
                    *group.weeklyWindow));
        }
        groups.append(std::move(row));
    }

    QVariantList resetCredits;
    resetCredits.reserve(
        snapshot.availableResetCredits.size());
    for (const auto& credit :
         snapshot.availableResetCredits) {
        QVariantMap row{
            {QStringLiteral("id"), credit.id},
            {
                QStringLiteral("displayTitle"),
                credit.displayTitle,
            },
        };
        if (credit.detail.has_value()) {
            row.insert(
                QStringLiteral("detail"),
                *credit.detail);
        }
        if (credit.expiresAt.has_value()) {
            row.insert(
                QStringLiteral("expiresAt"),
                bridgeDateMilliseconds(
                    *credit.expiresAt));
        }
        resetCredits.append(std::move(row));
    }

    QVariantMap result{
        {QStringLiteral("groups"), groups},
        {
            QStringLiteral("availableResetCount"),
            snapshot.availableResetCount,
        },
        {
            QStringLiteral("availableResetCredits"),
            resetCredits,
        },
        {
            QStringLiteral("updatedAt"),
            bridgeDateMilliseconds(
                snapshot.updatedAt),
        },
    };
    if (snapshot.planType.has_value()) {
        result.insert(
            QStringLiteral("planType"),
            *snapshot.planType);
    }
    return result;
}

} // namespace

namespace companion {

class MobileRelayUrlState final {
public:
    void set(
        std::optional<QString> value)
    {
        QMutexLocker locker(&mutex_);
        value_ = std::move(value);
    }

    std::optional<QString> value() const
    {
        QMutexLocker locker(&mutex_);
        return value_;
    }

private:
    mutable QMutex mutex_;
    std::optional<QString> value_;
};

Result<std::unique_ptr<CompanionRuntimeHost>>
CompanionRuntimeHost::createProduction(
    CompanionShellViewModel& shellViewModel,
    const AppSettings& settings,
    std::shared_ptr<CredentialStore>
        credentialStore,
    QObject* parent,
    CodexAccountRouter* accountRouter,
    CodexAccountProfileStore*
        accountProfileStore,
    CodexThreadAccountBindingStore*
        accountBindingStore,
    std::shared_ptr<
        MobilePresencePetCatalogService>
        presencePetCatalogService)
{
    const auto environment = CodexEnvironment::discover();
    if (!environment.hasValue()) {
        return Result<std::unique_ptr<CompanionRuntimeHost>>::failure(
            environment.error());
    }

    try {
        if (!credentialStore) {
            credentialStore =
                std::make_shared<
                    DpapiCredentialStore>();
        }

        std::shared_ptr<WindowsOnDeviceChatBackend> onDeviceBackend;
        const auto backend = acquireWindowsOnDeviceChatBackend();
        if (backend.hasValue()) {
            onDeviceBackend = backend.value();
        }

        CodexRuntimeProductionServices services;
        services.attachmentStore =
            std::make_shared<AttachmentStore>();
        services.taskCommandService =
            std::make_shared<TaskCommandService>(
                environment.value());
        services.approvalService =
            std::make_shared<ApprovalService>(
                environment.value());
        services.taskCreator =
            std::make_shared<TaskCreator>(
                environment.value());
        services.credentialStore =
            credentialStore;
        services.openAITransport =
            ChatService::
                createDefaultHttpTransport();
        services.lumoTransport =
            ChatService::
                createDefaultHttpTransport();
        services.onDeviceBackend =
            onDeviceBackend;
        services.goalService =
            std::make_shared<GoalService>(
                environment.value());
        services.usageService =
            std::make_shared<UsageService>(
                environment.value());
        if (accountRouter != nullptr) {
            const auto profiledTaskCreator =
                std::make_shared<
                    ProfiledTaskCreator>(
                    environment.value(),
                    *accountRouter);
            services.taskCreatePerformer =
                [profiledTaskCreator](
                    const CreateTaskRequest&
                        request) {
                    return profiledTaskCreator
                        ->create(request);
                };
        }

        auto production =
            CodexRuntimeDependencyFactory::
                build(
                    environment.value(),
                    std::move(services));
        if (!production.hasValue()) {
            return Result<
                std::unique_ptr<
                    CompanionRuntimeHost>>::
                failure(
                    production.error());
        }
        if (accountRouter != nullptr) {
            const auto profiledControl =
                std::make_shared<
                    ProfiledCodexControlService>(
                    environment.value(),
                    *accountRouter);
            auto& dependencies =
                production.value()
                    .dependencies;
            dependencies.goalLoader =
                [profiledControl](
                    const QVector<QString>&
                        threadIds,
                    std::stop_token
                        stopToken) {
                    return profiledControl
                        ->readGoalsSync(
                            threadIds,
                            stopToken);
                };
            dependencies.reads
                ->usageReadStarter =
                [profiledControl] {
                    return profiledControl
                        ->readUsage();
                };
            dependencies.mutations
                ->goalMutationStarter =
                [profiledControl](
                    RuntimeGoalMutationRequest
                        request) {
                    return profiledControl
                        ->mutateGoal(
                            std::move(
                                request));
                };
            dependencies.mutations
                ->usageResetMutationStarter =
                [profiledControl](
                    QString creditId,
                    QUuid idempotencyKey) {
                    return profiledControl
                        ->consumeUsageReset(
                            std::move(
                                creditId),
                            idempotencyKey);
                };
        }
        auto mobileBindings =
            CompanionMobileRuntimeAdapter::
                create(
                    production.value()
                        .dependencies);
        if (!mobileBindings.hasValue()) {
            return Result<
                std::unique_ptr<
                    CompanionRuntimeHost>>::
                failure(
                    mobileBindings.error());
        }

        auto state =
            std::make_unique<
                CompanionState>();
        auto commandBus =
            std::make_unique<
                CompanionCommandBus>();
        auto continuationHost =
            std::make_shared<
                RuntimeContinuationHost>();
        auto runtime =
            std::make_unique<CodexRuntime>(
                *state,
                *commandBus,
                std::move(
                    production.value()
                        .dependencies),
                continuationHost);

        std::unique_ptr<
            CompanionMobileHost> mobileHost;
        std::shared_ptr<
            MobileRelayUrlState>
            mobileRelayUrlState;
        std::optional<CompanionError>
            mobileStartupError;
        {
            CompanionInstallationIdentityStore
                identityStore;
            const auto identity =
                identityStore.loadOrCreate();
            const RelaySettings relaySettings =
                RelaySettings::
                    fromBundledConfiguration();
            const auto relayUrl =
                relaySettings.configuredUrl(
                    settings);

            Result<WindowsTlsIdentity>
                tlsIdentity =
                    Result<
                        WindowsTlsIdentity>::
                        failure(
                            runtimeHostError(
                                QStringLiteral(
                                    "mobile.tls_identity_unavailable"),
                                QStringLiteral(
                                    "The Windows nearby TLS identity is unavailable.")));
            if (identity.hasValue()) {
                WindowsTlsIdentityStore
                    tlsStore;
                tlsIdentity =
                    tlsStore.loadOrCreate(
                        identity.value());
            }

            if (!identity.hasValue()) {
                mobileStartupError =
                    identity.error();
            } else if (!relayUrl.hasValue()) {
                mobileStartupError =
                    relayUrl.error();
            } else if (!tlsIdentity.hasValue()) {
                mobileStartupError =
                    tlsIdentity.error();
            } else {
                QString hostName =
                    QSysInfo::
                        machineHostName()
                        .trimmed();
                if (hostName.isEmpty()) {
                    hostName =
                        QStringLiteral(
                            "Codex Companion Windows");
                }
                std::optional<QString>
                    relayUrlString;
                if (relayUrl.value()
                        .has_value()) {
                    relayUrlString =
                        relayUrl.value()
                            ->toString(
                                QUrl::
                                    FullyEncoded);
                }
                mobileRelayUrlState =
                    std::make_shared<
                        MobileRelayUrlState>();
                mobileRelayUrlState->set(
                    relayUrlString);
                MobileRequestDispatcherConfiguration
                    dispatcherConfiguration;
                dispatcherConfiguration.hostName =
                    hostName;
                dispatcherConfiguration.hostDeviceId =
                    identity.value();
                dispatcherConfiguration
                    .relayUrlProvider =
                    [mobileRelayUrlState] {
                        return mobileRelayUrlState
                            ->value();
                    };
                dispatcherConfiguration
                    .presencePetCatalogService =
                    presencePetCatalogService;
                MobileRequestDispatcher
                    dispatcher(
                        std::move(
                            mobileBindings
                                .value()
                                .reads),
                        std::move(
                            mobileBindings
                                .value()
                                .mutations),
                        std::move(
                            dispatcherConfiguration));
                auto dispatcherOwner =
                    std::make_shared<
                        MobileRequestDispatcher>(
                        std::move(dispatcher));

                CompanionMobileHostConfiguration
                    mobileConfiguration;
                mobileConfiguration.enabled =
                    settings.mobileEnabled;
                mobileConfiguration
                    .allowNearbyOnPublicNetworks =
                    settings
                        .allowNearbyOnPublicNetworks;
                mobileConfiguration
                    .installationId =
                    identity.value();
                mobileConfiguration
                    .computerName =
                    hostName;
                mobileConfiguration
                    .hostDisplayName =
                    hostName;
                mobileConfiguration
                    .sslConfiguration =
                    tlsIdentity.value()
                        .sslConfiguration;
                mobileConfiguration
                    .tlsFingerprintSha256 =
                    tlsIdentity.value()
                        .fingerprintSha256;
                mobileConfiguration
                    .pairingRecordsPath =
                    PairingRecordStore::
                        defaultFilePath();
                mobileConfiguration
                    .relayStatePath =
                    RelayStateStore::
                        defaultFilePath();
                mobileConfiguration
                    .transferRootPath =
                    NearbyTransferAssembler::
                        defaultRootPath();
                mobileConfiguration.relayUrl =
                    relayUrl.value();
                mobileConfiguration
                    .presencePetCatalogService =
                    presencePetCatalogService;

                auto createdMobile =
                    CompanionMobileHost::create(
                        std::move(
                            mobileConfiguration),
                        [dispatcherOwner](
                            QString,
                            BridgeRequest
                                request) {
                            return dispatcherOwner
                                ->handle(
                                    std::move(
                                        request));
                        });
                if (createdMobile
                        .hasValue()) {
                    mobileHost =
                        std::move(
                            createdMobile
                                .value());
                } else {
                    mobileStartupError =
                        createdMobile.error();
                }
            }
        }

        auto host =
            std::unique_ptr<
                CompanionRuntimeHost>(
                new CompanionRuntimeHost(
                    shellViewModel,
                    std::move(state),
                    std::move(commandBus),
                    std::move(runtime),
                    QDir(
                        environment.value()
                            .codexHome)
                        .filePath(
                            QStringLiteral(
                                ".codex-global-state.json")),
                    std::move(
                        continuationHost),
                    std::move(credentialStore),
                    std::move(onDeviceBackend),
                    {},
                    std::move(mobileHost),
                    std::move(
                        mobileRelayUrlState),
                    std::move(
                        mobileStartupError),
                    parent));
        host
            ->automaticAccountContinuationEnabled_ =
            settings
                .automaticallyContinuesAcrossCodexAccounts;
        if (accountRouter != nullptr
            && accountProfileStore
                != nullptr
            && accountBindingStore
                != nullptr) {
            const QString dataRoot =
                QDir(
                    environment.value()
                        .localAppData)
                    .filePath(
                        QStringLiteral(
                            "Codex Companion"));
            host->failedTaskRetryJournal_ =
                std::make_unique<
                    CodexFailedTaskRetryJournal>(
                    QDir(dataRoot)
                        .filePath(
                            QStringLiteral(
                                "failed-task-retries.v1.json")));
            host
                ->automaticContinuationJournal_ =
                std::make_unique<
                    CodexAutomaticContinuationJournal>(
                    QDir(dataRoot)
                        .filePath(
                            QStringLiteral(
                                "automatic-account-continuations.v1.json")));
            CodexContinuationCommands
                retryCommands =
                    createProductionCodexContinuationCommands(
                        environment.value(),
                        *accountRouter);
            CodexContinuationCommands
                automaticCommands =
                    retryCommands;
            host->failedTaskRetryService_ =
                std::make_unique<
                    CodexFailedTaskRetryService>(
                    *accountProfileStore,
                    *accountBindingStore,
                    *accountRouter,
                    std::move(
                        retryCommands),
                    *host
                         ->failedTaskRetryJournal_);
            host
                ->automaticContinuationCoordinator_ =
                std::make_unique<
                    CodexAutomaticAccountContinuationCoordinator>(
                    *accountProfileStore,
                    *accountBindingStore,
                    *accountRouter,
                    std::move(
                        automaticCommands),
                    *host
                         ->automaticContinuationJournal_);
        }
        return Result<
            std::unique_ptr<
                CompanionRuntimeHost>>::
            success(std::move(host));
    } catch (...) {
        return Result<std::unique_ptr<CompanionRuntimeHost>>::failure(
            runtimeHostError(
                QStringLiteral("companion.runtime_host_unavailable"),
                QStringLiteral("Could not initialize Companion runtime services.")));
    }
}

CompanionRuntimeHost::CompanionRuntimeHost(
    CompanionShellViewModel& shellViewModel,
    std::unique_ptr<CompanionState> state,
    std::unique_ptr<CompanionCommandBus> commandBus,
    std::unique_ptr<CodexRuntime> runtime,
    std::shared_ptr<CredentialStore> credentialStore,
    std::shared_ptr<WindowsOnDeviceChatBackend> onDeviceBackend,
    ChatRequestSender chatSender,
    QObject* parent)
    : CompanionRuntimeHost(
          shellViewModel,
          std::move(state),
          std::move(commandBus),
          std::move(runtime),
          {},
          {},
          std::move(credentialStore),
          std::move(onDeviceBackend),
          std::move(chatSender),
          {},
          {},
          std::nullopt,
          parent)
{
}

CompanionRuntimeHost::CompanionRuntimeHost(
    CompanionShellViewModel& shellViewModel,
    std::unique_ptr<CompanionState> state,
    std::unique_ptr<CompanionCommandBus> commandBus,
    std::unique_ptr<CodexRuntime> runtime,
    QString sidebarStatePath,
    std::shared_ptr<RuntimeContinuationHost>
        continuationHost,
    std::shared_ptr<CredentialStore> credentialStore,
    std::shared_ptr<WindowsOnDeviceChatBackend>
        onDeviceBackend,
    ChatRequestSender chatSender,
    std::unique_ptr<CompanionMobileHost>
        mobileHost,
    std::shared_ptr<MobileRelayUrlState>
        mobileRelayUrlState,
    std::optional<CompanionError>
        mobileStartupError,
    QObject* parent)
    : QObject(parent),
      shellViewModel_(shellViewModel),
      state_(std::move(state)),
      commandBus_(std::move(commandBus)),
      runtime_(std::move(runtime)),
      processListModel_(
          std::make_unique<ProcessListModel>(
              sidebarStatePath,
              sidebarStatePath.trimmed().isEmpty()
                  ? QString()
                  : ProcessListModel::
                        defaultFailureStatePath())),
      continuationHost_(
          std::move(
              continuationHost)),
      credentialStore_(std::move(credentialStore)),
      onDeviceBackend_(std::move(onDeviceBackend)),
      chatSender_(std::move(chatSender)),
      mobileHost_(
          std::move(mobileHost)),
      mobileRelayUrlState_(
          std::move(
              mobileRelayUrlState)),
      mobileStartupError_(
          std::move(
              mobileStartupError)),
      chatWatcher_(this),
      preparationWatcher_(this)
{
    if (mobileHost_) {
        connect(
            mobileHost_.get(),
            &CompanionMobileHost::
                failureOccurred,
            this,
            &CompanionRuntimeHost::
                reportRuntimeError);
        connect(
            mobileHost_.get(),
            &CompanionMobileHost::
                nearbyAccessChanged,
            this,
            &CompanionRuntimeHost::
                publishMobileNearbyAccess);
    }

    if (!chatSender_) {
        chatService_ =
            std::make_unique<ChatService>(
                credentialStore_,
                onDeviceBackend_
                    ? OnDeviceChatSender(
                          [backend =
                               onDeviceBackend_](
                              const ChatRequest&
                                  request) {
                              return backend->send(
                                  request);
                          })
                    : OnDeviceChatSender());
    }

    shellViewModel_.setProcessModel(
        processListModel_.get());

    connect(
        runtime_.get(),
        &CodexRuntime::loadingChanged,
        this,
        &CompanionRuntimeHost::refreshProcessStatus);
    connect(
        runtime_.get(),
        &CodexRuntime::statusChanged,
        this,
        &CompanionRuntimeHost::refreshProcessStatus);
    connect(
        runtime_.get(),
        &CodexRuntime::usageChanged,
        this,
        &CompanionRuntimeHost::refreshUsageStatus);
    connect(
        runtime_.get(),
        &CodexRuntime::usageResetFinished,
        this,
        &CompanionRuntimeHost::
            handleUsageResetFinished);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::selectedChatModelIdChanged,
        this,
        &CompanionRuntimeHost::refreshChatAvailability);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::localChatRequested,
        this,
        &CompanionRuntimeHost::sendLocalChat);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::onDevicePreparationRequested,
        this,
        &CompanionRuntimeHost::prepareOnDeviceChat);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::usageRefreshRequested,
        this,
        &CompanionRuntimeHost::requestUsage);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::
            usageResetRequested,
        this,
        &CompanionRuntimeHost::
            requestUsageReset);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::
            processMessageRequested,
        this,
        &CompanionRuntimeHost::
            sendProcessMessage);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::
            processApprovalRequested,
        this,
        &CompanionRuntimeHost::
            respondToProcessApproval);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::
            processRetryRequested,
        this,
        &CompanionRuntimeHost::
            retryFailedProcess);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::
            processCancelRequested,
        this,
        &CompanionRuntimeHost::
            cancelPendingProcessCommand);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::goalUpdateRequested,
        this,
        &CompanionRuntimeHost::updateGoal);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::goalPauseRequested,
        this,
        &CompanionRuntimeHost::pauseGoal);
    connect(
        &shellViewModel_,
        &CompanionShellViewModel::goalResumeRequested,
        this,
        &CompanionRuntimeHost::resumeGoal);
    connect(
        commandBus_.get(),
        &CompanionCommandBus::
            commandFinishedDetailed,
        this,
        &CompanionRuntimeHost::
            handleProcessCommandFinished);
    connect(
        commandBus_.get(),
        &CompanionCommandBus::commandFinished,
        this,
        &CompanionRuntimeHost::
            handleGoalCommandFinished);
    connect(
        commandBus_.get(),
        &CompanionCommandBus::commandFinished,
        this,
        &CompanionRuntimeHost::
            handleUsageCommandFinished);
    connect(
        runtime_.get(),
        &CodexRuntime::goalChanged,
        this,
        &CompanionRuntimeHost::handleGoalChanged);
    connect(
        runtime_.get(),
        &CodexRuntime::processSnapshotChanged,
        this,
        &CompanionRuntimeHost::refreshProcessModel);
    connect(
        runtime_.get(),
        &CodexRuntime::processSnapshotChanged,
        this,
        &CompanionRuntimeHost::refreshOpenGoalControls);
    connect(
        &chatWatcher_,
        &QFutureWatcher<Result<ChatResult>>::finished,
        this,
        &CompanionRuntimeHost::completeChatRequest);
    connect(
        &preparationWatcher_,
        &QFutureWatcher<Result<void>>::finished,
        this,
        &CompanionRuntimeHost::completeOnDevicePreparation);

    if (onDeviceBackend_) {
        const QPointer<CompanionRuntimeHost> host(this);
        onDeviceStatusDispatcher_ = std::make_shared<
            detail::RuntimeHostStatusDispatcher>(
            [host](WindowsOnDeviceChatStatus status) {
                if (!host.isNull()) {
                    host->handleOnDeviceStatus(status);
                }
            });
        const std::weak_ptr<
            detail::RuntimeHostStatusDispatcher>
            weakDispatcher(onDeviceStatusDispatcher_);
        onDeviceStatusSubscription_ =
            onDeviceBackend_->subscribeStatus(
                [weakDispatcher](
                    WindowsOnDeviceChatStatus status) {
                    if (const auto dispatcher =
                            weakDispatcher.lock()) {
                        dispatcher->publish(status);
                    }
                });
    }

    refreshProcessModel();
    refreshProcessStatus();
    refreshUsageStatus();
    refreshChatAvailability();
}

CompanionRuntimeHost::~CompanionRuntimeHost()
{
    automaticContinuationStopSource_
        .request_stop();
    if (shellViewModel_.processModel()
        == processListModel_.get()) {
        shellViewModel_.setProcessModel(nullptr);
    }
    if (pendingProcessMessage_.active()) {
        if (runtime_
            && !pendingProcessMessage_
                    .operationKey
                    .isEmpty()) {
            runtime_->requestOperationStop(
                pendingProcessMessage_
                    .operationKey);
        }
        clearPendingProcessCommand(
            pendingProcessMessage_);
        shellViewModel_.finishProcessMessage(
            false,
            QStringLiteral(
                "The Codex message stopped before it finished."));
    }
    if (pendingProcessApproval_.active()) {
        if (runtime_
            && !pendingProcessApproval_
                    .operationKey
                    .isEmpty()) {
            runtime_->requestOperationStop(
                pendingProcessApproval_
                    .operationKey);
        }
        clearPendingProcessCommand(
            pendingProcessApproval_);
        shellViewModel_.finishProcessApproval(
            false,
            QStringLiteral(
                "The approval stopped before it finished."));
    }
    if (!pendingGoalCommand_.isEmpty()) {
        pendingGoalCommand_.clear();
        pendingGoalThreadId_.clear();
        shellViewModel_.finishGoalMutation(
            false,
            QStringLiteral(
                "The goal update stopped before it finished."));
    }
    if (onDeviceStatusDispatcher_) {
        onDeviceStatusDispatcher_->invalidate();
    }
    onDeviceStatusSubscription_.reset();
    onDeviceStatusDispatcher_.reset();
    if (mobileHost_) {
        mobileHost_->stop();
        mobileHost_.reset();
    }
    if (runtime_) {
        runtime_->stop();
    }
    chatService_.reset();
    runtime_.reset();
    if (continuationHost_) {
        continuationHost_
            ->stopAcceptingAndDrain();
        continuationHost_.reset();
    }
    commandBus_.reset();
    state_.reset();
}

Result<void> CompanionRuntimeHost::start()
{
    if (started_) {
        return Result<void>::success();
    }

    const auto started = runtime_->start();
    if (!started.hasValue()) {
        shellViewModel_.setProcessStatus(
            false,
            started.error().message);
        return started;
    }

    started_ = true;
    if (mobileStartupError_
            .has_value()) {
        reportRuntimeError(
            *mobileStartupError_);
        mobileStartupError_.reset();
    }
    if (mobileHost_) {
        const auto mobileStarted =
            mobileHost_->start();
        if (!mobileStarted.hasValue()) {
            reportRuntimeError(
                mobileStarted.error());
        }
    }
    refreshProcessStatus();
    return Result<void>::success();
}

TaskListModel*
CompanionRuntimeHost::taskModel() const noexcept
{
    return state_ ? state_->tasks() : nullptr;
}

ProcessListModel*
CompanionRuntimeHost::processModel() const noexcept
{
    return processListModel_.get();
}

PairingCoordinator*
CompanionRuntimeHost::
    mobilePairingCoordinator() noexcept
{
    return mobileHost_
        ? &mobileHost_
               ->pairingCoordinator()
        : nullptr;
}

RelayPairingBootstrap*
CompanionRuntimeHost::
    mobileRelayPairingBootstrap() noexcept
{
    return mobileHost_
        ? &mobileHost_
               ->relayPairingBootstrap()
        : nullptr;
}

bool CompanionRuntimeHost::
mobileNearbyAccessAvailable()
    const noexcept
{
    return mobileHost_
        && mobileHost_->
            nearbyAccessAvailable();
}

QString CompanionRuntimeHost::
mobileNearbyAccessStatusText() const
{
    if (!mobileHost_) {
        return QStringLiteral(
            "The Windows mobile bridge is unavailable.");
    }
    if (!mobileHost_->isEnabled()) {
        return QStringLiteral(
            "Nearby Wi-Fi access is turned off.");
    }

    switch (mobileHost_->
                networkProfile()) {
    case WindowsNetworkProfile::Public:
        if (!mobileHost_->
                 allowsNearbyOnPublicNetworks()) {
            return QStringLiteral(
                "Nearby Wi-Fi is blocked on this Public network. Allow Public-network access below or set this trusted network to Private before pairing.");
        }
        return mobileHost_->
                       nearbyAccessAvailable()
            ? QStringLiteral(
                  "Nearby Wi-Fi is available on this Public network by your explicit setting. Pair only on networks you trust.")
            : QStringLiteral(
                  "Nearby Wi-Fi could not start on this Public network.");
    case WindowsNetworkProfile::Unavailable:
        return QStringLiteral(
            "Nearby Wi-Fi is unavailable because Windows has no connected network.");
    case WindowsNetworkProfile::Private:
    case WindowsNetworkProfile::Domain:
        return mobileHost_->
                       nearbyAccessAvailable()
            ? QStringLiteral(
                  "Nearby Wi-Fi is available on this trusted Windows network.")
            : QStringLiteral(
                  "Nearby Wi-Fi could not start on this trusted Windows network.");
    }
    return QStringLiteral(
        "Nearby Wi-Fi is unavailable.");
}

Result<void>
CompanionRuntimeHost::
applyMobileSettings(
    const AppSettings& settings)
{
    if (!mobileHost_) {
        return Result<void>::failure(
            runtimeHostError(
                QStringLiteral(
                    "mobile.host_unavailable"),
                QStringLiteral(
                    "The Windows mobile bridge is unavailable.")));
    }
    const RelaySettings relaySettings =
        RelaySettings::
            fromBundledConfiguration();
    const auto relayUrl =
        relaySettings.configuredUrl(
            settings);
    if (!relayUrl.hasValue()) {
        return Result<void>::failure(
            relayUrl.error());
    }
    const auto applied =
        mobileHost_->applyConfiguration(
            settings.mobileEnabled,
            settings
                .allowNearbyOnPublicNetworks,
            relayUrl.value());
    if (!applied.hasValue()) {
        return applied;
    }

    if (mobileRelayUrlState_) {
        std::optional<QString>
            relayUrlString;
        if (relayUrl.value()
                .has_value()) {
            relayUrlString =
                relayUrl.value()
                    ->toString(
                        QUrl::FullyEncoded);
        }
        mobileRelayUrlState_->set(
            std::move(relayUrlString));
    }
    return Result<void>::success();
}

void CompanionRuntimeHost::
setAutomaticAccountContinuationEnabled(
    bool enabled)
{
    if (automaticAccountContinuationEnabled_
        == enabled) {
        return;
    }
    automaticAccountContinuationEnabled_ =
        enabled;
    if (!enabled) {
        automaticContinuationStopSource_
            .request_stop();
    } else {
        scheduleAutomaticAccountContinuations();
    }
}

void CompanionRuntimeHost::
publishMobileNearbyAccess()
{
    emit mobileNearbyAccessChanged(
        mobileNearbyAccessAvailable(),
        mobileNearbyAccessStatusText());
}

void CompanionRuntimeHost::setProcessSurfaceVisible(bool visible)
{
    if (!runtime_) {
        return;
    }
    runtime_->setProcessSurfaceVisible(visible);
    if (visible) {
        runtime_->refreshNow();
    }
}

void CompanionRuntimeHost::refreshProcessStatus()
{
    if (!runtime_) {
        shellViewModel_.setProcessStatus(
            false,
            QStringLiteral("Codex runtime is unavailable"));
        return;
    }

    shellViewModel_.setProcessStatus(
        runtime_->loading(),
        runtime_->errorMessage());
}

void CompanionRuntimeHost::refreshUsageStatus()
{
    if (!runtime_) {
        shellViewModel_.setUsageStatus(
            false,
            {},
            QStringLiteral(
                "Codex usage is unavailable."));
        return;
    }

    QVariantMap snapshot;
    if (runtime_->usageSnapshot().has_value()) {
        snapshot = usageSnapshotVariant(
            *runtime_->usageSnapshot());
    }
    shellViewModel_.setUsageStatus(
        runtime_->usageLoading(),
        std::move(snapshot),
        runtime_->usageErrorMessage());
}

void CompanionRuntimeHost::requestUsage()
{
    if (!commandBus_) {
        shellViewModel_.setUsageStatus(
            false,
            {},
            QStringLiteral(
                "Codex usage is unavailable."));
        return;
    }
    commandBus_->execute(
        QStringLiteral("codex.usage.load"));
}

void CompanionRuntimeHost::requestUsageReset(
    QString creditId,
    QString idempotencyKey)
{
    if (!commandBus_) {
        shellViewModel_.finishUsageReset(
            false,
            QStringLiteral(
                "The Codex usage reset service is unavailable."));
        return;
    }

    commandBus_->execute(
        QStringLiteral(
            "codex.usage.consume-reset"),
        {
            {
                QStringLiteral(
                    "resetCreditId"),
                std::move(creditId),
            },
            {
                QStringLiteral(
                    "idempotencyKey"),
                std::move(idempotencyKey),
            },
        });
}

void CompanionRuntimeHost::sendProcessMessage(
    QString action,
    QString threadId,
    QString prompt,
    QString cwd,
    QString activeTurnId,
    QString model,
    QString reasoningEffort)
{
    if (!commandBus_) {
        shellViewModel_.finishProcessMessage(
            false,
            QStringLiteral(
                "The Codex task connection is unavailable."));
        return;
    }
    if (pendingProcessMessage_.active()) {
        shellViewModel_.finishProcessMessage(
            false,
            QStringLiteral(
                "Another Codex action is already running."));
        return;
    }

    pendingProcessMessage_.operationKey =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    pendingProcessMessage_.processId =
        shellViewModel_.processTargetId();
    QVariantMap arguments{
        {QStringLiteral("threadId"),
         std::move(threadId)},
        {QStringLiteral("text"),
         std::move(prompt)},
    };
    if (!cwd.trimmed().isEmpty()) {
        arguments.insert(
            QStringLiteral("cwd"),
            cwd.trimmed());
    }
    if (!model.trimmed().isEmpty()) {
        arguments.insert(
            QStringLiteral("model"),
            model.trimmed());
    }
    if (!reasoningEffort.trimmed().isEmpty()) {
        arguments.insert(
            QStringLiteral("reasoningEffort"),
            reasoningEffort.trimmed());
    }

    if (action == QStringLiteral("steer")) {
        if (!activeTurnId.trimmed().isEmpty()) {
            arguments.insert(
                QStringLiteral("expectedTurnId"),
                activeTurnId.trimmed());
        }
        pendingProcessMessage_.kind =
            PendingProcessCommand::Message;
        pendingProcessMessage_.command =
            QStringLiteral("codex.steer");
        if (!executePendingProcessCommand(
                pendingProcessMessage_,
                std::move(arguments))) {
            clearPendingProcessCommand(
                pendingProcessMessage_);
            shellViewModel_.finishProcessMessage(
                false,
                QStringLiteral(
                    "The Codex steer could not be started."));
        }
        return;
    }

    if (action
        == QStringLiteral(
            "approval-feedback")) {
        pendingProcessMessage_.kind =
            PendingProcessCommand::
                ApprovalFeedbackDecline;
        pendingProcessMessage_.command =
            QStringLiteral(
                "codex.approval.respond");
        pendingProcessMessage_.arguments =
            std::move(arguments);
        QVariantMap declineArguments{
                {
                    QStringLiteral("threadId"),
                    pendingProcessMessage_
                        .arguments
                        .value(
                            QStringLiteral(
                                "threadId")),
                },
                {
                    QStringLiteral(
                        "approvalDecision"),
                    QStringLiteral("decline"),
                },
            };
        if (!executePendingProcessCommand(
                pendingProcessMessage_,
                std::move(declineArguments))) {
            clearPendingProcessCommand(
                pendingProcessMessage_);
            shellViewModel_.finishProcessMessage(
                false,
                QStringLiteral(
                    "The approval response could not be started."));
        }
        return;
    }

    pendingProcessMessage_.kind =
        PendingProcessCommand::Message;
    pendingProcessMessage_.command =
        QStringLiteral("codex.reply");
    if (!executePendingProcessCommand(
            pendingProcessMessage_,
            std::move(arguments))) {
        clearPendingProcessCommand(
            pendingProcessMessage_);
        shellViewModel_.finishProcessMessage(
            false,
            QStringLiteral(
                "The Codex reply could not be started."));
    }
}

void CompanionRuntimeHost::
cancelPendingProcessCommand()
{
    if (!pendingProcessMessage_.active()) {
        return;
    }

    const QString operationKey =
        pendingProcessMessage_.operationKey;
    clearPendingProcessCommand(
        pendingProcessMessage_);
    if (runtime_
        && !operationKey.isEmpty()) {
        runtime_->requestOperationStop(
            operationKey);
    }
}

bool CompanionRuntimeHost::
executePendingProcessCommand(
    PendingProcessExecution& pending,
    QVariantMap arguments)
{
    if (!commandBus_
        || pending.command.isEmpty()
        || pending.operationKey.isEmpty()) {
        return false;
    }
    arguments.insert(
        codexRuntimeOperationKeyArgument(),
        pending.operationKey);
    pending.executionId =
        commandBus_->execute(
            pending.command,
            arguments);
    return pending.executionId != 0;
}

void CompanionRuntimeHost::
clearPendingProcessCommand(
    PendingProcessExecution& pending)
{
    pending = {};
}

void CompanionRuntimeHost::respondToProcessApproval(
    QString threadId,
    QString decision)
{
    if (!commandBus_) {
        shellViewModel_.finishProcessApproval(
            false,
            QStringLiteral(
                "The Codex approval connection is unavailable."));
        return;
    }
    if (pendingProcessApproval_.active()) {
        shellViewModel_.finishProcessApproval(
            false,
            QStringLiteral(
                "Another Codex action is already running."));
        return;
    }

    pendingProcessApproval_.kind =
        PendingProcessCommand::Approval;
    pendingProcessApproval_.command =
        QStringLiteral(
            "codex.approval.respond");
    pendingProcessApproval_.operationKey =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    QVariantMap arguments{
            {
                QStringLiteral("threadId"),
                std::move(threadId),
            },
            {
                QStringLiteral(
                    "approvalDecision"),
                std::move(decision),
            },
        };
    if (!executePendingProcessCommand(
            pendingProcessApproval_,
            std::move(arguments))) {
        clearPendingProcessCommand(
            pendingProcessApproval_);
        shellViewModel_.finishProcessApproval(
            false,
            QStringLiteral(
                "The Codex approval could not be started."));
    }
}

void CompanionRuntimeHost::retryFailedProcess(
    QVariantMap process)
{
    QString processId =
        process.value(QStringLiteral("id"))
            .toString()
            .trimmed();
    if (processId.isEmpty()) {
        processId =
            process.value(
                       QStringLiteral(
                           "processId"))
                .toString()
                .trimmed();
    }
    const QString threadId =
        process.value(
                   QStringLiteral("threadId"))
            .toString()
            .trimmed();
    const QString kind =
        process.value(QStringLiteral("kind"))
            .toString()
            .trimmed()
            .toLower();
    const QString status =
        process.value(
                   QStringLiteral("status"))
            .toString()
            .trimmed()
            .toLower();
    const QString rawRuntimeStatus =
        process.value(
                   QStringLiteral(
                       "runtimeStatus"))
            .toString()
            .trimmed();
    const auto runtimeStatus =
        rawRuntimeStatus.isEmpty()
            && status
                == QStringLiteral("failed")
        ? std::optional<ThreadRuntimeStatus>(
              ThreadRuntimeStatus::NotLoaded)
        : threadRuntimeStatus(
              rawRuntimeStatus);
    const QVariantMap goalValue =
        process.value(QStringLiteral("goal"))
            .toMap();
    const auto parsedGoal =
        goalFromVariant(goalValue);
    const bool recoverableGoal =
        parsedGoal.has_value()
        && (parsedGoal->status
                == GoalStatus::Paused
            || parsedGoal->status
                == GoalStatus::Blocked
            || parsedGoal->status
                == GoalStatus::UsageLimited);
    if (processId.isEmpty()
        || threadId.isEmpty()
        || kind != QStringLiteral("thread")
        || status == QStringLiteral("running")
        || (status != QStringLiteral("failed")
            && !recoverableGoal)
        || !runtimeStatus.has_value()
        || (!goalValue.isEmpty()
            && !parsedGoal.has_value())) {
        shellViewModel_.finishProcessRetry(
            processId,
            false,
            QStringLiteral(
                "This Codex task is not safe to retry."));
        return;
    }
    if (!continuationHost_
        || !failedTaskRetryService_) {
        shellViewModel_.finishProcessRetry(
            processId,
            false,
            QStringLiteral(
                "The Codex retry service is unavailable."));
        return;
    }

    CodexFailedTaskRetryRequest request{
        threadId,
        process.value(
                   QStringLiteral(
                       "rolloutPath"))
            .toString()
            .trimmed(),
        *runtimeStatus,
        processUpdatedAtMilliseconds(
            process),
        parsedGoal,
    };
    CodexFailedTaskRetryService* service =
        failedTaskRetryService_.get();
    const QPointer<CompanionRuntimeHost>
        host(this);
    const auto submitted =
        continuationHost_->submit(
            [host,
             service,
             processId,
             threadId,
             request = std::move(
                 request)]() mutable {
                const auto result =
                    service->retry(
                        std::move(
                            request));
                if (host.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    host.data(),
                    [host,
                     processId,
                     threadId,
                     result]() mutable {
                        if (host.isNull()) {
                            return;
                        }
                        const bool succeeded =
                            result.disposition
                                == CodexFailedTaskRetryDisposition::
                                    Continued
                            || result.disposition
                                == CodexFailedTaskRetryDisposition::
                                    AlreadyContinued;
                        QString message =
                            result.message
                                .trimmed();
                        if (message.isEmpty()) {
                            switch (
                                result
                                    .disposition) {
                            case CodexFailedTaskRetryDisposition::
                                Continued:
                                message =
                                    result
                                            .destinationLabel
                                            .trimmed()
                                            .isEmpty()
                                    ? QStringLiteral(
                                          "Codex resumed the task.")
                                    : QStringLiteral(
                                          "Resumed with %1.")
                                          .arg(
                                              result
                                                  .destinationLabel);
                                break;
                            case CodexFailedTaskRetryDisposition::
                                AlreadyContinued:
                                message =
                                    QStringLiteral(
                                        "This task was already resumed.");
                                break;
                            case CodexFailedTaskRetryDisposition::
                                NotEligible:
                                message =
                                    QStringLiteral(
                                        "This Codex task is not safe to retry.");
                                break;
                            case CodexFailedTaskRetryDisposition::
                                Failed:
                                message =
                                    QStringLiteral(
                                        "Codex could not retry the task.");
                                break;
                            }
                        }
                        if (succeeded
                            && host
                                ->processListModel_) {
                            host
                                ->processListModel_
                                ->markFailureHandled(
                                    processId,
                                    threadId);
                        }
                        host->shellViewModel_
                            .finishProcessRetry(
                                processId,
                                succeeded,
                                message);
                        if (succeeded
                            && host->runtime_) {
                            host->runtime_
                                ->refreshNow();
                        } else if (
                            result.disposition
                            == CodexFailedTaskRetryDisposition::
                                Failed) {
                            host->reportRuntimeError({
                                QStringLiteral(
                                    "codex.failed_task_retry_failed"),
                                message,
                                true,
                                {},
                            });
                        }
                    },
                    Qt::QueuedConnection);
            });
    if (!submitted.hasValue()) {
        shellViewModel_.finishProcessRetry(
            processId,
            false,
            submitted.error().message);
    }
}

void CompanionRuntimeHost::handleProcessCommandFinished(
    const QString& command,
    quint64 executionId,
    bool succeeded,
    const QString& errorCode,
    const QString& message)
{
    if (executionId == 0) {
        return;
    }
    PendingProcessExecution* pending = nullptr;
    if (pendingProcessMessage_.active()
        && command
            == pendingProcessMessage_.command
        && executionId
            == pendingProcessMessage_
                .executionId) {
        pending = &pendingProcessMessage_;
    } else if (
        pendingProcessApproval_.active()
        && command
            == pendingProcessApproval_.command
        && executionId
            == pendingProcessApproval_
                .executionId) {
        pending = &pendingProcessApproval_;
    }
    if (pending == nullptr) {
        return;
    }

    const QString completedProcessId =
        pending->processId;

    if (pending->kind
        == PendingProcessCommand::
            ApprovalFeedbackDecline) {
        if (succeeded) {
            pending->kind =
                PendingProcessCommand::
                    ApprovalFeedbackReply;
            pending->command =
                QStringLiteral("codex.reply");
            if (!executePendingProcessCommand(
                    *pending,
                    pending->arguments)) {
                clearPendingProcessCommand(
                    *pending);
                shellViewModel_.finishProcessMessage(
                    false,
                    QStringLiteral(
                        "The Codex reply could not be started."));
            }
            return;
        }

        clearPendingProcessCommand(
            *pending);
        shellViewModel_.finishProcessMessage(
            false,
            message);
    } else if (
        pending->kind
        == PendingProcessCommand::Approval) {
        clearPendingProcessCommand(
            *pending);
        shellViewModel_.finishProcessApproval(
            succeeded,
            message);
    } else {
        clearPendingProcessCommand(
            *pending);
        if (succeeded
            && processListModel_) {
            processListModel_
                ->markFailureHandled(
                    completedProcessId);
        }
        shellViewModel_.finishProcessMessage(
            succeeded,
            message);
    }

    if (succeeded && runtime_) {
        runtime_->refreshNow();
    }
    if (!succeeded) {
        reportRuntimeError({
            errorCode.trimmed().isEmpty()
                ? QStringLiteral(
                      "codex.process_action_failed")
                : errorCode,
            message.trimmed().isEmpty()
                ? QStringLiteral(
                      "The Codex process action failed.")
                : message,
            false,
            {},
        });
    }
}

void CompanionRuntimeHost::refreshChatAvailability()
{
    const QString modelId =
        shellViewModel_.selectedChatModelId();
    const QString response =
        shellViewModel_.chatResponse();
    const QString responsePrompt =
        shellViewModel_.chatResponsePrompt();
    const QString responseTitle =
        shellViewModel_.chatResponseTitle();
    const QString responseUsageSummary =
        shellViewModel_.chatResponseUsageSummary();

    if (modelId == QStringLiteral("on-device")) {
        if (!onDeviceBackend_) {
            shellViewModel_.setChatStatus(
                false,
                false,
                false,
                response,
                QStringLiteral("Windows on-device chat is unavailable"),
                responsePrompt,
                responseTitle,
                responseUsageSummary);
            return;
        }

        try {
            const auto status = onDeviceBackend_->status();
            shellViewModel_.setChatStatus(
                status.phase == WindowsOnDeviceChatPhase::Ready
                    && status.available,
                canPrepare(status),
                isPreparing(status),
                response,
                onDeviceStatusMessage(status),
                responsePrompt,
                responseTitle,
                responseUsageSummary);
        } catch (...) {
            shellViewModel_.setChatStatus(
                false,
                false,
                false,
                response,
                QStringLiteral("Could not read on-device chat status"),
                responsePrompt,
                responseTitle,
                responseUsageSummary);
        }
        return;
    }

    if (ChatCredentialAvailability::refresh(
            shellViewModel_,
            credentialStore_,
            modelId)) {
        return;
    }

    shellViewModel_.setChatStatus(
        false,
        false,
        false,
        response,
        QStringLiteral(
            "The selected chat model is unavailable"),
        responsePrompt,
        responseTitle,
        responseUsageSummary);
}

void CompanionRuntimeHost::prepareOnDeviceChat()
{
    if (!onDeviceBackend_ || preparationWatcher_.isRunning()) {
        return;
    }

    const auto consent =
        onDeviceBackend_->setDownloadConsent(true);
    if (!consent.hasValue()) {
        shellViewModel_.setChatStatus(
            false,
            true,
            false,
            shellViewModel_.chatResponse(),
            consent.error().message,
            shellViewModel_.chatResponsePrompt(),
            shellViewModel_.chatResponseTitle(),
            shellViewModel_.chatResponseUsageSummary());
        reportRuntimeError(consent.error());
        return;
    }

    shellViewModel_.setChatStatus(
        false,
        false,
        true,
        shellViewModel_.chatResponse(),
        QStringLiteral("Preparing the Windows on-device model"),
        shellViewModel_.chatResponsePrompt(),
        shellViewModel_.chatResponseTitle(),
        shellViewModel_.chatResponseUsageSummary());
    preparationWatcher_.setFuture(
        onDeviceBackend_->prepare());
}

void CompanionRuntimeHost::sendLocalChat(
    QString prompt,
    QString modelId)
{
    if ((!chatSender_ && !chatService_)
        || chatWatcher_.isRunning()) {
        return;
    }

    const bool openAI =
        modelId.startsWith(QStringLiteral("openai:"));
    const bool lumo =
        modelId.startsWith(QStringLiteral("lumo:"));
    const QString responseTitle =
        shellViewModel_.chatModelTitle(modelId);
    if (openAI || lumo) {
        const ChatProvider provider = openAI
            ? ChatProvider::OpenAIAPI
            : ChatProvider::LumoAPI;
        const bool hasCredential =
            credentialStore_
            && ChatService::hasUsableCredential(
                *credentialStore_,
                provider);
        if (!hasCredential) {
            emit petAnimationRequested(
                QStringLiteral("waiting"));
            shellViewModel_.setChatStatus(
                true,
                false,
                false,
                openAI
                    ? QStringLiteral(
                          "I did not open ChatGPT. To answer here, paste your OpenAI API key into Codex Companion Settings. ChatGPT Pro and API billing are separate.")
                    : QStringLiteral(
                          "Create a Lumo API key from Lumo's API Docs, then paste it into Codex Companion Settings. Lumo API access is included with Lumo+."),
                openAI
                    ? QStringLiteral(
                          "Add an OpenAI API key in Settings to answer inside Companion.")
                    : QStringLiteral(
                          "Add a Lumo API key in Settings to answer inside Companion."),
                prompt,
                responseTitle,
                {});
            return;
        }
    }

    pendingChatPrompt_ = prompt;
    pendingChatTitle_ = responseTitle;
    pendingChatModelId_ = modelId;
    shellViewModel_.setChatStatus(
        false,
        false,
        true,
        QStringLiteral("Thinking..."),
        QStringLiteral("Thinking..."),
        pendingChatPrompt_,
        pendingChatTitle_,
        {});
    emit petAnimationRequested(
        modelId == QStringLiteral("on-device")
            ? QStringLiteral("waiting")
            : QStringLiteral("waving"));
    const ChatRequest request =
        chatRequest(
            std::move(prompt),
            modelId);
    chatWatcher_.setFuture(
        chatSender_
            ? chatSender_(request)
            : chatService_->send(request));
}

void CompanionRuntimeHost::completeChatRequest()
{
    if (chatWatcher_.future().isCanceled()
        || chatWatcher_.future().resultCount() != 1) {
        const auto error = runtimeHostError(
            QStringLiteral("chat.canceled"),
            QStringLiteral("The local chat request did not finish."));
        refreshChatAvailability();
        shellViewModel_.setChatStatus(
            shellViewModel_.chatSendEnabled(),
            shellViewModel_.chatPreparationEnabled(),
            false,
            error.message,
            error.message,
            pendingChatPrompt_,
            pendingChatTitle_,
            {});
        pendingChatPrompt_.clear();
        pendingChatTitle_.clear();
        pendingChatModelId_.clear();
        emit petAnimationRequested(
            QStringLiteral("failed"));
        reportRuntimeError(error);
        return;
    }

    const auto result = chatWatcher_.future().result();
    if (!result.hasValue()) {
        refreshChatAvailability();
        shellViewModel_.setChatStatus(
            shellViewModel_.chatSendEnabled(),
            shellViewModel_.chatPreparationEnabled(),
            false,
            result.error().message,
            result.error().message,
            pendingChatPrompt_,
            pendingChatTitle_,
            {});
        pendingChatPrompt_.clear();
        pendingChatTitle_.clear();
        pendingChatModelId_.clear();
        emit petAnimationRequested(
            QStringLiteral("failed"));
        reportRuntimeError(result.error());
        return;
    }

    refreshChatAvailability();
    shellViewModel_.setChatStatus(
        shellViewModel_.chatSendEnabled(),
        shellViewModel_.chatPreparationEnabled(),
        false,
        result.value().text,
        QStringLiteral("Ready"),
        pendingChatPrompt_,
        pendingChatTitle_,
        chatUsageSummary(
            result.value(),
            pendingChatModelId_
                == QStringLiteral("on-device")));
    pendingChatPrompt_.clear();
    pendingChatTitle_.clear();
    pendingChatModelId_.clear();
    emit petAnimationRequested(
        QStringLiteral("review"));
}

void CompanionRuntimeHost::completeOnDevicePreparation()
{
    if (!preparationWatcher_.future().isCanceled()
        && preparationWatcher_.future().resultCount() == 1) {
        const auto result =
            preparationWatcher_.future().result();
        if (!result.hasValue()) {
            refreshChatAvailability();
            shellViewModel_.setChatStatus(
                false,
                true,
                false,
                shellViewModel_.chatResponse(),
                result.error().message,
                shellViewModel_.chatResponsePrompt(),
                shellViewModel_.chatResponseTitle(),
                shellViewModel_.chatResponseUsageSummary());
            reportRuntimeError(result.error());
            return;
        }
    }

    refreshChatAvailability();
}

void CompanionRuntimeHost::handleOnDeviceStatus(
    WindowsOnDeviceChatStatus)
{
    if (shellViewModel_.selectedChatModelId()
        == QStringLiteral("on-device")) {
        refreshChatAvailability();
    }
}

void CompanionRuntimeHost::updateGoal(
    QString threadId,
    QString objective)
{
    QVariantMap arguments{
        {QStringLiteral("threadId"), threadId},
        {QStringLiteral("goalObjective"),
         std::move(objective)},
    };
    executeGoalMutation(
        QStringLiteral("codex.goal.update"),
        std::move(threadId),
        std::move(arguments));
}

void CompanionRuntimeHost::pauseGoal(QString threadId)
{
    QVariantMap arguments{
        {QStringLiteral("threadId"), threadId},
    };
    executeGoalMutation(
        QStringLiteral("codex.goal.pause"),
        std::move(threadId),
        std::move(arguments));
}

void CompanionRuntimeHost::resumeGoal(QString threadId)
{
    QVariantMap arguments{
        {QStringLiteral("threadId"), threadId},
    };
    executeGoalMutation(
        QStringLiteral("codex.goal.resume"),
        std::move(threadId),
        std::move(arguments));
}

void CompanionRuntimeHost::executeGoalMutation(
    QString command,
    QString threadId,
    QVariantMap arguments)
{
    if (!commandBus_) {
        shellViewModel_.finishGoalMutation(
            false,
            QStringLiteral(
                "The Codex goal service is unavailable."));
        return;
    }
    if (!pendingGoalCommand_.isEmpty()) {
        shellViewModel_.finishGoalMutation(
            false,
            QStringLiteral(
                "Another goal update is already running."));
        return;
    }

    pendingGoalCommand_ = std::move(command);
    pendingGoalThreadId_ =
        std::move(threadId);
    commandBus_->execute(
        pendingGoalCommand_,
        arguments);
}

void CompanionRuntimeHost::handleGoalCommandFinished(
    const QString& command,
    bool succeeded,
    const QString& errorCode,
    const QString& message)
{
    if (command != pendingGoalCommand_) {
        return;
    }

    pendingGoalCommand_.clear();
    pendingGoalThreadId_.clear();
    shellViewModel_.finishGoalMutation(
        succeeded,
        message);
    if (!succeeded) {
        reportRuntimeError({
            errorCode.isEmpty()
                ? QStringLiteral(
                      "goal.update_failed")
                : errorCode,
            message.isEmpty()
                ? QStringLiteral(
                      "The goal could not be updated.")
                : message,
            false,
            {},
        });
    }
}

void CompanionRuntimeHost::handleUsageCommandFinished(
    const QString& command,
    bool succeeded,
    const QString& errorCode,
    const QString& message)
{
    if (command
        == QStringLiteral(
            "codex.usage.consume-reset")) {
        if (succeeded) {
            return;
        }
        const QString failureMessage =
            message.trimmed().isEmpty()
            ? QStringLiteral(
                  "Could not apply the Codex usage reset.")
            : message;
        shellViewModel_.finishUsageReset(
            false,
            failureMessage);
        reportRuntimeError({
            errorCode.trimmed().isEmpty()
                ? QStringLiteral(
                      "codex.usage_reset_failed")
                : errorCode,
            failureMessage,
            false,
            {},
        });
        return;
    }
    if (command
        != QStringLiteral("codex.usage.load")) {
        return;
    }
    if (succeeded) {
        refreshUsageStatus();
        return;
    }
    shellViewModel_.setUsageStatus(
        false,
        {},
        message.trimmed().isEmpty()
            ? QStringLiteral(
                  "Codex usage is unavailable.")
            : message);
}

void CompanionRuntimeHost::handleUsageResetFinished(
    UsageResetOutcome outcome)
{
    QString message;
    switch (outcome) {
    case UsageResetOutcome::Reset:
        message =
            QStringLiteral(
                "Codex usage reset applied.");
        break;
    case UsageResetOutcome::NothingToReset:
        message =
            QStringLiteral(
                "There is currently no Codex limit to reset.");
        break;
    case UsageResetOutcome::NoCredit:
        message =
            QStringLiteral(
                "That Codex reset is no longer available.");
        break;
    case UsageResetOutcome::AlreadyRedeemed:
        message =
            QStringLiteral(
                "That Codex reset was already used.");
        break;
    }
    shellViewModel_.finishUsageReset(
        true,
        std::move(message));
}

void CompanionRuntimeHost::handleGoalChanged(
    const BridgeGoal& goal)
{
    if (!pendingGoalThreadId_.isEmpty()) {
        if (goal.threadId.trimmed()
            != pendingGoalThreadId_) {
            return;
        }
        shellViewModel_.applyGoalMutationResult(
            goalVariant(goal));
    } else {
        shellViewModel_.applyGoalSnapshot(
            goalVariant(goal));
    }
}

void CompanionRuntimeHost::refreshOpenGoalControls()
{
    if (!state_
        || !shellViewModel_.goalControlVisible()) {
        return;
    }

    const QString threadId =
        shellViewModel_.goalThreadId().trimmed();
    if (threadId.isEmpty()) {
        return;
    }

    const QVector<BridgeTask>& tasks =
        state_->tasks()->snapshot();
    for (const BridgeTask& task : tasks) {
        if (task.id.trimmed() != threadId) {
            continue;
        }
        if (task.goal.has_value()) {
            shellViewModel_.applyGoalSnapshot(
                goalVariant(*task.goal));
        } else {
            shellViewModel_.removeGoalSnapshot(
                threadId);
        }
        return;
    }

    shellViewModel_.removeGoalSnapshot(threadId);
}

void CompanionRuntimeHost::
scheduleAutomaticAccountContinuations()
{
    if (!automaticAccountContinuationEnabled_
        || automaticAccountContinuationPending_
        || !automaticContinuationCoordinator_
        || !continuationHost_
        || !processListModel_) {
        return;
    }

    QVector<
        CodexAutomaticContinuationCandidate>
        candidates;
    for (const ProcessListItem& item :
         processListModel_->snapshot()) {
        if (item.kind
                != QStringLiteral("thread")
            || item.status
                != TaskStatus::Failed
            || item.threadId
                   .trimmed()
                   .isEmpty()
            || item.rolloutPath
                   .trimmed()
                   .isEmpty()
            || !item.runtimeStatus
                    .has_value()
            || (*item.runtimeStatus
                    != ThreadRuntimeStatus::
                        Idle
                && *item.runtimeStatus
                    != ThreadRuntimeStatus::
                        NotLoaded)) {
            continue;
        }
        candidates.append({
            item.threadId,
            item.rolloutPath,
            *item.runtimeStatus,
        });
    }
    if (candidates.isEmpty()) {
        return;
    }

    automaticContinuationStopSource_ =
        std::stop_source{};
    const std::stop_token stopToken =
        automaticContinuationStopSource_
            .get_token();
    automaticAccountContinuationPending_ =
        true;
    CodexAutomaticAccountContinuationCoordinator*
        coordinator =
            automaticContinuationCoordinator_
                .get();
    const QPointer<CompanionRuntimeHost>
        host(this);
    const auto submitted =
        continuationHost_->submit(
            [host,
             coordinator,
             candidates = std::move(
                 candidates),
             stopToken]() mutable {
                auto outcomes =
                    coordinator
                        ->continueEligible(
                            true,
                            candidates,
                            stopToken);
                if (host.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    host.data(),
                    [host,
                     outcomes =
                         std::move(
                             outcomes)]() mutable {
                        if (!host.isNull()) {
                            host
                                ->finishAutomaticAccountContinuations(
                                    std::move(
                                        outcomes));
                        }
                    },
                    Qt::QueuedConnection);
            });
    if (!submitted.hasValue()) {
        automaticAccountContinuationPending_ =
            false;
        reportRuntimeError(
            submitted.error());
    }
}

void CompanionRuntimeHost::
finishAutomaticAccountContinuations(
    QVector<
        CodexAutomaticContinuationOutcome>
        outcomes)
{
    automaticAccountContinuationPending_ =
        false;
    for (const auto& outcome : outcomes) {
        if (processListModel_) {
            processListModel_
                ->markFailureHandled(
                    outcome.threadId,
                    outcome.threadId);
        }
        shellViewModel_.finishProcessRetry(
            outcome.threadId,
            true,
            outcome.destinationLabel
                    .trimmed()
                    .isEmpty()
                ? QStringLiteral(
                      "Codex continued the task.")
                : QStringLiteral(
                      "Continued with %1.")
                      .arg(
                          outcome
                              .destinationLabel));
    }
    if (!outcomes.isEmpty()
        && runtime_) {
        runtime_->refreshNow();
    }
}

void CompanionRuntimeHost::refreshProcessModel()
{
    if (!runtime_ || !processListModel_) {
        return;
    }
    processListModel_->setSnapshot(
        runtime_->processSnapshot());
    scheduleAutomaticAccountContinuations();
}

ChatRequest CompanionRuntimeHost::chatRequest(
    QString prompt,
    const QString& modelId) const
{
    ChatRequest request;
    request.prompt = std::move(prompt);
    if (modelId.startsWith(QStringLiteral("openai:"))) {
        request.provider = ChatProvider::OpenAIAPI;
        request.modelId =
            modelId.sliced(QStringLiteral("openai:").size());
    } else if (modelId.startsWith(QStringLiteral("lumo:"))) {
        request.provider = ChatProvider::LumoAPI;
        request.modelId =
            modelId.sliced(QStringLiteral("lumo:").size());
    } else {
        request.provider = ChatProvider::OnDevice;
        request.modelId = QStringLiteral("on-device");
    }
    return request;
}

void CompanionRuntimeHost::reportRuntimeError(
    const CompanionError& error)
{
    emit runtimeErrorOccurred(error);
}

std::unique_ptr<CompanionRuntimeHost>
detail::CompanionRuntimeHostTestAccess::create(
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
    CompanionRuntimeHost::ChatRequestSender
        chatSender,
    QObject* parent)
{
    return std::unique_ptr<
        CompanionRuntimeHost>(
            new CompanionRuntimeHost(
                shellViewModel,
                std::move(state),
                std::move(commandBus),
                std::move(runtime),
                std::move(credentialStore),
                std::move(onDeviceBackend),
                std::move(chatSender),
                parent));
}

} // namespace companion
