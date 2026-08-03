#pragma once

#include "codex/attachments/AttachmentStore.h"
#include "codex/commands/CommitAwareMutation.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/ipc/FollowerClient.h"
#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QFuture>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QEvent;

namespace companion {

struct SendExecutionState final {
    std::atomic_bool retainedCurrentSettings =
        false;
};

struct SendRequest final {
    QString prompt;
    QString threadId;
    QString cwd;
    SendAction action = SendAction::Reply;
    QString expectedTurnId;
    QString clientMessageId;
    QString model;
    QString reasoningEffort;
    QVector<BridgeAttachment> attachments;
    std::shared_ptr<SendExecutionState>
        executionState;
};

struct CommandDiagnostic final {
    QString threadId;
    QString method;
    QString outcome;
    qsizetype attachmentCount = 0;
    qint64 durationMilliseconds = 0;
};

enum class CommandCompletionPhase {
    BeforeAddResult,
    AfterAddResultBeforeFinish,
};

using CommandAttachmentValidator =
    std::function<Result<void>(
        const QVector<BridgeAttachment>&)>;
using CommandAttachmentStager =
    std::function<Result<QVector<StagedAttachment>>(
        const QVector<BridgeAttachment>&,
        const QString&)>;
using CommandOwnedAttachmentStager =
    std::function<Result<StagedAttachmentBatch>(
        const QVector<BridgeAttachment>&,
        const QUuid&)>;
using CommandSettingsSender =
    std::function<QFuture<FollowerSendOutcome>(
        QString,
        QString,
        QString)>;
using CommandSubmitter =
    std::function<QFuture<FollowerSendOutcome>(
        QString,
        QString,
        SendAction,
        QString,
        QString,
        QVector<StagedAttachment>)>;
using CommandQueuedReplySender =
    std::function<QFuture<FollowerSendOutcome>(
        QString,
        QString,
        QString,
        QString,
        QVector<StagedAttachment>)>;
using CommandDiagnosticSink =
    std::function<void(CommandDiagnostic)>;
using CommandCompletionHook =
    std::function<void(CommandCompletionPhase)>;

CommandOwnedAttachmentStager makeRetainedLegacyStager(
    CommandAttachmentStager legacy);

class TaskCommandService final : public QObject {
    Q_OBJECT

public:
    explicit TaskCommandService(
        const CodexEnvironment& environment,
        QObject* parent = nullptr);

    TaskCommandService(
        CommandAttachmentValidator validator,
        CommandAttachmentStager stager,
        CommandSettingsSender settingsSender,
        CommandSubmitter submitter,
        CommandQueuedReplySender queuedReplySender,
        CommandDiagnosticSink diagnosticSink = {},
        QObject* parent = nullptr);

    TaskCommandService(
        CommandAttachmentValidator validator,
        CommandAttachmentStager stager,
        CommandSettingsSender settingsSender,
        CommandSubmitter submitter,
        CommandQueuedReplySender queuedReplySender,
        CommandDiagnosticSink diagnosticSink,
        CommandCompletionHook completionHook,
        QObject* parent = nullptr);

    TaskCommandService(
        CommandAttachmentValidator validator,
        CommandOwnedAttachmentStager stager,
        CommandSettingsSender settingsSender,
        CommandSubmitter submitter,
        CommandQueuedReplySender queuedReplySender,
        CommandDiagnosticSink diagnosticSink = {},
        QObject* parent = nullptr);

    TaskCommandService(
        CommandAttachmentValidator validator,
        CommandOwnedAttachmentStager stager,
        CommandSettingsSender settingsSender,
        CommandSubmitter submitter,
        CommandQueuedReplySender queuedReplySender,
        CommandDiagnosticSink diagnosticSink,
        CommandCompletionHook completionHook,
        QObject* parent = nullptr);

    ~TaskCommandService() override;

    TaskCommandService(const TaskCommandService&) = delete;
    TaskCommandService& operator=(const TaskCommandService&) = delete;

    CommitAwareMutationHandle<void> sendMutation(
        const SendRequest& request);

    QFuture<Result<void>> send(
        const SendRequest& request);

signals:
    void replyQueued(
        const QString& threadId,
        const QString& clientMessageId);

private:
    bool event(QEvent* event) override;

    struct ReplyAffinityState;
    struct State;
    std::shared_ptr<ReplyAffinityState> replyAffinityState_;
    std::shared_ptr<State> state_;
};

} // namespace companion
