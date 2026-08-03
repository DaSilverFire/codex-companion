#include "codex/ipc/FollowerRequestFactory.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <utility>

namespace companion {

namespace {

CompanionError requestError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

QJsonArray nativeAttachments(
    const QVector<StagedAttachment>& attachments)
{
    QJsonArray result;
    for (const StagedAttachment& attachment : attachments) {
        result.push_back(attachment.followerNative());
    }
    return result;
}

QJsonArray followerInput(
    const QString& prompt,
    const QVector<StagedAttachment>& attachments)
{
    QJsonArray result{
        QJsonObject({
            {
                QStringLiteral("type"),
                QStringLiteral("text"),
            },
            {QStringLiteral("text"), prompt},
            {
                QStringLiteral("text_elements"),
                QJsonArray{},
            },
        }),
    };
    for (const StagedAttachment& attachment : attachments) {
        const std::optional<QJsonObject> input =
            attachment.followerInput();
        if (input.has_value()) {
            result.push_back(*input);
        }
    }
    return result;
}

QJsonArray queuedFiles(
    const QVector<StagedAttachment>& attachments)
{
    QJsonArray result;
    for (const StagedAttachment& attachment : attachments) {
        const std::optional<QJsonObject> file =
            attachment.queuedFile();
        if (file.has_value()) {
            result.push_back(*file);
        }
    }
    return result;
}

QJsonArray queuedImages(
    const QVector<StagedAttachment>& attachments)
{
    QJsonArray result;
    for (const StagedAttachment& attachment : attachments) {
        const std::optional<QJsonObject> image =
            attachment.queuedImage();
        if (image.has_value()) {
            result.push_back(*image);
        }
    }
    return result;
}

QString normalizedWorkingDirectory(const QString& cwd)
{
    return cwd.trimmed();
}

QJsonObject restoreMessage(
    const QString& clientMessageId,
    const QString& prompt,
    const QString& cwd,
    const QVector<StagedAttachment>& attachments,
    qint64 createdAtMilliseconds)
{
    const QString workingDirectory =
        normalizedWorkingDirectory(cwd);
    const QJsonArray workspaceRoots =
        workingDirectory.isEmpty()
        ? QJsonArray{}
        : QJsonArray{workingDirectory};
    QJsonObject message{
        {QStringLiteral("id"), clientMessageId},
        {QStringLiteral("text"), prompt},
        {
            QStringLiteral("context"),
            QJsonObject({
                {QStringLiteral("prompt"), prompt},
                {
                    QStringLiteral("addedFiles"),
                    QJsonArray{},
                },
                {
                    QStringLiteral("fileAttachments"),
                    queuedFiles(attachments),
                },
                {
                    QStringLiteral("ideContext"),
                    QJsonValue(QJsonValue::Null),
                },
                {
                    QStringLiteral("imageAttachments"),
                    queuedImages(attachments),
                },
                {
                    QStringLiteral("workspaceRoots"),
                    workspaceRoots,
                },
            }),
        },
        {
            QStringLiteral("createdAt"),
            createdAtMilliseconds,
        },
    };
    if (!workingDirectory.isEmpty()) {
        message.insert(
            QStringLiteral("cwd"),
            workingDirectory);
    }
    return message;
}

QJsonObject regularRequest(
    const QString& requestId,
    const QString& clientId,
    const QString& method,
    QJsonObject parameters)
{
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("request"),
        },
        {QStringLiteral("requestId"), requestId},
        {QStringLiteral("sourceClientId"), clientId},
        {QStringLiteral("version"), 1},
        {QStringLiteral("method"), method},
        {
            QStringLiteral("params"),
            std::move(parameters),
        },
        {
            QStringLiteral("timeoutMs"),
            kFollowerRouterTimeoutMilliseconds,
        },
    };
}

Result<void> validateQueuedState(
    const QJsonObject& state)
{
    for (auto iterator = state.constBegin();
         iterator != state.constEnd();
         ++iterator) {
        if (!iterator.value().isArray()) {
            return Result<void>::failure(requestError(
                QStringLiteral(
                    "follower.queued_state_invalid"),
                QStringLiteral(
                    "The Codex queued follow-up state contains a malformed thread collection."),
                {
                    {
                        QStringLiteral("threadId"),
                        iterator.key(),
                    },
                }));
        }
        for (const QJsonValue& message :
             iterator.value().toArray()) {
            if (!message.isObject()) {
                return Result<void>::failure(requestError(
                    QStringLiteral(
                        "follower.queued_state_invalid"),
                    QStringLiteral(
                        "The Codex queued follow-up state contains a malformed message."),
                    {
                        {
                            QStringLiteral("threadId"),
                            iterator.key(),
                        },
                    }));
            }
        }
    }
    return Result<void>::success();
}

QJsonValue approvalDecision(
    ApprovalDecision decision,
    const PendingApproval& request)
{
    switch (decision) {
    case ApprovalDecision::ApproveOnce:
        return QStringLiteral("accept");
    case ApprovalDecision::ApproveSimilar:
        if (request.method
                == PendingApprovalMethod::CommandExecution
            && request.proposedExecpolicyAmendment.has_value()
            && !request.proposedExecpolicyAmendment->isEmpty()) {
            QJsonArray amendment;
            for (const QString& argument :
                 *request.proposedExecpolicyAmendment) {
                amendment.push_back(argument);
            }
            return QJsonObject({
                {
                    QStringLiteral(
                        "acceptWithExecpolicyAmendment"),
                    QJsonObject({
                        {
                            QStringLiteral(
                                "execpolicy_amendment"),
                            amendment,
                        },
                    }),
                },
            });
        }
        return QStringLiteral("acceptForSession");
    case ApprovalDecision::Decline:
        return QStringLiteral("decline");
    }
    return QStringLiteral("decline");
}

} // namespace

QJsonObject FollowerRequestFactory::initialize(
    const QString& requestId)
{
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("request"),
        },
        {QStringLiteral("requestId"), requestId},
        {
            QStringLiteral("sourceClientId"),
            QStringLiteral("initializing-client"),
        },
        {QStringLiteral("version"), 0},
        {
            QStringLiteral("method"),
            QStringLiteral("initialize"),
        },
        {
            QStringLiteral("params"),
            QJsonObject({
                {
                    QStringLiteral("clientType"),
                    QStringLiteral("codex-companion"),
                },
            }),
        },
    };
}

QJsonObject FollowerRequestFactory::threadSettings(
    const QString& requestId,
    const QString& clientId,
    const QString& threadId,
    const QString& model,
    const QString& reasoningEffort)
{
    QJsonObject settings;
    const QString normalizedModel = model.trimmed();
    if (!normalizedModel.isEmpty()) {
        settings.insert(
            QStringLiteral("model"),
            normalizedModel);
    }
    const QString normalizedEffort =
        reasoningEffort.trimmed();
    if (!normalizedEffort.isEmpty()) {
        settings.insert(
            QStringLiteral("effort"),
            normalizedEffort);
    }

    return regularRequest(
        requestId,
        clientId,
        QStringLiteral(
            "thread-follower-update-thread-settings"),
        {
            {
                QStringLiteral("conversationId"),
                threadId,
            },
            {
                QStringLiteral("threadSettings"),
                settings,
            },
        });
}

Result<QJsonObject> FollowerRequestFactory::action(
    const QString& requestId,
    const QString& clientId,
    const QString& threadId,
    const QString& prompt,
    SendAction action,
    const QString& clientMessageId,
    const QString& cwd,
    const QVector<StagedAttachment>& attachments,
    qint64 createdAtMilliseconds)
{
    const QJsonArray input =
        followerInput(prompt, attachments);
    const QJsonArray native =
        nativeAttachments(attachments);

    switch (action) {
    case SendAction::Reply:
        return Result<QJsonObject>::success(
            regularRequest(
                requestId,
                clientId,
                QStringLiteral(
                    "thread-follower-start-turn"),
                {
                    {
                        QStringLiteral("conversationId"),
                        threadId,
                    },
                    {
                        QStringLiteral("turnStartParams"),
                        QJsonObject({
                            {
                                QStringLiteral("input"),
                                input,
                            },
                            {
                                QStringLiteral(
                                    "clientUserMessageId"),
                                clientMessageId,
                            },
                            {
                                QStringLiteral("attachments"),
                                native,
                            },
                        }),
                    },
                }));
    case SendAction::Steer:
        return Result<QJsonObject>::success(
            regularRequest(
                requestId,
                clientId,
                QStringLiteral(
                    "thread-follower-steer-turn"),
                {
                    {
                        QStringLiteral("conversationId"),
                        threadId,
                    },
                    {
                        QStringLiteral(
                            "clientUserMessageId"),
                        clientMessageId,
                    },
                    {QStringLiteral("input"), input},
                    {
                        QStringLiteral("serviceTier"),
                        QJsonValue(QJsonValue::Null),
                    },
                    {
                        QStringLiteral("attachments"),
                        native,
                    },
                    {
                        QStringLiteral("restoreMessage"),
                        restoreMessage(
                            clientMessageId,
                            prompt,
                            cwd,
                            attachments,
                            createdAtMilliseconds),
                    },
                }));
    }

    return Result<QJsonObject>::failure(requestError(
        QStringLiteral("follower.action_invalid"),
        QStringLiteral(
            "The requested Codex follower action is invalid.")));
}

Result<QJsonObject> FollowerRequestFactory::queuedReply(
    const QString& requestId,
    const QString& clientId,
    const QString& threadId,
    const QString& prompt,
    const QString& clientMessageId,
    const QString& cwd,
    const QJsonObject& existingState,
    const QVector<StagedAttachment>& attachments,
    qint64 createdAtMilliseconds)
{
    const Result<void> valid =
        validateQueuedState(existingState);
    if (!valid.hasValue()) {
        return Result<QJsonObject>::failure(valid.error());
    }

    QJsonObject state = existingState;
    QJsonArray messages =
        state.value(threadId).toArray();
    bool duplicate = false;
    for (const QJsonValue& value : messages) {
        if (value.toObject()
                .value(QStringLiteral("id"))
                .toString()
            == clientMessageId) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        messages.push_back(restoreMessage(
            clientMessageId,
            prompt,
            cwd,
            attachments,
            createdAtMilliseconds));
    }
    state.insert(threadId, messages);

    return Result<QJsonObject>::success(
        regularRequest(
            requestId,
            clientId,
            QStringLiteral(
                "thread-follower-set-queued-follow-ups-state"),
            {
                {
                    QStringLiteral("conversationId"),
                    threadId,
                },
                {QStringLiteral("state"), state},
            }));
}

QJsonObject FollowerRequestFactory::approval(
    const QString& requestId,
    const QString& clientId,
    const PendingApproval& request,
    ApprovalDecision decision)
{
    const QString method = request.method
            == PendingApprovalMethod::CommandExecution
        ? QStringLiteral(
              "thread-follower-command-approval-decision")
        : QStringLiteral(
              "thread-follower-file-approval-decision");
    return regularRequest(
        requestId,
        clientId,
        method,
        {
            {
                QStringLiteral("conversationId"),
                request.threadId,
            },
            {
                QStringLiteral("requestId"),
                request.requestId,
            },
            {
                QStringLiteral("decision"),
                approvalDecision(decision, request),
            },
        });
}

Result<QJsonObject>
FollowerRequestFactory::loadQueuedFollowUpState(
    const QString& path)
{
    if (!QFileInfo::exists(path)) {
        return Result<QJsonObject>::success({});
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<QJsonObject>::failure(requestError(
            QStringLiteral(
                "follower.queued_state_unavailable"),
            QStringLiteral(
                "The Codex global state file could not be opened.")));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<QJsonObject>::failure(requestError(
            QStringLiteral(
                "follower.queued_state_invalid"),
            QStringLiteral(
                "The Codex global state file is not valid JSON.")));
    }

    const QJsonObject root = document.object();
    const QJsonValue rawState =
        root.value(QStringLiteral("queued-follow-ups"));
    if (rawState.isUndefined()) {
        return Result<QJsonObject>::success({});
    }
    if (!rawState.isObject()) {
        return Result<QJsonObject>::failure(requestError(
            QStringLiteral(
                "follower.queued_state_invalid"),
            QStringLiteral(
                "The Codex queued follow-up state must contain an object.")));
    }

    const QJsonObject state = rawState.toObject();
    const Result<void> valid = validateQueuedState(state);
    if (!valid.hasValue()) {
        return Result<QJsonObject>::failure(valid.error());
    }
    return Result<QJsonObject>::success(state);
}

FollowerSendOutcome
FollowerRequestFactory::sendOutcomeForError(
    const QString& error)
{
    const QString normalized = error.toLower();
    if (normalized.contains(
            QStringLiteral("no-client-found"))
        || normalized.contains(
            QStringLiteral("client-disconnected"))
        || normalized.contains(
            QStringLiteral("not being streamed"))
        || normalized.contains(
            QStringLiteral("no client found"))) {
        return FollowerSendOutcome::ThreadNotLoaded;
    }
    if (normalized.contains(QStringLiteral("timeout"))
        || normalized.contains(
            QStringLiteral("timed out"))) {
        return FollowerSendOutcome::TimedOut;
    }
    if (normalized.contains(
            QStringLiteral("active turn"))
        || normalized.contains(
            QStringLiteral("without an active"))
        || normalized.contains(
            QStringLiteral("no active turn"))) {
        return FollowerSendOutcome::NoActiveTurn;
    }
    return FollowerSendOutcome::Failed;
}

FollowerApprovalOutcome
FollowerRequestFactory::approvalOutcomeForError(
    const QString& error)
{
    const QString normalized = error.toLower();
    if (normalized.contains(
            QStringLiteral("request not found"))
        || normalized.contains(
            QStringLiteral("no pending approval"))
        || (normalized.contains(
                QStringLiteral("approval request"))
            && normalized.contains(
                QStringLiteral("not found")))) {
        return FollowerApprovalOutcome::RequestNotFound;
    }
    return FollowerApprovalOutcome::Failed;
}

} // namespace companion
