#pragma once

#include "codex/runtime/CodexRuntimeOperations.h"
#include "mobile/history/MobileHistoryCoordinator.h"

#include <QFuture>
#include <QHash>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace companion {

class MobilePresencePetCatalogService;

struct MobileTaskPage final {
    QVector<BridgeTask> tasks;
    std::optional<QString> nextCursor;

    friend bool operator==(
        const MobileTaskPage&,
        const MobileTaskPage&) = default;
};

using MobileGoalMap =
    QHash<QString, std::optional<BridgeGoal>>;
using MobileTaskPageLoader =
    std::function<QFuture<Result<MobileTaskPage>>(
        std::optional<QString>,
        qint64)>;
using MobileGoalLoader =
    std::function<QFuture<Result<MobileGoalMap>>(
        QVector<QString>)>;
using MobileHistoryLoader =
    std::function<QFuture<Result<HistorySnapshot>>(
        MobileHistoryKey)>;
using MobileCapabilityLoader =
    std::function<QFuture<Result<BridgeCapabilities>>(
        QString)>;
using MobileUsageLoader =
    std::function<
        QFuture<Result<BridgeUsageSnapshot>>()>;

struct MobileRequestReadDependencies final {
    MobileTaskPageLoader taskPageLoader;
    MobileGoalLoader goalLoader;
    MobileHistoryLoader historyLoader;
    MobileCapabilityLoader capabilityLoader;
    MobileUsageLoader usageLoader;
};

using MobileSendMessageMutation =
    std::function<QFuture<Result<void>>(
        SendRequest)>;
using MobileApprovalMutation =
    std::function<QFuture<Result<void>>(
        QString,
        ApprovalDecision)>;
using MobileTaskCreateMutation =
    std::function<QFuture<Result<QString>>(
        RuntimeTaskCreateRequest)>;
using MobileCasualChatMutation =
    std::function<QFuture<Result<ChatResult>>(
        ChatRequest)>;
using MobileUsageResetMutation =
    std::function<QFuture<Result<UsageResetOutcome>>(
        QString,
        QUuid)>;
using MobileGoalMutation =
    std::function<QFuture<Result<BridgeGoal>>(
        RuntimeGoalMutationRequest)>;

struct MobileRequestMutationDependencies final {
    MobileSendMessageMutation sendMessage;
    MobileApprovalMutation respondToApproval;
    MobileTaskCreateMutation createTask;
    MobileCasualChatMutation sendCasualChat;
    MobileUsageResetMutation consumeUsageReset;
    MobileGoalMutation mutateGoal;
};

using MobileRelayUrlProvider =
    std::function<std::optional<QString>()>;
using MobileNowProvider =
    std::function<BridgeDate()>;

struct MobileRequestDispatcherConfiguration final {
    QString hostName;
    QString hostDeviceId;
    MobileRelayUrlProvider relayUrlProvider;
    MobileNowProvider nowProvider;
    std::shared_ptr<
        MobilePresencePetCatalogService>
        presencePetCatalogService;
};

class MobileRequestDispatcher final {
public:
    MobileRequestDispatcher(
        MobileRequestReadDependencies reads,
        MobileRequestMutationDependencies mutations,
        MobileRequestDispatcherConfiguration
            configuration);

    QFuture<BridgeResponse> handle(
        BridgeRequest request) const;

private:
    struct State;
    std::shared_ptr<const State> state_;
};

} // namespace companion
