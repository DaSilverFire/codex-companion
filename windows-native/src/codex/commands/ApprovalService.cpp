#include "codex/commands/ApprovalService.h"

#include <QJsonArray>
#include <QPromise>
#include <QThread>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace {

Result<void> approvalFailure(
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

QThreadPool* approvalOrchestrationPool()
{
    static QThreadPool* pool = [] {
        auto* result = new QThreadPool();
        result->setMaxThreadCount(
            std::max(2, QThread::idealThreadCount()));
        return result;
    }();
    return pool;
}

enum class ApprovalWaitStatus {
    Outcome,
    Canceled,
    Malformed,
};

struct ApprovalWaitResult final {
    ApprovalWaitStatus status = ApprovalWaitStatus::Malformed;
    FollowerApprovalOutcome outcome = FollowerApprovalOutcome::Failed;
};

ApprovalWaitResult waitForApproval(
    QFuture<FollowerApprovalOutcome>& future)
{
    if (!future.isValid()) {
        return {ApprovalWaitStatus::Malformed};
    }
    while (!future.isFinished() && !future.isCanceled()) {
        QThread::msleep(1);
    }
    if (future.isCanceled() || future.resultCount() < 1) {
        return {ApprovalWaitStatus::Malformed};
    }
    try {
        return {
            ApprovalWaitStatus::Outcome,
            future.result(),
        };
    } catch (...) {
        return {ApprovalWaitStatus::Malformed};
    }
}

Result<void> approvalResult(
    FollowerApprovalOutcome outcome,
    const PendingApproval& request)
{
    switch (outcome) {
    case FollowerApprovalOutcome::Approved:
    case FollowerApprovalOutcome::Declined:
        return Result<void>::success();
    case FollowerApprovalOutcome::RequestNotFound:
        return approvalFailure(
            QStringLiteral("approval.request_not_found"),
            QStringLiteral("The Codex approval request is no longer pending."),
            {{QStringLiteral("threadId"), request.threadId}});
    case FollowerApprovalOutcome::SharedDaemonUnavailable:
        return approvalFailure(
            QStringLiteral("approval.shared_daemon_unavailable"),
            QStringLiteral("Codex shared daemon is unavailable."),
            {{QStringLiteral("threadId"), request.threadId}});
    case FollowerApprovalOutcome::TimedOut:
        return approvalFailure(
            QStringLiteral("approval.timed_out"),
            QStringLiteral("Codex did not acknowledge the approval in time."),
            {{QStringLiteral("threadId"), request.threadId}});
    case FollowerApprovalOutcome::Failed:
        return approvalFailure(
            QStringLiteral("approval.failed"),
            QStringLiteral("Codex could not apply the approval decision."),
            {{QStringLiteral("threadId"), request.threadId}});
    }
    return approvalFailure(
        QStringLiteral("approval.failed"),
        QStringLiteral("Codex could not apply the approval decision."),
        {{QStringLiteral("threadId"), request.threadId}});
}

Result<void> approvalExceptionFailure(
    const PendingApproval& request)
{
    return approvalFailure(
        QStringLiteral("approval.failed"),
        QStringLiteral("Codex could not apply the approval decision."),
        {{QStringLiteral("threadId"), request.threadId}});
}

std::optional<qint64> numericId(
    const QJsonObject& message)
{
    const QJsonValue value =
        message.value(QStringLiteral("id"));
    if (value.isString()) {
        bool ok = false;
        const qlonglong id =
            value.toString().toLongLong(&ok);
        if (ok) {
            return id;
        }
        return std::nullopt;
    }
    if (value.isDouble()) {
        bool ok = false;
        const qlonglong id =
            value.toVariant().toLongLong(&ok);
        if (ok) {
            return id;
        }
    }
    return std::nullopt;
}

std::optional<QVector<QString>> amendmentFrom(
    const QJsonObject& params,
    const QString& key)
{
    const QJsonValue value = params.value(key);
    if (!value.isArray()) {
        return std::nullopt;
    }
    QVector<QString> result;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) {
            return std::nullopt;
        }
        result.append(item.toString());
    }
    return result;
}

} // namespace

ApprovalService::ApprovalService(
    const CodexEnvironment& environment)
    : ApprovalService(
          [client = std::make_shared<FollowerClient>(
               environment)](
              PendingApproval request,
              ApprovalDecision decision) {
              return client->respondToApproval(
                  std::move(request),
                  decision);
          })
{
}

ApprovalService::ApprovalService(
    ApprovalResponder responder,
    ApprovalRemoval removal,
    ApprovalCommitProbe commitProbe)
    : responder_(std::move(responder)),
      removal_(std::move(removal)),
      commitProbe_(std::move(commitProbe))
{
}

CommitAwareMutationHandle<void>
ApprovalService::respondMutation(
    const PendingApproval& request,
    ApprovalDecision decision) const
{
    auto mutation =
        CommitAwareMutation<void>::create();
    CommitAwareMutationHandle<void> handle =
        mutation->handle();
    try {
        approvalOrchestrationPool()->start(
        [responder = responder_,
         removal = removal_,
         commitProbe = commitProbe_,
         request,
         decision,
         mutation] {
            const auto probe =
                [&commitProbe](const QString& phase) noexcept {
                if (!commitProbe) {
                    return;
                }
                try {
                    commitProbe(phase);
                } catch (...) {
                }
            };

            try {
                if (!responder) {
                    mutation->finish(
                        approvalFailure(
                        QStringLiteral("approval.failed"),
                        QStringLiteral(
                            "Codex approval transport is unavailable."),
                        {{QStringLiteral("threadId"), request.threadId}}));
                    return;
                }
                probe(QStringLiteral(
                    "responder.commitPending"));
                probe(QStringLiteral(
                    "responder.claimEstablished"));
                if (!mutation->tryCommit()) {
                    return;
                }
                probe(QStringLiteral(
                    "responder.committed"));
                QFuture<FollowerApprovalOutcome> future =
                    responder(request, decision);
                const ApprovalWaitResult wait =
                    waitForApproval(future);
                if (wait.status == ApprovalWaitStatus::Malformed) {
                    mutation->finish(
                        approvalExceptionFailure(request));
                    return;
                }
                const FollowerApprovalOutcome outcome =
                    wait.outcome;
                Result<void> result =
                    approvalResult(outcome, request);
                if (result.hasValue()
                    && (outcome
                            == FollowerApprovalOutcome::Approved
                        || outcome
                            == FollowerApprovalOutcome::Declined)
                    && removal) {
                    probe(QStringLiteral("commitPending"));
                    probe(QStringLiteral("claimEstablished"));
                    probe(QStringLiteral("committed"));
                    removal(request);
                }
                mutation->finish(std::move(result));
            } catch (...) {
                mutation->finish(
                    approvalExceptionFailure(request));
            }
        });
    } catch (...) {
        mutation->finish(
            approvalExceptionFailure(request));
    }
    return handle;
}

QFuture<Result<void>> ApprovalService::respond(
    const PendingApproval& request,
    ApprovalDecision decision) const
{
    return cancellationDetachedMutationFuture(
        respondMutation(
            request,
            decision)
            .terminalFuture);
}

std::optional<PendingApproval>
ApprovalService::pendingApprovalFromNotification(
    const QJsonObject& message)
{
    PendingApprovalMethod method;
    const QString rawMethod =
        message.value(QStringLiteral("method"))
            .toString();
    if (rawMethod
        == QStringLiteral(
            "item/commandExecution/requestApproval")) {
        method = PendingApprovalMethod::CommandExecution;
    } else if (rawMethod
               == QStringLiteral(
                   "item/fileChange/requestApproval")) {
        method = PendingApprovalMethod::FileChange;
    } else {
        return std::nullopt;
    }

    const std::optional<qint64> id =
        numericId(message);
    const QJsonObject params =
        message.value(QStringLiteral("params"))
            .toObject();
    const QString threadId =
        params.value(QStringLiteral("threadId"))
            .toString();
    if (!id.has_value() || threadId.isEmpty()) {
        return std::nullopt;
    }

    std::optional<QVector<QString>> amendment =
        amendmentFrom(
            params,
            QStringLiteral(
                "proposedExecpolicyAmendment"));
    if (!amendment.has_value()) {
        amendment = amendmentFrom(
            params,
            QStringLiteral(
                "proposed_execpolicy_amendment"));
    }
    if (amendment.has_value() && amendment->isEmpty()) {
        amendment = std::nullopt;
    }

    return PendingApproval{
        threadId,
        *id,
        method,
        std::move(amendment),
    };
}

} // namespace companion
