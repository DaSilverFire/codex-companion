#include "ui/CompanionShellViewModel.h"

#include <QUuid>
#include <QVariantMap>
#include <utility>

namespace {

QVariantMap chatModel(
    QString id,
    QString title,
    QString detail,
    QString group)
{
    return {
        {QStringLiteral("id"), std::move(id)},
        {QStringLiteral("title"), std::move(title)},
        {QStringLiteral("detail"), std::move(detail)},
        {QStringLiteral("group"), std::move(group)},
    };
}

} // namespace

namespace companion {

CompanionShellViewModel::CompanionShellViewModel(QObject* parent)
    : CompanionShellViewModel(
          QStringLiteral("on-device"),
          {},
          parent)
{
}

CompanionShellViewModel::CompanionShellViewModel(
    QString selectedChatModelId,
    ModelSelectionPersistCommand
        persistModelSelection,
    QObject* parent)
    : QObject(parent),
      chatModels_({
          chatModel(
              QStringLiteral("on-device"),
              QStringLiteral("On-device"),
              QStringLiteral("On-device reasoning - private on this PC"),
              QStringLiteral("on-device")),
          chatModel(
              QStringLiteral("openai:gpt56Luna"),
              QStringLiteral("5.6 Luna"),
              QStringLiteral("OpenAI API - lowest cost"),
              QStringLiteral("openai")),
          chatModel(
              QStringLiteral("openai:gpt56Terra"),
              QStringLiteral("5.6 Terra"),
              QStringLiteral("OpenAI API - balanced"),
              QStringLiteral("openai")),
          chatModel(
              QStringLiteral("openai:gpt56Sol"),
              QStringLiteral("5.6 Sol"),
              QStringLiteral("OpenAI API - highest capability"),
              QStringLiteral("openai")),
          chatModel(
              QStringLiteral("lumo:automatic"),
              QStringLiteral("Lumo Auto"),
              QStringLiteral("Lumo API - best available model"),
              QStringLiteral("lumo")),
          chatModel(
              QStringLiteral("lumo:fast"),
              QStringLiteral("Lumo Fast"),
              QStringLiteral("Lumo API - fast responses"),
              QStringLiteral("lumo")),
          chatModel(
              QStringLiteral("lumo:thinking"),
              QStringLiteral("Lumo Thinking"),
              QStringLiteral("Lumo API - deeper reasoning"),
              QStringLiteral("lumo")),
      }),
      persistModelSelection_(
          std::move(persistModelSelection))
{
    qRegisterMetaType<CompanionError>(
        "companion::CompanionError");
    if (hasChatModel(selectedChatModelId)) {
        selectedChatModelId_ =
            std::move(selectedChatModelId);
    }
}

QString CompanionShellViewModel::routeMode() const
{
    return routeMode_;
}

QString CompanionShellViewModel::chatAccentColor() const
{
    return chatAccentColor_;
}

void CompanionShellViewModel::setChatAccentColor(
    QString value)
{
    if (value == chatAccentColor_) {
        return;
    }
    chatAccentColor_ = std::move(value);
    emit chatAccentColorChanged();
}

QVariantList CompanionShellViewModel::chatModels() const
{
    return chatModels_;
}

QString CompanionShellViewModel::selectedChatModelId() const
{
    return selectedChatModelId_;
}

QString CompanionShellViewModel::chatModelTitle(
    const QString& modelId) const
{
    for (const QVariant& entry : chatModels_) {
        const QVariantMap model = entry.toMap();
        if (model.value(QStringLiteral("id")).toString()
            == modelId) {
            return model.value(
                QStringLiteral("title")).toString();
        }
    }
    return {};
}

QString CompanionShellViewModel::chatPromptPlaceholder() const
{
    for (const QVariant& entry : chatModels_) {
        const QVariantMap model = entry.toMap();
        if (model.value(QStringLiteral("id")).toString()
            != selectedChatModelId_) {
            continue;
        }

        const QString group =
            model.value(QStringLiteral("group")).toString();
        if (group == QStringLiteral("on-device")) {
            return QStringLiteral("Ask on device");
        }
        if (group == QStringLiteral("openai")) {
            return QStringLiteral("Ask ChatGPT");
        }
        if (group == QStringLiteral("lumo")) {
            return QStringLiteral("Ask Lumo");
        }
        break;
    }
    return QStringLiteral("Ask Companion");
}

void CompanionShellViewModel::setSelectedChatModelId(const QString& value)
{
    chooseChatModel(value);
}

bool CompanionShellViewModel::chooseChatModel(
    const QString& value)
{
    if (value == selectedChatModelId_) {
        return true;
    }
    if (!hasChatModel(value)) {
        return false;
    }
    if (persistModelSelection_) {
        const auto persisted =
            persistModelSelection_(value);
        if (!persisted.hasValue()) {
            emit runtimeErrorOccurred(
                persisted.error());
            return false;
        }
    }

    selectedChatModelId_ = value;
    emit selectedChatModelIdChanged();
    return true;
}

QAbstractItemModel* CompanionShellViewModel::processModel() const noexcept
{
    return processModel_.data();
}

void CompanionShellViewModel::setProcessModel(QAbstractItemModel* processModel)
{
    if (processModel_ == processModel) {
        return;
    }

    disconnectProcessModel();
    processModel_ = processModel;
    if (processModel != nullptr) {
        const auto reconcile = [this]() {
            reconcileProcessTarget();
            refreshActiveProcessCount();
        };
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::dataChanged,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::rowsInserted,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::rowsRemoved,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::rowsMoved,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::modelReset,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QAbstractItemModel::layoutChanged,
                this,
                reconcile));
        processModelConnections_.append(
            connect(
                processModel,
                &QObject::destroyed,
                this,
                [this]() {
                    processModel_.clear();
                    processModelConnections_.clear();
                    emit processModelChanged();
                    reconcileProcessTarget();
                    refreshActiveProcessCount();
                }));
    }
    emit processModelChanged();
    reconcileProcessTarget();
    refreshActiveProcessCount();
}

int CompanionShellViewModel::activeProcessCount() const noexcept
{
    return activeProcessCount_;
}

bool CompanionShellViewModel::processLoading() const noexcept
{
    return processLoading_;
}

QString CompanionShellViewModel::processErrorMessage() const
{
    return processErrorMessage_;
}

void CompanionShellViewModel::setProcessStatus(
    bool loading,
    QString errorMessage)
{
    if (processLoading_ == loading
        && processErrorMessage_ == errorMessage) {
        return;
    }

    processLoading_ = loading;
    processErrorMessage_ = std::move(errorMessage);
    emit processStatusChanged();
}

bool CompanionShellViewModel::processTargetActive() const noexcept
{
    return !processTargetId_.isEmpty();
}

QString CompanionShellViewModel::processTargetId() const
{
    return processTargetId_;
}

QString CompanionShellViewModel::processTargetTitle() const
{
    return processTargetTitle_;
}

QString CompanionShellViewModel::processTargetAction() const
{
    return processTargetAction_;
}

QString CompanionShellViewModel::processDraft() const
{
    return processDraft_;
}

void CompanionShellViewModel::setProcessDraft(
    const QString& value)
{
    if (!processTargetActive()
        || processDraft_ == value) {
        return;
    }

    processDraft_ = value;
    if (processFeedbackIsError_) {
        processFeedback_.clear();
        processFeedbackIsError_ = false;
    }
    emit processActionStateChanged();
}

bool CompanionShellViewModel::processSending() const noexcept
{
    return processSending_;
}

QString CompanionShellViewModel::processFeedback() const
{
    return processFeedback_;
}

bool CompanionShellViewModel::processFeedbackIsError() const noexcept
{
    return processFeedbackIsError_;
}

QString CompanionShellViewModel::approvingProcessId() const
{
    return approvingProcessId_;
}

QString CompanionShellViewModel::retryingProcessId() const
{
    return retryingProcessId_;
}

QString CompanionShellViewModel::processRetryStatusId() const
{
    return processRetryStatusId_;
}

QString CompanionShellViewModel::processRetryStatus() const
{
    return processRetryStatus_;
}

bool CompanionShellViewModel::
processRetryStatusIsError() const noexcept
{
    return processRetryStatusIsError_;
}

bool CompanionShellViewModel::processCommandBusy() const noexcept
{
    return processSending_
        || !approvingProcessId_.isEmpty()
        || !retryingProcessId_.isEmpty();
}

void CompanionShellViewModel::finishProcessMessage(
    bool succeeded,
    QString message)
{
    if (!processSending_) {
        return;
    }

    processSending_ = false;
    if (succeeded) {
        const bool composerStillMatches =
            processTargetActive()
            && processTargetId_
                == pendingProcessTargetId_
            && processTargetThreadId_
                == pendingProcessThreadId_
            && processTargetAction_
                == pendingProcessAction_
            && processDraft_.trimmed()
                == pendingProcessPrompt_;
        if (composerStillMatches) {
            clearProcessTargetState();
        } else {
            processFeedback_ =
                message.trimmed().isEmpty()
                ? QStringLiteral(
                      "Message sent. Your newer draft is still here.")
                : std::move(message);
            processFeedbackIsError_ = false;
        }
    } else {
        processFeedback_ =
            message.trimmed().isEmpty()
            ? QStringLiteral(
                  "Codex could not accept the message. Your draft is still here.")
            : std::move(message);
        processFeedbackIsError_ = true;
    }
    pendingProcessPrompt_.clear();
    pendingProcessTargetId_.clear();
    pendingProcessThreadId_.clear();
    pendingProcessAction_.clear();
    emit processActionStateChanged();
}

void CompanionShellViewModel::finishProcessApproval(
    bool succeeded,
    QString message)
{
    if (approvingProcessId_.isEmpty()) {
        return;
    }

    approvingProcessId_.clear();
    if (succeeded) {
        processFeedback_.clear();
        processFeedbackIsError_ = false;
    } else {
        processFeedback_ =
            message.trimmed().isEmpty()
            ? QStringLiteral(
                  "Codex could not apply the approval decision.")
            : std::move(message);
        processFeedbackIsError_ = true;
    }
    emit processActionStateChanged();
}

void CompanionShellViewModel::finishProcessRetry(
    QString processId,
    bool succeeded,
    QString message)
{
    processId = processId.trimmed();
    if (processId.isEmpty()
        || (!retryingProcessId_.isEmpty()
            && retryingProcessId_
                != processId)) {
        return;
    }

    if (retryingProcessId_ == processId) {
        retryingProcessId_.clear();
    }
    message = message.trimmed();
    if (message.isEmpty()) {
        message = succeeded
            ? QStringLiteral(
                  "Codex resumed the task.")
            : QStringLiteral(
                  "Codex could not retry the task.");
    }
    processRetryStatusId_ =
        std::move(processId);
    processRetryStatus_ =
        std::move(message);
    processRetryStatusIsError_ =
        !succeeded;
    emit processActionStateChanged();
}

bool CompanionShellViewModel::chatSendEnabled() const noexcept
{
    return chatSendEnabled_;
}

bool CompanionShellViewModel::chatPreparationEnabled() const noexcept
{
    return chatPreparationEnabled_;
}

bool CompanionShellViewModel::chatBusy() const noexcept
{
    return chatBusy_;
}

QString CompanionShellViewModel::chatResponse() const
{
    return chatResponse_;
}

QString CompanionShellViewModel::chatResponsePrompt() const
{
    return chatResponsePrompt_;
}

QString CompanionShellViewModel::chatResponseTitle() const
{
    return chatResponseTitle_;
}

QString CompanionShellViewModel::chatResponseUsageSummary() const
{
    return chatResponseUsageSummary_;
}

QString CompanionShellViewModel::chatStatusMessage() const
{
    return chatStatusMessage_;
}

void CompanionShellViewModel::setChatStatus(
    bool sendEnabled,
    bool preparationEnabled,
    bool busy,
    QString response,
    QString statusMessage,
    QString responsePrompt,
    QString responseTitle,
    QString responseUsageSummary)
{
    if (response.isEmpty()) {
        responsePrompt.clear();
        responseTitle.clear();
        responseUsageSummary.clear();
    }
    if (chatSendEnabled_ == sendEnabled
        && chatPreparationEnabled_ == preparationEnabled
        && chatBusy_ == busy
        && chatResponse_ == response
        && chatResponsePrompt_ == responsePrompt
        && chatResponseTitle_ == responseTitle
        && chatResponseUsageSummary_
            == responseUsageSummary
        && chatStatusMessage_ == statusMessage) {
        return;
    }

    chatSendEnabled_ = sendEnabled;
    chatPreparationEnabled_ = preparationEnabled;
    chatBusy_ = busy;
    chatResponse_ = std::move(response);
    chatResponsePrompt_ =
        std::move(responsePrompt);
    chatResponseTitle_ =
        std::move(responseTitle);
    chatResponseUsageSummary_ =
        std::move(responseUsageSummary);
    chatStatusMessage_ = std::move(statusMessage);
    emit chatStatusChanged();
}

bool CompanionShellViewModel::usageLoading() const noexcept
{
    return usageLoading_;
}

QVariantMap CompanionShellViewModel::usageSnapshot() const
{
    return usageSnapshot_;
}

QString CompanionShellViewModel::usageErrorMessage() const
{
    return usageErrorMessage_;
}

QVariantMap CompanionShellViewModel::
    usageResetConfirmation() const
{
    return usageResetConfirmation_;
}

bool CompanionShellViewModel::usageResetBusy() const noexcept
{
    return usageResetBusy_;
}

QString CompanionShellViewModel::
    usageResetStatusMessage() const
{
    return usageResetStatusMessage_;
}

void CompanionShellViewModel::setUsageStatus(
    bool loading,
    QVariantMap snapshot,
    QString errorMessage)
{
    if (snapshot.isEmpty()) {
        snapshot = usageSnapshot_;
    }
    if (usageLoading_ == loading
        && usageSnapshot_ == snapshot
        && usageErrorMessage_ == errorMessage) {
        return;
    }

    usageLoading_ = loading;
    usageSnapshot_ = std::move(snapshot);
    usageErrorMessage_ = std::move(errorMessage);
    emit usageStatusChanged();
}

void CompanionShellViewModel::finishUsageReset(
    bool succeeded,
    QString message)
{
    message = message.trimmed();
    if (message.isEmpty()) {
        message = succeeded
            ? QStringLiteral(
                  "Codex usage reset applied.")
            : QStringLiteral(
                  "Could not apply the Codex usage reset.");
    }
    if (!usageResetBusy_
        && usageResetStatusMessage_ == message) {
        return;
    }

    usageResetBusy_ = false;
    usageResetStatusMessage_ =
        std::move(message);
    emit usageStatusChanged();
}

bool CompanionShellViewModel::goalControlVisible() const noexcept
{
    return goalControlVisible_;
}

QString CompanionShellViewModel::goalTaskTitle() const
{
    return goalTaskTitle_;
}

QString CompanionShellViewModel::goalThreadId() const
{
    return goalThreadId_;
}

QString CompanionShellViewModel::goalObjective() const
{
    return goalObjective_;
}

QString CompanionShellViewModel::goalDraftObjective() const
{
    return goalDraftObjective_;
}

void CompanionShellViewModel::setGoalDraftObjective(
    const QString& value)
{
    if (!goalEditing_ || goalMutationPending()
        || goalDraftObjective_ == value) {
        return;
    }

    goalDraftObjective_ = value;
    if (!goalErrorMessage_.isEmpty()) {
        goalErrorMessage_.clear();
    }
    emit goalControlChanged();
}

QString CompanionShellViewModel::goalStatus() const
{
    return goalStatus_;
}

qint64 CompanionShellViewModel::goalElapsedSeconds() const noexcept
{
    return goalElapsedSeconds_;
}

bool CompanionShellViewModel::goalEditing() const noexcept
{
    return goalEditing_;
}

bool CompanionShellViewModel::goalMutationPending() const noexcept
{
    return goalMutation_ != GoalMutation::None;
}

bool CompanionShellViewModel::goalCanEdit() const noexcept
{
    return goalControlVisible_
        && goalStatus_ != QStringLiteral("complete");
}

bool CompanionShellViewModel::goalCanPause() const noexcept
{
    return goalControlVisible_
        && goalStatus_ == QStringLiteral("active");
}

bool CompanionShellViewModel::goalCanResume() const noexcept
{
    return goalControlVisible_
        && (goalStatus_ == QStringLiteral("paused")
            || goalStatus_ == QStringLiteral("blocked"));
}

QString CompanionShellViewModel::goalErrorMessage() const
{
    return goalErrorMessage_;
}

void CompanionShellViewModel::applyGoalSnapshot(
    const QVariantMap& goal)
{
    const QString threadId =
        goal.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    const QString objective =
        goal.value(QStringLiteral("objective"))
            .toString()
            .trimmed();
    const QString status =
        goal.value(QStringLiteral("status"))
            .toString();
    if (threadId.isEmpty()
        || threadId != goalThreadId_
        || objective.isEmpty()
        || !isKnownGoalStatus(status)) {
        return;
    }

    bool changed = false;
    if (goalObjective_ != objective) {
        goalObjective_ = objective;
        changed = true;
    }
    if (!goalEditing_
        && goalDraftObjective_ != objective) {
        goalDraftObjective_ = objective;
        changed = true;
    }
    if (goalStatus_ != status) {
        goalStatus_ = status;
        changed = true;
    }
    if (status == QStringLiteral("complete")
        && goalEditing_) {
        goalEditing_ = false;
        goalDraftObjective_ = objective;
        changed = true;
    }
    const qint64 elapsedSeconds =
        qMax<qint64>(
            0,
            goal.value(
                    QStringLiteral("elapsedSeconds"))
                .toLongLong());
    if (goalElapsedSeconds_ != elapsedSeconds) {
        goalElapsedSeconds_ = elapsedSeconds;
        changed = true;
    }
    if (changed) {
        emit goalControlChanged();
    }
}

void CompanionShellViewModel::applyGoalMutationResult(
    const QVariantMap& goal)
{
    applyGoalSnapshot(goal);
}

void CompanionShellViewModel::removeGoalSnapshot(
    const QString& threadId)
{
    if (!goalControlVisible_
        || threadId.trimmed() != goalThreadId_) {
        return;
    }

    goalControlVisible_ = false;
    goalEditing_ = false;
    goalErrorMessage_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::finishGoalMutation(
    bool succeeded,
    QString errorMessage)
{
    if (!goalMutationPending()) {
        return;
    }

    const GoalMutation completedMutation =
        goalMutation_;
    goalMutation_ = GoalMutation::None;

    if (succeeded) {
        goalErrorMessage_.clear();
        if (completedMutation
            == GoalMutation::Update) {
            goalEditing_ = false;
            goalDraftObjective_ =
                goalObjective_;
        }
    } else {
        goalErrorMessage_ =
            errorMessage.trimmed().isEmpty()
            ? QStringLiteral(
                  "The goal could not be updated.")
            : std::move(errorMessage);
    }
    pendingGoalObjective_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::showProcesses()
{
    setRouteMode(QStringLiteral("processes"));
    dismissChatResponse();
}

void CompanionShellViewModel::showLocalChat()
{
    clearProcessTarget();
    setRouteMode(QStringLiteral("local-chat"));
}

void CompanionShellViewModel::beginProcessAction(
    const QVariantMap& process,
    const QString& action)
{
    const QString normalizedAction =
        action.trimmed().toLower();
    const QString processId =
        process.value(QStringLiteral("id"))
            .toString()
            .trimmed();
    const QString threadId =
        process.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    if (!isKnownProcessAction(normalizedAction)
        || processId.isEmpty()
        || threadId.isEmpty()
        || (normalizedAction
                == QStringLiteral(
                    "approval-feedback")
            && !process.value(
                    QStringLiteral("needsApproval"))
                    .toBool())) {
        return;
    }

    processTargetId_ = processId;
    processTargetThreadId_ = threadId;
    processTargetTitle_ =
        process.value(QStringLiteral("title"))
            .toString()
            .trimmed();
    if (processTargetTitle_.isEmpty()) {
        processTargetTitle_ =
            QStringLiteral("Codex task");
    }
    processTargetAction_ = normalizedAction;
    processTargetCwd_ =
        process.value(QStringLiteral("cwd"))
            .toString()
            .trimmed();
    processTargetActiveTurnId_ =
        process.value(
                QStringLiteral("activeTurnId"))
            .toString()
            .trimmed();
    processTargetModel_ =
        process.value(QStringLiteral("model"))
            .toString()
            .trimmed();
    processTargetReasoningEffort_ =
        process.value(
                QStringLiteral("reasoningEffort"))
            .toString()
            .trimmed();
    processDraft_.clear();
    processFeedback_.clear();
    processFeedbackIsError_ = false;
    setRouteMode(QStringLiteral("processes"));
    emit processActionStateChanged();
}

void CompanionShellViewModel::clearProcessTarget()
{
    if (!processTargetActive()
        && processDraft_.isEmpty()
        && processFeedback_.isEmpty()) {
        return;
    }

    clearProcessTargetState();
    emit processActionStateChanged();
}

void CompanionShellViewModel::cancelProcessTarget()
{
    if (!processTargetActive()
        && processDraft_.isEmpty()
        && processFeedback_.isEmpty()
        && !processSending_) {
        return;
    }

    if (processSending_) {
        emit processCancelRequested();
    }
    clearProcessTargetState();
    processSending_ = false;
    pendingProcessPrompt_.clear();
    pendingProcessTargetId_.clear();
    pendingProcessThreadId_.clear();
    pendingProcessAction_.clear();
    emit processActionStateChanged();
}

void CompanionShellViewModel::submitProcessMessage()
{
    const QString prompt =
        processDraft_.trimmed();
    if (!processTargetActive()
        || processSending_
        || prompt.isEmpty()) {
        return;
    }

    pendingProcessPrompt_ = prompt;
    pendingProcessTargetId_ = processTargetId_;
    pendingProcessThreadId_ =
        processTargetThreadId_;
    pendingProcessAction_ =
        processTargetAction_;
    processSending_ = true;
    processFeedbackIsError_ = false;
    if (processTargetAction_
        == QStringLiteral("steer")) {
        processFeedback_ =
            QStringLiteral("Steering %1...")
                .arg(processTargetTitle_);
    } else if (
        processTargetAction_
        == QStringLiteral(
            "approval-feedback")) {
        processFeedback_ =
            QStringLiteral(
                "Telling Codex what to do instead...");
    } else {
        processFeedback_ =
            QStringLiteral("Sending reply to %1...")
                .arg(processTargetTitle_);
    }
    emit processActionStateChanged();
    emit processMessageRequested(
        processTargetAction_,
        processTargetThreadId_,
        prompt,
        processTargetCwd_,
        processTargetActiveTurnId_,
        processTargetModel_,
        processTargetReasoningEffort_);
}

void CompanionShellViewModel::respondToProcessApproval(
    const QVariantMap& process,
    const QString& decision)
{
    const QString normalizedDecision =
        decision.trimmed();
    const QString processId =
        process.value(QStringLiteral("id"))
            .toString()
            .trimmed();
    const QString threadId =
        process.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    if (!approvingProcessId_.isEmpty()
        || processId.isEmpty()
        || threadId.isEmpty()
        || !process.value(
                QStringLiteral("needsApproval"))
                .toBool()
        || (normalizedDecision
                != QStringLiteral("approveOnce")
            && normalizedDecision
                != QStringLiteral(
                    "approveSimilar"))) {
        return;
    }

    approvingProcessId_ = processId;
    processFeedback_.clear();
    processFeedbackIsError_ = false;
    emit processActionStateChanged();
    emit processApprovalRequested(
        threadId,
        normalizedDecision);
}

void CompanionShellViewModel::retryFailedProcess(
    const QVariantMap& process)
{
    if (processCommandBusy()
        || !isRetryEligibleProcess(process)) {
        return;
    }

    QVariantMap normalized = process;
    const auto normalizeText =
        [&normalized](
            const QString& key) {
            normalized.insert(
                key,
                normalized.value(key)
                    .toString()
                    .trimmed());
        };
    normalizeText(QStringLiteral("id"));
    normalizeText(QStringLiteral("processId"));
    normalizeText(QStringLiteral("threadId"));
    normalizeText(QStringLiteral("kind"));
    normalizeText(QStringLiteral("status"));
    normalizeText(
        QStringLiteral("runtimeStatus"));
    normalizeText(
        QStringLiteral("rolloutPath"));
    if (normalized.value(
                      QStringLiteral("status"))
            .toString()
            == QStringLiteral("failed")
        && normalized.value(
                         QStringLiteral(
                             "runtimeStatus"))
               .toString()
               .isEmpty()) {
        normalized.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("notLoaded"));
    }

    retryingProcessId_ =
        normalized.value(
                      QStringLiteral("id"))
            .toString();
    processRetryStatusId_ =
        retryingProcessId_;
    processRetryStatus_ =
        QStringLiteral("Retrying...");
    processRetryStatusIsError_ =
        false;
    emit processActionStateChanged();
    emit processRetryRequested(
        std::move(normalized));
}

void CompanionShellViewModel::submitLocalChat(const QString& prompt)
{
    const QString normalized = prompt.trimmed();
    if (!chatSendEnabled_ || chatBusy_ || normalized.isEmpty()) {
        return;
    }

    emit localChatRequested(normalized, selectedChatModelId_);
}

void CompanionShellViewModel::dismissChatResponse()
{
    if (chatResponse_.isEmpty()) {
        return;
    }

    chatResponse_.clear();
    chatResponsePrompt_.clear();
    chatResponseTitle_.clear();
    chatResponseUsageSummary_.clear();
    emit chatStatusChanged();
}

void CompanionShellViewModel::prepareOnDeviceChat()
{
    if (!chatPreparationEnabled_ || chatBusy_) {
        return;
    }

    emit onDevicePreparationRequested();
}

void CompanionShellViewModel::refreshUsage()
{
    if (usageLoading_) {
        return;
    }

    usageLoading_ = true;
    usageErrorMessage_.clear();
    emit usageStatusChanged();
    emit usageRefreshRequested();
}

void CompanionShellViewModel::
refreshUsageAfterAccountChange()
{
    usageLoading_ = true;
    usageErrorMessage_.clear();
    emit usageStatusChanged();
    emit usageRefreshRequested();
}

void CompanionShellViewModel::prepareUsageReset(
    const QVariantMap& credit)
{
    if (usageResetBusy_) {
        return;
    }

    const QString requestedId =
        credit.value(QStringLiteral("id"))
            .toString()
            .trimmed();
    QVariantMap availableCredit;
    const QVariantList credits =
        usageSnapshot_
            .value(QStringLiteral(
                "availableResetCredits"))
            .toList();
    for (const QVariant& candidate : credits) {
        const QVariantMap row =
            candidate.toMap();
        if (row.value(QStringLiteral("id"))
                .toString()
                .trimmed()
            == requestedId) {
            availableCredit = row;
            break;
        }
    }

    if (requestedId.isEmpty()
        || availableCredit.isEmpty()) {
        if (usageResetStatusMessage_
            != QStringLiteral(
                "That Codex reset is not available.")) {
            usageResetStatusMessage_ =
                QStringLiteral(
                    "That Codex reset is not available.");
            emit usageStatusChanged();
        }
        return;
    }

    QString displayTitle =
        availableCredit
            .value(QStringLiteral(
                "displayTitle"))
            .toString()
            .trimmed();
    if (displayTitle.isEmpty()) {
        displayTitle =
            QStringLiteral("Codex usage reset");
    }
    usageResetConfirmation_ = {
        {
            QStringLiteral("creditId"),
            requestedId,
        },
        {
            QStringLiteral("displayTitle"),
            displayTitle,
        },
        {
            QStringLiteral("idempotencyKey"),
            QUuid::createUuid().toString(
                QUuid::WithoutBraces),
        },
    };
    usageResetStatusMessage_.clear();
    emit usageStatusChanged();
}

void CompanionShellViewModel::cancelUsageReset()
{
    if (usageResetBusy_
        || usageResetConfirmation_.isEmpty()) {
        return;
    }

    usageResetConfirmation_.clear();
    emit usageStatusChanged();
}

void CompanionShellViewModel::confirmUsageReset()
{
    if (usageResetBusy_
        || usageResetConfirmation_.isEmpty()) {
        return;
    }

    const QString creditId =
        usageResetConfirmation_
            .value(QStringLiteral("creditId"))
            .toString()
            .trimmed();
    const QString displayTitle =
        usageResetConfirmation_
            .value(QStringLiteral(
                "displayTitle"))
            .toString()
            .trimmed();
    const QString idempotencyKey =
        usageResetConfirmation_
            .value(QStringLiteral(
                "idempotencyKey"))
            .toString()
            .trimmed();
    if (creditId.isEmpty()
        || QUuid(idempotencyKey).isNull()) {
        usageResetConfirmation_.clear();
        usageResetStatusMessage_ =
            QStringLiteral(
                "That Codex reset is not available.");
        emit usageStatusChanged();
        return;
    }

    usageResetConfirmation_.clear();
    usageResetBusy_ = true;
    usageResetStatusMessage_ =
        QStringLiteral("Applying %1...")
            .arg(
                displayTitle.isEmpty()
                    ? QStringLiteral(
                          "Codex usage reset")
                    : displayTitle);
    emit usageStatusChanged();
    emit usageResetRequested(
        creditId,
        idempotencyKey);
}

void CompanionShellViewModel::openGoalControls(
    const QString& taskTitle,
    const QVariantMap& goal)
{
    if (goalMutationPending()) {
        return;
    }

    const QString threadId =
        goal.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    const QString objective =
        goal.value(QStringLiteral("objective"))
            .toString()
            .trimmed();
    const QString status =
        goal.value(QStringLiteral("status"))
            .toString();
    if (threadId.isEmpty()
        || objective.isEmpty()
        || !isKnownGoalStatus(status)) {
        return;
    }

    goalControlVisible_ = true;
    goalTaskTitle_ = taskTitle.trimmed();
    goalThreadId_ = threadId;
    goalObjective_ = objective;
    goalDraftObjective_ = objective;
    goalStatus_ = status;
    goalElapsedSeconds_ =
        qMax<qint64>(
            0,
            goal.value(
                    QStringLiteral("elapsedSeconds"))
                .toLongLong());
    goalEditing_ = false;
    goalErrorMessage_.clear();
    pendingGoalObjective_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::dismissGoalControls()
{
    if (!goalControlVisible_) {
        return;
    }

    goalControlVisible_ = false;
    goalEditing_ = false;
    goalErrorMessage_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::beginGoalEditing()
{
    if (!goalCanEdit()
        || goalMutationPending()
        || goalEditing_) {
        return;
    }

    goalEditing_ = true;
    goalDraftObjective_ = goalObjective_;
    goalErrorMessage_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::cancelGoalEditing()
{
    if (!goalEditing_
        || goalMutationPending()) {
        return;
    }

    goalEditing_ = false;
    goalDraftObjective_ = goalObjective_;
    goalErrorMessage_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::saveGoalEdit()
{
    if (!goalEditing_
        || !goalCanEdit()
        || goalMutationPending()) {
        return;
    }

    const QString objective =
        goalDraftObjective_.trimmed();
    if (objective.isEmpty()) {
        goalErrorMessage_ =
            QStringLiteral(
                "Enter a goal objective before saving.");
        emit goalControlChanged();
        return;
    }
    if (objective == goalObjective_) {
        goalEditing_ = false;
        goalDraftObjective_ = goalObjective_;
        goalErrorMessage_.clear();
        emit goalControlChanged();
        return;
    }

    pendingGoalObjective_ = objective;
    beginGoalMutation(GoalMutation::Update);
    emit goalUpdateRequested(
        goalThreadId_,
        objective);
}

void CompanionShellViewModel::pauseGoal()
{
    if (!goalCanPause()
        || goalMutationPending()) {
        return;
    }

    beginGoalMutation(GoalMutation::Pause);
    emit goalPauseRequested(goalThreadId_);
}

void CompanionShellViewModel::resumeGoal()
{
    if (!goalCanResume()
        || goalMutationPending()) {
        return;
    }

    beginGoalMutation(GoalMutation::Resume);
    emit goalResumeRequested(goalThreadId_);
}

void CompanionShellViewModel::setRouteMode(QString routeMode)
{
    if (routeMode_ == routeMode) {
        return;
    }

    if (routeMode_ == QStringLiteral("processes")
        && routeMode != QStringLiteral("processes")
        && goalControlVisible_) {
        dismissGoalControls();
    }
    routeMode_ = std::move(routeMode);
    emit routeModeChanged();
}

bool CompanionShellViewModel::hasChatModel(const QString& modelId) const
{
    for (const QVariant& modelValue : chatModels_) {
        if (modelValue.toMap().value(QStringLiteral("id")).toString()
            == modelId) {
            return true;
        }
    }
    return false;
}

bool CompanionShellViewModel::isKnownProcessAction(
    const QString& action)
{
    return action == QStringLiteral("reply")
        || action == QStringLiteral("steer")
        || action
            == QStringLiteral(
                "approval-feedback");
}

bool CompanionShellViewModel::
isRetryEligibleProcess(
    const QVariantMap& process)
{
    const QString processId =
        process.value(QStringLiteral("id"))
            .toString()
            .trimmed();
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
    QString runtimeStatus =
        process.value(
                   QStringLiteral(
                       "runtimeStatus"))
            .toString()
            .trimmed();
    if (runtimeStatus.isEmpty()
        && status == QStringLiteral("failed")) {
        runtimeStatus =
            QStringLiteral("notLoaded");
    }
    const QVariantMap goal =
        process.value(QStringLiteral("goal"))
            .toMap();
    const QString goalStatus =
        goal.value(QStringLiteral("status"))
            .toString()
            .trimmed();
    const bool recoverableGoal =
        goalStatus == QStringLiteral("paused")
        || goalStatus == QStringLiteral("blocked")
        || goalStatus
            == QStringLiteral("usageLimited");
    if (processId.isEmpty()
        || threadId.isEmpty()
        || kind != QStringLiteral("thread")
        || status == QStringLiteral("running")
        || (status != QStringLiteral("failed")
            && !recoverableGoal)
        || (runtimeStatus
                != QStringLiteral("idle")
            && runtimeStatus
                != QStringLiteral(
                    "notLoaded")
            && runtimeStatus
                != QStringLiteral(
                    "systemError"))) {
        return false;
    }
    return goalStatus
               != QStringLiteral("complete")
        && goalStatus
               != QStringLiteral(
                   "budgetLimited");
}

bool CompanionShellViewModel::isKnownGoalStatus(
    const QString& status)
{
    return status == QStringLiteral("active")
        || status == QStringLiteral("paused")
        || status == QStringLiteral("blocked")
        || status == QStringLiteral("usageLimited")
        || status == QStringLiteral("budgetLimited")
        || status == QStringLiteral("complete");
}

void CompanionShellViewModel::beginGoalMutation(
    GoalMutation mutation)
{
    goalMutation_ = mutation;
    goalErrorMessage_.clear();
    emit goalControlChanged();
}

void CompanionShellViewModel::disconnectProcessModel()
{
    for (const QMetaObject::Connection& connection :
         std::as_const(processModelConnections_)) {
        disconnect(connection);
    }
    processModelConnections_.clear();
}

void CompanionShellViewModel::refreshActiveProcessCount()
{
    int count = 0;
    QAbstractItemModel* model = processModel_.data();
    if (model != nullptr) {
        const QHash<int, QByteArray> roles =
            model->roleNames();
        const auto roleFor =
            [&roles](const QByteArray& name) {
                for (auto iterator = roles.cbegin();
                     iterator != roles.cend();
                     ++iterator) {
                    if (iterator.value() == name) {
                        return iterator.key();
                    }
                }
                return -1;
            };
        const int kindRole =
            roleFor(QByteArrayLiteral("kind"));
        const int statusRole =
            roleFor(QByteArrayLiteral("status"));

        if (statusRole >= 0) {
            for (int row = 0;
                 row < model->rowCount();
                 ++row) {
                const QModelIndex index =
                    model->index(row, 0);
                const QString kind =
                    kindRole < 0
                    ? QString()
                    : model->data(index, kindRole)
                          .toString()
                          .trimmed()
                          .toLower();
                if (kind == QStringLiteral("notice")) {
                    continue;
                }

                const QString status =
                    model->data(index, statusRole)
                        .toString()
                        .trimmed()
                        .toLower();
                if (status == QStringLiteral("running")
                    || status == QStringLiteral("waiting")) {
                    ++count;
                }
            }
        }
    }

    if (count == activeProcessCount_) {
        return;
    }

    activeProcessCount_ = count;
    emit activeProcessCountChanged();
}

void CompanionShellViewModel::reconcileProcessTarget()
{
    if (!processTargetActive()) {
        return;
    }

    const auto clearIdleTarget = [this]() {
        if (processSending_
            || !processDraft_.trimmed().isEmpty()) {
            return;
        }
        clearProcessTargetState();
        emit processActionStateChanged();
    };

    QAbstractItemModel* model = processModel_.data();
    if (model == nullptr) {
        clearIdleTarget();
        return;
    }

    const QHash<int, QByteArray> roles =
        model->roleNames();
    const auto roleFor =
        [&roles](const QByteArray& name) {
            for (auto iterator = roles.cbegin();
                 iterator != roles.cend();
                 ++iterator) {
                if (iterator.value() == name) {
                    return iterator.key();
                }
            }
            return -1;
        };
    int idRole =
        roleFor(QByteArrayLiteral("processId"));
    if (idRole < 0) {
        idRole = roleFor(QByteArrayLiteral("id"));
    }
    if (idRole < 0) {
        clearIdleTarget();
        return;
    }

    const int threadIdRole =
        roleFor(QByteArrayLiteral("threadId"));
    const int titleRole =
        roleFor(QByteArrayLiteral("title"));
    const int kindRole =
        roleFor(QByteArrayLiteral("kind"));
    const int cwdRole =
        roleFor(QByteArrayLiteral("cwd"));
    const int activeTurnIdRole =
        roleFor(QByteArrayLiteral("activeTurnId"));
    const int modelRole =
        roleFor(QByteArrayLiteral("model"));
    const int reasoningEffortRole =
        roleFor(QByteArrayLiteral(
            "reasoningEffort"));
    const auto text =
        [](QAbstractItemModel* source,
           const QModelIndex& index,
           int role) {
            return role < 0
                ? QString()
                : source->data(index, role)
                      .toString()
                      .trimmed();
        };

    for (int row = 0; row < model->rowCount();
         ++row) {
        const QModelIndex index =
            model->index(row, 0);
        if (text(model, index, idRole)
            != processTargetId_) {
            continue;
        }

        const QString threadId =
            text(model, index, threadIdRole);
        const QString kind =
            text(model, index, kindRole)
                .toLower();
        if (threadId.isEmpty()
            || kind == QStringLiteral("notice")) {
            clearIdleTarget();
            return;
        }

        QString title =
            text(model, index, titleRole);
        if (title.isEmpty()) {
            title = QStringLiteral("Codex task");
        }
        const QString cwd =
            text(model, index, cwdRole);
        const QString activeTurnId =
            text(model, index, activeTurnIdRole);
        const QString modelId =
            text(model, index, modelRole);
        const QString reasoningEffort =
            text(
                model,
                index,
                reasoningEffortRole);
        if (processTargetThreadId_ == threadId
            && processTargetTitle_ == title
            && processTargetCwd_ == cwd
            && processTargetActiveTurnId_
                == activeTurnId
            && processTargetModel_ == modelId
            && processTargetReasoningEffort_
                == reasoningEffort) {
            return;
        }

        processTargetThreadId_ = threadId;
        processTargetTitle_ = std::move(title);
        processTargetCwd_ = cwd;
        processTargetActiveTurnId_ =
            activeTurnId;
        processTargetModel_ = modelId;
        processTargetReasoningEffort_ =
            reasoningEffort;
        emit processActionStateChanged();
        return;
    }

    clearIdleTarget();
}

void CompanionShellViewModel::clearProcessTargetState()
{
    processTargetId_.clear();
    processTargetThreadId_.clear();
    processTargetTitle_.clear();
    processTargetAction_.clear();
    processTargetCwd_.clear();
    processTargetActiveTurnId_.clear();
    processTargetModel_.clear();
    processTargetReasoningEffort_.clear();
    processDraft_.clear();
    processFeedback_.clear();
    processFeedbackIsError_ = false;
}

} // namespace companion
