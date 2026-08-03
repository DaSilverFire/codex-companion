#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "codex/ipc/FollowerRequestFactory.h"
#include "core/Result.h"

#include <QFuture>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <chrono>
#include <functional>
#include <memory>

namespace companion {

using FollowerCandidateProvider =
    std::function<QVector<QString>()>;
using FollowerServerVerifier =
    std::function<bool(quint32 processId)>;
using FollowerQueuedStateLoader =
    std::function<Result<QJsonObject>()>;

class FollowerClient final {
public:
    explicit FollowerClient(
        const CodexEnvironment& environment);

    FollowerClient(
        FollowerCandidateProvider candidateProvider,
        std::chrono::milliseconds connectTimeout,
        std::chrono::milliseconds responseTimeout,
        FollowerServerVerifier serverVerifier,
        FollowerQueuedStateLoader queuedStateLoader = {});

    ~FollowerClient();

    FollowerClient(const FollowerClient&) = delete;
    FollowerClient& operator=(const FollowerClient&) = delete;
    FollowerClient(FollowerClient&&) noexcept = default;
    FollowerClient& operator=(FollowerClient&&) noexcept = default;

    static constexpr std::chrono::milliseconds
    defaultConnectTimeout()
    {
        return std::chrono::milliseconds(500);
    }

    static constexpr std::chrono::milliseconds
    defaultResponseTimeout()
    {
        return std::chrono::seconds(45);
    }

    static FollowerServerVerifier trustedServerVerifier(
        const CodexEnvironment& environment);

    QFuture<FollowerSendOutcome> updateThreadSettings(
        QString threadId,
        QString model,
        QString reasoningEffort) const;

    QFuture<FollowerSendOutcome> submit(
        QString prompt,
        QString threadId,
        SendAction action,
        QString clientMessageId,
        QString cwd,
        QVector<StagedAttachment> attachments) const;

    QFuture<FollowerSendOutcome> queueReply(
        QString prompt,
        QString threadId,
        QString clientMessageId,
        QString cwd,
        QVector<StagedAttachment> attachments) const;

    QFuture<FollowerApprovalOutcome> respondToApproval(
        PendingApproval request,
        ApprovalDecision decision) const;

private:
    struct State;
    std::shared_ptr<const State> state_;
};

} // namespace companion
