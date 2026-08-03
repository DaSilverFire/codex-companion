import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQml.Models
import QtTest
import CodexCompanion

TestCase {
    name: "CompanionMenuWindow"
    when: windowShown
    property var createdWindows: []

    Component {
        id: signalSpy
        SignalSpy {}
    }

    Component {
        id: processModelComponent
        ListModel {
            dynamicRoles: true

            Component.onCompleted: {
                append({
                    "id": "thread-goal",
                    "processId": "thread-goal",
                    "threadId": "thread-goal",
                    "kind": "thread",
                    "title": "Port Codex Companion",
                    "preview": "Implementing the Windows shell",
                    "sourceStatus": "",
                    "updatedAt":
                        Date.now() / 1000 - 978307200,
                    "status": "running",
                    "needsApproval": false,
                    "runtimeStatus": "active",
                    "rolloutPath":
                        "C:\\rollouts\\thread-goal.jsonl",
                    "cwd": "C:\\worktree",
                    "activeTurnId": "turn-goal",
                    "model": "gpt-test",
                    "reasoningEffort": "high",
                    "goal": {
                        "threadId": "thread-goal",
                        "objective": "Ship Windows Companion",
                        "status": "active",
                        "elapsedSeconds": 125
                    }
                })
                append({
                    "id": "thread-tray",
                    "processId": "thread-tray",
                    "threadId": "thread-tray",
                    "kind": "thread",
                    "title": "Verify tray behavior",
                    "preview": "Waiting for interaction",
                    "sourceStatus": "",
                    "updatedAt":
                        Date.now() / 1000 - 978307200,
                    "status": "waiting",
                    "needsApproval": true,
                    "runtimeStatus": "waitingOnApproval",
                    "rolloutPath":
                        "C:\\rollouts\\thread-tray.jsonl",
                    "cwd": "C:\\worktree",
                    "activeTurnId": "turn-approval",
                    "model": "gpt-test",
                    "reasoningEffort": "high",
                    "goal": null
                })
            }
        }
    }

    Component {
        id: shellModelComponent
        QtObject {
            property string routeMode: "processes"
            property string chatAccentColor: "#ff6e14"
            property string selectedChatModelId: "on-device"
            readonly property string chatPromptPlaceholder:
                selectedChatModelId === "on-device"
                    ? "Ask on device"
                    : selectedChatModelId.indexOf("openai:") === 0
                        ? "Ask ChatGPT"
                        : selectedChatModelId.indexOf("lumo:") === 0
                            ? "Ask Lumo"
                            : "Ask Companion"
            property bool rejectChatModelSelection: false
            property var processModel: null
            property bool processLoading: false
            property string processErrorMessage: ""
            property bool processTargetActive: false
            property string processTargetId: ""
            property string processTargetTitle: ""
            property string processTargetAction: ""
            property string processDraft: ""
            property bool processSending: false
            property string processFeedback: ""
            property bool processFeedbackIsError: false
            property string approvingProcessId: ""
            property string retryingProcessId: ""
            property string processRetryStatusId: ""
            property string processRetryStatus: ""
            property bool processRetryStatusIsError: false
            readonly property bool processCommandBusy:
                processSending
                || approvingProcessId.length > 0
                || retryingProcessId.length > 0
            property bool chatSendEnabled: false
            property bool chatPreparationEnabled: true
            property bool chatBusy: false
            property string chatResponse: ""
            property string chatResponsePrompt: ""
            property string chatResponseTitle: ""
            property string chatResponseUsageSummary: ""
            property string chatStatusMessage: "Local chat runtime is connecting"
            property bool goalControlVisible: false
            property string goalTaskTitle: ""
            property string goalThreadId: ""
            property string goalObjective: ""
            property string goalDraftObjective: ""
            property string goalStatus: ""
            property int goalElapsedSeconds: 0
            property bool goalEditing: false
            property bool goalMutationPending: false
            property string goalErrorMessage: ""
            property bool usageLoading: false
            property var usageSnapshot: ({
                "planType": "plus",
                "groups": [{
                    "title": "Codex",
                    "shortWindow": {
                        "remainingPercent": 68,
                        "durationLabel": "5 hours",
                        "resetsAt": 1784862000000
                    },
                    "weeklyWindow": {
                        "remainingPercent": 42,
                        "durationLabel": "7 days",
                        "resetsAt": 1785294000000
                    }
                }],
                "availableResetCount": 2,
                "availableResetCredits": [{
                    "id": "credit-weekly",
                    "displayTitle": "Weekly Codex reset",
                    "detail": "Restores the eligible Codex limit.",
                    "expiresAt": 1785294000000
                }],
                "updatedAt": 1784847600000
            })
            property string usageErrorMessage: ""
            property var usageResetConfirmation: ({})
            property bool usageResetBusy: false
            property string usageResetStatusMessage: ""
            readonly property bool goalCanEdit:
                goalStatus !== "complete"
            readonly property bool goalCanPause:
                goalStatus === "active"
            readonly property bool goalCanResume:
                goalStatus === "paused"
                    || goalStatus === "blocked"
            property int preparationRequestCount: 0
            property int goalUpdateRequestCount: 0
            property int goalPauseRequestCount: 0
            property int goalResumeRequestCount: 0
            property int usageRefreshRequestCount: 0
            property int usageResetRequestCount: 0
            property int processMessageRequestCount: 0
            property int processCancelRequestCount: 0
            property int processApprovalRequestCount: 0
            property int processRetryRequestCount: 0
            property int chatMessageRequestCount: 0
            property string lastChatPrompt: ""
            property string lastChatModel: ""
            property string lastProcessApprovalDecision: ""
            property var lastProcessTarget: ({})
            readonly property var chatModels: [
                {
                    "id": "on-device",
                    "title": "On-device",
                    "detail": "On-device reasoning - private on this PC",
                    "group": "on-device"
                },
                {
                    "id": "openai:gpt56Luna",
                    "title": "5.6 Luna",
                    "detail": "OpenAI API - lowest cost",
                    "group": "openai"
                },
                {
                    "id": "openai:gpt56Terra",
                    "title": "5.6 Terra",
                    "detail": "OpenAI API - balanced",
                    "group": "openai"
                },
                {
                    "id": "openai:gpt56Sol",
                    "title": "5.6 Sol",
                    "detail": "OpenAI API - highest capability",
                    "group": "openai"
                },
                {
                    "id": "lumo:automatic",
                    "title": "Lumo Auto",
                    "detail": "Lumo API - best available model",
                    "group": "lumo"
                },
                {
                    "id": "lumo:fast",
                    "title": "Lumo Fast",
                    "detail": "Lumo API - fast responses",
                    "group": "lumo"
                },
                {
                    "id": "lumo:thinking",
                    "title": "Lumo Thinking",
                    "detail": "Lumo API - deeper reasoning",
                    "group": "lumo"
                }
            ]

            function showProcesses() {
                routeMode = "processes"
            }

            function showLocalChat() {
                routeMode = "local-chat"
            }

            function beginProcessAction(process, action) {
                lastProcessTarget = process
                processTargetActive = true
                processTargetId = process.id
                processTargetTitle = process.title
                processTargetAction = action
                processDraft = ""
                processFeedback = ""
                processFeedbackIsError = false
            }

            function cancelProcessTarget() {
                if (processSending) {
                    processCancelRequestCount += 1
                }
                processSending = false
                processTargetActive = false
                processTargetId = ""
                processTargetTitle = ""
                processTargetAction = ""
                processDraft = ""
                processFeedback = ""
                processFeedbackIsError = false
            }

            function submitProcessMessage() {
                if (processSending
                        || processDraft.trim().length === 0) {
                    return
                }
                processSending = true
                processMessageRequestCount += 1
            }

            function respondToProcessApproval(process, decision) {
                if (approvingProcessId.length > 0) {
                    return
                }
                approvingProcessId = process.id
                lastProcessApprovalDecision = decision
                processApprovalRequestCount += 1
            }

            function retryFailedProcess(process) {
                if (processCommandBusy) {
                    return
                }
                retryingProcessId = process.id
                processRetryStatusId = process.id
                processRetryStatus = "Retrying..."
                processRetryStatusIsError = false
                lastProcessTarget = process
                processRetryRequestCount += 1
            }

            function setSelectedChatModelId(value) {
                selectedChatModelId = value
            }

            function chooseChatModel(value) {
                if (rejectChatModelSelection) {
                    return false
                }
                selectedChatModelId = value
                return true
            }

            function submitLocalChat(prompt) {
                if (!chatSendEnabled
                        || chatBusy
                        || prompt.trim().length === 0) {
                    return
                }
                chatMessageRequestCount += 1
                lastChatPrompt = prompt.trim()
                lastChatModel = selectedChatModelId
            }

            function dismissChatResponse() {
                chatResponse = ""
                chatResponsePrompt = ""
                chatResponseTitle = ""
                chatResponseUsageSummary = ""
            }

            function prepareOnDeviceChat() {
                preparationRequestCount += 1
            }

            function openGoalControls(taskTitle, goal) {
                goalTaskTitle = taskTitle
                goalThreadId = goal.threadId
                goalObjective = goal.objective
                goalDraftObjective = goal.objective
                goalStatus = goal.status
                goalElapsedSeconds = goal.elapsedSeconds
                goalControlVisible = true
                goalEditing = false
                goalErrorMessage = ""
            }

            function dismissGoalControls() {
                goalControlVisible = false
                goalEditing = false
            }

            function beginGoalEditing() {
                goalEditing = true
                goalDraftObjective = goalObjective
            }

            function cancelGoalEditing() {
                goalEditing = false
                goalDraftObjective = goalObjective
            }

            function saveGoalEdit() {
                goalUpdateRequestCount += 1
                goalMutationPending = true
            }

            function pauseGoal() {
                goalPauseRequestCount += 1
                goalMutationPending = true
            }

            function resumeGoal() {
                goalResumeRequestCount += 1
                goalMutationPending = true
            }

            function refreshUsage() {
                if (usageLoading) {
                    return
                }
                usageLoading = true
                usageRefreshRequestCount += 1
            }

            function refreshUsageAfterAccountChange() {
                usageLoading = true
                usageRefreshRequestCount += 1
            }

            function prepareUsageReset(credit) {
                usageResetConfirmation = ({
                    "creditId": credit.id,
                    "displayTitle": credit.displayTitle,
                    "idempotencyKey":
                        "50000000-0000-0000-0000-000000000001"
                })
                usageResetStatusMessage = ""
            }

            function cancelUsageReset() {
                if (!usageResetBusy) {
                    usageResetConfirmation = ({})
                }
            }

            function confirmUsageReset() {
                if (usageResetBusy
                        || usageResetConfirmation.creditId
                            === undefined) {
                    return
                }
                usageResetBusy = true
                usageResetStatusMessage =
                    "Applying "
                    + usageResetConfirmation.displayTitle
                    + "..."
                usageResetConfirmation = ({})
                usageResetRequestCount += 1
            }
        }
    }

    Component {
        id: attentionModelComponent
        QtObject {
            property var latestAttentionHighlight:
                ({})
            property int goalConfettiTrigger: 0
        }
    }

    Component {
        id: settingsModelComponent
        QtObject {
            property string backdropMode: "mica"
            property string effectiveBackdropMode: "mica"
            property var codexAccountProfiles: [{
                "id": "main-profile",
                "label": "Main"
            }, {
                "id": "backup-profile",
                "label": "Account 3"
            }]
            property string selectedCodexAccountProfileId:
                "main-profile"
        }
    }

    Component {
        id: backdropStateComponent
        QtObject {
            property string settingsEffectiveMode: "mica"
            property string companionMenuEffectiveMode: "mica"
            property string modelPickerEffectiveMode: "mica"
            property string goalEffectiveMode: "mica"
            property string usageEffectiveMode: "mica"
            property string attentionEffectiveMode: "mica"
        }
    }

    Component {
        id: placementControllerComponent
        QtObject {
            function availableWorkAreaAt(point) {
                return point.x < 0
                    ? Qt.rect(-2560, 738, 2560, 1414)
                    : Qt.rect(0, 26, 3840, 2134)
            }
        }
    }

    Component {
        id: windowComponent
        CompanionMenuWindow {
            visible: true
        }
    }

    Component {
        id: usageWindowComponent
        UsageWindow {
            visible: false
        }
    }

    function createWindow() {
        const model = createTemporaryObject(shellModelComponent, null)
        verify(model)
        model.processModel = createTemporaryObject(processModelComponent, model)
        const attention = createTemporaryObject(
            attentionModelComponent,
            model)
        verify(attention)
        const placement = createTemporaryObject(
            placementControllerComponent,
            model)
        verify(placement)
        const settings = createTemporaryObject(
            settingsModelComponent,
            model)
        verify(settings)
        const window = createTemporaryObject(windowComponent, null, {
            shellModel: model,
            attentionModel: attention,
            settingsModel: settings,
            placementController: placement
        })
        verify(window)
        tryCompare(window, "visible", true)
        createdWindows.push(window)
        window.raise()
        window.requestActivate()
        wait(0)
        return window
    }

    function createUsageWindow() {
        const model = createTemporaryObject(
            shellModelComponent,
            null)
        verify(model)
        const settings = createTemporaryObject(
            settingsModelComponent,
            model)
        verify(settings)
        const window = createTemporaryObject(
            usageWindowComponent,
            null,
            {
                shellModel: model,
                settingsModel: settings
            })
        verify(window)
        window.show()
        tryCompare(window, "visible", true)
        createdWindows.push(window)
        window.raise()
        window.requestActivate()
        tryCompare(window, "active", true)
        return window
    }

    function cleanup() {
        for (let index = 0; index < createdWindows.length; ++index) {
            const window = createdWindows[index]
            if (window === null || window === undefined) {
                continue
            }
            if (window.chatModelPopup !== undefined) {
                window.chatModelPopup.close()
            }
            if (window.goalPopup !== undefined) {
                window.goalPopup.close()
            }
            if (window.visible) {
                if (window.reconcileProcessHoverAt
                        !== undefined) {
                    moveProcessPointerOutside(window)
                } else {
                    mouseMove(window, -64, -64)
                }
            }
            window.visible = false
        }
        createdWindows = []
        wait(0)
    }

    function moveProcessPointer(
        window,
        item,
        localX,
        localY) {
        const pointerX =
            localX === undefined ? item.width / 2 : localX
        const pointerY =
            localY === undefined ? item.height / 2 : localY
        mouseMove(
            item,
            pointerX,
            pointerY)
        const globalPoint =
            item.mapToGlobal(
                pointerX,
                pointerY)
        const windowPoint =
            window.contentItem.mapFromGlobal(
                globalPoint.x,
                globalPoint.y)
        window.reconcileProcessHoverAt(
            windowPoint.x,
            windowPoint.y)
    }

    function moveProcessPointerOutside(window) {
        mouseMove(window, -64, -64)
        window.reconcileProcessHoverAt(
            -64,
            -64)
    }

    function hoverProcessCard(window, card, localX, localY) {
        moveProcessPointer(
            window,
            window.processSurface,
            2,
            2)
        wait(1)
        moveProcessPointer(
            window,
            card,
            localX === undefined ? card.width / 2 : localX,
            localY === undefined ? card.height / 2 : localY)
    }

    function appendProcessFixture(window, id, status) {
        window.shellModel.processModel.append({
            "id": id,
            "processId": id,
            "threadId": id,
            "kind": "thread",
            "title": "Process " + id,
            "preview": "Process fixture " + id,
            "sourceStatus": "",
            "updatedAt":
                Date.now() / 1000 - 978307200,
            "status": status,
            "needsApproval": false,
            "runtimeStatus":
                status === "waiting"
                    ? "waitingOnUserInput"
                    : "active",
            "rolloutPath": "",
            "cwd": "",
            "activeTurnId": "",
            "model": "",
            "reasoningEffort": "",
            "goal": null
        })
    }

    function processHeightDiagnostics(window, firstCard, secondCard) {
        return " target=" + window.targetHeight
            + " listTarget=" + window.processTargetListHeight
            + " viewport=" + window.processViewportHeight
            + " min=" + window.minimumHeight
            + " max=" + window.maximumHeight
            + " hoveredId=" + window.hoveredProcessId
            + " firstActions=" + firstCard.showsAnyActions
            + " firstTarget=" + firstCard.targetCardHeight
            + " firstRendered=" + firstCard.height
            + " secondActions=" + secondCard.showsAnyActions
            + " secondTarget=" + secondCard.targetCardHeight
            + " secondRendered=" + secondCard.height
    }

    function test_process_surface_matches_macos_tray_metrics() {
        const window = createWindow()
        compare(window.routeMode, "processes")
        compare(window.width, 292)
        tryCompare(window, "height", 182)
        compare(window.menuHostSurface.color.a, 0)
        compare(window.menuHostSurface.border.width, 0)
        verify(window.processSurface.radius >= 22)
        compare(window.processHeader.visible, false)
        compare(window.routeFooter.visible, false)
        compare(window.processScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.processScrollBar.visible, false)
        compare(window.processList.count, 2)
        compare(window.processScrollableBottomInset, 0)
        compare(window.processViewport.layer.enabled, false)
    }

    function test_three_process_cards_expand_without_clipping() {
        const window = createWindow()
        window.shellModel.processModel.append({
            "id": "thread-third-goal",
            "processId": "thread-third-goal",
            "threadId": "thread-third-goal",
            "kind": "thread",
            "title": "Verify rounded process layout",
            "preview": "Keep every process card visible",
            "sourceStatus": "",
            "updatedAt":
                Date.now() / 1000 - 978307200,
            "status": "running",
            "needsApproval": false,
            "runtimeStatus": "active",
            "rolloutPath":
                "C:\\rollouts\\thread-third-goal.jsonl",
            "cwd": "C:\\worktree",
            "activeTurnId": "turn-third-goal",
            "model": "gpt-test",
            "reasoningEffort": "high",
            "goal": {
                "threadId": "thread-third-goal",
                "objective": "Verify rounded process layout",
                "status": "active",
                "elapsedSeconds": 42
            }
        })
        window.shellModel.processModel.setProperty(
            1,
            "status",
            "running")
        window.shellModel.processModel.setProperty(
            1,
            "needsApproval",
            false)
        window.shellModel.processModel.setProperty(
            1,
            "runtimeStatus",
            "active")
        window.shellModel.processModel.setProperty(
            1,
            "goal",
            {
                "threadId": "thread-tray",
                "objective": "Match the macOS process surface",
                "status": "active",
                "elapsedSeconds": 125
            })
        window.processList.forceLayout()
        tryCompare(window.processList, "count", 3)
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
                && window.processList.itemAtIndex(1) !== null
                && window.processList.itemAtIndex(2) !== null
        })

        const hoveredCard = window.processList.itemAtIndex(1)
        const lastCard = window.processList.itemAtIndex(2)
        hoverProcessCard(window, hoveredCard)
        tryCompare(hoveredCard, "targetCardHeight", 101)
        tryCompare(window, "processTargetListHeight", 259)
        tryCompare(window, "height", 303)
        compare(window.processListNeedsScrolling, false)
        compare(window.processScrollableBottomInset, 0)
        verify(
            window.processList.contentHeight
                <= window.processList.height,
            "Three process cards should fit without scrolling")
        const lastCardBottom = lastCard.mapToItem(
            window.processList,
            0,
            lastCard.height).y
        verify(
            lastCardBottom <= window.processList.height,
            "The third process card is clipped: "
                + lastCardBottom + " > "
                + window.processList.height)
    }

    function test_goal_completion_confetti_covers_quick_bar() {
        const window = createWindow()

        verify(window.goalConfettiOverlay !== undefined)
        const overlay = window.goalConfettiOverlay
        overlay.liveAnimation = false
        compare(overlay.x, 0)
        compare(overlay.y, 0)
        compare(overlay.width, window.width)
        compare(overlay.height, window.height)
        compare(overlay.enabled, false)
        compare(overlay.particleRepeater.count, 0)

        window.attentionModel.goalConfettiTrigger = 5

        tryCompare(overlay, "activeTrigger", 5)
        compare(overlay.particleRepeater.count, 30)
        compare(overlay.burstProgress, 0)
    }

    function test_scrollable_four_process_hover_softens_partial_card_clip() {
        const window = createWindow()
        window.shellModel.processModel.append({
            "id": "thread-third-goal",
            "processId": "thread-third-goal",
            "threadId": "thread-third-goal",
            "kind": "thread",
            "title": "Verify rounded process clipping",
            "preview": "Keep partial cards clear of the tray corners",
            "sourceStatus": "",
            "updatedAt":
                Date.now() / 1000 - 978307200,
            "status": "running",
            "needsApproval": false,
            "runtimeStatus": "active",
            "cwd": "C:\\worktree",
            "activeTurnId": "turn-third-goal",
            "model": "gpt-test",
            "reasoningEffort": "high",
            "goal": {
                "threadId": "thread-third-goal",
                "objective": "Verify rounded process clipping",
                "status": "active",
                "elapsedSeconds": 42
            }
        })
        appendProcessFixture(
            window,
            "thread-fourth",
            "completed")
        window.processList.forceLayout()
        tryCompare(window.processList, "count", 4)
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
                && window.processList.itemAtIndex(2) !== null
        })
        window.shellModel.processModel.setProperty(
            1,
            "status",
            "running")
        window.shellModel.processModel.setProperty(
            1,
            "needsApproval",
            false)
        window.shellModel.processModel.setProperty(
            1,
            "runtimeStatus",
            "active")
        window.shellModel.processModel.setProperty(
            1,
            "goal",
            {
                "threadId": "thread-tray",
                "objective": "Match the macOS process surface",
                "status": "active",
                "elapsedSeconds": 125
            })

        const card = window.processList.itemAtIndex(1)
        hoverProcessCard(window, card)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId,
            1000,
            processHeightDiagnostics(
                window,
                window.processList.itemAtIndex(0),
                card))
        verify(
            card.showsAnyActions,
            processHeightDiagnostics(
                window,
                window.processList.itemAtIndex(0),
                card))
        compare(
            card.targetCardHeight,
            101,
            processHeightDiagnostics(
                window,
                window.processList.itemAtIndex(0),
                card))
        wait(
            window.processHoverExitGraceDuration
                + 200)
        compare(
            window.hoveredProcessId,
            card.processId,
            processHeightDiagnostics(
                window,
                window.processList.itemAtIndex(0),
                card))
        tryCompare(card, "height", 101)
        tryCompare(window, "processTargetListHeight", 230)
        tryCompare(window, "height", 274)
        verify(
            window.processList.contentHeight
                > window.processList.height,
            "Hover expansion must make this fixture scrollable")
        compare(
            window.processScrollableBottomInset,
            window.nativeBackdropRegionInsetBottom)

        const listBottom = window.processList.mapToItem(
            window.processSurface,
            0,
            window.processList.height).y
        const bottomClearance =
            window.processSurface.height - listBottom
        verify(
            bottomClearance >= 22,
            "Scrollable process clip touches the rounded surface: "
                + bottomClearance + "px")
        const listBottomInWindow =
            window.processList.mapToItem(
                window.contentItem,
                0,
                window.processList.height).y
        const nativeCurveBottom =
            window.height
                - window.nativeBackdropRegionInsetBottom
        const nativeCurveClearance =
            nativeCurveBottom - listBottomInWindow
        verify(
            nativeCurveClearance >= 8,
            "Scrollable process clip enters the native rounded curve: "
                + nativeCurveClearance + "px")
        compare(window.processViewport.layer.enabled, true)
        verify(window.processViewport.layer.effect !== null)
        tryVerify(function() {
            return window.processEdgeMask !== null
        })
        compare(window.processEdgeMask.hideSource, true)
        compare(
            window.processEdgeMask.sourceItem.objectName,
            "processEdgeMaskGradient")
        verify(
            window.processEdgeMask.sourceItem.z < 0,
            "Process edge mask source must stay behind visible content")
        compare(
            Math.round(window.processEdgeMask.width),
            Math.round(window.processViewport.width))
        compare(
            Math.round(window.processEdgeMask.height),
            Math.round(window.processViewport.height))
        verify(window.processEdgeFadeHeight >= 16)
        compare(window.processScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.processScrollBar.visible, false)

        wait(180)
        waitForRendering(window.contentItem)
        const maskedImage =
            grabImage(window.contentItem)
        const viewportOrigin =
            window.processViewport.mapToItem(
                window.contentItem,
                0,
                0)
        const fadeStartLogicalY =
            Math.floor(
                window.processList.height
                    - window.processEdgeFadeHeight
                    + 2)
        const fadeEndLogicalY =
            Math.floor(window.processList.height - 2)

        window.processEdgeMask.sourceItem.visible = false
        window.processViewport.layer.enabled = false
        waitForRendering(window.contentItem)
        const unmaskedImage =
            grabImage(window.contentItem)

        function averageRowDifference(logicalY) {
            let difference = 0
            let samples = 0
            for (let logicalX = 18;
                    logicalX <= window.processViewport.width - 18;
                    logicalX += 4) {
                const imageX = Math.floor(
                    (viewportOrigin.x + logicalX)
                        * maskedImage.width
                        / window.contentItem.width)
                const imageY = Math.floor(
                    (viewportOrigin.y + logicalY)
                        * maskedImage.height
                        / window.contentItem.height)
                difference += Math.abs(
                    maskedImage.red(imageX, imageY)
                        - unmaskedImage.red(imageX, imageY))
                difference += Math.abs(
                    maskedImage.green(imageX, imageY)
                        - unmaskedImage.green(imageX, imageY))
                difference += Math.abs(
                    maskedImage.blue(imageX, imageY)
                        - unmaskedImage.blue(imageX, imageY))
                difference += Math.abs(
                    maskedImage.alpha(imageX, imageY)
                        - unmaskedImage.alpha(imageX, imageY))
                samples += 1
            }
            return difference / Math.max(1, samples * 4)
        }

        const fadeStartDifference =
            averageRowDifference(fadeStartLogicalY)
        const fadeEndDifference =
            averageRowDifference(fadeEndLogicalY)
        verify(
            fadeEndDifference >= 6,
            "Process edge mask did not alter the visible clip edge: "
                + fadeEndDifference)
        verify(
            fadeEndDifference >= fadeStartDifference + 4,
            "Process edge mask did not progressively soften the clip: "
                + fadeStartDifference + " -> "
                + fadeEndDifference)
    }

    function test_process_cards_use_macos_translucent_materials() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
                && window.processList.itemAtIndex(1) !== null
        })

        const runningCard = window.processList.itemAtIndex(0)
        const waitingCard = window.processList.itemAtIndex(1)

        compare(runningCard.hasProcessAccent, false)
        verify(Math.abs(runningCard.color.a - 0.18) < 0.01)
        verify(Math.abs(runningCard.border.color.a - 0.16) < 0.01)
        verify(Math.abs(
            runningCard.processStatusBadge.color.a - 0.18) < 0.01)
        verify(Math.abs(
            runningCard.processStatusBadge.border.color.a - 0.18) < 0.01)
        verify(Math.abs(
            waitingCard.processStatusBadge.border.color.a - 0.55) < 0.01)

        hoverProcessCard(window, runningCard)
        tryCompare(runningCard, "isHovered", true)
        tryVerify(function() {
            return Math.abs(runningCard.color.a - 0.11) < 0.01
        })
        tryVerify(function() {
            return Math.abs(runningCard.border.color.a - 0.30) < 0.01
        })

        moveProcessPointer(
            window,
            window.processSurface,
            2,
            2)
        tryCompare(runningCard, "isHovered", false)
        tryCompare(window, "hoveredProcessId", "")
    }

    function test_backdrop_modes_change_the_complete_rounded_menu_material() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const processCard = window.processList.itemAtIndex(0)

        compare(window.routeMode, "processes")
        compare(window.menuHostSurface.color.a, 0)
        verify(Math.abs(
            window.processSurface.color.a - 0.42) < 0.01)
        const micaSurfaceRed = window.processSurface.color.r
        const micaSurfaceGreen = window.processSurface.color.g
        const micaSurfaceBlue = window.processSurface.color.b
        compare(window.processSurface.border.width, 1)
        verify(Math.abs(processCard.color.a - 0.18) < 0.01)
        verify(Math.abs(
            processCard.border.color.a - 0.16) < 0.01)
        verify(Math.abs(
            processCard.processStatusBadge.color.a - 0.18) < 0.01)

        window.settingsModel.effectiveBackdropMode =
            "windows-glass"
        compare(window.menuHostSurface.color.a, 0)
        verify(Math.abs(
            window.processSurface.color.a - 0.26) < 0.01)
        verify(window.processSurface.color.r
               - micaSurfaceRed > 0.25)
        verify(window.processSurface.color.g
               - micaSurfaceGreen > 0.25)
        verify(window.processSurface.color.b
               - micaSurfaceBlue > 0.25)
        tryVerify(function() {
            return Math.abs(processCard.color.a - 0.12)
                < 0.01
        })
        verify(Math.abs(
            processCard.border.color.a - 0.22) < 0.01)
        verify(Math.abs(
            processCard.processStatusBadge.color.a - 0.12) < 0.01)

        window.settingsModel.effectiveBackdropMode =
            "solid-black"
        compare(window.menuHostSurface.color.a, 0)
        compare(window.processSurface.color.a, 1)
        compare(window.processSurface.color,
                CompanionTheme.window)
        tryCompare(processCard.color, "a", 1)
        compare(processCard.color,
                CompanionTheme.surface)
        compare(processCard.border.color.a, 1)
        compare(
            processCard.processStatusBadge.color.a,
            1)

        window.shellModel.showLocalChat()
        compare(window.routeMode, "local-chat")
        compare(window.menuHostSurface.color.a, 1)
        compare(window.menuHostSurface.border.width, 1)
        compare(
            window.chatModelPopup.background.color.a,
            1)

        window.settingsModel.effectiveBackdropMode =
            "windows-glass"
        verify(Math.abs(
            window.menuHostSurface.color.a - 0.26) < 0.01)
        verify(Math.abs(
            window.chatModelPopup.background.color.a
                - 0.78) < 0.01)
        compare(window.menuHostSurface.radius, 28)
    }

    function test_surfaces_follow_role_specific_effective_backdrop_modes() {
        const state = createTemporaryObject(
            backdropStateComponent,
            null)
        verify(state)

        const window = createWindow()
        verify(window.backdropState !== undefined)
        verify(window.modelPickerEffectiveBackdropMode !== undefined)
        verify(window.goalEffectiveBackdropMode !== undefined)
        window.backdropState = state

        state.companionMenuEffectiveMode = "windows-glass"
        state.modelPickerEffectiveMode = "solid-black"
        state.goalEffectiveMode = "mica"
        window.settingsModel.effectiveBackdropMode = "solid-black"

        compare(window.effectiveBackdropMode, "windows-glass")
        compare(
            window.modelPickerEffectiveBackdropMode,
            "solid-black")
        compare(window.goalEffectiveBackdropMode, "mica")

        const usage = createUsageWindow()
        verify(usage.backdropState !== undefined)
        usage.backdropState = state
        state.usageEffectiveMode = "solid-black"
        compare(usage.effectiveBackdropMode, "solid-black")

        state.usageEffectiveMode = "mica"
        compare(usage.effectiveBackdropMode, "mica")
        compare(window.effectiveBackdropMode, "windows-glass")
    }

    function test_native_backdrop_region_tracks_the_visible_material_bounds() {
        const window = createWindow()

        verify(window.nativeBackdropRegionEnabled !== undefined)
        compare(window.nativeBackdropRegionEnabled, true)
        compare(window.nativeBackdropRegionInsetLeft, 4)
        compare(window.nativeBackdropRegionInsetTop, 14)
        compare(window.nativeBackdropRegionInsetRight, 4)
        compare(window.nativeBackdropRegionInsetBottom, 14)
        compare(window.nativeBackdropRegionRadius, 28)

        window.shellModel.showLocalChat()
        window.shellModel.chatBusy = true

        tryCompare(window, "height", 420)
        compare(window.nativeBackdropRegionInsetTop, 22)
        compare(window.nativeBackdropRegionInsetLeft, 4)
        compare(window.nativeBackdropRegionInsetRight, 4)
        compare(window.nativeBackdropRegionInsetBottom, 14)
        compare(window.nativeBackdropRegionRadius, 28)

        const usage = createUsageWindow()
        verify(usage.nativeBackdropRegionEnabled !== undefined)
        compare(usage.nativeBackdropRegionEnabled, true)
        compare(usage.nativeBackdropRegionInsetLeft, 0)
        compare(usage.nativeBackdropRegionInsetTop, 0)
        compare(usage.nativeBackdropRegionInsetRight, 0)
        compare(usage.nativeBackdropRegionInsetBottom, 0)
        compare(usage.nativeBackdropRegionRadius, 20)
    }

    function test_empty_process_feed_uses_macos_notice_card() {
        const window = createWindow()
        window.shellModel.processModel.clear()
        window.processList.forceLayout()

        tryCompare(window.processList, "count", 0)
        tryCompare(window, "processFeedEmpty", true)
        tryCompare(window, "height", 108)
        compare(window.minimumHeight, 108)
        compare(window.maximumHeight, 108)
        compare(window.processNoticeCard.visible, true)
        tryCompare(window.processNoticeCard, "opacity", 1)
        compare(window.processNoticeTitle.text,
                "No active Codex processes")
        compare(window.processNoticeSubtitle.text,
                "Nothing running right now")
        compare(window.processNoticeCard.radius, 22)
        compare(window.processNoticeCard.ToolTip.text,
                "Open a Codex thread or start a job and it will appear here.")

        window.shellModel.processLoading = true

        tryCompare(window, "height", 90)
        compare(window.minimumHeight, 90)
        compare(window.maximumHeight, 90)
        compare(window.processNoticeTitle.text,
                "Loading Codex processes")
        compare(window.processNoticeSubtitle.text,
                "Checking recent work")

        window.shellModel.processLoading = false
        window.shellModel.processErrorMessage =
            "Synthetic refresh failure"

        tryCompare(window, "height", 108)
        compare(window.processNoticeTitle.text,
                "Could not load Codex processes")
        compare(window.processNoticeSubtitle.text,
                "Refresh failed")
        compare(window.processNoticeCard.ToolTip.text,
                "Synthetic refresh failure")
    }

    function test_process_status_badges_match_macos_help() {
        const window = createWindow()

        compare(window.processStatusHelp("running"), "Still working")
        compare(window.processStatusHelp("completed"), "Completed")
        compare(window.processStatusHelp("failed"),
                "Failed or disconnected")
        compare(window.processStatusHelp("waiting"),
                "Needs your attention")

        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
                && window.processList.itemAtIndex(1) !== null
        })

        const runningCard = window.processList.itemAtIndex(0)
        const waitingCard = window.processList.itemAtIndex(1)
        compare(runningCard.processStatusBadge.ToolTip.text,
                "Still working")
        compare(runningCard.processStatusBadge.Accessible.name,
                "Still working")
        compare(waitingCard.processStatusBadge.ToolTip.text,
                "Needs your attention")
        compare(waitingCard.processStatusBadge.Accessible.name,
                "Needs your attention")
    }

    function test_running_process_status_badge_has_visible_motion() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })

        const runningCard = window.processList.itemAtIndex(0)
        verify(runningCard.processSpinner !== undefined)
        compare(runningCard.processSpinner.visible, true)
        compare(runningCard.processSpinner.width, 16)
        compare(runningCard.processSpinner.height, 16)
        verify(runningCard.processSpinnerArc !== undefined)
        compare(runningCard.processSpinnerArc.visible, true)
        compare(runningCard.processSpinnerArc.width, 14)
        compare(runningCard.processSpinnerArc.height, 14)

        const initialRotation = runningCard.processSpinner.rotation
        wait(180)
        verify(Math.abs(
            runningCard.processSpinner.rotation
                - initialRotation) > 2)
    }

    function test_process_subtitles_and_goal_badges_match_macos_copy() {
        const window = createWindow()
        const nowReferenceSeconds =
            Date.now() / 1000 - 978307200

        compare(
            window.processStatusSubtitle(
                "job",
                "running",
                false,
                null,
                nowReferenceSeconds,
                "",
                "queued"),
            "Queued - Updated just now")
        compare(
            window.processStatusSubtitle(
                "job",
                "running",
                false,
                null,
                nowReferenceSeconds,
                "",
                ""),
            "Running - Updated just now")
        compare(
            window.processStatusSubtitle(
                "thread",
                "waiting",
                false,
                null,
                nowReferenceSeconds,
                "",
                ""),
            "Needs your answer")
        compare(window.goalStatusTitle("usageLimited"),
                "Usage")
        compare(window.goalStatusTitle("budgetLimited"),
                "Budget")
        compare(window.goalStatusTitle("complete"),
                "Reached")
    }

    function test_goal_status_colors_match_macos_semantics() {
        const window = createWindow()

        compare(window.goalStatusColor("active"),
                CompanionTheme.info)
        compare(window.goalStatusColor("paused"),
                CompanionTheme.warning)
        compare(window.goalStatusColor("blocked"),
                CompanionTheme.warning)
        compare(window.goalStatusColor("usageLimited"),
                CompanionTheme.warning)
        compare(window.goalStatusColor("budgetLimited"),
                CompanionTheme.warning)
        compare(window.goalStatusColor("complete"),
                CompanionTheme.success)
        compare(window.goalStatusColor("unknown"),
                CompanionTheme.textMuted)
    }

    function test_goal_status_glyphs_match_windows_native_macos_semantics() {
        const window = createWindow()

        compare(window.goalStatusGlyph("active"), "\uf272")
        compare(window.goalStatusGlyph("paused"), "\ue769")
        compare(window.goalStatusGlyph("blocked"), "\ue7ba")
        compare(window.goalStatusGlyph("usageLimited"), "\ue917")
        compare(window.goalStatusGlyph("budgetLimited"), "\uec4a")
        compare(window.goalStatusGlyph("complete"), "\uec61")
        compare(window.goalStatusGlyph("unknown"), "\ue946")
    }

    function test_goal_popup_prefers_above_and_falls_below_near_screen_top() {
        const window = createWindow()

        compare(
            window.goalPopupOriginY(
                130, 13, 235, 484, 26, 2160),
            -112)
        compare(
            window.goalPopupOriginY(
                30, 13, 235, 34, 26, 2160),
            50)
    }

    function test_pet_routes_resize_one_window_without_inline_model_selector() {
        const window = createWindow()
        const originalWidth = window.width
        const originalHeight = window.height

        compare(window.minimumWidth, 292)
        compare(window.maximumWidth, 292)
        compare(window.minimumHeight, originalHeight)
        compare(window.maximumHeight, originalHeight)

        window.shellModel.showLocalChat()

        compare(window.routeMode, "local-chat")
        compare(window["chatModelSelector"], undefined)
        verify(window.chatModelPopup !== undefined)
        compare(window.chatModelPopup.visible, false)
        compare(window.width, originalWidth)
        tryCompare(window, "height", 94)
        compare(window.minimumHeight, 94)
        compare(window.maximumHeight, 94)

        window.shellModel.showProcesses()

        compare(window.routeMode, "processes")
        compare(window["chatModelSelector"], undefined)
        compare(window.chatModelPopup.visible, false)
        compare(window.width, originalWidth)
        tryCompare(window, "height", originalHeight)
        compare(window.minimumHeight, originalHeight)
        compare(window.maximumHeight, originalHeight)
    }

    function test_rapid_route_changes_keep_native_menu_bounded() {
        const window = createWindow()

        for (let index = 0; index < 12; ++index) {
            window.shellModel.showLocalChat()
            window.shellModel.chatResponse =
                index % 2 === 0 ? "Response " + index : ""
            tryCompare(
                window,
                "height",
                index % 2 === 0 ? 420 : 94)
            compare(window.width, 292)
            compare(window.minimumHeight, window.height)
            compare(window.maximumHeight, window.height)

            window.shellModel.showProcesses()
            tryCompare(window, "height", 182)
            compare(window.width, 292)
            compare(window.minimumHeight, window.height)
            compare(window.maximumHeight, window.height)
        }
    }

    function test_long_process_list_scrolls_without_a_visible_scrollbar() {
        const window = createWindow()
        for (let index = 0; index < 4; ++index) {
            window.shellModel.processModel.append({
                "id": "extra-" + index,
                "processId": "extra-" + index,
                "threadId": "extra-" + index,
                "kind": "thread",
                "title": "Additional Codex task " + index,
                "preview": "Queued work",
                "updatedAt":
                    Date.now() / 1000 - 978307200,
                "status": "waiting",
                "needsApproval": false,
                "cwd": "",
                "activeTurnId": "",
                "model": "",
                "reasoningEffort": "",
                "goal": null
            })
        }
        window.processList.forceLayout()

        tryCompare(window, "height", 274)
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })
        compare(window.processScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.processScrollBar.visible, false)
    }

    function test_first_waiting_process_scrolls_into_view_without_attention_highlight() {
        const window = createWindow()
        window.attentionModel.latestAttentionHighlight = ({})
        window.shellModel.processTargetActive = false
        window.shellModel.processTargetId = ""
        window.shellModel.processModel.clear()
        for (let index = 0; index < 8; ++index) {
            appendProcessFixture(
                window,
                "waiting-scroll-" + index,
                "running")
        }
        window.processList.forceLayout()
        tryCompare(window.processList, "count", 8)
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })
        wait(50)
        window.processList.contentY = 0
        compare(window.processList.contentY, 0)

        window.shellModel.processModel.setProperty(
            7,
            "status",
            "waiting")
        window.shellModel.processModel.setProperty(
            7,
            "runtimeStatus",
            "waitingOnUserInput")

        tryVerify(function() {
            return window.processList.contentY > 0
        })
        tryVerify(function() {
            return window.processList.itemAtIndex(7) !== null
        })
        const waitingCard =
            window.processList.itemAtIndex(7)
        const waitingOrigin =
            waitingCard.mapToItem(
                window.processList,
                0,
                0)
        verify(waitingOrigin.y < window.processList.height)
        verify(waitingOrigin.y + waitingCard.height > 0)
    }

    function test_process_wheel_scrolls_without_visible_scrollbar() {
        const window = createWindow()
        window.attentionModel.latestAttentionHighlight = ({})
        window.shellModel.processTargetActive = false
        window.shellModel.processTargetId = ""
        window.shellModel.processModel.clear()
        for (let index = 0; index < 8; ++index) {
            appendProcessFixture(
                window,
                "wheel-scroll-" + index,
                "running")
        }
        window.processList.forceLayout()
        tryCompare(window.processList, "count", 8)
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })
        wait(50)
        window.processList.contentY = 0
        compare(window.processList.contentY, 0)
        compare(
            window.processScrollBar.policy,
            ScrollBar.AlwaysOff)
        compare(window.processScrollBar.visible, false)

        mouseWheel(
            window.processList,
            window.processList.width / 2,
            window.processList.height / 2,
            0,
            -120)

        tryVerify(function() {
            return window.processList.contentY > 0
        })
        compare(
            window.processScrollBar.policy,
            ScrollBar.AlwaysOff)
        compare(window.processScrollBar.visible, false)
    }

    function test_process_target_and_latest_attention_scroll_into_view() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            1,
            "status",
            "completed")
        window.shellModel.processModel.setProperty(
            1,
            "needsApproval",
            false)
        for (let index = 0; index < 7; ++index) {
            window.shellModel.processModel.append({
                "id": "scroll-" + index,
                "processId": "scroll-" + index,
                "threadId": "scroll-" + index,
                "kind": "thread",
                "title": "Scroll target " + index,
                "preview": "Completed process " + index,
                "updatedAt":
                    Date.now() / 1000 - 978307200,
                "status": "completed",
                "needsApproval": false,
                "cwd": "",
                "activeTurnId": "",
                "model": "",
                "reasoningEffort": "",
                "goal": null
            })
        }
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })

        window.shellModel.processTargetActive = true
        window.shellModel.processTargetId = "scroll-6"
        tryVerify(function() {
            return window.processList.contentY > 0
        })

        window.shellModel.processTargetActive = false
        window.shellModel.processTargetId = ""
        window.processList.contentY = 0
        window.attentionModel.latestAttentionHighlight = ({
            "processId": "scroll-6",
            "kind": "completion"
        })

        tryVerify(function() {
            return window.processList.contentY > 0
        })
        tryVerify(function() {
            return window.processList.itemAtIndex(8) !== null
        })
        const card = window.processList.itemAtIndex(8)
        window.hoveredProcessId = ""
        compare(card.attentionHighlighted, true)
        compare(card.isHovered, false)
        compare(card.processAccentColor,
                CompanionTheme.success)
        compare(card.usesSubduedResponseAccent, false)
        compare(card.processAccentFillAmount, 0.16)
        compare(card.processAccentBorderAlpha, 0.48)
        verify(card.border.color
            !== CompanionTheme.border)
        tryVerify(function() {
            return card.color !== CompanionTheme.surfaceRaised
        })

        window.attentionModel.latestAttentionHighlight = ({
            "processId": "scroll-6",
            "kind": "response"
        })
        tryCompare(card,
                   "processAccentColor",
                   CompanionTheme.info)
        compare(card.usesSubduedResponseAccent, true)
        compare(card.processAccentFillAmount, 0.07)
        compare(card.processAccentBorderAlpha, 0.40)
        verify(card.border.color
            !== CompanionTheme.border)
        tryVerify(function() {
            return card.color !== CompanionTheme.surfaceRaised
        })
    }

    function test_hover_expansion_preserves_manual_process_scroll() {
        const window = createWindow()
        for (let index = 0; index < 7; ++index) {
            window.shellModel.processModel.append({
                "id": "hover-scroll-" + index,
                "processId": "hover-scroll-" + index,
                "threadId": "hover-scroll-" + index,
                "kind": "thread",
                "title": "Completed process " + index,
                "preview": "Completed process " + index,
                "updatedAt":
                    Date.now() / 1000 - 978307200,
                "status": "completed",
                "needsApproval": false,
                "runtimeStatus": "completed",
                "cwd": "",
                "activeTurnId": "",
                "model": "",
                "reasoningEffort": "",
                "goal": null
            })
        }
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })
        wait(50)

        window.attentionModel.latestAttentionHighlight = ({
            "processId": "process-1",
            "kind": "response"
        })
        wait(50)

        window.processList.positionViewAtIndex(
            8,
            ListView.End)
        wait(0)
        const lowerCard =
            window.processList.itemAtIndex(8)
        verify(lowerCard)
        const manualContentY =
            window.processList.contentY
        verify(manualContentY > 0)

        const globalHoverPoint = lowerCard.mapToGlobal(
            lowerCard.width / 2,
            lowerCard.height / 2)
        const hoverPoint = window.processList.mapFromGlobal(
            globalHoverPoint.x,
            globalHoverPoint.y)
        verify(hoverPoint.y >= 0)
        verify(hoverPoint.y <= window.processList.height)
        moveProcessPointer(
            window,
            window.processSurface,
            2,
            2)
        wait(1)
        moveProcessPointer(
            window,
            window.processList,
            hoverPoint.x,
            hoverPoint.y)
        tryCompare(lowerCard, "height", 87)
        wait(50)

        verify(
            Math.abs(
                window.processList.contentY
                - manualContentY) < 1)
    }

    function test_attention_change_recenters_an_active_process_target() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            1,
            "status",
            "completed")
        window.shellModel.processModel.setProperty(
            1,
            "needsApproval",
            false)
        window.shellModel.processModel.setProperty(
            1,
            "runtimeStatus",
            "completed")
        for (let index = 0; index < 7; ++index) {
            window.shellModel.processModel.append({
                "id": "active-scroll-" + index,
                "processId": "active-scroll-" + index,
                "threadId": "active-scroll-" + index,
                "kind": "thread",
                "title": "Active scroll target " + index,
                "preview": "Completed process " + index,
                "updatedAt":
                    Date.now() / 1000 - 978307200,
                "status": "completed",
                "needsApproval": false,
                "runtimeStatus": "completed",
                "cwd": "",
                "activeTurnId": "",
                "model": "",
                "reasoningEffort": "",
                "goal": null
            })
        }
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.contentHeight
                > window.processList.height
        })

        window.shellModel.processTargetActive = true
        window.shellModel.processTargetId =
            "active-scroll-6"
        tryVerify(function() {
            return window.processList.contentY > 0
        })

        window.processList.contentY = 0
        compare(window.processList.contentY, 0)
        window.attentionModel.latestAttentionHighlight = ({
            "processId": "active-scroll-0",
            "kind": "completion"
        })

        tryVerify(function() {
            return window.processList.contentY > 0
        })
    }

    function test_model_picker_updates_the_shell_model() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        compare(window.shellModel.selectedChatModelId, "on-device")
        compare(window.chatModelPopup.width, 238)
        window.chatModelPopup.open()
        tryCompare(window, "modelPickerOpen", true)
        const option = window.chatModelOptions.itemAt(2)
        verify(option)
        compare(
            option.optionButton.interactionId,
            "model.select")

        mouseClick(option.optionButton)

        compare(
            window.shellModel.selectedChatModelId,
            "openai:gpt56Terra")
        tryCompare(window, "modelPickerOpen", false)
    }

    function test_compact_chat_placeholder_matches_selected_delivery_provider() {
        const window = createWindow()
        window.shellModel.showLocalChat()

        compare(window.promptInput.placeholderText, "Ask on device")

        window.shellModel.selectedChatModelId =
            "openai:gpt56Terra"
        tryCompare(
            window.promptInput,
            "placeholderText",
            "Ask ChatGPT")

        window.shellModel.selectedChatModelId =
            "lumo:thinking"
        tryCompare(
            window.promptInput,
            "placeholderText",
            "Ask Lumo")
    }

    function test_chat_send_button_uses_macos_hover_deformation() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.chatSendEnabled = true
        window.promptInput.text = "Hover the send button"
        window.x = 120
        window.y = 120
        window.requestActivate()

        tryCompare(window, "active", true)
        tryCompare(window, "x", 120)
        tryCompare(window, "y", 120)
        tryCompare(window.sendButton, "visible", true)
        tryCompare(window.sendButton, "enabled", true)
        waitForRendering(window.contentItem)
        tryVerify(function() {
            return window.sendButton.x > 0
                && window.sendButton.width > 0
                && window.sendButton.height > 0
        })
        const initialX = window.sendButton.x
        const initialY = window.sendButton.y
        const initialWidth = window.sendButton.width
        const initialHeight = window.sendButton.height
        compare(
            window.sendButton.transform.length,
            0,
            "Chat send hit bounds must not deform")
        mouseMove(
            window.sendButton,
            window.sendButton.width / 2,
            window.sendButton.height / 2)
        tryCompare(window.sendButton, "hoverActive", true)
        tryVerify(function() {
            return window.sendButton.horizontalScale > 1
                && window.sendButton.verticalScale < 1
        })
        compare(window.sendButton.x, initialX)
        compare(window.sendButton.y, initialY)
        compare(window.sendButton.width, initialWidth)
        compare(window.sendButton.height, initialHeight)
    }

    function test_rejected_model_selection_restores_the_picker() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.rejectChatModelSelection = true
        window.chatModelPopup.open()
        tryCompare(window, "modelPickerOpen", true)
        tryCompare(window, "height", 94)
        const option = window.chatModelOptions.itemAt(2)
        verify(option)

        mouseClick(option.optionButton)

        compare(window.shellModel.selectedChatModelId, "on-device")
        tryCompare(window, "modelPickerOpen", true)
        tryCompare(window, "height", 94)
    }

    function test_chat_response_dismiss_button_clears_response() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.chatResponseTitle = "5.6 Terra"
        window.shellModel.chatResponsePrompt = "Dismiss this prompt"
        window.shellModel.chatResponse = "Dismiss this response"
        window.shellModel.chatResponseUsageSummary = "12 in \u00b7 34 out"

        tryCompare(window, "height", 420)
        compare(window.chatResponseTitleLabel.text, "5.6 Terra")
        compare(window.chatResponsePromptLabel.text, "Dismiss this prompt")
        compare(window.chatResponseLabel.text, "Dismiss this response")
        compare(window.chatResponseUsageLabel.text, "12 in \u00b7 34 out")
        tryCompare(window.chatResponseDismissButton, "visible", true)
        compare(
            window.chatResponseDismissButton.interactionId,
            "chat.response.close")
        window.requestActivate()
        tryCompare(window, "active", true)
        mouseClick(window.chatResponseDismissButton)

        compare(window.shellModel.chatResponse, "")
        compare(window.shellModel.chatResponsePrompt, "")
        compare(window.shellModel.chatResponseTitle, "")
        compare(window.shellModel.chatResponseUsageSummary, "")
        compare(window.chatResponseDismissButton.visible, false)
    }

    function test_pet_model_picker_request_opens_the_existing_selector() {
        const window = createWindow()

        window.openModelPicker()

        compare(window.routeMode, "local-chat")
        tryCompare(window, "modelPickerOpen", true)
        tryCompare(window, "height", 94)
        verify(window.chatModelPopup.height > 300)
    }

    function test_native_close_requests_only_the_reusable_surface() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        compare(window.closeButton.visible, false)
        window.requestActivate()
        tryCompare(window, "active", true)
        const spy = signalSpy.createObject(window, {
            target: window,
            signalName: "closeRequested"
        })

        window.close()

        compare(spy.count, 1)
        compare(Qt.application.state, Qt.ApplicationActive)
    }

    function test_on_device_preparation_button_executes_its_action() {
        const window = createWindow()
        window.shellModel.showLocalChat()

        tryCompare(window, "height", 94)
        compare(window.prepareChatButton.visible, true)
        compare(window.prepareChatButton.enabled, true)
        compare(
            window.prepareChatButton.interactionId,
            "composer.prepare")
        window.requestActivate()
        tryCompare(window, "active", true)
        tryVerify(function() {
            return window.prepareChatButton.width > 0
                && window.prepareChatButton.height > 0
        })
        mouseClick(
            window.prepareChatButton,
            window.prepareChatButton.width / 2,
            window.prepareChatButton.height / 2,
            Qt.LeftButton)

        compare(window.shellModel.preparationRequestCount, 1)
    }

    function test_chat_response_card_tracks_the_current_request() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.chatResponseTitle = "5.6 Sol"
        window.shellModel.chatResponsePrompt = "Explain the current task"
        window.shellModel.chatResponse = "Thinking..."
        window.shellModel.chatStatusMessage = "Asking 5.6 Sol..."
        window.shellModel.chatBusy = true

        tryCompare(window, "height", 420)
        compare(window.chatResponseTitleLabel.text, "5.6 Sol")
        compare(window.chatResponsePromptLabel.text,
                "Explain the current task")
        compare(window.chatResponseLabel.text, "Thinking...")
        compare(window.chatResponseUsageLabel.visible, false)

        window.shellModel.chatBusy = false
        window.shellModel.chatResponse = "Current answer"
        window.shellModel.chatResponseUsageSummary =
            "12 in \u00b7 34 out"

        compare(window.chatResponseLabel.text, "Current answer")
        compare(window.chatResponseUsageLabel.text,
                "12 in \u00b7 34 out")
        compare(window.chatResponseUsageLabel.visible, true)
    }

    function test_hover_expands_running_process_and_reveals_reply_and_steer() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        verify(card)
        compare(card.height, 72)
        compare(card.replyButton.parent.opacity, 0)
        compare(card.steerButton.parent.opacity, 0)
        compare(card.processStatusMessage.text,
                "Goal running 2m 5s")
        compare(card.processStatusMessage.opacity, 1)
        compare(card.processFullMessage.text,
                "Implementing the Windows shell")
        compare(card.processFullMessage.opacity, 0)
        compare(card.ToolTip.text,
                "Implementing the Windows shell")

        hoverProcessCard(window, card)
        tryCompare(card, "height", 101)

        tryCompare(card.replyButton.parent, "opacity", 1)
        tryCompare(card.steerButton.parent, "opacity", 1)
        tryCompare(card.processStatusMessage, "opacity", 0)
        tryCompare(card.processFullMessage, "opacity", 1)
        compare(card.width, window.processList.width)

        moveProcessPointer(
            window,
            window.processSurface,
            2,
            2)
        tryCompare(card, "height", 72)
        tryCompare(card.processStatusMessage, "opacity", 1)
        tryCompare(card.processFullMessage, "opacity", 0)
    }

    function test_empty_process_identity_does_not_inherit_no_attention_sentinel() {
        const window = createWindow()
        window.attentionModel.latestAttentionHighlight = ({})
        window.shellModel.processModel.setProperty(
            0,
            "processId",
            "")
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        tryCompare(card, "processId", "")
        compare(window.latestAttentionProcessId, "")
        compare(card.attentionHighlighted, false)
        compare(card.hasProcessAccent, false)
    }

    function test_hover_animation_uses_stable_final_window_metrics() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
                && window.processList.itemAtIndex(1) !== null
        })
        const firstCard = window.processList.itemAtIndex(0)
        const secondCard = window.processList.itemAtIndex(1)
        const heightTrace = []
        const recordHeight = function() {
            heightTrace.push(
                window.height
                + "/" + window.targetHeight
                + "/" + window.hoveredProcessId)
            if (heightTrace.length > 16) {
                heightTrace.shift()
            }
        }
        window.heightChanged.connect(recordHeight)
        window.targetHeightChanged.connect(recordHeight)
        window.hoveredProcessIdChanged.connect(recordHeight)

        tryCompare(window, "height", 182)
        window.visible = false
        tryCompare(window, "visible", false)
        window.hoveredProcessId = ""
        tryCompare(window, "hoveredProcessId", "")

        for (let cycle = 0; cycle < 12; ++cycle) {
            window.hoveredProcessId = firstCard.processId
            tryCompare(firstCard, "showsAnyActions", true)
            tryCompare(window, "height", 211)
            for (let sample = 0; sample < 5; ++sample) {
                wait(12)
                compare(window.height, 211)
            }

            window.hoveredProcessId = secondCard.processId
            tryCompare(secondCard, "showsAnyActions", true)
            tryCompare(window, "height", 211)
            for (let sample = 0; sample < 5; ++sample) {
                wait(12)
                compare(window.height, 211)
            }

            window.hoveredProcessId = ""
            tryCompare(secondCard, "showsAnyActions", false)
            wait(120)
            const collapseDiagnostics =
                processHeightDiagnostics(
                    window,
                    firstCard,
                    secondCard)
                + " trace=" + heightTrace.join(",")
            compare(
                window.hoveredProcessId,
                "",
                collapseDiagnostics)
            compare(
                firstCard.showsAnyActions,
                false,
                collapseDiagnostics)
            compare(
                secondCard.showsAnyActions,
                false,
                collapseDiagnostics)
            compare(
                window.targetHeight,
                182,
                collapseDiagnostics)
            compare(
                window.height,
                182,
                collapseDiagnostics)
            for (let sample = 0; sample < 5; ++sample) {
                wait(12)
                compare(
                    window.height,
                    182,
                    processHeightDiagnostics(
                        window,
                        firstCard,
                        secondCard)
                    + " trace=" + heightTrace.join(","))
            }
        }
    }

    function test_process_hover_survives_transient_popup_relayout() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId)
        tryCompare(card, "height", 101)

        window.endProcessHover(card.processId)
        if (window.processHoverExitGraceDuration > 1) {
            wait(
                Math.floor(
                    window.processHoverExitGraceDuration
                    / 2))
        }
        const pointer =
            card.mapToItem(
                window.contentItem,
                card.width / 2,
                card.height / 2)
        window.reconcileProcessHoverAt(
            pointer.x,
            pointer.y)
        wait(
            window.processHoverExitGraceDuration
            + 20)

        compare(
            window.hoveredProcessId,
            card.processId)
        compare(card.height, 101)

        moveProcessPointerOutside(window)
        tryCompare(
            window,
            "hoveredProcessId",
            "",
            window.processHoverExitGraceDuration
                + 100)
        tryCompare(card, "height", 72)
    }

    function test_process_hover_survives_delayed_native_reconciliation() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId)
        tryCompare(card, "height", 101)

        const pointer = card.mapToItem(
            window.contentItem,
            card.width / 2,
            card.height - 2)
        window.endProcessHover(card.processId)
        wait(45)
        window.reconcileProcessHoverAt(
            pointer.x,
            pointer.y)
        wait(
            window.processHoverExitGraceDuration
            + 20)

        compare(
            window.hoveredProcessId,
            card.processId)
        compare(card.isHovered, true)
        compare(card.height, 101)
    }

    function test_process_hover_latches_collapsed_and_expanded_card_bounds() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        const collapsedBounds =
            window.processCardWindowBounds(
                card.processId,
                false)
        verify(collapsedBounds.width > 0)
        verify(collapsedBounds.height > 0)

        window.beginProcessHover(card.processId)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId)
        const expandedBounds =
            window.processCardWindowBounds(
                card.processId,
                true)
        verify(
            expandedBounds.height
                > collapsedBounds.height)
        const pointerX =
            expandedBounds.x
                + expandedBounds.width / 2
        const pointerY = Math.min(
            expandedBounds.y
                + expandedBounds.height - 1,
            collapsedBounds.y
                + collapsedBounds.height + 12)
        verify(
            pointerY
                >= collapsedBounds.y
                    + collapsedBounds.height)

        window.reconcileProcessHoverAt(
            pointerX,
            pointerY)
        wait(120)
        compare(
            window.hoveredProcessId,
            card.processId)
        wait(60)
        compare(
            window.hoveredProcessId,
            card.processId)
        compare(card.isHovered, true)

        window.reconcileProcessHoverAt(
            window.processSurface.x - 20,
            window.processSurface.y - 20)
        wait(
            Math.max(
                window.processHoverExitGraceDuration,
                window.processHoverExpansionDuration)
                + 20)
        compare(window.hoveredProcessId, "")
    }

    function test_process_hover_exit_clears_after_bounded_reconciliation_grace() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        window.beginProcessHover(card.processId)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId)

        window.endProcessHover(card.processId)
        if (window.processHoverExitGraceDuration > 1) {
            wait(
                Math.floor(
                    window.processHoverExitGraceDuration
                    / 2))
            compare(
                window.hoveredProcessId,
                card.processId)
        }
        wait(
            window.processHoverExitGraceDuration
            + 20)

        compare(window.hoveredProcessId, "")
    }

    function test_process_hover_remains_stable_past_tooltip_delay() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(
            window,
            "hoveredProcessId",
            card.processId)
        tryCompare(card, "height", 101)

        wait(900)

        compare(window.hoveredProcessId, card.processId)
        compare(card.isHovered, true)
        compare(card.height, 101)
        compare(card.ToolTip.visible, true)

        moveProcessPointer(
            window,
            window.processSurface,
            2,
            2)
        tryCompare(window, "hoveredProcessId", "")
        tryCompare(card, "height", 72)
    }

    function test_process_height_ignores_transient_rendered_content() {
        const window = createWindow()
        window.x += window.width + 200
        wait(1)
        window.clearProcessHover()
        window.shellModel.processModel.clear()
        for (let index = 0; index < 3; ++index) {
            window.shellModel.processModel.append({
                "id": "stable-height-" + index,
                "processId": "stable-height-" + index,
                "threadId": "stable-height-" + index,
                "kind": "thread",
                "title": "Stable process " + index,
                "preview": "Stable process " + index,
                "sourceStatus": "",
                "updatedAt":
                    Date.now() / 1000 - 978307200,
                "status": "completed",
                "needsApproval": false,
                "runtimeStatus": "completed",
                "cwd": "",
                "activeTurnId": "",
                "model": "",
                "reasoningEffort": "",
                "goal": null
            })
        }

        tryCompare(window.processList, "count", 3)
        tryCompare(window, "processFeedEmpty", false)
        tryCompare(window, "processTargetListHeight", 188)
        tryCompare(window, "height", 232)

        window.shellModel.showLocalChat()
        tryCompare(window, "height", 94)
        window.shellModel.showProcesses()
        tryCompare(window, "height", 232)
    }

    function test_typed_process_draft_updates_writable_model_property() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        window.shellModel.beginProcessAction({
            "id": "thread-goal",
            "title": "Port Codex Companion",
            "status": "running",
            "needsApproval": false
        }, "steer")
        tryCompare(card.inlineComposer, "visible", true)

        card.processPromptInput.text = "Continue the parity pass"

        compare(window.shellModel.processDraft,
                "Continue the parity pass")
    }

    function test_process_action_carries_runtime_target_metadata() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        const target = card.targetData()
        window.shellModel.beginProcessAction(target, "steer")

        compare(window.shellModel.processTargetActive, true)
        compare(window.shellModel.lastProcessTarget.id, "thread-goal")
        compare(window.shellModel.lastProcessTarget.threadId,
                "thread-goal")
        compare(window.shellModel.lastProcessTarget.cwd, "C:\\worktree")
        compare(window.shellModel.lastProcessTarget.activeTurnId,
                "turn-goal")
        compare(window.shellModel.lastProcessTarget.model, "gpt-test")
        compare(window.shellModel.lastProcessTarget.reasoningEffort,
                "high")
        compare(window.shellModel.lastProcessTarget.kind, "thread")
        compare(window.shellModel.lastProcessTarget.runtimeStatus,
                "active")
        compare(window.shellModel.lastProcessTarget.rolloutPath,
                "C:\\rollouts\\thread-goal.jsonl")
    }

    function test_failed_stopped_process_exposes_safe_retry_action() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "failed")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "idle")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            null)
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersRetry", true)
        tryCompare(card.retryButton, "visible", true)
        tryCompare(card.retryButton.parent, "opacity", 1)
        verify(card.retryButton.enabled)
        compare(
            card.retryButton.interactionId,
            "process.retry")

        mouseClick(card.retryButton)

        compare(
            window.shellModel.processRetryRequestCount,
            1)
        compare(
            window.shellModel.lastProcessTarget.threadId,
            "thread-goal")
        compare(
            window.shellModel.lastProcessTarget.rolloutPath,
            "C:\\rollouts\\thread-goal.jsonl")
        compare(
            window.shellModel.retryingProcessId,
            "thread-goal")
        compare(card.retryStatusLabel.visible, true)
        compare(card.retryStatusLabel.text, "Retrying...")

        window.shellModel.retryingProcessId = ""
        window.shellModel.processRetryStatus =
            "Resumed with Account 2."
        compare(card.retryStatusLabel.visible, true)
        compare(
            card.retryStatusLabel.text,
            "Resumed with Account 2.")

        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "active")
        tryCompare(card, "offersRetry", false)
        compare(card.retryButton.visible, false)

        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "idle")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            {
                "threadId": "thread-goal",
                "objective": "Ship Windows Companion",
                "status": "complete",
                "updatedAt": 2000
            })
        tryCompare(card, "offersRetry", false)
        compare(card.retryButton.visible, false)
    }

    function test_system_error_failed_process_exposes_reply_and_retry() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "failed")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "systemError")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            null)
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersReply", true)
        tryCompare(card, "offersRetry", true)
        tryCompare(card.replyButton, "visible", true)
        tryCompare(card.retryButton, "visible", true)
        verify(card.replyButton.enabled)
        verify(card.retryButton.enabled)
    }

    function test_failed_process_without_runtime_metadata_exposes_reply_and_retry() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "failed")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            null)
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersReply", true)
        tryCompare(card, "offersSteer", false)
        tryCompare(card, "offersRetry", true)
        tryCompare(card.replyButton, "visible", true)
        tryCompare(card.retryButton, "visible", true)
        verify(card.replyButton.enabled)
        verify(card.retryButton.enabled)
    }

    function test_running_recoverable_goal_keeps_reply_and_steer_without_retry() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "running")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "active")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            {
                "threadId": "thread-goal",
                "objective": "Ship Windows Companion",
                "status": "blocked",
                "updatedAt": 2000
            })
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersReply", true)
        tryCompare(card, "offersSteer", true)
        tryCompare(card, "offersRetry", false)

        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "idle")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            {
                "threadId": "thread-goal",
                "objective": "Ship Windows Companion",
                "status": "usageLimited",
                "updatedAt": 3000
            })

        tryCompare(card, "offersReply", true)
        tryCompare(card, "offersSteer", true)
        tryCompare(card, "offersRetry", false)
        compare(card.retryButton.visible, false)
    }

    function test_usage_limited_stopped_process_exposes_reply_and_retry() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "waiting")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "idle")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            {
                "threadId": "thread-goal",
                "objective": "Ship Windows Companion",
                "status": "usageLimited",
                "updatedAt": 2000
            })
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersReply", true)
        tryCompare(card, "offersRetry", true)
        tryCompare(card.replyButton, "visible", true)
        tryCompare(card.retryButton, "visible", true)
        tryCompare(card.replyButton.parent, "opacity", 1)
        verify(card.replyButton.enabled)
        verify(card.retryButton.enabled)

        mouseClick(card.replyButton)
        compare(window.shellModel.processTargetAction, "reply")
        compare(window.shellModel.processTargetId, "thread-goal")

        window.shellModel.cancelProcessTarget()
        mouseClick(card.retryButton)
        compare(window.shellModel.processRetryRequestCount, 1)
        compare(window.shellModel.lastProcessTarget.id, "thread-goal")
    }

    function test_waiting_for_input_recoverable_goal_never_exposes_retry() {
        const window = createWindow()
        window.shellModel.processModel.setProperty(
            0,
            "status",
            "waiting")
        window.shellModel.processModel.setProperty(
            0,
            "runtimeStatus",
            "waitingOnUserInput")
        window.shellModel.processModel.setProperty(
            0,
            "goal",
            {
                "threadId": "thread-goal",
                "objective": "Ship Windows Companion",
                "status": "blocked",
                "updatedAt": 2000
            })
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)

        hoverProcessCard(window, card)
        tryCompare(card, "offersRetry", false)
        compare(card.retryButton.visible, false)
    }

    function test_job_cards_target_assigned_threads_only() {
        const window = createWindow()
        window.shellModel.processModel.append({
            "id": "job-build",
            "processId": "job-build",
            "threadId": "thread-build",
            "kind": "job",
            "title": "Build Windows Companion",
            "preview": "Compiling the native app",
            "updatedAt":
                Date.now() / 1000 - 978307200,
            "status": "running",
            "needsApproval": false,
            "cwd": "",
            "activeTurnId": "",
            "model": "",
            "reasoningEffort": "",
            "goal": null
        })
        window.shellModel.processModel.append({
            "id": "job-unassigned",
            "processId": "job-unassigned",
            "threadId": "",
            "kind": "job",
            "title": "Unassigned job",
            "preview": "Waiting for a thread",
            "updatedAt":
                Date.now() / 1000 - 978307200,
            "status": "running",
            "needsApproval": false,
            "cwd": "",
            "activeTurnId": "",
            "model": "",
            "reasoningEffort": "",
            "goal": null
        })
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(3) !== null
        })

        const assigned =
            window.processList.itemAtIndex(2)
        hoverProcessCard(window, assigned)
        tryCompare(assigned.replyButton, "visible", true)
        tryCompare(assigned.steerButton, "visible", true)
        const target = assigned.targetData()
        compare(target.id, "job-build")
        compare(target.threadId, "thread-build")
        window.shellModel.beginProcessAction(
            target,
            "steer")
        compare(
            window.shellModel.processTargetId,
            "job-build")

        const unassigned =
            window.processList.itemAtIndex(3)
        hoverProcessCard(window, unassigned)
        compare(unassigned.replyButton.visible, false)
        compare(unassigned.steerButton.visible, false)
        compare(unassigned.showsAnyActions, false)
    }

    function test_return_submits_inline_process_composer() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        window.shellModel.beginProcessAction(
            card.targetData(),
            "reply")
        tryCompare(card.inlineComposer, "visible", true)
        card.processPromptInput.text = "Submit with Return"
        window.requestActivate()
        tryCompare(window, "active", true)
        card.processPromptInput.forceActiveFocus()
        tryCompare(card.processPromptInput, "activeFocus", true)
        tryCompare(card.processSendButton, "enabled", true)

        keyClick(Qt.Key_Return)

        compare(window.shellModel.processMessageRequestCount, 1)
        compare(window.shellModel.processSending, true)
    }

    function test_reply_and_steer_buttons_open_and_submit_inline_composer() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        hoverProcessCard(window, card)
        tryCompare(card.replyButton.parent, "opacity", 1)
        verify(card.replyButton.width > 0)
        verify(card.steerButton.width > 0)
        verify(card.replyButton.parent.width > 0,
               "action row width=" + card.replyButton.parent.width)
        tryCompare(card.replyButton.parent.parent, "height", 24)
        verify(card.replyButton.enabled)
        compare(
            card.replyButton.interactionId,
            "process.reply")
        compare(
            card.steerButton.interactionId,
            "process.steer")
        compare(
            card.processSendButton.interactionId,
            "process.send")
        verify(card.replyButton.x + card.replyButton.width
               <= card.steerButton.x,
               "reply=" + card.replyButton.x + "+"
               + card.replyButton.width + ", steer="
               + card.steerButton.x + "+" + card.steerButton.width)
        compare(
            card.replyButton.transform.length,
            0,
            "Process action hit bounds must not deform")

        mouseMove(card.replyButton)
        tryCompare(card.replyButton, "hovered", true)
        tryCompare(card.replyButton, "horizontalScale", 1.014)
        tryCompare(card.replyButton, "verticalScale", 0.992)
        mouseClick(card.replyButton)

        compare(window.shellModel.processTargetActive, true)
        compare(window.shellModel.processTargetId, "thread-goal")
        compare(window.shellModel.processTargetAction, "reply")
        tryCompare(card.inlineComposer, "visible", true)
        compare(card.processPromptInput.placeholderText,
                "Reply to Port Codex Companion")

        card.processPromptInput.text = "Continue the parity pass"
        mouseClick(card.processSendButton)

        compare(window.shellModel.processMessageRequestCount, 1)
        compare(window.shellModel.processSending, true)
        compare(window.shellModel.processDraft,
                "Continue the parity pass")
    }

    function test_steer_button_and_composer_cancel_route_actions() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        hoverProcessCard(window, card)
        tryCompare(card.steerButton.parent, "opacity", 1)
        compare(
            card.cancelProcessButton.interactionId,
            "process.cancel")

        mouseClick(card.steerButton)

        compare(window.shellModel.processTargetActive, true)
        compare(window.shellModel.processTargetAction, "steer")
        tryCompare(card.inlineComposer, "visible", true)
        verify(card.cancelProcessButton.visible)
        verify(card.cancelProcessButton.enabled)

        card.processPromptInput.text = "Cancel this steer"
        mouseClick(card.processSendButton)
        compare(window.shellModel.processSending, true)
        verify(card.cancelProcessButton.enabled)

        mouseClick(card.cancelProcessButton)

        compare(window.shellModel.processCancelRequestCount, 1)
        compare(window.shellModel.processSending, false)
        compare(window.shellModel.processTargetActive, false)
        compare(window.shellModel.processTargetId, "")
        compare(window.shellModel.processTargetAction, "")
        compare(window.shellModel.processDraft, "")
        tryCompare(card.inlineComposer, "visible", false)
    }

    function test_approval_buttons_execute_and_tell_codex_opens_guidance() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(1) !== null
        })
        const card = window.processList.itemAtIndex(1)
        hoverProcessCard(window, card)
        tryCompare(card.approveOnceButton.parent, "opacity", 1)
        compare(card.approveSimilarButton.parent.opacity, 1)
        compare(card.tellCodexButton.parent.opacity, 1)
        verify(card.approveOnceButton.width > 0)
        verify(card.approveSimilarButton.width > 0)
        verify(card.tellCodexButton.width > 0)
        verify(card.approveOnceButton.parent.width > 0,
               "action row width="
               + card.approveOnceButton.parent.width)
        tryCompare(card.approveOnceButton.parent.parent, "height", 24)
        verify(card.approveOnceButton.enabled)
        compare(
            card.approveOnceButton.interactionId,
            "process.approve-once")
        compare(
            card.approveSimilarButton.interactionId,
            "process.approve-similar")
        compare(
            card.tellCodexButton.interactionId,
            "process.tell-codex")
        verify(card.approveOnceButton.x
               + card.approveOnceButton.width
               <= card.approveSimilarButton.x,
               "once=" + card.approveOnceButton.x + "+"
               + card.approveOnceButton.width + ", similar="
               + card.approveSimilarButton.x + "+"
               + card.approveSimilarButton.width)
        verify(card.approveSimilarButton.x
               + card.approveSimilarButton.width
               <= card.tellCodexButton.x)
        verify(card.tellCodexButton.x
               + card.tellCodexButton.width
               <= card.tellCodexButton.parent.width,
               "tell=" + card.tellCodexButton.x + "+"
               + card.tellCodexButton.width + ", row="
               + card.tellCodexButton.parent.width)

        mouseMove(card.approveOnceButton)
        tryCompare(card.approveOnceButton, "hovered", true)
        mouseClick(card.approveOnceButton)

        compare(window.shellModel.processApprovalRequestCount, 1)
        compare(window.shellModel.lastProcessApprovalDecision,
                "approveOnce")

        window.shellModel.approvingProcessId = ""
        mouseMove(card.approveSimilarButton)
        tryCompare(card.approveSimilarButton, "hovered", true)
        mouseClick(card.approveSimilarButton)

        compare(window.shellModel.processApprovalRequestCount, 2)
        compare(window.shellModel.lastProcessApprovalDecision,
                "approveSimilar")

        window.shellModel.approvingProcessId = ""
        mouseMove(card.tellCodexButton)
        tryCompare(card.tellCodexButton, "hovered", true)
        mouseClick(card.tellCodexButton)

        compare(window.shellModel.processTargetActive, true)
        compare(window.shellModel.processTargetId, "thread-tray")
        compare(window.shellModel.processTargetAction,
                "approval-feedback")
        tryCompare(card.inlineComposer, "visible", true)
    }

    function test_approval_actions_require_waiting_runtime_state() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(1) !== null
        })
        const card = window.processList.itemAtIndex(1)

        window.shellModel.processModel.setProperty(
            1,
            "runtimeStatus",
            "active")
        hoverProcessCard(window, card)

        compare(card.needsApproval, true)
        compare(card.offersApproval, false)

        window.shellModel.processModel.setProperty(
            1,
            "runtimeStatus",
            "waitingOnApproval")

        tryCompare(card, "offersApproval", true)
    }

    function test_approval_pending_state_matches_the_active_process_card() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(1) !== null
        })
        const card = window.processList.itemAtIndex(1)
        hoverProcessCard(window, card)
        tryCompare(card.approveOnceButton.parent, "opacity", 1)

        window.shellModel.processSending = true

        compare(card.approveOnceButton.enabled, true)
        compare(card.approveSimilarButton.enabled, true)
        compare(card.tellCodexButton.enabled, true)
        compare(card.approveOnceButton.opacity, 1)
        compare(card.approveSimilarButton.opacity, 1)
        compare(card.tellCodexButton.opacity, 1)

        window.shellModel.processSending = false
        window.shellModel.approvingProcessId = "another-thread"

        compare(card.approveOnceButton.enabled, false)
        compare(card.approveSimilarButton.enabled, false)
        compare(card.tellCodexButton.enabled, true)
        tryCompare(card.approveOnceButton, "opacity", 0.55)
        tryCompare(card.approveSimilarButton, "opacity", 0.55)
        compare(card.tellCodexButton.opacity, 1)

        window.shellModel.approvingProcessId = "thread-tray"

        compare(card.approveOnceButton.enabled, false)
        compare(card.approveSimilarButton.enabled, false)
        compare(card.tellCodexButton.enabled, false)
        tryCompare(card.approveOnceButton, "opacity", 0.55)
        tryCompare(card.approveSimilarButton, "opacity", 0.55)
        tryCompare(card.tellCodexButton, "opacity", 0.55)
    }

    function test_goal_badge_opens_edit_pause_and_resume_controls() {
        const window = createWindow()
        window.processList.forceLayout()
        tryVerify(function() {
            return window.processList.itemAtIndex(0) !== null
        })
        const card = window.processList.itemAtIndex(0)
        verify(card.goalButton.visible)
        compare(card.height, 72)
        compare(card.goalButton.text, "Goal 2m 5s")
        compare(
            card.goalButton.interactionId,
            "process.goal.open")
        window.x = 400
        const screenGeometry =
            window.placementController.availableWorkAreaAt(
                Qt.point(
                    window.x + window.width / 2,
                    window.y + window.height / 2))
        window.y = screenGeometry.y + 420
        tryVerify(function() {
            return card.goalButton.text !== "Goal 2m 5s"
        }, 2000)

        mouseClick(card.goalButton)

        tryCompare(window.goalPopup, "visible", true)
        compare(window.goalStatusIcon.text,
                window.goalStatusGlyph("active"))
        compare(window.goalStatusIcon.font.family,
                "Segoe Fluent Icons")
        verify(window.goalPopupAnchored)
        const anchor = card.goalButton.mapToItem(
            window.contentItem,
            card.goalButton.width / 2,
            0)
        compare(
            Math.round(window.goalPopupAnchorCenterX),
            Math.round(anchor.x))
        compare(
            Math.round(window.goalPopupAnchorTopY),
            Math.round(anchor.y))
        verify(window.goalPopup.height > 0)
        const expectedPopupY =
            window.goalPopupOriginY(
                window.goalPopupAnchorTopY,
                window.goalPopupAnchorHeight,
                window.goalPopup.height,
                window.y,
                screenGeometry.y,
                screenGeometry.y
                    + screenGeometry.height)
        tryCompare(
            window.goalPopup.contentItem.Window.window,
            "y",
            window.y + expectedPopupY)
        tryCompare(
            window.goalPopup.contentItem.Window.window,
            "x",
            window.x
                + window.goalPopupTargetOriginX())
        moveProcessPointer(
            window,
            window.goalCloseButton)
        tryCompare(window, "hoveredProcessId", "")
        tryCompare(window, "height", 182)
        compare(window.goalObjectiveLabel.text, "Ship Windows Companion")
        compare(window.goalStatusLabel.text, "Goal active")
        compare(window.goalPauseButton.visible, true)
        compare(window.goalPauseButton.enabled, true)
        compare(
            window.goalPauseButton.interactionId,
            "goal.pause")
        compare(
            window.goalResumeButton.interactionId,
            "goal.resume")
        compare(
            window.goalEditButton.interactionId,
            "goal.edit")
        compare(
            window.goalCancelButton.interactionId,
            "goal.cancel")
        compare(
            window.goalSaveButton.interactionId,
            "goal.save")
        compare(
            window.goalCloseButton.interactionId,
            "goal.close")
        verify(window.goalPauseButton.width > 0)
        verify(window.goalPauseButton.height > 0)
        compare(window.goalResumeButton.visible, false)
        tryVerify(function() {
            const gap = window.goalEditButton.x
                - (window.goalPauseButton.x
                    + window.goalPauseButton.width)
            return gap >= 0 && gap <= 10
        })

        mouseClick(window.goalPauseButton)
        compare(window.shellModel.goalPauseRequestCount, 1)
        compare(window.shellModel.goalMutationPending, true)
        compare(window.goalCloseButton.enabled, true)
        window.shellModel.goalStatus = "paused"
        window.shellModel.goalMutationPending = false

        mouseClick(window.goalEditButton)
        compare(window.shellModel.goalEditing, true)
        compare(window.goalEditor.visible, true)
        tryCompare(window.goalEditor, "focus", true)
        if (window.active) {
            compare(window.goalEditor.activeFocus, true)
        }
        window.goalEditor.text = "Verify complete Windows parity"
        tryCompare(window.goalCancelButton, "visible", true)
        tryCompare(window.goalCancelButton, "enabled", true)
        tryVerify(function() {
            return window.goalCancelButton.width > 0
                && window.goalCancelButton.height > 0
        })
        tryVerify(function() {
            return window.goalSaveButton.x
                >= window.goalCancelButton.x
                    + window.goalCancelButton.width
        })
        mouseClick(
            window.goalCancelButton,
            window.goalCancelButton.width / 2,
            window.goalCancelButton.height / 2,
            Qt.LeftButton)
        compare(window.shellModel.goalEditing, false)
        compare(window.shellModel.goalDraftObjective,
                "Ship Windows Companion")

        mouseClick(window.goalEditButton)
        compare(window.shellModel.goalEditing, true)
        window.goalEditor.text = "Verify complete Windows parity"
        mouseClick(window.goalSaveButton)
        compare(window.shellModel.goalUpdateRequestCount, 1)
        compare(window.shellModel.goalMutationPending, true)

        window.shellModel.goalObjective =
            "Verify complete Windows parity"
        window.shellModel.goalEditing = false
        window.shellModel.goalStatus = "paused"
        window.shellModel.goalMutationPending = false
        compare(window.goalPauseButton.visible, false)
        compare(window.goalResumeButton.visible, true)
        mouseClick(window.goalResumeButton)
        compare(window.shellModel.goalResumeRequestCount, 1)

        window.shellModel.goalMutationPending = false
        mouseClick(window.goalCloseButton)
        tryCompare(window.goalPopup, "visible", false)
        compare(window.height, 182)
        compare(window.shellModel.goalControlVisible, false)
    }

    function test_goal_popup_native_window_stays_bounded_for_long_objectives() {
        const window = createWindow()
        verify(window.goalWindow !== undefined)
        compare(window.goalWindow, null)
        const longObjective =
            "Design, build, and verify a native C++ and Qt Windows edition "
            + "of Codex Companion that preserves the complete published "
            + "macOS behavior while retaining Windows-native tray, mobile, "
            + "security, backdrop, packaging, and update behavior."

        window.shellModel.openGoalControls(
            "Codex Companion Windows",
            {
                "threadId": "thread-long-goal",
                "objective": longObjective,
                "status": "active",
                "elapsedSeconds": 125
            })

        tryCompare(window.goalPopup, "visible", true)
        tryVerify(function() {
            return window.goalWindow !== null
        })
        compare(
            window.goalWindow,
            window.goalPopup
                .contentItem.Window.window)
        compare(window.goalPopup.parent, window.contentItem)
        compare(window.goalPopup.width, 286)
        compare(window.goalPopup.implicitWidth, 286)
        compare(window.goalPopup.contentWidth, 262)
        verify(window.goalPopup.contentItem.Window.window !== null)
        tryCompare(
            window.goalPopup.contentItem.Window.window,
            "width",
            286)
        verify(window.goalPopup.contentItem.Window.window.height <= 420)
        verify(window.goalObjectiveLabel.width
               <= window.goalPopup.availableWidth)
    }

    function test_chat_model_picker_is_a_separate_stable_popover() {
        const window = createWindow()
        window.shellModel.showLocalChat()

        verify(window.modelPickerWindow !== undefined)
        compare(window.modelPickerWindow, null)
        tryCompare(window, "height", 94)

        window.chatModelPopup.open()
        tryCompare(window, "modelPickerOpen", true)
        tryVerify(function() {
            return window.modelPickerWindow !== null
        })
        compare(
            window.modelPickerWindow,
            window.chatModelPopup
                .contentItem.Window.window)
        tryCompare(window, "height", 94)
        compare(
            window.chatModelPopup.parent,
            window.contentItem)
        compare(window.chatModelPopup.width, 238)
        compare(window.chatModelPopup.implicitWidth, 238)
        compare(window.chatModelPopup.contentWidth, 218)
        verify(window.chatModelPopup.height > 300)
        verify(window.chatModelPopup.height <= 420)
        verify(
            window.chatModelPopup
                .contentItem.Window.window !== null)
        tryCompare(
            window.chatModelPopup
                .contentItem.Window.window,
            "width",
            238)
        verify(
            window.chatModelPopup
                .contentItem.Window.window.height <= 420)

        window.shellModel.chatResponse = "Retained response"
        tryCompare(window, "height", 420)

        window.chatModelPopup.close()
        tryCompare(window, "modelPickerOpen", false)
        tryCompare(window, "height", 420)

        window.shellModel.chatResponse = ""
        tryCompare(window, "height", 94)

        for (let index = 0; index < 4; ++index) {
            window.chatModelPopup.open()
            tryCompare(window, "modelPickerOpen", true)
            compare(window.height, 94)
            compare(window.chatModelPopup.width, 238)
            verify(window.chatModelPopup.height <= 420)
            tryCompare(
                window.chatModelPopup
                    .contentItem.Window.window,
                "width",
                238)
            verify(
                window.chatModelPopup
                    .contentItem.Window.window.height <= 420)
            window.chatModelPopup.close()
            tryCompare(window, "modelPickerOpen", false)
            compare(window.height, 94)
        }
    }

    function test_active_goal_badge_uses_authoritative_updated_at() {
        const window = createWindow()
        window.goalClockMs = 1700000005000

        compare(window.goalBadgeText({
            "status": "active",
            "elapsedSeconds": 125,
            "updatedAt": 1700000000000
        }), "Goal 2m 10s")
        compare(window.goalBadgeText({
            "status": "active",
            "elapsedSeconds": 125,
            "updatedAt": 1700000000
        }), "Goal 2m 10s")
    }

    function test_local_chat_send_button_submits_and_clears_prompt() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.chatSendEnabled = true
        window.shellModel.chatBusy = false
        window.promptInput.text = "  Verify local chat routing  "
        tryCompare(window.sendButton, "enabled", true)
        compare(
            window.sendButton.interactionId,
            "composer.send")

        mouseClick(window.sendButton)

        compare(window.shellModel.chatMessageRequestCount, 1)
        compare(window.shellModel.lastChatPrompt,
                "Verify local chat routing")
        compare(window.shellModel.lastChatModel, "on-device")
        compare(window.promptInput.text, "")
    }

    function test_local_chat_return_submits_and_clears_prompt() {
        const window = createWindow()
        window.shellModel.showLocalChat()
        window.shellModel.chatSendEnabled = true
        window.shellModel.chatBusy = false
        window.promptInput.text = "Send with Return"
        window.requestActivate()
        tryCompare(window, "active", true)
        window.promptInput.forceActiveFocus()
        tryCompare(window.promptInput, "activeFocus", true)

        keyClick(Qt.Key_Return)

        compare(window.shellModel.chatMessageRequestCount, 1)
        compare(window.shellModel.lastChatPrompt, "Send with Return")
        compare(window.promptInput.text, "")
    }

    function test_usage_window_refreshes_and_closes_as_a_reusable_surface() {
        const window = createUsageWindow()
        compare(window.width, 292)
        compare(window.height, 396)
        compare(window.accountSelector.currentValue, "main-profile")
        compare(window.accountSelector.count, 2)
        compare(window.usageScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.usageScrollBar.visible, false)
        tryCompare(window.groupRepeater, "count", 1)
        compare(window.shellModel.usageRefreshRequestCount, 1)

        window.accountSelector.currentIndex = 1
        window.accountSelector.activated(1)
        compare(
            window.settingsModel.selectedCodexAccountProfileId,
            "backup-profile")
        compare(window.shellModel.usageRefreshRequestCount, 2)
        tryCompare(window.resetCreditRepeater, "count", 1)
        const resetButton =
            window.resetCreditRepeater.itemAt(0)
        verify(resetButton)
        compare(
            resetButton.interactionId,
            "usage.reset.select")
        compare(
            window.resetCancelButton.interactionId,
            "usage.reset.cancel")
        compare(
            window.resetApplyButton.interactionId,
            "usage.reset.apply")
        compare(
            window.refreshButton.interactionId,
            "usage.refresh")
        compare(
            window.closeButton.interactionId,
            "usage.close")
        compare(
            resetButton.Accessible.name,
            "Review Weekly Codex reset")
        const resetCenter = resetButton.mapToItem(
            window.contentItem,
            resetButton.width / 2,
            resetButton.height / 2)
        verify(resetCenter.x >= 0)
        verify(resetCenter.x < window.width)
        verify(resetCenter.y >= 0)
        verify(resetCenter.y < window.height)

        mouseClick(resetButton)
        tryCompare(
            window.shellModel.usageResetConfirmation,
            "creditId",
            "credit-weekly")
        tryCompare(
            window.resetConfirmation,
            "visible",
            true)
        compare(
            window.resetConfirmationTitle.text,
            "Apply usage reset?")
        compare(window.resetApplyButton.enabled, true)

        mouseClick(window.resetCancelButton)
        tryCompare(
            window.resetConfirmation,
            "visible",
            false)

        mouseClick(resetButton)
        mouseClick(window.resetApplyButton)
        compare(
            window.shellModel.usageResetRequestCount,
            1)
        compare(
            window.shellModel.usageResetBusy,
            true)
        compare(
            window.resetStatusLabel.text,
            "Applying Weekly Codex reset...")
        compare(window.resetStatusLabel.visible, true)

        window.shellModel.usageLoading = false
        mouseClick(window.refreshButton)
        compare(window.shellModel.usageRefreshRequestCount, 3)

        const spy = signalSpy.createObject(window, {
            target: window,
            signalName: "closeRequested"
        })
        mouseClick(window.closeButton)
        compare(spy.count, 1)
    }
}
