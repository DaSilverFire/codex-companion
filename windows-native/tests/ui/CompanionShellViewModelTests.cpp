#include "ui/CompanionShellViewModel.h"

#include <QMetaProperty>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QUuid>
#include <QVariantMap>
#include <QtTest>

namespace {

QVariantMap goal(
    QString status = QStringLiteral("active"),
    QString objective = QStringLiteral("Ship Windows Companion"))
{
    return {
        {QStringLiteral("threadId"), QStringLiteral("thread-goal")},
        {QStringLiteral("objective"), std::move(objective)},
        {QStringLiteral("status"), std::move(status)},
        {QStringLiteral("tokensUsed"), 4200},
        {QStringLiteral("elapsedSeconds"), 125},
        {QStringLiteral("createdAt"), 1000},
        {QStringLiteral("updatedAt"), 2000},
    };
}

QVariantMap process(
    QString status = QStringLiteral("running"),
    bool needsApproval = false)
{
    const QString runtimeStatus =
        status == QStringLiteral("failed")
        ? QStringLiteral("idle")
        : QStringLiteral("active");
    return {
        {QStringLiteral("id"), QStringLiteral(" thread-process ")},
        {QStringLiteral("threadId"), QStringLiteral(" thread-process ")},
        {QStringLiteral("kind"), QStringLiteral("thread")},
        {QStringLiteral("title"), QStringLiteral(" Port Codex Companion ")},
        {QStringLiteral("status"), std::move(status)},
        {QStringLiteral("needsApproval"), needsApproval},
        {QStringLiteral("runtimeStatus"), runtimeStatus},
        {QStringLiteral("rolloutPath"), QStringLiteral(" C:\\rollouts\\thread-process.jsonl ")},
        {QStringLiteral("updatedAt"), 1700000000.0},
        {QStringLiteral("goal"), QVariant()},
        {QStringLiteral("cwd"), QStringLiteral(" C:\\worktree ")},
        {QStringLiteral("activeTurnId"), QStringLiteral(" turn-active ")},
        {QStringLiteral("model"), QStringLiteral(" gpt-test ")},
        {QStringLiteral("reasoningEffort"), QStringLiteral(" high ")},
    };
}

enum TestProcessRole {
    TestIdRole = Qt::UserRole + 1,
    TestThreadIdRole,
    TestTitleRole,
    TestKindRole,
    TestStatusRole,
    TestRuntimeStatusRole,
    TestRolloutPathRole,
    TestUpdatedAtRole,
    TestGoalRole,
    TestCwdRole,
    TestActiveTurnIdRole,
    TestModelRole,
    TestReasoningEffortRole,
};

void configureProcessModel(
    QStandardItemModel& model)
{
    model.setColumnCount(1);
    model.setItemRoleNames({
        {TestIdRole, QByteArrayLiteral("id")},
        {TestThreadIdRole,
         QByteArrayLiteral("threadId")},
        {TestTitleRole, QByteArrayLiteral("title")},
        {TestKindRole, QByteArrayLiteral("kind")},
        {TestStatusRole, QByteArrayLiteral("status")},
        {TestRuntimeStatusRole,
         QByteArrayLiteral("runtimeStatus")},
        {TestRolloutPathRole,
         QByteArrayLiteral("rolloutPath")},
        {TestUpdatedAtRole,
         QByteArrayLiteral("updatedAt")},
        {TestGoalRole, QByteArrayLiteral("goal")},
        {TestCwdRole, QByteArrayLiteral("cwd")},
        {TestActiveTurnIdRole,
         QByteArrayLiteral("activeTurnId")},
        {TestModelRole, QByteArrayLiteral("model")},
        {TestReasoningEffortRole,
         QByteArrayLiteral("reasoningEffort")},
    });
}

void appendProcess(
    QStandardItemModel& model,
    const QVariantMap& process)
{
    const int row = model.rowCount();
    model.insertRow(row);
    const QModelIndex index = model.index(row, 0);
    model.setData(
        index,
        process.value(QStringLiteral("id")),
        TestIdRole);
    model.setData(
        index,
        process.value(QStringLiteral("threadId")),
        TestThreadIdRole);
    model.setData(
        index,
        process.value(QStringLiteral("title")),
        TestTitleRole);
    model.setData(
        index,
        process.value(
            QStringLiteral("kind"),
            QStringLiteral("thread")),
        TestKindRole);
    model.setData(
        index,
        process.value(QStringLiteral("status")),
        TestStatusRole);
    model.setData(
        index,
        process.value(
            QStringLiteral("runtimeStatus")),
        TestRuntimeStatusRole);
    model.setData(
        index,
        process.value(
            QStringLiteral("rolloutPath")),
        TestRolloutPathRole);
    model.setData(
        index,
        process.value(QStringLiteral("updatedAt")),
        TestUpdatedAtRole);
    model.setData(
        index,
        process.value(QStringLiteral("goal")),
        TestGoalRole);
    model.setData(
        index,
        process.value(QStringLiteral("cwd")),
        TestCwdRole);
    model.setData(
        index,
        process.value(
            QStringLiteral("activeTurnId")),
        TestActiveTurnIdRole);
    model.setData(
        index,
        process.value(QStringLiteral("model")),
        TestModelRole);
    model.setData(
        index,
        process.value(
            QStringLiteral("reasoningEffort")),
        TestReasoningEffortRole);
}

} // namespace

class CompanionShellViewModelTests final : public QObject {
    Q_OBJECT

private slots:
    void defaultsMatchWindowsCompanionShell()
    {
        companion::CompanionShellViewModel viewModel;

        QCOMPARE(viewModel.routeMode(), QStringLiteral("local-chat"));
        QCOMPARE(viewModel.selectedChatModelId(), QStringLiteral("on-device"));
        QVERIFY(!viewModel.chatModels().isEmpty());
        QVERIFY(!viewModel.chatSendEnabled());
        QVERIFY(!viewModel.chatPreparationEnabled());
        QVERIFY(!viewModel.processCommandBusy());
    }

    void routeCommandsSwitchOnlyWhenNeeded()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy routeSpy(
            &viewModel,
            &companion::CompanionShellViewModel::routeModeChanged);
        QVERIFY(routeSpy.isValid());

        viewModel.showLocalChat();
        QCOMPARE(routeSpy.count(), 0);

        viewModel.showProcesses();
        QCOMPARE(viewModel.routeMode(), QStringLiteral("processes"));
        QCOMPARE(routeSpy.count(), 1);

        viewModel.showProcesses();
        QCOMPARE(routeSpy.count(), 1);

        viewModel.showLocalChat();
        QCOMPARE(viewModel.routeMode(), QStringLiteral("local-chat"));
        QCOMPARE(routeSpy.count(), 2);
    }

    void showingProcessesClearsChatResponseLikeMacReference()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.setChatStatus(
            true,
            false,
            false,
            QStringLiteral("Stale answer"),
            QStringLiteral("Ready"),
            QStringLiteral("Old prompt"),
            QStringLiteral("5.6 Terra"),
            QStringLiteral("12 in \u00b7 34 out"));
        QSignalSpy routeSpy(
            &viewModel,
            &companion::CompanionShellViewModel::routeModeChanged);
        QSignalSpy statusSpy(
            &viewModel,
            &companion::CompanionShellViewModel::chatStatusChanged);
        QVERIFY(routeSpy.isValid());
        QVERIFY(statusSpy.isValid());

        viewModel.showProcesses();

        QCOMPARE(viewModel.routeMode(), QStringLiteral("processes"));
        QVERIFY(viewModel.chatResponse().isEmpty());
        QVERIFY(viewModel.chatResponsePrompt().isEmpty());
        QVERIFY(viewModel.chatResponseTitle().isEmpty());
        QVERIFY(viewModel.chatResponseUsageSummary().isEmpty());
        QVERIFY(viewModel.chatSendEnabled());
        QCOMPARE(
            viewModel.chatStatusMessage(),
            QStringLiteral("Ready"));
        QCOMPARE(routeSpy.count(), 1);
        QCOMPARE(statusSpy.count(), 1);

        viewModel.showLocalChat();

        QCOMPARE(viewModel.routeMode(), QStringLiteral("local-chat"));
        QVERIFY(viewModel.chatResponse().isEmpty());
        QCOMPARE(routeSpy.count(), 2);
        QCOMPARE(statusSpy.count(), 1);

        viewModel.showProcesses();

        QCOMPARE(viewModel.routeMode(), QStringLiteral("processes"));
        QVERIFY(viewModel.chatResponse().isEmpty());
        QCOMPARE(routeSpy.count(), 3);
        QCOMPARE(statusSpy.count(), 1);
    }

    void modelSelectionAcceptsCatalogIdsAndRejectsUnknownIds()
    {
        companion::CompanionShellViewModel viewModel;
        const QVariantList models = viewModel.chatModels();
        QSignalSpy selectionSpy(
            &viewModel,
            &companion::CompanionShellViewModel::selectedChatModelIdChanged);
        QVERIFY(selectionSpy.isValid());

        QCOMPARE(models.size(), 7);
        QCOMPARE(
            models.at(0).toMap().value(QStringLiteral("group")).toString(),
            QStringLiteral("on-device"));
        QCOMPARE(
            models.at(1).toMap().value(QStringLiteral("group")).toString(),
            QStringLiteral("openai"));
        QCOMPARE(
            models.at(4).toMap().value(QStringLiteral("group")).toString(),
            QStringLiteral("lumo"));
        QVERIFY(
            !models.at(0)
                 .toMap()
                 .value(QStringLiteral("detail"))
                 .toString()
                 .isEmpty());

        viewModel.setSelectedChatModelId(QStringLiteral("openai:gpt56Terra"));

        QCOMPARE(
            viewModel.selectedChatModelId(),
            QStringLiteral("openai:gpt56Terra"));
        QCOMPARE(selectionSpy.count(), 1);

        viewModel.setSelectedChatModelId(QStringLiteral("unknown"));

        QCOMPARE(
            viewModel.selectedChatModelId(),
            QStringLiteral("openai:gpt56Terra"));
        QCOMPARE(selectionSpy.count(), 1);
    }

    void persistedModelSelectionIsRestoredAndUnknownIdsFallBack()
    {
        companion::CompanionShellViewModel restored(
            QStringLiteral("lumo:thinking"),
            {});
        companion::CompanionShellViewModel invalid(
            QStringLiteral("future:model"),
            {});

        QCOMPARE(
            restored.selectedChatModelId(),
            QStringLiteral("lumo:thinking"));
        QCOMPARE(
            invalid.selectedChatModelId(),
            QStringLiteral("on-device"));
    }

    void failedModelSelectionPersistenceKeepsCurrentSelection()
    {
        companion::CompanionShellViewModel viewModel(
            QStringLiteral("on-device"),
            [](const QString&) {
                return companion::Result<void>::failure({
                    QStringLiteral(
                        "settings.write-failed"),
                    QStringLiteral(
                        "Failed to persist model selection."),
                    false,
                    {},
                });
            });
        QSignalSpy selectionSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                selectedChatModelIdChanged);
        QSignalSpy errorSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                runtimeErrorOccurred);
        QVERIFY(selectionSpy.isValid());
        QVERIFY(errorSpy.isValid());

        QVERIFY(!viewModel.chooseChatModel(
            QStringLiteral("openai:gpt56Terra")));

        QCOMPARE(
            viewModel.selectedChatModelId(),
            QStringLiteral("on-device"));
        QCOMPARE(selectionSpy.count(), 0);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(
            qvariant_cast<companion::CompanionError>(
                errorSpy.takeFirst().at(0))
                .code,
            QStringLiteral("settings.write-failed"));
    }

    void modelSelectionPreservesResponseFromPreviousProvider()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.setChatStatus(
            true,
            false,
            false,
            QStringLiteral("Previous provider answer"),
            QStringLiteral("Ready"),
            QStringLiteral("Previous prompt"),
            QStringLiteral("On-device"),
            QStringLiteral("Private on-device response"));
        QSignalSpy statusSpy(
            &viewModel,
            &companion::CompanionShellViewModel::chatStatusChanged);
        QVERIFY(statusSpy.isValid());

        viewModel.setSelectedChatModelId(
            QStringLiteral(
                "openai:gpt56Luna"));

        QCOMPARE(
            viewModel.selectedChatModelId(),
            QStringLiteral(
                "openai:gpt56Luna"));
        QCOMPARE(
            viewModel.chatResponse(),
            QStringLiteral("Previous provider answer"));
        QCOMPARE(
            viewModel.chatResponsePrompt(),
            QStringLiteral("Previous prompt"));
        QCOMPARE(
            viewModel.chatResponseTitle(),
            QStringLiteral("On-device"));
        QCOMPARE(
            viewModel.chatResponseUsageSummary(),
            QStringLiteral("Private on-device response"));
        QCOMPARE(statusSpy.count(), 0);
    }

    void dismissingChatResponsePreservesAvailabilityAndStatus()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.setChatStatus(
            true,
            false,
            false,
            QStringLiteral("Answer to dismiss"),
            QStringLiteral("Ready"),
            QStringLiteral("Dismiss this prompt"),
            QStringLiteral("5.6 Terra"),
            QStringLiteral("12 in \u00b7 34 out"));
        QSignalSpy statusSpy(
            &viewModel,
            &companion::CompanionShellViewModel::chatStatusChanged);
        QVERIFY(statusSpy.isValid());

        viewModel.dismissChatResponse();

        QVERIFY(viewModel.chatResponse().isEmpty());
        QVERIFY(viewModel.chatResponsePrompt().isEmpty());
        QVERIFY(viewModel.chatResponseTitle().isEmpty());
        QVERIFY(viewModel.chatResponseUsageSummary().isEmpty());
        QVERIFY(viewModel.chatSendEnabled());
        QCOMPARE(
            viewModel.chatStatusMessage(),
            QStringLiteral("Ready"));
        QCOMPARE(statusSpy.count(), 1);

        viewModel.dismissChatResponse();
        QCOMPARE(statusSpy.count(), 1);
    }

    void chatModelTitlesMatchTheVisibleModelCatalog()
    {
        companion::CompanionShellViewModel viewModel;

        QCOMPARE(
            viewModel.chatModelTitle(
                QStringLiteral("on-device")),
            QStringLiteral("On-device"));
        QCOMPARE(
            viewModel.chatModelTitle(
                QStringLiteral(
                    "openai:gpt56Terra")),
            QStringLiteral("5.6 Terra"));
        QCOMPARE(
            viewModel.chatModelTitle(
                QStringLiteral("lumo:thinking")),
            QStringLiteral("Lumo Thinking"));
        QVERIFY(
            viewModel.chatModelTitle(
                QStringLiteral("unknown"))
                .isEmpty());
    }

    void chatPromptPlaceholderMatchesTheSelectedProvider()
    {
        companion::CompanionShellViewModel viewModel;

        QCOMPARE(
            viewModel.chatPromptPlaceholder(),
            QStringLiteral("Ask on device"));

        viewModel.setSelectedChatModelId(
            QStringLiteral("openai:gpt56Terra"));
        QCOMPARE(
            viewModel.chatPromptPlaceholder(),
            QStringLiteral("Ask ChatGPT"));

        viewModel.setSelectedChatModelId(
            QStringLiteral("lumo:thinking"));
        QCOMPARE(
            viewModel.chatPromptPlaceholder(),
            QStringLiteral("Ask Lumo"));
    }

    void processModelCanBeAttachedWithoutCopyingIt()
    {
        companion::CompanionShellViewModel viewModel;
        QStandardItemModel processModel;
        QSignalSpy modelSpy(
            &viewModel,
            &companion::CompanionShellViewModel::processModelChanged);
        QVERIFY(modelSpy.isValid());

        viewModel.setProcessModel(&processModel);

        QCOMPARE(viewModel.processModel(), &processModel);
        QCOMPARE(modelSpy.count(), 1);

        viewModel.setProcessModel(&processModel);
        QCOMPARE(modelSpy.count(), 1);
    }

    void activeProcessCountTracksRunningAndWaitingRows()
    {
        companion::CompanionShellViewModel viewModel;
        QStandardItemModel processModel;
        configureProcessModel(processModel);
        QSignalSpy countSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                activeProcessCountChanged);
        QVERIFY(countSpy.isValid());

        viewModel.setProcessModel(&processModel);
        QCOMPARE(viewModel.activeProcessCount(), 0);

        appendProcess(processModel, process());
        QCOMPARE(viewModel.activeProcessCount(), 1);

        QVariantMap notice =
            process(QStringLiteral("waiting"));
        notice.insert(
            QStringLiteral("kind"),
            QStringLiteral("notice"));
        appendProcess(processModel, notice);
        QCOMPARE(viewModel.activeProcessCount(), 1);

        appendProcess(
            processModel,
            process(QStringLiteral("completed")));
        QCOMPARE(viewModel.activeProcessCount(), 1);

        appendProcess(
            processModel,
            process(QStringLiteral("waiting")));
        QCOMPARE(viewModel.activeProcessCount(), 2);

        processModel.setData(
            processModel.index(0, 0),
            QStringLiteral("completed"),
            TestStatusRole);
        QCOMPARE(viewModel.activeProcessCount(), 1);

        processModel.removeRow(3);
        QCOMPARE(viewModel.activeProcessCount(), 0);
        QVERIFY(countSpy.count() >= 4);
    }

    void selectedProcessTargetTracksModelChanges()
    {
        companion::CompanionShellViewModel viewModel;
        QStandardItemModel processModel;
        configureProcessModel(processModel);
        appendProcess(processModel, process());
        viewModel.setProcessModel(&processModel);
        viewModel.beginProcessAction(
            process(),
            QStringLiteral("steer"));
        QSignalSpy requestSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processMessageRequested);
        QVERIFY(requestSpy.isValid());

        const QModelIndex index =
            processModel.index(0, 0);
        processModel.setData(
            index,
            QStringLiteral("thread-refreshed"),
            TestThreadIdRole);
        processModel.setData(
            index,
            QStringLiteral("Refreshed title"),
            TestTitleRole);
        processModel.setData(
            index,
            QStringLiteral("D:\\refreshed"),
            TestCwdRole);
        processModel.setData(
            index,
            QStringLiteral("turn-refreshed"),
            TestActiveTurnIdRole);
        processModel.setData(
            index,
            QStringLiteral("gpt-refreshed"),
            TestModelRole);
        processModel.setData(
            index,
            QStringLiteral("medium"),
            TestReasoningEffortRole);

        QCOMPARE(
            viewModel.processTargetTitle(),
            QStringLiteral("Refreshed title"));
        viewModel.setProcessDraft(
            QStringLiteral("Use the current target"));
        viewModel.submitProcessMessage();

        QCOMPARE(requestSpy.count(), 1);
        const QList<QVariant> request =
            requestSpy.takeFirst();
        QCOMPARE(
            request.at(1).toString(),
            QStringLiteral("thread-refreshed"));
        QCOMPARE(
            request.at(3).toString(),
            QStringLiteral("D:\\refreshed"));
        QCOMPARE(
            request.at(4).toString(),
            QStringLiteral("turn-refreshed"));
        QCOMPARE(
            request.at(5).toString(),
            QStringLiteral("gpt-refreshed"));
        QCOMPARE(
            request.at(6).toString(),
            QStringLiteral("medium"));
    }

    void disappearingProcessClearsOnlyAnIdleEmptyTarget()
    {
        companion::CompanionShellViewModel viewModel;
        QStandardItemModel processModel;
        configureProcessModel(processModel);
        appendProcess(processModel, process());
        viewModel.setProcessModel(&processModel);

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("reply"));
        QVERIFY(viewModel.processTargetActive());
        processModel.removeRow(0);
        QVERIFY(!viewModel.processTargetActive());

        appendProcess(processModel, process());
        viewModel.beginProcessAction(
            process(),
            QStringLiteral("reply"));
        viewModel.setProcessDraft(
            QStringLiteral("Preserve this draft"));
        processModel.removeRow(0);

        QVERIFY(viewModel.processTargetActive());
        QCOMPARE(
            viewModel.processDraft(),
            QStringLiteral("Preserve this draft"));
    }

    void unassignedProcessCannotBecomeAnActionTarget()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy approvalSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processApprovalRequested);
        QVERIFY(approvalSpy.isValid());
        QVariantMap unassigned =
            process(QStringLiteral("waiting"), true);
        unassigned.insert(
            QStringLiteral("id"),
            QStringLiteral("job-unassigned"));
        unassigned.insert(
            QStringLiteral("threadId"),
            QStringLiteral(" "));

        viewModel.beginProcessAction(
            unassigned,
            QStringLiteral("reply"));
        QVERIFY(!viewModel.processTargetActive());

        viewModel.respondToProcessApproval(
            unassigned,
            QStringLiteral("approveOnce"));
        QCOMPARE(approvalSpy.count(), 0);
    }

    void processDraftPropertyIsWritableThroughMetaObject()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.beginProcessAction(
            process(),
            QStringLiteral("reply"));

        const int propertyIndex =
            viewModel.metaObject()->indexOfProperty(
                "processDraft");
        QVERIFY(propertyIndex >= 0);
        const QMetaProperty property =
            viewModel.metaObject()->property(
                propertyIndex);
        QVERIFY(property.isWritable());
        QVERIFY(viewModel.setProperty(
            "processDraft",
            QStringLiteral("Typed through QML")));
        QCOMPARE(
            viewModel.processDraft(),
            QStringLiteral("Typed through QML"));
    }

    void processTargetSubmissionPreservesDraftOnFailureAndClearsOnSuccess()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy requestSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processMessageRequested);
        QVERIFY(requestSpy.isValid());

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("reply"));

        QVERIFY(viewModel.processTargetActive());
        QCOMPARE(
            viewModel.processTargetId(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            viewModel.processTargetTitle(),
            QStringLiteral("Port Codex Companion"));
        QCOMPARE(
            viewModel.processTargetAction(),
            QStringLiteral("reply"));
        QVERIFY(viewModel.processDraft().isEmpty());
        QVERIFY(!viewModel.processSending());

        viewModel.setProcessDraft(
            QStringLiteral("  Keep working on parity  "));
        viewModel.submitProcessMessage();

        QCOMPARE(requestSpy.count(), 1);
        const QList<QVariant> request =
            requestSpy.takeFirst();
        QCOMPARE(
            request.at(0).toString(),
            QStringLiteral("reply"));
        QCOMPARE(
            request.at(1).toString(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            request.at(2).toString(),
            QStringLiteral("Keep working on parity"));
        QCOMPARE(
            request.at(3).toString(),
            QStringLiteral("C:\\worktree"));
        QCOMPARE(
            request.at(4).toString(),
            QStringLiteral("turn-active"));
        QCOMPARE(
            request.at(5).toString(),
            QStringLiteral("gpt-test"));
        QCOMPARE(
            request.at(6).toString(),
            QStringLiteral("high"));
        QVERIFY(viewModel.processSending());

        viewModel.submitProcessMessage();
        QCOMPARE(requestSpy.count(), 0);

        viewModel.finishProcessMessage(
            false,
            QStringLiteral("Synthetic send failure."));

        QVERIFY(!viewModel.processSending());
        QVERIFY(viewModel.processTargetActive());
        QCOMPARE(
            viewModel.processDraft(),
            QStringLiteral("  Keep working on parity  "));
        QCOMPARE(
            viewModel.processFeedback(),
            QStringLiteral("Synthetic send failure."));
        QVERIFY(viewModel.processFeedbackIsError());

        viewModel.submitProcessMessage();
        QCOMPARE(requestSpy.count(), 1);
        viewModel.finishProcessMessage(true, {});

        QVERIFY(!viewModel.processSending());
        QVERIFY(!viewModel.processTargetActive());
        QVERIFY(viewModel.processDraft().isEmpty());
        QVERIFY(viewModel.processFeedback().isEmpty());
        QVERIFY(!viewModel.processFeedbackIsError());
    }

    void processActionsUseThreadIdentityWithoutLosingCardIdentity()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy messageSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processMessageRequested);
        QSignalSpy approvalSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processApprovalRequested);
        QVERIFY(messageSpy.isValid());
        QVERIFY(approvalSpy.isValid());

        QVariantMap assignedJob =
            process(QStringLiteral("waiting"), true);
        assignedJob.insert(
            QStringLiteral("id"),
            QStringLiteral("job-build"));
        assignedJob.insert(
            QStringLiteral("threadId"),
            QStringLiteral("thread-build"));

        viewModel.beginProcessAction(
            assignedJob,
            QStringLiteral("reply"));
        QCOMPARE(
            viewModel.processTargetId(),
            QStringLiteral("job-build"));
        viewModel.setProcessDraft(
            QStringLiteral("Continue the build"));
        viewModel.submitProcessMessage();

        QCOMPARE(messageSpy.count(), 1);
        QCOMPARE(
            messageSpy.takeFirst().at(1).toString(),
            QStringLiteral("thread-build"));
        viewModel.finishProcessMessage(true, {});

        viewModel.respondToProcessApproval(
            assignedJob,
            QStringLiteral("approveOnce"));

        QCOMPARE(approvalSpy.count(), 1);
        QCOMPARE(
            approvalSpy.takeFirst().at(0).toString(),
            QStringLiteral("thread-build"));
        QCOMPARE(
            viewModel.approvingProcessId(),
            QStringLiteral("job-build"));
    }

    void processTargetRejectsInvalidActionsAndCancelClearsDraft()
    {
        companion::CompanionShellViewModel viewModel;

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("unknown"));
        QVERIFY(!viewModel.processTargetActive());

        QVariantMap missingThread = process();
        missingThread.insert(
            QStringLiteral("id"),
            QStringLiteral(" "));
        viewModel.beginProcessAction(
            missingThread,
            QStringLiteral("reply"));
        QVERIFY(!viewModel.processTargetActive());

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("steer"));
        viewModel.setProcessDraft(
            QStringLiteral("Redirect the active turn"));
        viewModel.cancelProcessTarget();

        QVERIFY(!viewModel.processTargetActive());
        QVERIFY(viewModel.processDraft().isEmpty());
        QVERIFY(viewModel.processFeedback().isEmpty());
    }

    void cancelingPendingProcessRequestsRuntimeCancellation()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy cancelSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processCancelRequested);
        QVERIFY(cancelSpy.isValid());

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("steer"));
        viewModel.setProcessDraft(
            QStringLiteral("Redirect the active turn"));
        viewModel.submitProcessMessage();
        QVERIFY(viewModel.processSending());

        viewModel.cancelProcessTarget();

        QCOMPARE(cancelSpy.count(), 1);
        QVERIFY(!viewModel.processSending());
        QVERIFY(!viewModel.processTargetActive());
        QVERIFY(viewModel.processDraft().isEmpty());

        viewModel.cancelProcessTarget();
        QCOMPARE(cancelSpy.count(), 1);
    }

    void olderCompletionDoesNotEraseNewerProcessComposer()
    {
        companion::CompanionShellViewModel viewModel;
        QVariantMap first = process();
        QVariantMap second = process();
        second.insert(
            QStringLiteral("id"),
            QStringLiteral("thread-new"));
        second.insert(
            QStringLiteral("threadId"),
            QStringLiteral("thread-new"));
        second.insert(
            QStringLiteral("title"),
            QStringLiteral("New task"));

        viewModel.beginProcessAction(
            first,
            QStringLiteral("reply"));
        viewModel.setProcessDraft(
            QStringLiteral("Same prompt"));
        viewModel.submitProcessMessage();
        QVERIFY(viewModel.processSending());

        viewModel.beginProcessAction(
            second,
            QStringLiteral("reply"));
        viewModel.setProcessDraft(
            QStringLiteral("Same prompt"));

        QCOMPARE(
            viewModel.processTargetId(),
            QStringLiteral("thread-new"));
        QCOMPARE(
            viewModel.processDraft(),
            QStringLiteral("Same prompt"));

        viewModel.finishProcessMessage(true, {});

        QVERIFY(!viewModel.processSending());
        QVERIFY(viewModel.processTargetActive());
        QCOMPARE(
            viewModel.processTargetId(),
            QStringLiteral("thread-new"));
        QCOMPARE(
            viewModel.processDraft(),
            QStringLiteral("Same prompt"));
        QCOMPARE(
            viewModel.processFeedback(),
            QStringLiteral(
                "Message sent. Your newer draft is still here."));
    }

    void approvalRequestsNormalizeDecisionAndSuppressDuplicates()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy approvalSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processApprovalRequested);
        QVERIFY(approvalSpy.isValid());

        viewModel.respondToProcessApproval(
            process(QStringLiteral("waiting"), true),
            QStringLiteral("approveSimilar"));

        QCOMPARE(approvalSpy.count(), 1);
        const QList<QVariant> request =
            approvalSpy.takeFirst();
        QCOMPARE(
            request.at(0).toString(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            request.at(1).toString(),
            QStringLiteral("approveSimilar"));
        QCOMPARE(
            viewModel.approvingProcessId(),
            QStringLiteral("thread-process"));

        viewModel.respondToProcessApproval(
            process(QStringLiteral("waiting"), true),
            QStringLiteral("approveOnce"));
        QCOMPARE(approvalSpy.count(), 0);

        viewModel.finishProcessApproval(
            false,
            QStringLiteral("Synthetic approval failure."));
        QVERIFY(viewModel.approvingProcessId().isEmpty());
        QCOMPARE(
            viewModel.processFeedback(),
            QStringLiteral("Synthetic approval failure."));
        QVERIFY(viewModel.processFeedbackIsError());

        viewModel.respondToProcessApproval(
            process(QStringLiteral("waiting"), false),
            QStringLiteral("approveOnce"));
        QCOMPARE(approvalSpy.count(), 0);
    }

    void processMessagesAndApprovalsTrackIndependentPendingState()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy messageSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processMessageRequested);
        QSignalSpy approvalSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processApprovalRequested);
        QVERIFY(messageSpy.isValid());
        QVERIFY(approvalSpy.isValid());

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("reply"));
        viewModel.setProcessDraft(
            QStringLiteral("Keep going"));
        viewModel.submitProcessMessage();

        QVERIFY(viewModel.processCommandBusy());
        QCOMPARE(messageSpy.count(), 1);

        viewModel.respondToProcessApproval(
            process(QStringLiteral("waiting"), true),
            QStringLiteral("approveOnce"));
        QCOMPARE(approvalSpy.count(), 1);
        QVERIFY(!viewModel.approvingProcessId().isEmpty());

        viewModel.finishProcessMessage(true, {});
        QVERIFY(viewModel.processCommandBusy());

        viewModel.beginProcessAction(
            process(),
            QStringLiteral("steer"));
        QVERIFY(viewModel.processTargetActive());
        QCOMPARE(
            viewModel.processTargetAction(),
            QStringLiteral("steer"));

        viewModel.finishProcessApproval(true, {});
        QVERIFY(!viewModel.processCommandBusy());
    }

    void failedStoppedProcessCanRequestOneRetryAndPublishResult()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());
        const QVariantMap failed =
            process(QStringLiteral("failed"));

        viewModel.retryFailedProcess(failed);

        QCOMPARE(retrySpy.count(), 1);
        QCOMPARE(
            viewModel.retryingProcessId(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            viewModel.processRetryStatusId(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            viewModel.processRetryStatus(),
            QStringLiteral("Retrying..."));
        QVERIFY(
            !viewModel
                 .processRetryStatusIsError());
        QVERIFY(viewModel.processCommandBusy());
        const QVariantMap requested =
            retrySpy.takeFirst().at(0).toMap();
        QCOMPARE(
            requested.value(
                         QStringLiteral("id"))
                .toString(),
            QStringLiteral("thread-process"));
        QCOMPARE(
            requested.value(
                         QStringLiteral(
                             "rolloutPath"))
                .toString(),
            QStringLiteral(
                "C:\\rollouts\\thread-process.jsonl"));

        viewModel.retryFailedProcess(failed);
        QCOMPARE(retrySpy.count(), 0);

        viewModel.finishProcessRetry(
            QStringLiteral("thread-process"),
            false,
            QStringLiteral(
                "No Codex account has available usage."));

        QVERIFY(
            viewModel.retryingProcessId()
                .isEmpty());
        QCOMPARE(
            viewModel.processRetryStatus(),
            QStringLiteral(
                "No Codex account has available usage."));
        QVERIFY(
            viewModel
                .processRetryStatusIsError());
        QVERIFY(!viewModel.processCommandBusy());

        viewModel.retryFailedProcess(failed);
        QCOMPARE(retrySpy.count(), 1);
        viewModel.finishProcessRetry(
            QStringLiteral("thread-process"),
            true,
            QStringLiteral(
                "Resumed with Account 2."));
        QCOMPARE(
            viewModel.processRetryStatus(),
            QStringLiteral(
                "Resumed with Account 2."));
        QVERIFY(
            !viewModel
                 .processRetryStatusIsError());
    }

    void systemErrorFailedProcessCanRequestRetry()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());
        QVariantMap failed =
            process(QStringLiteral("failed"));
        failed.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("systemError"));

        viewModel.retryFailedProcess(failed);

        QCOMPARE(retrySpy.count(), 1);
        QCOMPARE(
            viewModel.retryingProcessId(),
            QStringLiteral("thread-process"));
    }

    void failedWithoutRuntimeMetadataNormalizesToNotLoaded()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());
        QVariantMap failed =
            process(QStringLiteral("failed"));
        failed.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("   "));

        viewModel.retryFailedProcess(failed);

        QCOMPARE(retrySpy.count(), 1);
        const QVariantMap requested =
            retrySpy.takeFirst().at(0).toMap();
        QCOMPARE(
            requested.value(
                         QStringLiteral(
                             "runtimeStatus"))
                .toString(),
            QStringLiteral("notLoaded"));
    }

    void visiblyRunningRecoverableGoalCannotRequestRetry()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());
        QVariantMap running =
            process(QStringLiteral("running"));
        running.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("idle"));
        running.insert(
            QStringLiteral("goal"),
            goal(QStringLiteral("blocked")));

        viewModel.retryFailedProcess(running);

        QCOMPARE(retrySpy.count(), 0);
        QVERIFY(
            viewModel.retryingProcessId()
                .isEmpty());
    }

    void unsafeProcessesCannotRequestRetry()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());

        QVariantMap active =
            process(QStringLiteral("failed"));
        active.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("active"));
        viewModel.retryFailedProcess(active);

        QVariantMap completedGoal =
            process(QStringLiteral("failed"));
        completedGoal.insert(
            QStringLiteral("goal"),
            goal(QStringLiteral("complete")));
        viewModel.retryFailedProcess(
            completedGoal);

        QVariantMap budgetGoal =
            process(QStringLiteral("failed"));
        budgetGoal.insert(
            QStringLiteral("goal"),
            goal(QStringLiteral(
                "budgetLimited")));
        viewModel.retryFailedProcess(
            budgetGoal);

        viewModel.retryFailedProcess(process());

        QCOMPARE(retrySpy.count(), 0);
        QVERIFY(
            viewModel.retryingProcessId()
                .isEmpty());
        QVERIFY(
            viewModel.processRetryStatus()
                .isEmpty());
    }

    void stoppedUsageLimitedGoalCanRequestRetry()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy retrySpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                processRetryRequested);
        QVERIFY(retrySpy.isValid());

        QVariantMap stopped =
            process(QStringLiteral("waiting"));
        stopped.insert(
            QStringLiteral("runtimeStatus"),
            QStringLiteral("idle"));
        stopped.insert(
            QStringLiteral("goal"),
            goal(QStringLiteral(
                "usageLimited")));

        viewModel.retryFailedProcess(stopped);

        QCOMPARE(retrySpy.count(), 1);
        QCOMPARE(
            viewModel.retryingProcessId(),
            QStringLiteral("thread-process"));
    }

    void localChatSubmissionRequiresAvailabilityAndNormalizesPrompt()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy chatSpy(
            &viewModel,
            &companion::CompanionShellViewModel::localChatRequested);
        QVERIFY(chatSpy.isValid());

        viewModel.submitLocalChat(QStringLiteral(" ignored "));
        QCOMPARE(chatSpy.count(), 0);

        viewModel.setChatStatus(
            true,
            false,
            false,
            {},
            QStringLiteral("Ready"));
        viewModel.submitLocalChat(QStringLiteral("  hello Companion  "));

        QCOMPARE(chatSpy.count(), 1);
        QCOMPARE(chatSpy.takeFirst().at(0).toString(),
                 QStringLiteral("hello Companion"));
    }

    void onDevicePreparationRequiresAnAvailablePreparationAction()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy preparationSpy(
            &viewModel,
            &companion::CompanionShellViewModel::onDevicePreparationRequested);
        QVERIFY(preparationSpy.isValid());

        viewModel.prepareOnDeviceChat();
        QCOMPARE(preparationSpy.count(), 0);

        viewModel.setChatStatus(
            false,
            true,
            false,
            {},
            QStringLiteral("Model download required"));
        viewModel.prepareOnDeviceChat();

        QCOMPARE(preparationSpy.count(), 1);
    }

    void usageRefreshRetainsTheLastSnapshotAndSuppressesDuplicates()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy refreshSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                usageRefreshRequested);
        QVERIFY(refreshSpy.isValid());
        const QVariantMap snapshot{
            {QStringLiteral("planType"),
             QStringLiteral("plus")},
            {QStringLiteral("availableResetCount"), 2},
        };

        viewModel.setUsageStatus(
            false,
            snapshot,
            {});
        QCOMPARE(viewModel.usageSnapshot(), snapshot);
        QVERIFY(!viewModel.usageLoading());
        QVERIFY(viewModel.usageErrorMessage().isEmpty());

        viewModel.refreshUsage();
        QVERIFY(viewModel.usageLoading());
        QCOMPARE(refreshSpy.count(), 1);

        viewModel.refreshUsage();
        QCOMPARE(refreshSpy.count(), 1);

        viewModel.refreshUsageAfterAccountChange();
        QVERIFY(viewModel.usageLoading());
        QCOMPARE(refreshSpy.count(), 2);

        viewModel.setUsageStatus(
            false,
            {},
            QStringLiteral(
                "Codex usage is temporarily unavailable."));
        QVERIFY(!viewModel.usageLoading());
        QCOMPARE(viewModel.usageSnapshot(), snapshot);
        QCOMPARE(
            viewModel.usageErrorMessage(),
            QStringLiteral(
                "Codex usage is temporarily unavailable."));
    }

    void usageResetRequiresConfirmationAndSuppressesDuplicates()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy resetSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                usageResetRequested);
        QVERIFY(resetSpy.isValid());
        const QVariantMap credit{
            {QStringLiteral("id"),
             QStringLiteral("credit-weekly")},
            {QStringLiteral("displayTitle"),
             QStringLiteral("Weekly Codex reset")},
        };
        const QVariantMap snapshot{
            {QStringLiteral("availableResetCount"), 1},
            {
                QStringLiteral(
                    "availableResetCredits"),
                QVariantList{credit},
            },
        };
        viewModel.setUsageStatus(
            false,
            snapshot,
            {});

        viewModel.prepareUsageReset({
            {QStringLiteral("id"),
             QStringLiteral("not-available")},
            {QStringLiteral("displayTitle"),
             QStringLiteral("Unknown reset")},
        });
        QVERIFY(
            viewModel
                .usageResetConfirmation()
                .isEmpty());

        viewModel.prepareUsageReset(credit);
        const QVariantMap firstConfirmation =
            viewModel.usageResetConfirmation();
        QCOMPARE(
            firstConfirmation.value(
                QStringLiteral("creditId"))
                .toString(),
            QStringLiteral("credit-weekly"));
        QCOMPARE(
            firstConfirmation.value(
                QStringLiteral("displayTitle"))
                .toString(),
            QStringLiteral("Weekly Codex reset"));
        QVERIFY(
            !QUuid(
                 firstConfirmation.value(
                     QStringLiteral(
                         "idempotencyKey"))
                     .toString())
                 .isNull());
        QVERIFY(!viewModel.usageResetBusy());

        viewModel.cancelUsageReset();
        QVERIFY(
            viewModel
                .usageResetConfirmation()
                .isEmpty());

        viewModel.prepareUsageReset(credit);
        viewModel.confirmUsageReset();

        QVERIFY(viewModel.usageResetBusy());
        QVERIFY(
            viewModel
                .usageResetConfirmation()
                .isEmpty());
        QCOMPARE(
            viewModel.usageResetStatusMessage(),
            QStringLiteral(
                "Applying Weekly Codex reset..."));
        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(
            resetSpy.at(0).at(0).toString(),
            QStringLiteral("credit-weekly"));
        QVERIFY(
            !QUuid(
                 resetSpy.at(0).at(1).toString())
                 .isNull());

        viewModel.confirmUsageReset();
        QCOMPARE(resetSpy.count(), 1);

        viewModel.finishUsageReset(
            false,
            QStringLiteral(
                "That Codex reset is no longer available."));
        QVERIFY(!viewModel.usageResetBusy());
        QCOMPARE(
            viewModel.usageResetStatusMessage(),
            QStringLiteral(
                "That Codex reset is no longer available."));
    }

    void opensGoalControlsWithStatusCapabilities()
    {
        companion::CompanionShellViewModel viewModel;

        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());

        QVERIFY(viewModel.goalControlVisible());
        QCOMPARE(
            viewModel.goalTaskTitle(),
            QStringLiteral("Port Codex Companion"));
        QCOMPARE(
            viewModel.goalThreadId(),
            QStringLiteral("thread-goal"));
        QCOMPARE(
            viewModel.goalObjective(),
            QStringLiteral("Ship Windows Companion"));
        QCOMPARE(
            viewModel.goalDraftObjective(),
            QStringLiteral("Ship Windows Companion"));
        QCOMPARE(
            viewModel.goalStatus(),
            QStringLiteral("active"));
        QCOMPARE(viewModel.goalElapsedSeconds(), 125);
        QVERIFY(viewModel.goalCanEdit());
        QVERIFY(viewModel.goalCanPause());
        QVERIFY(!viewModel.goalCanResume());
        QVERIFY(!viewModel.goalEditing());
        QVERIFY(!viewModel.goalMutationPending());
        QVERIFY(viewModel.goalErrorMessage().isEmpty());

        viewModel.applyGoalSnapshot(goal(QStringLiteral("paused")));

        QCOMPARE(
            viewModel.goalStatus(),
            QStringLiteral("paused"));
        QVERIFY(viewModel.goalCanEdit());
        QVERIFY(!viewModel.goalCanPause());
        QVERIFY(viewModel.goalCanResume());

        viewModel.applyGoalSnapshot(goal(QStringLiteral("blocked")));
        QVERIFY(viewModel.goalCanResume());

        viewModel.applyGoalSnapshot(goal(QStringLiteral("complete")));
        QVERIFY(!viewModel.goalCanEdit());
        QVERIFY(!viewModel.goalCanPause());
        QVERIFY(!viewModel.goalCanResume());
    }

    void goalEditingValidatesAndPreservesDraftOnFailure()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy updateSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                goalUpdateRequested);
        QVERIFY(updateSpy.isValid());
        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());

        viewModel.beginGoalEditing();
        QVERIFY(viewModel.goalEditing());

        viewModel.setGoalDraftObjective(
            QStringLiteral("   "));
        viewModel.saveGoalEdit();

        QCOMPARE(updateSpy.count(), 0);
        QCOMPARE(
            viewModel.goalErrorMessage(),
            QStringLiteral(
                "Enter a goal objective before saving."));
        QVERIFY(viewModel.goalEditing());

        viewModel.setGoalDraftObjective(
            QStringLiteral("  Verify Windows parity  "));
        viewModel.saveGoalEdit();

        QCOMPARE(updateSpy.count(), 1);
        const QList<QVariant> request =
            updateSpy.takeFirst();
        QCOMPARE(
            request.at(0).toString(),
            QStringLiteral("thread-goal"));
        QCOMPARE(
            request.at(1).toString(),
            QStringLiteral("Verify Windows parity"));
        QVERIFY(viewModel.goalMutationPending());

        viewModel.finishGoalMutation(
            false,
            QStringLiteral("Synthetic goal failure."));

        QVERIFY(!viewModel.goalMutationPending());
        QVERIFY(viewModel.goalEditing());
        QCOMPARE(
            viewModel.goalDraftObjective(),
            QStringLiteral("  Verify Windows parity  "));
        QCOMPARE(
            viewModel.goalErrorMessage(),
            QStringLiteral("Synthetic goal failure."));

        viewModel.saveGoalEdit();
        QCOMPARE(updateSpy.count(), 1);
        viewModel.applyGoalSnapshot(goal());
        viewModel.applyGoalMutationResult(
            goal(
                QStringLiteral("complete"),
                QStringLiteral("Normalized Windows parity")));
        viewModel.finishGoalMutation(true, {});

        QVERIFY(!viewModel.goalMutationPending());
        QVERIFY(!viewModel.goalEditing());
        QCOMPARE(
            viewModel.goalObjective(),
            QStringLiteral("Normalized Windows parity"));
        QCOMPARE(
            viewModel.goalStatus(),
            QStringLiteral("complete"));
        QVERIFY(viewModel.goalErrorMessage().isEmpty());

        viewModel.applyGoalSnapshot(
            goal(
                QStringLiteral("active"),
                QStringLiteral("Verify Windows parity")));

        QCOMPARE(
            viewModel.goalObjective(),
            QStringLiteral("Verify Windows parity"));
        QVERIFY(viewModel.goalErrorMessage().isEmpty());
    }

    void completedGoalRefreshEndsEditingAndRemovalDismissesControls()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());
        viewModel.beginGoalEditing();
        viewModel.setGoalDraftObjective(
            QStringLiteral("Draft objective"));

        viewModel.applyGoalSnapshot(
            goal(
                QStringLiteral("complete"),
                QStringLiteral("Completed remotely")));

        QVERIFY(!viewModel.goalEditing());
        QCOMPARE(
            viewModel.goalDraftObjective(),
            QStringLiteral("Completed remotely"));
        QVERIFY(!viewModel.goalCanEdit());

        viewModel.removeGoalSnapshot(
            QStringLiteral("thread-goal"));

        QVERIFY(!viewModel.goalControlVisible());
    }

    void successfulUpdateWithoutSnapshotDoesNotGuessServerState()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());
        viewModel.beginGoalEditing();
        viewModel.setGoalDraftObjective(
            QStringLiteral("Requested objective"));
        viewModel.saveGoalEdit();

        viewModel.finishGoalMutation(true, {});

        QVERIFY(!viewModel.goalMutationPending());
        QVERIFY(!viewModel.goalEditing());
        QCOMPARE(
            viewModel.goalObjective(),
            QStringLiteral("Ship Windows Companion"));
        QCOMPARE(
            viewModel.goalStatus(),
            QStringLiteral("active"));
        QCOMPARE(
            viewModel.goalDraftObjective(),
            QStringLiteral("Ship Windows Companion"));
    }

    void routeChangeDismissesControlsWhileMutationContinues()
    {
        companion::CompanionShellViewModel viewModel;
        viewModel.showProcesses();
        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());
        viewModel.pauseGoal();

        QVERIFY(viewModel.goalMutationPending());
        viewModel.showLocalChat();

        QCOMPARE(
            viewModel.routeMode(),
            QStringLiteral("local-chat"));
        QVERIFY(!viewModel.goalControlVisible());
        QVERIFY(viewModel.goalMutationPending());

        viewModel.finishGoalMutation(
            false,
            QStringLiteral("Synthetic failure."));
        QVERIFY(!viewModel.goalControlVisible());
    }

    void goalPauseAndResumeRequestsFollowCurrentStatus()
    {
        companion::CompanionShellViewModel viewModel;
        QSignalSpy pauseSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                goalPauseRequested);
        QSignalSpy resumeSpy(
            &viewModel,
            &companion::CompanionShellViewModel::
                goalResumeRequested);
        QVERIFY(pauseSpy.isValid());
        QVERIFY(resumeSpy.isValid());
        viewModel.openGoalControls(
            QStringLiteral("Port Codex Companion"),
            goal());

        viewModel.pauseGoal();

        QCOMPARE(pauseSpy.count(), 1);
        QCOMPARE(
            pauseSpy.takeFirst().at(0).toString(),
            QStringLiteral("thread-goal"));
        QVERIFY(viewModel.goalMutationPending());
        viewModel.applyGoalSnapshot(goal(QStringLiteral("active")));
        viewModel.applyGoalMutationResult(
            goal(QStringLiteral("paused")));
        viewModel.finishGoalMutation(true, {});
        QCOMPARE(
            viewModel.goalStatus(),
            QStringLiteral("paused"));
        QVERIFY(viewModel.goalCanResume());

        viewModel.resumeGoal();

        QCOMPARE(resumeSpy.count(), 1);
        QCOMPARE(
            resumeSpy.takeFirst().at(0).toString(),
            QStringLiteral("thread-goal"));
        QVERIFY(viewModel.goalMutationPending());
        viewModel.applyGoalSnapshot(goal(QStringLiteral("paused")));
        viewModel.applyGoalMutationResult(
            goal(QStringLiteral("active")));
        viewModel.finishGoalMutation(true, {});
        QVERIFY(viewModel.goalCanPause());
    }
};

QTEST_GUILESS_MAIN(CompanionShellViewModelTests)
#include "CompanionShellViewModelTests.moc"
