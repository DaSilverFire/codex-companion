#pragma once

#include "core/Result.h"

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>

namespace companion {

class CompanionShellViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString routeMode READ routeMode NOTIFY routeModeChanged)
    Q_PROPERTY(QString chatAccentColor READ chatAccentColor NOTIFY chatAccentColorChanged)
    Q_PROPERTY(QVariantList chatModels READ chatModels CONSTANT)
    Q_PROPERTY(QString selectedChatModelId READ selectedChatModelId WRITE setSelectedChatModelId NOTIFY selectedChatModelIdChanged)
    Q_PROPERTY(QString chatPromptPlaceholder READ chatPromptPlaceholder NOTIFY selectedChatModelIdChanged)
    Q_PROPERTY(QAbstractItemModel* processModel READ processModel NOTIFY processModelChanged)
    Q_PROPERTY(int activeProcessCount READ activeProcessCount NOTIFY activeProcessCountChanged)
    Q_PROPERTY(bool processLoading READ processLoading NOTIFY processStatusChanged)
    Q_PROPERTY(QString processErrorMessage READ processErrorMessage NOTIFY processStatusChanged)
    Q_PROPERTY(bool processTargetActive READ processTargetActive NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processTargetId READ processTargetId NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processTargetTitle READ processTargetTitle NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processTargetAction READ processTargetAction NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processDraft READ processDraft WRITE setProcessDraft NOTIFY processActionStateChanged)
    Q_PROPERTY(bool processSending READ processSending NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processFeedback READ processFeedback NOTIFY processActionStateChanged)
    Q_PROPERTY(bool processFeedbackIsError READ processFeedbackIsError NOTIFY processActionStateChanged)
    Q_PROPERTY(QString approvingProcessId READ approvingProcessId NOTIFY processActionStateChanged)
    Q_PROPERTY(QString retryingProcessId READ retryingProcessId NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processRetryStatusId READ processRetryStatusId NOTIFY processActionStateChanged)
    Q_PROPERTY(QString processRetryStatus READ processRetryStatus NOTIFY processActionStateChanged)
    Q_PROPERTY(bool processRetryStatusIsError READ processRetryStatusIsError NOTIFY processActionStateChanged)
    Q_PROPERTY(bool processCommandBusy READ processCommandBusy NOTIFY processActionStateChanged)
    Q_PROPERTY(bool chatSendEnabled READ chatSendEnabled NOTIFY chatStatusChanged)
    Q_PROPERTY(bool chatPreparationEnabled READ chatPreparationEnabled NOTIFY chatStatusChanged)
    Q_PROPERTY(bool chatBusy READ chatBusy NOTIFY chatStatusChanged)
    Q_PROPERTY(QString chatResponse READ chatResponse NOTIFY chatStatusChanged)
    Q_PROPERTY(QString chatResponsePrompt READ chatResponsePrompt NOTIFY chatStatusChanged)
    Q_PROPERTY(QString chatResponseTitle READ chatResponseTitle NOTIFY chatStatusChanged)
    Q_PROPERTY(QString chatResponseUsageSummary READ chatResponseUsageSummary NOTIFY chatStatusChanged)
    Q_PROPERTY(QString chatStatusMessage READ chatStatusMessage NOTIFY chatStatusChanged)
    Q_PROPERTY(bool usageLoading READ usageLoading NOTIFY usageStatusChanged)
    Q_PROPERTY(QVariantMap usageSnapshot READ usageSnapshot NOTIFY usageStatusChanged)
    Q_PROPERTY(QString usageErrorMessage READ usageErrorMessage NOTIFY usageStatusChanged)
    Q_PROPERTY(QVariantMap usageResetConfirmation READ usageResetConfirmation NOTIFY usageStatusChanged)
    Q_PROPERTY(bool usageResetBusy READ usageResetBusy NOTIFY usageStatusChanged)
    Q_PROPERTY(QString usageResetStatusMessage READ usageResetStatusMessage NOTIFY usageStatusChanged)
    Q_PROPERTY(bool goalControlVisible READ goalControlVisible NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalTaskTitle READ goalTaskTitle NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalThreadId READ goalThreadId NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalObjective READ goalObjective NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalDraftObjective READ goalDraftObjective WRITE setGoalDraftObjective NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalStatus READ goalStatus NOTIFY goalControlChanged)
    Q_PROPERTY(qint64 goalElapsedSeconds READ goalElapsedSeconds NOTIFY goalControlChanged)
    Q_PROPERTY(bool goalEditing READ goalEditing NOTIFY goalControlChanged)
    Q_PROPERTY(bool goalMutationPending READ goalMutationPending NOTIFY goalControlChanged)
    Q_PROPERTY(bool goalCanEdit READ goalCanEdit NOTIFY goalControlChanged)
    Q_PROPERTY(bool goalCanPause READ goalCanPause NOTIFY goalControlChanged)
    Q_PROPERTY(bool goalCanResume READ goalCanResume NOTIFY goalControlChanged)
    Q_PROPERTY(QString goalErrorMessage READ goalErrorMessage NOTIFY goalControlChanged)

public:
    using ModelSelectionPersistCommand =
        std::function<Result<void>(
            const QString&)>;

    explicit CompanionShellViewModel(QObject* parent = nullptr);
    CompanionShellViewModel(
        QString selectedChatModelId,
        ModelSelectionPersistCommand
            persistModelSelection,
        QObject* parent = nullptr);

    QString routeMode() const;
    QString chatAccentColor() const;
    void setChatAccentColor(QString value);
    QVariantList chatModels() const;
    QString selectedChatModelId() const;
    QString chatModelTitle(const QString& modelId) const;
    QString chatPromptPlaceholder() const;
    void setSelectedChatModelId(const QString& value);
    Q_INVOKABLE bool chooseChatModel(const QString& value);

    QAbstractItemModel* processModel() const noexcept;
    void setProcessModel(QAbstractItemModel* processModel);
    int activeProcessCount() const noexcept;
    bool processLoading() const noexcept;
    QString processErrorMessage() const;
    void setProcessStatus(bool loading, QString errorMessage = {});
    bool processTargetActive() const noexcept;
    QString processTargetId() const;
    QString processTargetTitle() const;
    QString processTargetAction() const;
    QString processDraft() const;
    void setProcessDraft(const QString& value);
    bool processSending() const noexcept;
    QString processFeedback() const;
    bool processFeedbackIsError() const noexcept;
    QString approvingProcessId() const;
    QString retryingProcessId() const;
    QString processRetryStatusId() const;
    QString processRetryStatus() const;
    bool processRetryStatusIsError() const noexcept;
    bool processCommandBusy() const noexcept;
    void finishProcessMessage(
        bool succeeded,
        QString message);
    void finishProcessApproval(
        bool succeeded,
        QString message);
    void finishProcessRetry(
        QString processId,
        bool succeeded,
        QString message);

    bool chatSendEnabled() const noexcept;
    bool chatPreparationEnabled() const noexcept;
    bool chatBusy() const noexcept;
    QString chatResponse() const;
    QString chatResponsePrompt() const;
    QString chatResponseTitle() const;
    QString chatResponseUsageSummary() const;
    QString chatStatusMessage() const;
    void setChatStatus(
        bool sendEnabled,
        bool preparationEnabled,
        bool busy,
        QString response,
        QString statusMessage,
        QString responsePrompt = {},
        QString responseTitle = {},
        QString responseUsageSummary = {});

    bool usageLoading() const noexcept;
    QVariantMap usageSnapshot() const;
    QString usageErrorMessage() const;
    QVariantMap usageResetConfirmation() const;
    bool usageResetBusy() const noexcept;
    QString usageResetStatusMessage() const;
    void setUsageStatus(
        bool loading,
        QVariantMap snapshot,
        QString errorMessage);
    void finishUsageReset(
        bool succeeded,
        QString message);

    bool goalControlVisible() const noexcept;
    QString goalTaskTitle() const;
    QString goalThreadId() const;
    QString goalObjective() const;
    QString goalDraftObjective() const;
    void setGoalDraftObjective(const QString& value);
    QString goalStatus() const;
    qint64 goalElapsedSeconds() const noexcept;
    bool goalEditing() const noexcept;
    bool goalMutationPending() const noexcept;
    bool goalCanEdit() const noexcept;
    bool goalCanPause() const noexcept;
    bool goalCanResume() const noexcept;
    QString goalErrorMessage() const;
    void applyGoalSnapshot(const QVariantMap& goal);
    void applyGoalMutationResult(const QVariantMap& goal);
    void removeGoalSnapshot(const QString& threadId);
    void finishGoalMutation(
        bool succeeded,
        QString errorMessage);

    Q_INVOKABLE void showProcesses();
    Q_INVOKABLE void showLocalChat();
    Q_INVOKABLE void beginProcessAction(
        const QVariantMap& process,
        const QString& action);
    Q_INVOKABLE void clearProcessTarget();
    Q_INVOKABLE void cancelProcessTarget();
    Q_INVOKABLE void submitProcessMessage();
    Q_INVOKABLE void respondToProcessApproval(
        const QVariantMap& process,
        const QString& decision);
    Q_INVOKABLE void retryFailedProcess(
        const QVariantMap& process);
    Q_INVOKABLE void submitLocalChat(const QString& prompt);
    Q_INVOKABLE void dismissChatResponse();
    Q_INVOKABLE void prepareOnDeviceChat();
    Q_INVOKABLE void refreshUsage();
    Q_INVOKABLE void refreshUsageAfterAccountChange();
    Q_INVOKABLE void prepareUsageReset(
        const QVariantMap& credit);
    Q_INVOKABLE void cancelUsageReset();
    Q_INVOKABLE void confirmUsageReset();
    Q_INVOKABLE void openGoalControls(
        const QString& taskTitle,
        const QVariantMap& goal);
    Q_INVOKABLE void dismissGoalControls();
    Q_INVOKABLE void beginGoalEditing();
    Q_INVOKABLE void cancelGoalEditing();
    Q_INVOKABLE void saveGoalEdit();
    Q_INVOKABLE void pauseGoal();
    Q_INVOKABLE void resumeGoal();

signals:
    void routeModeChanged();
    void chatAccentColorChanged();
    void selectedChatModelIdChanged();
    void processModelChanged();
    void activeProcessCountChanged();
    void processStatusChanged();
    void processActionStateChanged();
    void chatStatusChanged();
    void usageStatusChanged();
    void goalControlChanged();
    void runtimeErrorOccurred(
        companion::CompanionError error);
    void processMessageRequested(
        QString action,
        QString threadId,
        QString prompt,
        QString cwd,
        QString activeTurnId,
        QString model,
        QString reasoningEffort);
    void processApprovalRequested(
        QString threadId,
        QString decision);
    void processRetryRequested(
        QVariantMap process);
    void processCancelRequested();
    void localChatRequested(QString prompt, QString modelId);
    void onDevicePreparationRequested();
    void usageRefreshRequested();
    void usageResetRequested(
        QString creditId,
        QString idempotencyKey);
    void goalUpdateRequested(
        QString threadId,
        QString objective);
    void goalPauseRequested(QString threadId);
    void goalResumeRequested(QString threadId);

private:
    enum class GoalMutation {
        None,
        Update,
        Pause,
        Resume,
    };

    void setRouteMode(QString routeMode);
    bool hasChatModel(const QString& modelId) const;
    static bool isKnownProcessAction(const QString& action);
    static bool isRetryEligibleProcess(
        const QVariantMap& process);
    static bool isKnownGoalStatus(const QString& status);
    void beginGoalMutation(GoalMutation mutation);
    void disconnectProcessModel();
    void refreshActiveProcessCount();
    void reconcileProcessTarget();
    void clearProcessTargetState();

    QString routeMode_ = QStringLiteral("local-chat");
    QString chatAccentColor_ =
        QStringLiteral("#297af5");
    QVariantList chatModels_;
    QString selectedChatModelId_ = QStringLiteral("on-device");
    ModelSelectionPersistCommand
        persistModelSelection_;
    QPointer<QAbstractItemModel> processModel_;
    QVector<QMetaObject::Connection>
        processModelConnections_;
    int activeProcessCount_ = 0;
    bool processLoading_ = false;
    QString processErrorMessage_;
    QString processTargetId_;
    QString processTargetThreadId_;
    QString processTargetTitle_;
    QString processTargetAction_;
    QString processTargetCwd_;
    QString processTargetActiveTurnId_;
    QString processTargetModel_;
    QString processTargetReasoningEffort_;
    QString processDraft_;
    QString pendingProcessPrompt_;
    QString pendingProcessTargetId_;
    QString pendingProcessThreadId_;
    QString pendingProcessAction_;
    bool processSending_ = false;
    QString processFeedback_;
    bool processFeedbackIsError_ = false;
    QString approvingProcessId_;
    QString retryingProcessId_;
    QString processRetryStatusId_;
    QString processRetryStatus_;
    bool processRetryStatusIsError_ = false;
    bool chatSendEnabled_ = false;
    bool chatPreparationEnabled_ = false;
    bool chatBusy_ = false;
    QString chatResponse_;
    QString chatResponsePrompt_;
    QString chatResponseTitle_;
    QString chatResponseUsageSummary_;
    QString chatStatusMessage_ =
        QStringLiteral("Local chat runtime is connecting");
    bool usageLoading_ = false;
    QVariantMap usageSnapshot_;
    QString usageErrorMessage_;
    QVariantMap usageResetConfirmation_;
    bool usageResetBusy_ = false;
    QString usageResetStatusMessage_;
    bool goalControlVisible_ = false;
    QString goalTaskTitle_;
    QString goalThreadId_;
    QString goalObjective_;
    QString goalDraftObjective_;
    QString goalStatus_;
    qint64 goalElapsedSeconds_ = 0;
    bool goalEditing_ = false;
    GoalMutation goalMutation_ = GoalMutation::None;
    QString pendingGoalObjective_;
    QString goalErrorMessage_;
};

} // namespace companion
