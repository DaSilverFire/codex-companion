#pragma once

#include "codex/attachments/AttachmentStore.h"
#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace companion {

inline constexpr qint64 kFollowerRouterTimeoutMilliseconds =
    30'000;

enum class FollowerSendOutcome {
    Sent,
    SharedDaemonUnavailable,
    TimedOut,
    ThreadNotLoaded,
    NoActiveTurn,
    Failed,
};

enum class FollowerApprovalOutcome {
    Approved,
    Declined,
    RequestNotFound,
    SharedDaemonUnavailable,
    TimedOut,
    Failed,
};

enum class PendingApprovalMethod {
    CommandExecution,
    FileChange,
};

struct PendingApproval final {
    QString threadId;
    qint64 requestId = 0;
    PendingApprovalMethod method =
        PendingApprovalMethod::CommandExecution;
    std::optional<QVector<QString>>
        proposedExecpolicyAmendment;

    friend bool operator==(
        const PendingApproval&,
        const PendingApproval&) = default;
};

class FollowerRequestFactory final {
public:
    static QJsonObject initialize(
        const QString& requestId);

    static QJsonObject threadSettings(
        const QString& requestId,
        const QString& clientId,
        const QString& threadId,
        const QString& model,
        const QString& reasoningEffort);

    static Result<QJsonObject> action(
        const QString& requestId,
        const QString& clientId,
        const QString& threadId,
        const QString& prompt,
        SendAction action,
        const QString& clientMessageId,
        const QString& cwd,
        const QVector<StagedAttachment>& attachments,
        qint64 createdAtMilliseconds);

    static Result<QJsonObject> queuedReply(
        const QString& requestId,
        const QString& clientId,
        const QString& threadId,
        const QString& prompt,
        const QString& clientMessageId,
        const QString& cwd,
        const QJsonObject& existingState,
        const QVector<StagedAttachment>& attachments,
        qint64 createdAtMilliseconds);

    static QJsonObject approval(
        const QString& requestId,
        const QString& clientId,
        const PendingApproval& request,
        ApprovalDecision decision);

    static Result<QJsonObject> loadQueuedFollowUpState(
        const QString& path);

    static FollowerSendOutcome sendOutcomeForError(
        const QString& error);
    static FollowerApprovalOutcome approvalOutcomeForError(
        const QString& error);
};

} // namespace companion
