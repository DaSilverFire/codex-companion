#pragma once

#include "codex/chat/ChatService.h"
#include "codex/commands/ApprovalService.h"
#include "codex/commands/CommitAwareMutation.h"
#include "codex/commands/TaskCommandService.h"
#include "codex/commands/UsageService.h"

#include <QString>
#include <QUuid>
#include <QVector>

#include <functional>
#include <optional>

namespace companion {

struct RuntimeTaskCreateRequest final {
    QString text;
    QString cwd;
    QString clientMessageId;
    QString model;
    QString reasoningEffort;
    QString skillName;
    QString skillPath;
    QVector<BridgeAttachment> attachments;
};

enum class RuntimeGoalMutationKind {
    Create,
    Update,
    Pause,
    Resume,
};

struct RuntimeGoalMutationRequest final {
    RuntimeGoalMutationKind kind =
        RuntimeGoalMutationKind::Create;
    QString threadId;
    std::optional<QString> objective;
    std::optional<qint64> tokenBudget;
};

using RuntimeSendMutationStarter =
    std::function<CommitAwareMutationHandle<void>(
        SendRequest)>;
using RuntimeApprovalMutationStarter =
    std::function<CommitAwareMutationHandle<void>(
        PendingApproval,
        ApprovalDecision)>;
using RuntimeTaskCreateMutationStarter =
    std::function<CommitAwareMutationHandle<QString>(
        RuntimeTaskCreateRequest)>;
using RuntimeChatMutationStarter =
    std::function<CommitAwareMutationHandle<ChatResult>(
        ChatRequest)>;
using RuntimeGoalMutationStarter =
    std::function<CommitAwareMutationHandle<BridgeGoal>(
        RuntimeGoalMutationRequest)>;
using RuntimeUsageResetMutationStarter =
    std::function<
        CommitAwareMutationHandle<UsageResetOutcome>(
            QString,
            QUuid)>;

struct CodexRuntimeMutationDependencies final {
    RuntimeSendMutationStarter sendMutationStarter;
    RuntimeApprovalMutationStarter
        approvalMutationStarter;
    RuntimeTaskCreateMutationStarter
        taskCreateMutationStarter;
    RuntimeChatMutationStarter chatMutationStarter;
    RuntimeGoalMutationStarter goalMutationStarter;
    RuntimeUsageResetMutationStarter
        usageResetMutationStarter;
};

enum class CodexRuntimeMode {
    Interactive,
    ReadOnlyProbe,
};

inline QString codexRuntimeOperationKeyArgument()
{
    return QStringLiteral(
        "_companionOperationKey");
}

} // namespace companion
