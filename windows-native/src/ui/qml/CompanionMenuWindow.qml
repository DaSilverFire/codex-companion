import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import CodexCompanion

Window {
    id: root

    property var shellModel: null
    property var attentionModel: null
    property var settingsModel: null
    property var backdropState: null
    property var placementController: null
    readonly property string routeMode: shellModel === null
        || shellModel === undefined
        ? "processes"
        : shellModel.routeMode
    readonly property string effectiveBackdropMode: {
        if (backdropState !== null
                && backdropState !== undefined
                && backdropState.companionMenuEffectiveMode
                    !== undefined) {
            return backdropState.companionMenuEffectiveMode
        }
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.effectiveBackdropMode === undefined) {
            return "mica"
        }
        return settingsModel.effectiveBackdropMode
    }
    readonly property string modelPickerEffectiveBackdropMode:
        backdropState !== null
        && backdropState !== undefined
        && backdropState.modelPickerEffectiveMode !== undefined
            ? backdropState.modelPickerEffectiveMode
            : effectiveBackdropMode
    readonly property string goalEffectiveBackdropMode:
        backdropState !== null
        && backdropState !== undefined
        && backdropState.goalEffectiveMode !== undefined
            ? backdropState.goalEffectiveMode
            : effectiveBackdropMode
    readonly property bool enhancedBackdropActive:
        effectiveBackdropMode !== "solid-black"
    readonly property bool nativeBackdropRegionEnabled: true
    readonly property real nativeBackdropRegionInsetLeft: 4
    readonly property real nativeBackdropRegionInsetTop:
        routeMode === "processes"
            ? 14
            : showsChatResponsePanel
                ? 22
                : 14
    readonly property real nativeBackdropRegionInsetRight: 4
    readonly property real nativeBackdropRegionInsetBottom: 14
    readonly property real nativeBackdropRegionRadius: 28
    property bool processFeedEmpty: true
    property string hoveredProcessId: ""
    property string pendingProcessHoverExitId: ""
    readonly property int processHoverExitGraceDuration: 96
    readonly property int processHoverExpansionDuration: 160
    property rect latchedProcessCardWindowBounds:
        Qt.rect(0, 0, 0, 0)
    property rect finalProcessCardWindowBounds:
        Qt.rect(0, 0, 0, 0)
    property int processTargetLayoutRevision: 0
    readonly property var latestAttentionHighlight:
        attentionModel === null
        || attentionModel === undefined
        || attentionModel.latestAttentionHighlight === undefined
            ? ({})
            : attentionModel.latestAttentionHighlight
    readonly property string latestAttentionProcessId:
        latestAttentionHighlight.processId === undefined
            ? ""
            : String(latestAttentionHighlight.processId)
    readonly property string latestAttentionKind:
        latestAttentionHighlight.kind === undefined
            ? ""
            : String(latestAttentionHighlight.kind)
    readonly property string requestedProcessScrollId:
        shellModel !== null
        && shellModel !== undefined
        && shellModel.processTargetActive
            ? shellModel.processTargetId
            : latestAttentionProcessId
    readonly property real processViewportMaximumHeight:
        processList.count > 3 ? 230 : 372
    readonly property real processTargetListHeight: {
        const layoutRevision =
            processTargetLayoutRevision
        if (processFeedEmpty) {
            return 0
        }
        if (processList.count > 3) {
            return 230
        }

        let total = 2
        for (let index = 0;
                index < processList.count;
                ++index) {
            const card = processList.itemAtIndex(index)
            if (index > 0) {
                total += processList.spacing
            }
            total += card === null
                    || card === undefined
                ? 58
                : card.targetCardHeight
        }
        return Math.max(
            64,
            Math.min(
                processViewportMaximumHeight,
                Math.ceil(total)))
    }
    readonly property real processNaturalListHeight: {
        if (processFeedEmpty) {
            return shellModel !== null
                    && shellModel !== undefined
                    && shellModel.processLoading
                ? 46
                : 64
        }
        return processTargetListHeight > 0
            ? processTargetListHeight
            : 64
    }
    readonly property real processViewportHeight:
        Math.min(
            processViewportMaximumHeight,
            processNaturalListHeight)
    readonly property bool processListNeedsScrolling:
        !processFeedEmpty
        && processList.contentHeight > processViewportHeight
    readonly property real processScrollableBottomInset:
        processListNeedsScrolling
            ? nativeBackdropRegionInsetBottom
            : 0
    readonly property real processEdgeFadeHeight: 18
    readonly property bool goalControlsOpen:
        shellModel !== null
        && shellModel !== undefined
        && shellModel.goalControlVisible
    readonly property bool modelPickerOpen:
        chatModelPopup.visible
    property var modelPickerWindow: null
    readonly property bool hasChatResponse:
        shellModel !== null
        && shellModel !== undefined
        && shellModel.chatResponse.length > 0
    readonly property bool showsChatResponsePanel:
        hasChatResponse
        || (shellModel !== null
            && shellModel !== undefined
            && shellModel.chatBusy)
    readonly property int targetHeight:
        routeMode === "processes"
            ? Math.min(
                416,
                Math.round(processViewportHeight + 44))
            : showsChatResponsePanel
                ? 420
                : 94
    readonly property int minimumTargetHeight:
        routeMode === "processes" ? 90 : 94
    property real goalPopupAnchorCenterX: width / 2
    property real goalPopupAnchorTopY: 78
    property real goalPopupAnchorHeight: 13
    property bool goalPopupAnchored: false
    property var goalWindow: null

    function synchronizeWindowHeight() {
        const nextHeight = Math.max(
            minimumTargetHeight,
            Math.min(420, targetHeight))
        if (nextHeight >= height) {
            maximumHeight = nextHeight
            minimumHeight = nextHeight
        } else {
            minimumHeight = nextHeight
            maximumHeight = nextHeight
        }
        height = nextHeight
    }

    property alias closeButton: closeButton
    property alias chatRouteButton: chatRouteButton
    property alias codexRouteButton: codexRouteButton
    property alias processHeader: processHeader
    property alias routeFooter: routeFooter
    property alias menuHostSurface: materialSurface
    property alias chatModelPopup: chatModelPopup
    property alias chatModelOptions: chatModelOptions
    property alias processList: processList
    property alias processScrollBar: processScrollBar
    property alias processSurface: processSurface
    property alias processViewport: processViewport
    property var processEdgeMask: null
    property alias processNoticeCard: processNoticeCard
    property alias processNoticeTitle: processNoticeTitle
    property alias processNoticeSubtitle: processNoticeSubtitle
    property alias composerSurface: composerSurface
    property alias promptInput: promptInput
    property alias sendButton: sendButton
    property alias prepareChatButton: prepareChatButton
    property alias chatResponseTitleLabel:
        chatResponseTitleLabel
    property alias chatResponsePromptLabel:
        chatResponsePromptLabel
    property alias chatResponseLabel: chatResponseLabel
    property alias chatResponseUsageLabel:
        chatResponseUsageLabel
    property alias chatResponseDismissButton:
        chatResponseDismissButton
    property alias goalPopup: goalPopup
    property alias goalObjectiveLabel: goalObjectiveLabel
    property alias goalStatusIcon: goalStatusIcon
    property alias goalStatusLabel: goalStatusLabel
    property alias goalEditor: goalEditor
    property alias goalCloseButton: goalCloseButton
    property alias goalPauseButton: goalPauseButton
    property alias goalResumeButton: goalResumeButton
    property alias goalEditButton: goalEditButton
    property alias goalCancelButton: goalCancelButton
    property alias goalSaveButton: goalSaveButton
    property alias goalConfettiOverlay:
        goalConfettiOverlay

    signal closeRequested()

    onRequestedProcessScrollIdChanged:
        processScrollTimer.restart()
    onLatestAttentionProcessIdChanged:
        processScrollTimer.restart()
    onProcessTargetLayoutRevisionChanged:
        processHoverBoundsRefreshTimer.restart()
    onXChanged:
        scheduleGoalPopupPosition()
    onYChanged:
        scheduleGoalPopupPosition()
    onGoalPopupAnchorCenterXChanged:
        scheduleGoalPopupPosition()
    onGoalPopupAnchorTopYChanged:
        scheduleGoalPopupPosition()
    onGoalPopupAnchorHeightChanged:
        scheduleGoalPopupPosition()
    onVisibleChanged: {
        if (visible) {
            processScrollTimer.restart()
        } else {
            clearProcessHover()
        }
    }

    function normalizedProcessId(processId) {
        return processId === null
                || processId === undefined
            ? ""
            : String(processId)
    }

    function processCardForId(processId) {
        const normalized =
            normalizedProcessId(processId)
        if (normalized.length === 0) {
            return null
        }
        processList.forceLayout()
        for (let index = 0;
                index < processList.count;
                ++index) {
            const card =
                processList.itemAtIndex(index)
            if (card !== null
                    && card !== undefined
                    && card.processId === normalized) {
                return card
            }
        }
        return null
    }

    function processCardWindowBounds(
        processId,
        useTargetHeight) {
        const card = processCardForId(processId)
        if (card === null
                || card === undefined
                || !card.visible) {
            return Qt.rect(0, 0, 0, 0)
        }
        const origin = card.mapToItem(
            root.contentItem,
            0,
            0)
        const targetHeight =
            useTargetHeight
                && card.targetCardHeight !== undefined
            ? Math.max(
                card.height,
                card.targetCardHeight)
            : card.height
        return Qt.rect(
            origin.x,
            origin.y,
            card.width,
            targetHeight)
    }

    function validProcessCardBounds(bounds) {
        return bounds.width > 0
            && bounds.height > 0
    }

    function processCardBoundsContains(
        bounds,
        windowX,
        windowY) {
        return validProcessCardBounds(bounds)
            && windowX >= bounds.x
            && windowY >= bounds.y
            && windowX < bounds.x + bounds.width
            && windowY < bounds.y + bounds.height
    }

    function hoveredProcessCardWindowBounds() {
        return validProcessCardBounds(
                    finalProcessCardWindowBounds)
            ? finalProcessCardWindowBounds
            : latchedProcessCardWindowBounds
    }

    function refreshHoveredProcessCardBounds() {
        if (hoveredProcessId.length === 0) {
            return
        }
        const bounds = processCardWindowBounds(
            hoveredProcessId,
            true)
        if (validProcessCardBounds(bounds)) {
            finalProcessCardWindowBounds = bounds
        }
    }

    function beginProcessHover(processId) {
        const nextProcessId =
            normalizedProcessId(processId)
        if (nextProcessId.length === 0) {
            clearProcessHover()
            return
        }
        processHoverExitTimer.stop()
        pendingProcessHoverExitId = ""
        if (hoveredProcessId === nextProcessId) {
            refreshHoveredProcessCardBounds()
            return
        }
        latchedProcessCardWindowBounds =
            processCardWindowBounds(
                nextProcessId,
                false)
        hoveredProcessId = nextProcessId
        finalProcessCardWindowBounds =
            processCardWindowBounds(
                nextProcessId,
                true)
        processHoverExpansionTimer.restart()
        processHoverBoundsRefreshTimer.restart()
    }

    function endProcessHover(processId) {
        const leavingProcessId =
            normalizedProcessId(processId)
        if (leavingProcessId.length === 0
                || hoveredProcessId
                    !== leavingProcessId) {
            return
        }
        pendingProcessHoverExitId =
            leavingProcessId
        processHoverExitTimer.restart()
    }

    function clearProcessHover() {
        processHoverExitTimer.stop()
        processHoverExpansionTimer.stop()
        processHoverBoundsRefreshTimer.stop()
        pendingProcessHoverExitId = ""
        hoveredProcessId = ""
        latchedProcessCardWindowBounds =
            Qt.rect(0, 0, 0, 0)
        finalProcessCardWindowBounds =
            Qt.rect(0, 0, 0, 0)
    }

    function reconcileProcessHoverAt(
        windowX,
        windowY) {
        if (!visible
                || routeMode !== "processes") {
            clearProcessHover()
            return
        }

        if (hoveredProcessId.length > 0
                && (processCardBoundsContains(
                        finalProcessCardWindowBounds,
                        windowX,
                        windowY)
                    || (processHoverExpansionTimer.running
                        && processCardBoundsContains(
                            latchedProcessCardWindowBounds,
                            windowX,
                            windowY)))) {
            beginProcessHover(hoveredProcessId)
            return
        }

        const surfacePoint =
            processSurface.mapFromItem(
                root.contentItem,
                windowX,
                windowY)
        if (surfacePoint.x < 0
                || surfacePoint.y < 0
                || surfacePoint.x
                    >= processSurface.width
                || surfacePoint.y
                    >= processSurface.height) {
            endProcessHover(hoveredProcessId)
            return
        }

        processList.forceLayout()
        for (let index = 0;
                index < processList.count;
                ++index) {
            const card =
                processList.itemAtIndex(index)
            if (card === null
                    || card === undefined
                    || !card.visible) {
                continue
            }
            const cardPoint =
                card.mapFromItem(
                    root.contentItem,
                    windowX,
                    windowY)
            if (cardPoint.x >= 0
                    && cardPoint.y >= 0
                    && cardPoint.x < card.width
                    && cardPoint.y < card.height) {
                beginProcessHover(
                    card.processId)
                return
            }
        }

        endProcessHover(hoveredProcessId)
    }

    function relevantProcessRow() {
        processList.forceLayout()
        if (shellModel !== null
                && shellModel !== undefined
                && shellModel.processTargetActive) {
            for (let index = 0;
                    index < processList.count;
                    ++index) {
                const card =
                    processList.itemAtIndex(index)
                if (card !== null
                        && card.processId
                            === shellModel.processTargetId) {
                    return index
                }
            }
        }
        for (let index = 0;
                index < processList.count;
                ++index) {
            const card =
                processList.itemAtIndex(index)
            if (card !== null
                    && card.kind !== "notice"
                    && card.status === "waiting") {
                return index
            }
        }
        if (latestAttentionProcessId.length > 0) {
            for (let index = 0;
                    index < processList.count;
                    ++index) {
                const card =
                    processList.itemAtIndex(index)
                if (card !== null
                        && card.processId
                            === latestAttentionProcessId) {
                    return index
                }
            }
        }
        return -1
    }

    function scrollToRelevantProcess() {
        if (routeMode !== "processes"
                || processList.count === 0) {
            return
        }
        const row = relevantProcessRow()
        if (row >= 0) {
            processList.positionViewAtIndex(
                row,
                ListView.Center)
            processList.forceLayout()
        }
    }

    Timer {
        id: processHoverExitTimer
        interval:
            Math.max(
                root.processHoverExitGraceDuration,
                root.processHoverExpansionDuration)
        repeat: false
        onTriggered: {
            if (root.hoveredProcessId
                    === root
                        .pendingProcessHoverExitId) {
                root.hoveredProcessId = ""
            }
            root.pendingProcessHoverExitId = ""
        }
    }

    Timer {
        id: processHoverExpansionTimer
        interval:
            root.processHoverExpansionDuration
        repeat: false
        onTriggered: {
            root.refreshHoveredProcessCardBounds()
            root.latchedProcessCardWindowBounds =
                root.finalProcessCardWindowBounds
        }
    }

    Timer {
        id: processHoverBoundsRefreshTimer
        interval: 0
        repeat: false
        onTriggered:
            root.refreshHoveredProcessCardBounds()
    }

    Timer {
        id: processScrollTimer
        interval: 0
        repeat: false
        onTriggered:
            root.scrollToRelevantProcess()
    }

    function goalStatusTitle(status) {
        switch (status) {
        case "active":
            return "Goal"
        case "paused":
            return "Paused"
        case "blocked":
            return "Blocked"
        case "usageLimited":
            return "Usage"
        case "budgetLimited":
            return "Budget"
        case "complete":
            return "Reached"
        default:
            return "Goal"
        }
    }

    function openModelPicker() {
        if (shellModel === null
                || shellModel === undefined) {
            return
        }
        if (routeMode !== "local-chat") {
            shellModel.showLocalChat()
        }
        Qt.callLater(function() {
            if (root.routeMode !== "local-chat") {
                return
            }
            chatModelPopup.open()
        })
    }

    function selectedChatModelTitle() {
        if (shellModel === null
                || shellModel === undefined) {
            return "Companion"
        }
        for (let index = 0;
                index < shellModel.chatModels.length;
                ++index) {
            const model = shellModel.chatModels[index]
            if (model.id === shellModel.selectedChatModelId) {
                return model.title
            }
        }
        return "Companion"
    }

    function submitLocalChatPrompt() {
        if (!sendButton.enabled
                || shellModel === null
                || shellModel === undefined) {
            return
        }
        shellModel.submitLocalChat(promptInput.text)
        promptInput.text = ""
    }

    function goalStatusDisplayTitle(status) {
        switch (status) {
        case "active":
            return "Goal active"
        case "paused":
            return "Goal paused"
        case "blocked":
            return "Goal blocked"
        case "usageLimited":
            return "Waiting for usage"
        case "budgetLimited":
            return "Goal budget reached"
        case "complete":
            return "Goal complete"
        default:
            return "Goal"
        }
    }

    function attentionAccent(kind) {
        switch (kind) {
        case "response":
            return CompanionTheme.info
        case "attention":
            return CompanionTheme.warning
        case "completion":
            return CompanionTheme.success
        case "goal":
            return CompanionTheme.goal
        case "failure":
            return CompanionTheme.danger
        default:
            return "transparent"
        }
    }

    function blendColor(base, tint, amount) {
        const weight = Math.max(0, Math.min(1, amount))
        return Qt.rgba(
            base.r * (1 - weight) + tint.r * weight,
            base.g * (1 - weight) + tint.g * weight,
            base.b * (1 - weight) + tint.b * weight,
            1)
    }

    function colorWithAlpha(color, alpha) {
        return Qt.rgba(
            color.r,
            color.g,
            color.b,
            Math.max(0, Math.min(1, alpha)))
    }

    function goalStatusColor(status) {
        switch (status) {
        case "active":
            return CompanionTheme.info
        case "paused":
        case "blocked":
        case "usageLimited":
        case "budgetLimited":
            return CompanionTheme.warning
        case "complete":
            return CompanionTheme.success
        default:
            return CompanionTheme.textMuted
        }
    }

    function goalStatusGlyph(status) {
        switch (status) {
        case "active":
            return "\uf272"
        case "paused":
            return "\ue769"
        case "blocked":
            return "\ue7ba"
        case "usageLimited":
            return "\ue917"
        case "budgetLimited":
            return "\uec4a"
        case "complete":
            return "\uec61"
        default:
            return "\ue946"
        }
    }

    function goalPopupOriginY(anchorTop, anchorHeight,
                              popupHeight, windowTop,
                              availableTop, availableBottom) {
        const gap = 7
        const margin = 6
        const above = anchorTop - popupHeight - gap
        if (windowTop + above >= availableTop + margin) {
            return Math.round(above)
        }

        const below = anchorTop + anchorHeight + gap
        if (windowTop + below + popupHeight
                <= availableBottom - margin) {
            return Math.round(below)
        }

        const clampedGlobal = Math.max(
            availableTop + margin,
            Math.min(
                windowTop + above,
                availableBottom - popupHeight - margin))
        return Math.round(clampedGlobal - windowTop)
    }

    function rememberGoalPopupAnchor(item) {
        const point = item.mapToItem(
            root.contentItem,
            item.width / 2,
            0)
        goalPopupAnchorCenterX = point.x
        goalPopupAnchorTopY = point.y
        goalPopupAnchorHeight = item.height
        goalPopupAnchored = true
    }

    function goalPopupTargetOriginX() {
        return Math.round(Math.max(
            3,
            Math.min(
                root.width - goalPopup.width - 3,
                root.goalPopupAnchorCenterX
                    - goalPopup.width / 2)))
    }

    function goalPopupAvailableWorkArea() {
        if (root.placementController === null
                || root.placementController === undefined) {
            return null
        }
        const geometry =
            root.placementController.availableWorkAreaAt(
                Qt.point(
                    root.x + root.width / 2,
                    root.y + root.height / 2))
        if (geometry === null
                || geometry === undefined
                || !Number.isFinite(geometry.y)
                || !Number.isFinite(geometry.height)) {
            return null
        }
        return geometry
    }

    function goalPopupTargetOriginY() {
        if (!root.goalPopupAnchored) {
            return 78
        }
        const geometry =
            root.goalPopupAvailableWorkArea()
        if (geometry === null) {
            return 78
        }
        return root.goalPopupOriginY(
            root.goalPopupAnchorTopY,
            root.goalPopupAnchorHeight,
            goalPopup.height,
            root.y,
            geometry.y,
            geometry.y + geometry.height)
    }

    function scheduleGoalPopupPosition() {
        if (goalPopup.visible) {
            goalPopupPositionTimer.restart()
        }
    }

    function positionGoalPopupWindow() {
        if (!goalPopup.visible) {
            return
        }
        const popupWindow =
            goalPopup.contentItem.Window.window
        if (popupWindow === null
                || popupWindow === undefined) {
            return
        }
        popupWindow.x =
            root.x + root.goalPopupTargetOriginX()
        popupWindow.y =
            root.y + root.goalPopupTargetOriginY()
    }

    function goalDuration(seconds) {
        const total = Math.max(0, Number(seconds) || 0)
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const remainingSeconds = Math.floor(total % 60)
        if (hours > 0) {
            return minutes === 0
                ? hours + "h"
                : hours + "h " + minutes + "m"
        }
        if (minutes > 0) {
            return minutes + "m " + remainingSeconds + "s"
        }
        return Math.floor(total) + "s"
    }

    function unixTimestampMilliseconds(value) {
        const timestamp = Number(value)
        if (!Number.isFinite(timestamp)
                || timestamp <= 0) {
            return NaN
        }
        return timestamp < 100000000000
            ? timestamp * 1000
            : timestamp
    }

    function goalBadgeText(goal, anchorMs) {
        if (goal === null || goal === undefined) {
            return ""
        }
        let elapsedSeconds = Number(goal.elapsedSeconds) || 0
        if (goal.status === "active") {
            const updatedAtMs =
                root.unixTimestampMilliseconds(
                    goal.updatedAt)
            const fallbackAnchorMs = Number(anchorMs)
            const referenceMs = Number.isFinite(updatedAtMs)
                    && updatedAtMs > 0
                ? updatedAtMs
                : (Number.isFinite(fallbackAnchorMs)
                    ? fallbackAnchorMs
                    : goalClockMs)
            elapsedSeconds += Math.max(
                0,
                Math.floor((goalClockMs - referenceMs) / 1000))
        }
        return goalStatusTitle(goal.status)
            + " " + goalDuration(elapsedSeconds)
    }

    function processActionTitle(action) {
        switch (action) {
        case "steer":
            return "Steer"
        case "approval-feedback":
            return "Tell Codex"
        default:
            return "Reply"
        }
    }

    function processStatusHelp(status) {
        switch (status) {
        case "running":
            return "Still working"
        case "completed":
            return "Completed"
        case "failed":
            return "Failed or disconnected"
        default:
            return "Needs your attention"
        }
    }

    function processDisplayTitle(value) {
        return String(value)
            .split("-")
            .filter(function(part) {
                return part.length > 0
            })
            .map(function(part) {
                return part.charAt(0).toUpperCase()
                    + part.slice(1)
            })
            .join(" ")
    }

    function processRelativeSubtitle(updatedAt, fallback) {
        const referenceDateUnixSeconds = 978307200
        const referenceSeconds = Number(updatedAt)
        if (!Number.isFinite(referenceSeconds)
                || referenceSeconds <= 0) {
            return fallback.length > 0
                ? fallback
                : "Recent thread"
        }

        const updatedAtMs =
            (referenceSeconds + referenceDateUnixSeconds) * 1000
        const elapsedSeconds = Math.max(
            0,
            Math.floor((Date.now() - updatedAtMs) / 1000))
        if (elapsedSeconds < 60) {
            return "Updated just now"
        }
        if (elapsedSeconds < 3600) {
            return "Updated "
                + Math.floor(elapsedSeconds / 60)
                + "m ago"
        }
        if (elapsedSeconds < 86400) {
            return "Updated "
                + Math.floor(elapsedSeconds / 3600)
                + "h ago"
        }
        return "Updated "
            + Math.floor(elapsedSeconds / 86400)
            + "d ago"
    }

    function processStatusSubtitle(
            kind,
            status,
            needsApproval,
            goal,
            updatedAt,
            cwd,
            sourceStatus) {
        if (kind === "job") {
            const rawStatus =
                sourceStatus === null
                    || sourceStatus === undefined
                ? ""
                : String(sourceStatus).trim()
            let label = rawStatus.length > 0
                ? root.processDisplayTitle(rawStatus)
                : "Waiting"
            if (rawStatus.length === 0) {
                switch (status) {
                case "running":
                    label = "Running"
                    break
                case "completed":
                    label = "Completed"
                    break
                case "failed":
                    label = "Failed"
                    break
                }
            }
            return label + " - "
                + root.processRelativeSubtitle(
                    updatedAt,
                    "recently")
        }

        if (needsApproval) {
            return "Needs your approval"
        }

        if (goal !== null
                && goal !== undefined
                && goal.status !== undefined) {
            let elapsedSeconds =
                Number(goal.elapsedSeconds) || 0
            if (goal.status === "active") {
                const updatedAtMs =
                    root.unixTimestampMilliseconds(
                        goal.updatedAt)
                const referenceMs =
                    Number.isFinite(updatedAtMs)
                        && updatedAtMs > 0
                    ? updatedAtMs
                    : root.goalClockMs
                elapsedSeconds += Math.max(
                    0,
                    Math.floor(
                        (root.goalClockMs - referenceMs)
                        / 1000))
            }
            const duration =
                root.goalDuration(elapsedSeconds)
            switch (goal.status) {
            case "active":
                return "Goal running " + duration
            case "paused":
                return "Goal paused at " + duration
            case "blocked":
                return "Goal blocked at " + duration
            case "usageLimited":
                return "Goal usage limited at " + duration
            case "budgetLimited":
                return "Goal budget limited at " + duration
            case "complete":
                return "Goal reached in " + duration
            }
        }

        if (status === "running") {
            return "Working now"
        }
        if (status === "waiting") {
            return "Needs your answer"
        }
        return root.processRelativeSubtitle(
            updatedAt,
            cwd === null || cwd === undefined
                ? ""
                : String(cwd))
    }

    component ProcessActionButton: Button {
        id: processAction

        required property string interactionId
        property string glyph: ""
        property bool compact: false
        property real horizontalScale:
            processAction.hovered ? 1.014 : 1
        property real verticalScale:
            processAction.hovered ? 0.992 : 1

        implicitWidth: Math.ceil(contentItem.implicitWidth
                                 + leftPadding + rightPadding)
        implicitHeight: 24
        hoverEnabled: true
        leftPadding: compact ? 7 : 9
        rightPadding: compact ? 7 : 9
        topPadding: 0
        bottomPadding: 0
        opacity: enabled ? 1 : 0.55

        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }

        Behavior on horizontalScale {
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }

        Behavior on verticalScale {
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }

        contentItem: RowLayout {
            id: processActionContent

            spacing: processAction.compact ? 4 : 6
            clip: true

            transform: Scale {
                origin.x:
                    processActionContent.width / 2
                origin.y:
                    processActionContent.height / 2
                xScale:
                    processAction.horizontalScale
                yScale:
                    processAction.verticalScale
            }

            Label {
                text: processAction.glyph
                color: processAction.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.family: "Segoe Fluent Icons"
                font.pixelSize: 9
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            Label {
                text: processAction.text
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                color: processAction.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: processAction.compact ? 9 : 10
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        background: Rectangle {
            id: processActionBackground

            radius: 12
            color: processAction.down
                ? CompanionTheme.controlPressed
                : processAction.hovered
                    ? CompanionTheme.controlSelected
                    : CompanionTheme.control
            border.width: processAction.activeFocus ? 1 : 0
            border.color: CompanionTheme.accent

            transform: Scale {
                origin.x:
                    processActionBackground.width / 2
                origin.y:
                    processActionBackground.height / 2
                xScale:
                    processAction.horizontalScale
                yScale:
                    processAction.verticalScale
            }
        }
    }

    component GoalActionButton: Button {
        id: goalAction

        required property string interactionId
        property bool emphasized: false
        property string glyph: ""

        implicitHeight: 28
        leftPadding: 11
        rightPadding: 11

        contentItem: RowLayout {
            spacing: 5

            Label {
                text: goalAction.glyph
                color: goalAction.enabled
                    ? goalAction.emphasized
                        ? CompanionTheme.accentText
                        : CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.family: "Segoe Fluent Icons"
                font.pixelSize: 10
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            Label {
                text: goalAction.text
                color: goalAction.enabled
                    ? goalAction.emphasized
                        ? CompanionTheme.accentText
                        : CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        background: Rectangle {
            radius: 14
            color: goalAction.emphasized
                ? goalAction.down
                    ? CompanionTheme.accentPressed
                    : CompanionTheme.accent
                : goalAction.down
                    ? CompanionTheme.controlPressed
                    : goalAction.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
            border.width: goalAction.emphasized ? 0 : 1
            border.color: CompanionTheme.border
        }
    }

    objectName: "companionMenuWindow"
    visible: false
    width: 292
    height: 94
    minimumWidth: 292
    maximumWidth: 292
    minimumHeight: 94
    maximumHeight: 94
    title: routeMode === "local-chat" ? "Local Chat" : "Codex Processes"
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    property double goalClockMs: Date.now()

    Component.onCompleted: synchronizeWindowHeight()
    onTargetHeightChanged: synchronizeWindowHeight()

    Timer {
        interval: 1000
        repeat: true
        running: root.visible
        onTriggered: root.goalClockMs = Date.now()
    }

    Timer {
        id: goalPopupPositionTimer
        interval: 0
        repeat: false
        onTriggered:
            root.positionGoalPopupWindow()
    }

    onClosing: function(close) {
        close.accepted = false
        closeRequested()
    }

    onRouteModeChanged: {
        if (routeMode !== "local-chat") {
            chatModelPopup.close()
        }
        if (routeMode !== "processes") {
            clearProcessHover()
        }
    }

    Rectangle {
        id: materialSurface
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin:
            root.nativeBackdropRegionInsetLeft
        anchors.rightMargin:
            root.nativeBackdropRegionInsetRight
        anchors.topMargin:
            root.nativeBackdropRegionInsetTop
        anchors.bottomMargin:
            root.nativeBackdropRegionInsetBottom
        radius: root.nativeBackdropRegionRadius
        color: root.routeMode === "local-chat"
            ? CompanionTheme.traySurfaceForBackdrop(
                root.effectiveBackdropMode)
            : "transparent"
        border.width: root.routeMode === "local-chat"
            ? 1
            : 0
        border.color:
            CompanionTheme.traySurfaceBorderForBackdrop(
                root.effectiveBackdropMode)

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.routeMode === "processes"
                ? 0
                : 10
            spacing: root.routeMode === "processes"
                ? 0
                : 8

            RowLayout {
                id: processHeader

                Layout.fillWidth: true
                Layout.preferredHeight: 0
                spacing: 8
                visible: false

                Label {
                    Layout.fillWidth: true
                    leftPadding: 6
                    text: root.title
                    color: CompanionTheme.textPrimary
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                ToolButton {
                    id: closeButton
                    property string interactionId:
                        "companion.close"
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    text: "\u00d7"
                    font.pixelSize: 18
                    font.weight: Font.Medium
                    padding: 0
                    Accessible.name: "Close Companion Menu"
                    ToolTip.text: "Close Companion Menu"
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    onClicked: root.closeRequested()

                    contentItem: Label {
                        text: closeButton.text
                        color: closeButton.hovered
                            ? CompanionTheme.textPrimary
                            : CompanionTheme.textSecondary
                        font: closeButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 13
                        color: closeButton.down
                            ? CompanionTheme.controlPressed
                            : closeButton.hovered
                                ? CompanionTheme.controlHover
                                : "transparent"
                        border.width: closeButton.activeFocus ? 1 : 0
                        border.color: CompanionTheme.accent
                    }
                }
            }

            StackLayout {
                id: routeStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.routeMode === "local-chat" ? 1 : 0

                Rectangle {
                    id: processSurface
                    radius: 28
                    color:
                        CompanionTheme.traySurfaceForBackdrop(
                            root.effectiveBackdropMode)
                    border.width: 1
                    border.color:
                        CompanionTheme
                            .traySurfaceBorderForBackdrop(
                                root.effectiveBackdropMode)

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6

                        Item {
                            id: processViewport

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            layer.enabled:
                                root.processListNeedsScrolling
                            layer.effect: MultiEffect {
                                autoPaddingEnabled: false
                                maskEnabled: true
                                maskThresholdMin: 0.5
                                maskSpreadAtMin: 1
                                maskThresholdMax: 1
                                maskSpreadAtMax: 0
                                maskSource: ShaderEffectSource {
                                    id: processEdgeMask

                                    width:
                                        processEdgeMaskGradient.width
                                    height:
                                        processEdgeMaskGradient.height
                                    sourceItem:
                                        processEdgeMaskGradient
                                    hideSource: true
                                    live: true
                                    Component.onCompleted:
                                        root.processEdgeMask = this
                                    Component.onDestruction: {
                                        if (root.processEdgeMask
                                                === this) {
                                            root.processEdgeMask = null
                                        }
                                    }
                                }
                            }

                            ListView {
                                id: processList
                                anchors.fill: parent
                                anchors.bottomMargin:
                                    root.processScrollableBottomInset
                                clip: true
                                spacing: 6
                                cacheBuffer: 2600
                                boundsBehavior: Flickable.StopAtBounds
                                model: root.shellModel === null
                                    || root.shellModel === undefined
                                    ? null
                                    : root.shellModel.processModel
                                onCountChanged: {
                                    root.processTargetLayoutRevision += 1
                                    processCountSyncTimer.restart()
                                    processScrollTimer.restart()
                                }
                                onContentYChanged:
                                    processHoverBoundsRefreshTimer.restart()
                                Component.onCompleted: {
                                    processCountSyncTimer.restart()
                                    processScrollTimer.restart()
                                }

                                Timer {
                                    id: processCountSyncTimer
                                    interval: 0
                                    repeat: false
                                    onTriggered:
                                        root.processFeedEmpty =
                                            processList.count === 0
                                }

                                ScrollBar.vertical: ScrollBar {
                                    id: processScrollBar
                                    policy: ScrollBar.AlwaysOff
                                }

                                delegate: Rectangle {
                                    id: processCard

                                    required property string processId
                                    required property string threadId
                                    required property string kind
                                    required property string title
                                    required property string preview
                                    required property string sourceStatus
                                    required property double updatedAt
                                    required property string status
                                    required property bool needsApproval
                                    required property string runtimeStatus
                                    required property string rolloutPath
                                    required property var cwd
                                    required property var activeTurnId
                                    required property var model
                                    required property var reasoningEffort
                                    required property var goal
                                    onStatusChanged:
                                        processScrollTimer.restart()
                                    property alias goalButton: goalButton
                                    property alias replyButton: replyButton
                                    property alias steerButton: steerButton
                                    property alias retryButton: retryButton
                                    property alias retryStatusLabel:
                                        retryStatusLabel
                                    property alias approveOnceButton:
                                        approveOnceButton
                                    property alias approveSimilarButton:
                                        approveSimilarButton
                                    property alias tellCodexButton:
                                        tellCodexButton
                                    property alias inlineComposer:
                                        inlineComposer
                                    property alias processPromptInput:
                                        processPromptInput
                                    property alias processSendButton:
                                        processSendButton
                                    property alias cancelProcessButton:
                                        cancelProcessButton
                                    property alias processStatusMessage:
                                        processStatusMessage
                                    property alias processFullMessage:
                                        processFullMessage
                                    property alias processStatusBadge:
                                        processStatusBadge
                                    property alias processSpinner:
                                        processSpinner
                                    property alias processSpinnerArc:
                                        processSpinnerArc
                                    property double goalAnchorMs: Date.now()
                                    readonly property bool targeted:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel.processTargetActive
                                        && root.shellModel.processTargetId
                                             === processCard.processId
                                    readonly property bool attentionHighlighted:
                                        root.latestAttentionProcessId
                                            .length > 0
                                        && root.latestAttentionProcessId
                                            === processCard.processId
                                    readonly property color processAccentColor: {
                                        if (processCard.status === "failed") {
                                            return CompanionTheme.danger
                                        }
                                        if (processCard.runtimeStatus
                                                === "waitingOnApproval") {
                                            return CompanionTheme.warning
                                        }
                                        return processCard.attentionHighlighted
                                            ? root.attentionAccent(
                                                root.latestAttentionKind)
                                            : "transparent"
                                    }
                                    readonly property bool hasProcessAccent:
                                        processAccentColor.a > 0
                                    readonly property bool usesSubduedResponseAccent:
                                        processCard.attentionHighlighted
                                        && root.latestAttentionKind
                                            === "response"
                                        && processCard.status !== "failed"
                                        && processCard.runtimeStatus
                                            !== "waitingOnApproval"
                                    readonly property real processAccentFillAmount:
                                        processCard.usesSubduedResponseAccent
                                        ? (processCard.isHovered
                                            ? 0.10
                                            : 0.07)
                                        : (processCard.isHovered
                                            ? 0.23
                                            : 0.16)
                                    readonly property real processAccentBorderAlpha:
                                        processCard.usesSubduedResponseAccent
                                        ? (processCard.isHovered
                                            ? 0.56
                                            : 0.40)
                                        : (processCard.isHovered
                                            ? 0.68
                                            : 0.48)
                                    readonly property bool canTargetThread:
                                        processCard.threadId.trim().length > 0
                                    readonly property bool offersApproval:
                                        processCard.canTargetThread
                                        && processCard.runtimeStatus
                                            === "waitingOnApproval"
                                        && processCard.status !== "failed"
                                    readonly property bool offersReply:
                                        processCard.canTargetThread
                                        && !processCard.offersApproval
                                        && (processCard.status === "running"
                                            || processCard.status === "completed"
                                            || processCard.status === "failed"
                                            || processCard.isRecoverableStopped)
                                    readonly property bool offersSteer:
                                        processCard.canTargetThread
                                        && !processCard.offersApproval
                                        && (processCard.status === "running"
                                            || processCard.status === "completed")
                                    readonly property string goalStatusValue:
                                        processCard.goal !== null
                                        && processCard.goal !== undefined
                                        && processCard.goal.status !== undefined
                                        ? processCard.goal.status
                                        : ""
                                    readonly property bool hasRecoverableGoal:
                                        processCard.goalStatusValue === "paused"
                                        || processCard.goalStatusValue === "blocked"
                                        || processCard.goalStatusValue
                                            === "usageLimited"
                                    readonly property bool runtimeMetadataUnavailable:
                                        processCard.runtimeStatus
                                            .trim().length === 0
                                    readonly property bool failedWithUnavailableRuntime:
                                        processCard.status === "failed"
                                        && processCard
                                            .runtimeMetadataUnavailable
                                    readonly property bool safelyStopped:
                                        processCard.runtimeStatus === "idle"
                                        || processCard.runtimeStatus
                                            === "notLoaded"
                                        || processCard.runtimeStatus
                                            === "systemError"
                                        || processCard
                                            .failedWithUnavailableRuntime
                                    readonly property bool isStopped:
                                        processCard.safelyStopped
                                    readonly property bool isRecoverableStopped:
                                        processCard.kind === "thread"
                                        && processCard.canTargetThread
                                        && processCard.safelyStopped
                                        && processCard.status !== "running"
                                        && (processCard.status === "failed"
                                            || processCard.hasRecoverableGoal)
                                        && processCard.goalStatusValue
                                            !== "complete"
                                        && processCard.goalStatusValue
                                            !== "budgetLimited"
                                    readonly property bool offersRetry:
                                        processCard.isRecoverableStopped
                                    readonly property bool processComposerActionsEnabled:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                    readonly property bool approvalPending:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel.approvingProcessId
                                            === processCard.processId
                                    readonly property bool approvalCommandBusy:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel.approvingProcessId
                                            .length > 0
                                    readonly property bool retryPending:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel.retryingProcessId
                                            === processCard.processId
                                    readonly property bool retryStatusVisible:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel.processRetryStatusId
                                            === processCard.processId
                                        && root.shellModel.processRetryStatus
                                            .length > 0
                                    readonly property bool isHovered:
                                        root.hoveredProcessId
                                            === processCard.processId
                                    readonly property bool showApprovalActions:
                                        processCard.isHovered
                                        && processCard.offersApproval
                                    readonly property bool showReply:
                                        !processCard.offersApproval
                                        && processCard.isHovered
                                        && processCard.offersReply
                                    readonly property bool showSteer:
                                        !processCard.offersApproval
                                        && processCard.isHovered
                                        && processCard.offersSteer
                                    readonly property bool showRetry:
                                        !processCard.offersApproval
                                        && processCard.isHovered
                                        && processCard.offersRetry
                                    readonly property bool showsAnyActions:
                                        showApprovalActions
                                        || showReply
                                        || showSteer
                                        || showRetry
                                    readonly property real collapsedHeight:
                                        goalButton.visible ? 72 : 58
                                    readonly property real composerHeight:
                                        targeted
                                            ? inlineComposer.implicitHeight + 5
                                            : 0
                                    readonly property real retryStatusHeight:
                                        retryStatusVisible ? 19 : 0
                                    readonly property real targetCardHeight:
                                        collapsedHeight
                                        + (showsAnyActions ? 29 : 0)
                                        + retryStatusHeight
                                        + composerHeight

                                    onTargetCardHeightChanged:
                                        root.processTargetLayoutRevision += 1
                                    onHeightChanged: {
                                        if (processCard.isHovered) {
                                            processHoverBoundsRefreshTimer
                                                .restart()
                                        }
                                    }
                                    onYChanged: {
                                        if (processCard.isHovered) {
                                            processHoverBoundsRefreshTimer
                                                .restart()
                                        }
                                    }
                                    Component.onCompleted:
                                        root.processTargetLayoutRevision += 1

                                    function targetData() {
                                        return {
                                            "id": processCard.processId,
                                            "threadId": processCard.threadId,
                                            "kind": processCard.kind,
                                            "title": processCard.title,
                                            "status": processCard.status,
                                            "runtimeStatus":
                                                processCard.runtimeStatus,
                                            "rolloutPath":
                                                processCard.rolloutPath,
                                            "updatedAt":
                                                processCard.updatedAt,
                                            "goal": processCard.goal,
                                            "needsApproval":
                                                processCard.needsApproval,
                                            "cwd": processCard.cwd,
                                            "activeTurnId":
                                                processCard.activeTurnId,
                                            "model": processCard.model,
                                            "reasoningEffort":
                                                processCard.reasoningEffort
                                        }
                                    }

                                    function submitProcessPrompt() {
                                        if (processSendButton.enabled) {
                                            root.shellModel.submitProcessMessage()
                                        }
                                    }

                                    width: processList.width
                                    height: targetCardHeight
                                    radius: 22
                                    color: {
                                        return processCard.hasProcessAccent
                                            ? root.colorWithAlpha(
                                                processCard.processAccentColor,
                                                processCard
                                                    .processAccentFillAmount)
                                            : processCard.isHovered
                                                ? CompanionTheme
                                                    .traySelectedControlFillForBackdrop(
                                                        root.effectiveBackdropMode)
                                                : CompanionTheme
                                                    .trayControlFillForBackdrop(
                                                        root.effectiveBackdropMode)
                                    }
                                    border.width: 1
                                    border.color:
                                        processCard.hasProcessAccent
                                        ? root.colorWithAlpha(
                                            processCard.processAccentColor,
                                            processCard
                                                .processAccentBorderAlpha)
                                        : processCard.isHovered
                                            ? CompanionTheme
                                                .traySelectedControlBorderForBackdrop(
                                                    root.effectiveBackdropMode)
                                            : CompanionTheme
                                                .trayControlBorderForBackdrop(
                                                    root.effectiveBackdropMode)
                                    scale: processCard.isHovered ? 1.004 : 1
                                    ToolTip.text:
                                        processCard.preview
                                    ToolTip.visible:
                                        processCard.isHovered
                                        && ToolTip.text.length > 0
                                    ToolTip.delay: 650

                                    Behavior on color {
                                        ColorAnimation {
                                            duration: 160
                                        }
                                    }

                                    Behavior on scale {
                                        NumberAnimation {
                                            duration: 120
                                            easing.type:
                                                Easing.OutCubic
                                        }
                                    }

                                    Behavior on height {
                                        NumberAnimation {
                                            duration: 160
                                            easing.type:
                                                Easing.OutCubic
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        anchors.topMargin: 6
                                        anchors.bottomMargin: 6
                                        spacing: 5

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight:
                                                processCard.collapsedHeight - 12
                                            spacing: 9

                                             Label {
                                                 Layout.preferredWidth: 15
                                                 text: processCard.kind === "job"
                                                     ? "\ue713"
                                                     : "\ue8bd"
                                                color:
                                                    CompanionTheme.textSecondary
                                                font.family:
                                                    "Segoe Fluent Icons"
                                                font.pixelSize: 11
                                                horizontalAlignment:
                                                    Text.AlignHCenter
                                                verticalAlignment:
                                                    Text.AlignVCenter
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: processCard.title
                                                    color:
                                                        CompanionTheme.textPrimary
                                                    font.pixelSize: 12
                                                    font.weight:
                                                        Font.DemiBold
                                                    elide: Text.ElideRight
                                                }

                                                Button {
                                                    id: goalButton
                                                    property string interactionId:
                                                        "process.goal.open"
                                                    Layout.preferredHeight: 13
                                                    Layout.maximumWidth:
                                                        processCard.width - 70
                                                    visible:
                                                        processCard.goal !== null
                                                        && processCard.goal !== undefined
                                                        && processCard.goal.threadId !== undefined
                                                        && processCard.goal.threadId.length > 0
                                                    enabled: visible
                                                        && root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                        && !root.shellModel.goalMutationPending
                                                    text: root.goalBadgeText(
                                                        processCard.goal,
                                                        processCard.goalAnchorMs)
                                                    Accessible.name:
                                                        "Open goal controls for "
                                                        + processCard.title
                                                    ToolTip.text:
                                                        processCard.goal === null
                                                            || processCard.goal === undefined
                                                        ? ""
                                                        : processCard.goal.objective
                                                    ToolTip.visible: hovered
                                                    ToolTip.delay: 450
                                                    onClicked: {
                                                        root.rememberGoalPopupAnchor(
                                                            goalButton)
                                                        root.shellModel.openGoalControls(
                                                            processCard.title,
                                                            processCard.goal)
                                                    }

                                                    contentItem: Label {
                                                        leftPadding: 6
                                                        rightPadding: 6
                                                        text: goalButton.text
                                                        color:
                                                            CompanionTheme.textSecondary
                                                        font.pixelSize: 9
                                                        font.weight:
                                                            Font.DemiBold
                                                        elide:
                                                            Text.ElideRight
                                                        verticalAlignment:
                                                            Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 7
                                                        color:
                                                            goalButton.down
                                                            ? CompanionTheme.controlPressed
                                                            : goalButton.hovered
                                                                ? CompanionTheme.controlHover
                                                                : CompanionTheme.control
                                                        border.width: 1
                                                        border.color:
                                                            processCard.goal !== null
                                                                && processCard.goal !== undefined
                                                                && processCard.goal.status === "active"
                                                            ? CompanionTheme.accentMuted
                                                            : CompanionTheme.border
                                                    }
                                                }

                                                Item {
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 14
                                                    clip: true

                                                    Label {
                                                        id:
                                                            processStatusMessage
                                                        anchors.fill: parent
                                                        text:
                                                            root.processStatusSubtitle(
                                                                processCard.kind,
                                                                processCard.status,
                                                                processCard.runtimeStatus
                                                                    === "waitingOnApproval",
                                                                processCard.goal,
                                                                processCard.updatedAt,
                                                                processCard.cwd,
                                                                processCard.sourceStatus)
                                                        color:
                                                            CompanionTheme.textSecondary
                                                        font.pixelSize: 10
                                                        elide:
                                                            Text.ElideRight
                                                        maximumLineCount: 1
                                                        opacity:
                                                            processCard.isHovered
                                                            ? 0
                                                            : 1

                                                        Behavior on opacity {
                                                            NumberAnimation {
                                                                duration: 160
                                                            }
                                                        }
                                                    }

                                                    Label {
                                                        id:
                                                            processFullMessage
                                                        anchors.fill: parent
                                                        text:
                                                            processCard.preview
                                                        color:
                                                            CompanionTheme.textSecondary
                                                        font.pixelSize: 10
                                                        elide:
                                                            Text.ElideRight
                                                        maximumLineCount: 1
                                                        opacity:
                                                            processCard.isHovered
                                                            ? 1
                                                            : 0

                                                        Behavior on opacity {
                                                            NumberAnimation {
                                                                duration: 160
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                id: processStatusBadge

                                                Layout.preferredWidth: 24
                                                 Layout.preferredHeight: 24
                                                 radius: 12
                                                 color:
                                                     CompanionTheme
                                                         .trayControlFillForBackdrop(
                                                             root.effectiveBackdropMode)
                                                 border.width: 1
                                                 border.color:
                                                     processCard.status === "completed"
                                                     ? root.colorWithAlpha(
                                                         CompanionTheme.success,
                                                         0.55)
                                                     : processCard.status === "failed"
                                                         ? root.colorWithAlpha(
                                                             CompanionTheme.danger,
                                                             0.55)
                                                         : processCard.status === "waiting"
                                                             ? root.colorWithAlpha(
                                                                 CompanionTheme.warning,
                                                                 0.55)
                                                             : CompanionTheme
                                                                 .trayStatusBorderForBackdrop(
                                                                     root.effectiveBackdropMode)

                                                Item {
                                                    id: processSpinner

                                                    anchors.centerIn: parent
                                                    width: 16
                                                    height: 16
                                                    transformOrigin:
                                                        Item.Center
                                                    visible:
                                                        processCard.status === "running"

                                                    Canvas {
                                                        id: processSpinnerArc

                                                        anchors.centerIn: parent
                                                        width: 14
                                                        height: 14
                                                        antialiasing: true
                                                        visible: parent.visible
                                                        onPaint: {
                                                            const context =
                                                                getContext("2d")
                                                            context.clearRect(
                                                                0,
                                                                0,
                                                                width,
                                                                height)
                                                            context.lineWidth = 1.8
                                                            context.lineCap = "round"
                                                            context.strokeStyle =
                                                                CompanionTheme
                                                                    .textPrimary
                                                            context.beginPath()
                                                            context.arc(
                                                                width / 2,
                                                                height / 2,
                                                                4.8,
                                                                -Math.PI * 0.42,
                                                                Math.PI * 1.18,
                                                                false)
                                                            context.stroke()
                                                        }
                                                    }

                                                    NumberAnimation on rotation {
                                                        from: 0
                                                        to: 360
                                                        duration: 760
                                                        loops:
                                                            Animation.Infinite
                                                        running:
                                                            processSpinner
                                                                .visible
                                                    }
                                                }

                                                Label {
                                                    anchors.centerIn: parent
                                                    visible:
                                                        processCard.status !== "running"
                                                    text:
                                                        processCard.status === "completed"
                                                        ? "\ue73e"
                                                        : processCard.status === "failed"
                                                            ? "\ue711"
                                                            : "\ue7ba"
                                                    color:
                                                        processCard.status === "completed"
                                                        ? CompanionTheme.success
                                                        : processCard.status === "failed"
                                                            ? CompanionTheme.danger
                                                            : CompanionTheme.warning
                                                    font.family:
                                                        "Segoe Fluent Icons"
                                                    font.pixelSize: 10
                                                    font.weight:
                                                        Font.Bold
                                                }

                                                Accessible.name:
                                                    root.processStatusHelp(
                                                        processCard.status)
                                                ToolTip.text:
                                                    root.processStatusHelp(
                                                        processCard.status)
                                                ToolTip.visible:
                                                    processStatusBadgeHover.hovered
                                                ToolTip.delay: 450

                                                HoverHandler {
                                                    id:
                                                        processStatusBadgeHover
                                                }
                                            }
                                        }

                                        Item {
                                            readonly property real actionHeight:
                                                processCard.showsAnyActions
                                                    ? 24
                                                    : 0

                                            Layout.fillWidth: true
                                            Layout.minimumHeight: actionHeight
                                            Layout.preferredHeight: actionHeight
                                            Layout.maximumHeight: actionHeight
                                            clip: true

                                            Row {
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                height: 24
                                                spacing:
                                                    processCard.offersApproval
                                                        ? 4
                                                        : 6
                                                opacity:
                                                    processCard.showsAnyActions
                                                        ? 1
                                                        : 0
                                                enabled:
                                                    processCard.showsAnyActions

                                                Behavior on opacity {
                                                    NumberAnimation {
                                                        duration: 160
                                                    }
                                                }

                                                ProcessActionButton {
                                                    id: approveOnceButton
                                                    interactionId:
                                                        "process.approve-once"
                                                    visible:
                                                        processCard.offersApproval
                                                    text: "Approve once"
                                                    glyph: "\ue73e"
                                                    compact: true
                                                    width:
                                                        (parent.width
                                                         - parent.spacing * 2)
                                                        / 3
                                                    enabled:
                                                        parent.enabled
                                                        && !processCard.approvalCommandBusy
                                                    Accessible.ignored:
                                                        !processCard.showApprovalActions
                                                    Accessible.name:
                                                        "Approve once for "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.respondToProcessApproval(
                                                            processCard.targetData(),
                                                            "approveOnce")
                                                }

                                                ProcessActionButton {
                                                    id: approveSimilarButton
                                                    interactionId:
                                                        "process.approve-similar"
                                                    visible:
                                                        processCard.offersApproval
                                                    text: "Approve similar"
                                                    glyph: "\ue83d"
                                                    compact: true
                                                    width:
                                                        (parent.width
                                                         - parent.spacing * 2)
                                                        / 3
                                                    enabled:
                                                        parent.enabled
                                                        && !processCard.approvalCommandBusy
                                                    Accessible.ignored:
                                                        !processCard.showApprovalActions
                                                    Accessible.name:
                                                        "Approve similar commands for "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.respondToProcessApproval(
                                                            processCard.targetData(),
                                                            "approveSimilar")
                                                }

                                                ProcessActionButton {
                                                    id: tellCodexButton
                                                    interactionId:
                                                        "process.tell-codex"
                                                    visible:
                                                        processCard.offersApproval
                                                    text: "Tell Codex"
                                                    glyph: "\ue8bd"
                                                    compact: true
                                                    width:
                                                        (parent.width
                                                         - parent.spacing * 2)
                                                        / 3
                                                    enabled:
                                                        parent.enabled
                                                        && !processCard.approvalPending
                                                    Accessible.ignored:
                                                        !processCard.showApprovalActions
                                                    Accessible.name:
                                                        "Tell Codex something else for "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.beginProcessAction(
                                                            processCard.targetData(),
                                                            "approval-feedback")
                                                }

                                                ProcessActionButton {
                                                    id: replyButton
                                                    interactionId:
                                                        "process.reply"
                                                    visible:
                                                        !processCard.offersApproval
                                                        && processCard.offersReply
                                                    text: "Reply"
                                                    glyph: "\ue72a"
                                                    enabled:
                                                        parent.enabled
                                                        && processCard.processComposerActionsEnabled
                                                    Accessible.ignored:
                                                        !processCard.showReply
                                                    Accessible.name:
                                                        "Reply to "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.beginProcessAction(
                                                            processCard.targetData(),
                                                            "reply")
                                                }

                                                ProcessActionButton {
                                                    id: steerButton
                                                    interactionId:
                                                        "process.steer"
                                                    visible:
                                                        !processCard.offersApproval
                                                        && processCard.offersSteer
                                                    text: "Steer"
                                                    glyph: "\ue72b"
                                                    enabled:
                                                        parent.enabled
                                                        && processCard.processComposerActionsEnabled
                                                    Accessible.ignored:
                                                        !processCard.showSteer
                                                    Accessible.name:
                                                        "Steer "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.beginProcessAction(
                                                            processCard.targetData(),
                                                            "steer")
                                                }

                                                ProcessActionButton {
                                                    id: retryButton
                                                    interactionId:
                                                        "process.retry"
                                                    visible:
                                                        !processCard.offersApproval
                                                        && processCard.offersRetry
                                                    text:
                                                        processCard.retryPending
                                                        ? "Retrying"
                                                        : "Retry"
                                                    glyph: "\ue72c"
                                                    enabled:
                                                        parent.enabled
                                                        && processCard.processComposerActionsEnabled
                                                        && !root.shellModel.processCommandBusy
                                                    Accessible.ignored:
                                                        !processCard.showRetry
                                                    Accessible.name:
                                                        "Retry "
                                                        + processCard.title
                                                    onClicked:
                                                        root.shellModel.retryFailedProcess(
                                                            processCard.targetData())
                                                }
                                            }
                                        }

                                        Label {
                                            id: retryStatusLabel

                                            Layout.fillWidth: true
                                            Layout.preferredHeight:
                                                visible ? 14 : 0
                                            visible:
                                                processCard.retryStatusVisible
                                            text: visible
                                                ? root.shellModel
                                                    .processRetryStatus
                                                : ""
                                            color:
                                                root.shellModel
                                                    .processRetryStatusIsError
                                                ? CompanionTheme.danger
                                                : CompanionTheme
                                                    .textSecondary
                                            font.pixelSize: 9
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                        }

                                        ColumnLayout {
                                            id: inlineComposer

                                            Layout.fillWidth: true
                                            Layout.preferredHeight:
                                                visible ? implicitHeight : 0
                                            spacing: 6
                                            visible:
                                                processCard.targeted
                                            implicitHeight: 22 + 6
                                                + promptSurface.implicitHeight
                                                + (processFeedbackLabel.visible
                                                    ? 36
                                                    : 0)

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                spacing: 6

                                                Label {
                                                    text:
                                                        processCard.targeted
                                                            && root.shellModel.processTargetAction
                                                                === "steer"
                                                        ? "\ue72b"
                                                        : "\ue72a"
                                                    color:
                                                        CompanionTheme.textSecondary
                                                    font.family:
                                                        "Segoe Fluent Icons"
                                                    font.pixelSize: 9
                                                }

                                                Label {
                                                    Layout.fillWidth: true
                                                    text:
                                                        processCard.targeted
                                                        ? root.processActionTitle(
                                                            root.shellModel.processTargetAction)
                                                        : ""
                                                    color:
                                                        CompanionTheme.textPrimary
                                                    font.pixelSize: 10
                                                    font.weight:
                                                        Font.DemiBold
                                                }

                                                ToolButton {
                                                    id: cancelProcessButton
                                                    property string interactionId:
                                                        "process.cancel"
                                                    Layout.preferredWidth: 20
                                                    Layout.preferredHeight: 20
                                                    text: "\ue711"
                                                    padding: 0
                                                    enabled:
                                                        processCard.targeted
                                                    Accessible.name:
                                                        "Cancel "
                                                        + root.processActionTitle(
                                                            root.shellModel.processTargetAction)
                                                    onClicked:
                                                        root.shellModel.cancelProcessTarget()

                                                    contentItem: Label {
                                                        text:
                                                            cancelProcessButton.text
                                                        color:
                                                            CompanionTheme.textSecondary
                                                        font.family:
                                                            "Segoe Fluent Icons"
                                                        font.pixelSize: 8
                                                        horizontalAlignment:
                                                            Text.AlignHCenter
                                                        verticalAlignment:
                                                            Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 10
                                                        color:
                                                            cancelProcessButton.hovered
                                                            ? CompanionTheme.controlHover
                                                            : CompanionTheme.control
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                id: promptSurface

                                                Layout.fillWidth: true
                                                Layout.preferredHeight:
                                                    implicitHeight
                                                implicitHeight: Math.min(
                                                    68,
                                                    Math.max(
                                                        38,
                                                        processPromptInput.contentHeight
                                                            + 16))
                                                radius: implicitHeight / 2
                                                color:
                                                    CompanionTheme.control
                                                border.width: 1
                                                border.color:
                                                    processPromptInput.activeFocus
                                                    ? CompanionTheme.accent
                                                    : CompanionTheme.border

                                                TextArea {
                                                    id: processPromptInput

                                                    anchors.left: parent.left
                                                    anchors.right:
                                                        processSendButton.left
                                                    anchors.top: parent.top
                                                    anchors.bottom:
                                                        parent.bottom
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 6
                                                    anchors.topMargin: 5
                                                    anchors.bottomMargin: 5
                                                    text:
                                                        processCard.targeted
                                                            ? root.shellModel.processDraft
                                                            : ""
                                                    placeholderText:
                                                        processCard.targeted
                                                        ? root.processActionTitle(
                                                            root.shellModel.processTargetAction)
                                                            + " to "
                                                            + processCard.title
                                                        : ""
                                                    placeholderTextColor:
                                                        CompanionTheme.textMuted
                                                    color:
                                                        CompanionTheme.textPrimary
                                                    font.pixelSize: 11
                                                    wrapMode: TextEdit.Wrap
                                                    background: null
                                                    enabled:
                                                        root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                    Accessible.name:
                                                        placeholderText
                                                    onTextChanged: {
                                                        if (processCard.targeted
                                                                && root.shellModel.processDraft
                                                                    !== text) {
                                                            root.shellModel.processDraft =
                                                            text
                                                        }
                                                    }
                                                    Keys.priority:
                                                        Keys.BeforeItem
                                                    Keys.onReturnPressed:
                                                        function(event) {
                                                            if ((event.modifiers
                                                                    & Qt.ShiftModifier)
                                                                    === 0) {
                                                                processCard.submitProcessPrompt()
                                                                event.accepted = true
                                                            }
                                                        }
                                                    Keys.onEnterPressed:
                                                        function(event) {
                                                            if ((event.modifiers
                                                                    & Qt.ShiftModifier)
                                                                    === 0) {
                                                                processCard.submitProcessPrompt()
                                                                event.accepted = true
                                                            }
                                                        }
                                                }

                                                ToolButton {
                                                    id: processSendButton
                                                    property string interactionId:
                                                        "process.send"

                                                    anchors.right:
                                                        parent.right
                                                    anchors.rightMargin: 5
                                                    anchors.verticalCenter:
                                                        parent.verticalCenter
                                                    width: 28
                                                    height: 28
                                                    text: root.shellModel.processSending
                                                        ? "\ue895"
                                                        : "\ue74a"
                                                    padding: 0
                                                    enabled:
                                                        processCard.targeted
                                                        && !root.shellModel.processSending
                                                        && processPromptInput.text.trim().length > 0
                                                    Accessible.name:
                                                        "Send "
                                                        + root.processActionTitle(
                                                            root.shellModel.processTargetAction)
                                                    onClicked:
                                                        processCard.submitProcessPrompt()

                                                    contentItem: Label {
                                                        text:
                                                            processSendButton.text
                                                        color:
                                                            processSendButton.enabled
                                                            ? CompanionTheme.accentText
                                                            : CompanionTheme.textMuted
                                                        font.family:
                                                            "Segoe Fluent Icons"
                                                        font.pixelSize: 11
                                                        font.weight:
                                                            Font.Bold
                                                        horizontalAlignment:
                                                            Text.AlignHCenter
                                                        verticalAlignment:
                                                            Text.AlignVCenter
                                                    }

                                                    background: Rectangle {
                                                        radius: 14
                                                        color:
                                                            processSendButton.enabled
                                                            ? processSendButton.down
                                                                ? CompanionTheme.accentPressed
                                                                : CompanionTheme.accent
                                                            : CompanionTheme.surfaceRaised
                                                    }
                                                }
                                            }

                                            Label {
                                                id: processFeedbackLabel

                                                Layout.fillWidth: true
                                                Layout.preferredHeight:
                                                    visible ? 30 : 0
                                                visible:
                                                    processCard.targeted
                                                    && root.shellModel.processFeedback.length > 0
                                                text: visible
                                                    ? root.shellModel.processFeedback
                                                    : ""
                                                color:
                                                    root.shellModel.processFeedbackIsError
                                                    ? CompanionTheme.warning
                                                    : CompanionTheme.textSecondary
                                                font.pixelSize: 9
                                                font.weight:
                                                    Font.Medium
                                                wrapMode: Text.Wrap
                                                maximumLineCount: 2
                                                elide: Text.ElideRight
                                            }

                                            onVisibleChanged: {
                                                if (visible) {
                                                    Qt.callLater(function() {
                                                        processPromptInput.forceActiveFocus()
                                                    })
                                                }
                                            }
                                        }
                                    }

                                    onGoalChanged:
                                        goalAnchorMs = Date.now()

                                }
                            }

                            Rectangle {
                                id: processNoticeCard

                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                height: root.shellModel !== null
                                    && root.shellModel !== undefined
                                    && root.shellModel.processLoading
                                    ? 46
                                    : 58
                                radius: 22
                                 visible: true
                                 opacity: root.processFeedEmpty ? 1 : 0
                                 enabled: opacity > 0
                                 z: opacity > 0 ? 1 : -1
                                 color:
                                     CompanionTheme
                                         .trayControlFillForBackdrop(
                                             root.effectiveBackdropMode)
                                 border.width: 1
                                 border.color: root.shellModel !== null
                                     && root.shellModel !== undefined
                                     && root.shellModel.processErrorMessage.length > 0
                                     ? root.colorWithAlpha(
                                         CompanionTheme.danger,
                                         0.55)
                                     : CompanionTheme
                                         .trayControlBorderForBackdrop(
                                             root.effectiveBackdropMode)
                                ToolTip.text: {
                                    if (root.shellModel === null
                                            || root.shellModel === undefined) {
                                        return ""
                                    }
                                    if (root.shellModel.processErrorMessage
                                            .length > 0) {
                                        return root.shellModel
                                            .processErrorMessage
                                    }
                                    if (root.shellModel.processLoading) {
                                        return ""
                                    }
                                    return "Open a Codex thread or start a job and it will appear here."
                                }
                                ToolTip.visible: processNoticeHover.hovered
                                    && ToolTip.text.length > 0
                                ToolTip.delay: 450

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    anchors.topMargin: 6
                                    anchors.bottomMargin: 6
                                    spacing: 9

                                    Item {
                                        Layout.preferredWidth: 24
                                        Layout.preferredHeight: 24

                                        BusyIndicator {
                                            anchors.centerIn: parent
                                            running: visible
                                            visible: root.shellModel !== null
                                                && root.shellModel !== undefined
                                                && root.shellModel.processLoading
                                            width: 20
                                            height: 20
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: root.shellModel === null
                                                || root.shellModel === undefined
                                                || !root.shellModel.processLoading
                                            text: root.shellModel !== null
                                                && root.shellModel !== undefined
                                                && root.shellModel.processErrorMessage.length > 0
                                                ? "\ue783"
                                                : "\ue946"
                                            color: root.shellModel !== null
                                                && root.shellModel !== undefined
                                                && root.shellModel.processErrorMessage.length > 0
                                                ? CompanionTheme.danger
                                                : CompanionTheme.textSecondary
                                            font.family: "Segoe Fluent Icons"
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Label {
                                            id: processNoticeTitle

                                            Layout.fillWidth: true
                                            text: {
                                                if (root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                        && root.shellModel.processErrorMessage.length > 0) {
                                                    return "Could not load Codex processes"
                                                }
                                                if (root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                        && root.shellModel.processLoading) {
                                                    return "Loading Codex processes"
                                                }
                                                return "No active Codex processes"
                                            }
                                            color: CompanionTheme.textPrimary
                                            font.pixelSize: 12
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }

                                        Label {
                                            id: processNoticeSubtitle

                                            Layout.fillWidth: true
                                            text: {
                                                if (root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                        && root.shellModel.processErrorMessage.length > 0) {
                                                    return "Refresh failed"
                                                }
                                                if (root.shellModel !== null
                                                        && root.shellModel !== undefined
                                                        && root.shellModel.processLoading) {
                                                    return "Checking recent work"
                                                }
                                                return "Nothing running right now"
                                            }
                                            color: CompanionTheme.textSecondary
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                HoverHandler {
                                    id: processNoticeHover
                                    enabled: processNoticeCard.opacity > 0
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: processEdgeMaskGradient

                        objectName: "processEdgeMaskGradient"
                        z: -1
                        width: processViewport.width
                        height: processViewport.height
                        gradient: Gradient {
                            orientation:
                                Gradient.Vertical

                            GradientStop {
                                position: 0
                                color: "white"
                            }

                            GradientStop {
                                position: Math.max(
                                    0,
                                    1
                                        - (root
                                            .processScrollableBottomInset
                                            + root
                                                .processEdgeFadeHeight)
                                            / Math.max(
                                                1,
                                                processEdgeMaskGradient
                                                    .height))
                                color: "white"
                            }

                            GradientStop {
                                position: Math.max(
                                    0,
                                    Math.min(
                                        1,
                                        1
                                            - root
                                                .processScrollableBottomInset
                                                / Math.max(
                                                    1,
                                                    processEdgeMaskGradient
                                                        .height)))
                                color: "transparent"
                            }

                            GradientStop {
                                position: 1
                                color: "transparent"
                            }
                        }
                    }

                }

                ColumnLayout {
                    spacing: root.showsChatResponsePanel ? 8 : 0

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !root.showsChatResponsePanel
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: root.showsChatResponsePanel
                        radius: 17
                        color:
                            CompanionTheme
                                .trayControlFillForBackdrop(
                                    root.effectiveBackdropMode)
                        border.width: 1
                        border.color:
                            CompanionTheme
                                .trayControlBorderForBackdrop(
                                    root.effectiveBackdropMode)

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight:
                                    visible ? 22 : 0
                                spacing: 7
                                visible:
                                    chatResponseLabel.text.length > 0

                                Label {
                                    text: "\ue8bd"
                                    color: CompanionTheme.textSecondary
                                    font.family: "Segoe Fluent Icons"
                                    font.pixelSize: 10
                                }

                                Label {
                                    id: chatResponseTitleLabel
                                    Layout.fillWidth: true
                                    text: root.shellModel === null
                                        || root.shellModel === undefined
                                        || root.shellModel
                                            .chatResponseTitle
                                            .length === 0
                                        ? root.selectedChatModelTitle()
                                        : root.shellModel
                                            .chatResponseTitle
                                    color: CompanionTheme.textPrimary
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }

                                ToolButton {
                                    id: chatResponseDismissButton
                                    property string interactionId:
                                        "chat.response.close"
                                    Layout.preferredWidth: 20
                                    Layout.preferredHeight: 20
                                    visible:
                                        chatResponseLabel.text.length > 0
                                    text: "\ue711"
                                    padding: 0
                                    Accessible.name: "Dismiss response"
                                    ToolTip.text: "Dismiss"
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 450
                                    onClicked:
                                        root.shellModel
                                            .dismissChatResponse()

                                    contentItem: Label {
                                        text:
                                            chatResponseDismissButton.text
                                        color:
                                            CompanionTheme.textSecondary
                                        font.family:
                                            "Segoe Fluent Icons"
                                        font.pixelSize: 9
                                        font.weight: Font.Bold
                                        horizontalAlignment:
                                            Text.AlignHCenter
                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    background: Rectangle {
                                        radius: 10
                                        color:
                                            chatResponseDismissButton.down
                                            ? CompanionTheme
                                                .controlPressed
                                            : chatResponseDismissButton
                                                .hovered
                                                ? CompanionTheme
                                                    .controlHover
                                                : CompanionTheme.control
                                    }
                                }
                            }

                            Label {
                                id: chatResponsePromptLabel
                                Layout.fillWidth: true
                                text: root.shellModel === null
                                    || root.shellModel === undefined
                                    ? ""
                                    : root.shellModel
                                        .chatResponsePrompt
                                visible: text.length > 0
                                color: CompanionTheme.textPrimary
                                opacity: 0.82
                                font.pixelSize: 10
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            Flickable {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                contentWidth: width
                                contentHeight:
                                    chatResponseLabel.implicitHeight
                                boundsBehavior:
                                    Flickable.StopAtBounds
                                visible:
                                    chatResponseLabel.text.length > 0

                                Label {
                                    id: chatResponseLabel
                                    width: parent.width
                                    text: root.shellModel === null
                                        || root.shellModel === undefined
                                        ? ""
                                        : root.shellModel.chatResponse
                                    color:
                                        CompanionTheme.textSecondary
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                            }

                            Label {
                                id: chatResponseUsageLabel
                                Layout.fillWidth: true
                                text: root.shellModel === null
                                    || root.shellModel === undefined
                                    ? ""
                                    : root.shellModel
                                        .chatResponseUsageSummary
                                visible: text.length > 0
                                color: CompanionTheme.textSecondary
                                opacity: 0.72
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Rectangle {
                        id: composerSurface
                        Layout.fillWidth: true
                        Layout.preferredHeight: 46
                        Layout.minimumHeight: 46
                        Layout.maximumHeight: 46
                        radius: 18
                        color:
                            CompanionTheme
                                .trayControlFillForBackdrop(
                                    root.effectiveBackdropMode)
                        border.width: 1
                        border.color: promptInput.activeFocus
                            ? CompanionTheme.accentMuted
                            : CompanionTheme
                                .trayControlBorderForBackdrop(
                                    root.effectiveBackdropMode)

                        RowLayout {
                            id: composerRow
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 4

                            TextArea {
                                id: promptInput
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumWidth: 52
                                leftPadding: 8
                                rightPadding: 4
                                topPadding: 8
                                bottomPadding: 8
                                color: CompanionTheme.textPrimary
                                placeholderText:
                                    root.shellModel === null
                                    || root.shellModel === undefined
                                    || root.shellModel
                                        .chatPromptPlaceholder === undefined
                                        ? "Ask Companion"
                                        : root.shellModel
                                            .chatPromptPlaceholder
                                placeholderTextColor:
                                    CompanionTheme.textMuted
                                font.pixelSize: 11
                                wrapMode: TextEdit.Wrap
                                background: null
                                Accessible.name: "Local chat prompt"
                                Keys.priority: Keys.BeforeItem
                                Keys.onReturnPressed:
                                    function(event) {
                                        if ((event.modifiers
                                                & Qt.ShiftModifier)
                                                === 0) {
                                            root.submitLocalChatPrompt()
                                            event.accepted = true
                                        }
                                    }
                                Keys.onEnterPressed:
                                    function(event) {
                                        if ((event.modifiers
                                                & Qt.ShiftModifier)
                                                === 0) {
                                            root.submitLocalChatPrompt()
                                            event.accepted = true
                                        }
                                    }
                            }

                            Button {
                                id: prepareChatButton
                                property string interactionId:
                                    "composer.prepare"
                                Layout.preferredWidth: visible ? 66 : 0
                                Layout.preferredHeight: 30
                                text: "Prepare"
                                visible: root.shellModel !== null
                                    && root.shellModel !== undefined
                                    && root.shellModel
                                        .selectedChatModelId
                                        === "on-device"
                                    && !root.shellModel.chatSendEnabled
                                enabled: visible
                                    && root.shellModel
                                        .chatPreparationEnabled
                                    && !root.shellModel.chatBusy
                                Accessible.name:
                                    "Prepare on-device chat"
                                ToolTip.text:
                                    "Prepare on-device chat"
                                ToolTip.visible: hovered
                                ToolTip.delay: 450
                                onClicked:
                                    root.shellModel
                                        .prepareOnDeviceChat()

                                contentItem: Label {
                                    text: prepareChatButton.text
                                    color: prepareChatButton.enabled
                                        ? CompanionTheme.textPrimary
                                        : CompanionTheme.textMuted
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    horizontalAlignment:
                                        Text.AlignHCenter
                                    verticalAlignment:
                                        Text.AlignVCenter
                                }

                                background: Rectangle {
                                    radius: 13
                                    color: prepareChatButton.down
                                        ? CompanionTheme.controlPressed
                                        : prepareChatButton.hovered
                                            ? CompanionTheme
                                                .controlHover
                                            : CompanionTheme
                                                .controlSelected
                                    border.width: 1
                                    border.color:
                                        CompanionTheme.border
                                }
                            }

                            ToolButton {
                                id: sendButton
                                property string interactionId:
                                    "composer.send"
                                readonly property bool hoverActive:
                                    sendHoverArea.containsMouse
                                property real horizontalScale:
                                    sendButton.hoverActive
                                        ? 1.014
                                        : 1
                                property real verticalScale:
                                    sendButton.hoverActive
                                        ? 0.992
                                        : 1
                                Layout.preferredWidth:
                                    visible ? 31 : 0
                                Layout.preferredHeight: 30
                                visible: !prepareChatButton.visible
                                text: "\u2191"
                                padding: 0
                                enabled: root.shellModel !== null
                                    && root.shellModel !== undefined
                                    && root.shellModel.chatSendEnabled
                                    && !root.shellModel.chatBusy
                                    && promptInput.text.trim().length > 0
                                Accessible.name:
                                    "Send local chat message"
                                ToolTip.text: enabled
                                    ? "Send local chat message"
                                    : root.shellModel === null
                                        || root.shellModel === undefined
                                        ? "Local chat is unavailable"
                                        : root.shellModel
                                            .chatStatusMessage
                                ToolTip.visible:
                                    sendButton.hoverActive
                                ToolTip.delay: 450
                                onClicked:
                                    root.submitLocalChatPrompt()

                                MouseArea {
                                    id: sendHoverArea

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }

                                Behavior on horizontalScale {
                                    NumberAnimation {
                                        duration: 160
                                        easing.type:
                                            Easing.OutCubic
                                    }
                                }

                                Behavior on verticalScale {
                                    NumberAnimation {
                                        duration: 160
                                        easing.type:
                                            Easing.OutCubic
                                    }
                                }

                                contentItem: Label {
                                    id: sendButtonContent

                                    text: sendButton.text
                                    color: sendButton.enabled
                                        ? CompanionTheme.accentText
                                        : CompanionTheme.textMuted
                                    font.pixelSize: 15
                                    font.weight: Font.Bold
                                    horizontalAlignment:
                                        Text.AlignHCenter
                                    verticalAlignment:
                                        Text.AlignVCenter

                                    transform: Scale {
                                        origin.x:
                                            sendButtonContent.width
                                                / 2
                                        origin.y:
                                            sendButtonContent.height
                                                / 2
                                        xScale:
                                            sendButton
                                                .horizontalScale
                                        yScale:
                                            sendButton
                                                .verticalScale
                                    }
                                }

                                background: Rectangle {
                                    id: sendButtonBackground

                                    radius: 13
                                    color: sendButton.enabled
                                        ? sendButton.down
                                            ? CompanionTheme
                                                .accentPressed
                                            : CompanionTheme.accent
                                        : CompanionTheme.controlSelected

                                    transform: Scale {
                                        origin.x:
                                            sendButtonBackground.width
                                                / 2
                                        origin.y:
                                            sendButtonBackground.height
                                                / 2
                                        xScale:
                                            sendButton
                                                .horizontalScale
                                        yScale:
                                            sendButton
                                                .verticalScale
                                    }
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                id: routeFooter

                Layout.fillWidth: true
                Layout.preferredHeight: 0
                spacing: 5
                visible: false

                Button {
                    id: chatRouteButton
                    property string interactionId:
                        "composer.route.chat"
                    Layout.preferredWidth: 68
                    Layout.preferredHeight: 30
                    text: "Chat"
                    visible: false
                    checkable: true
                    checked: root.routeMode === "local-chat"
                    Accessible.name: "Local Chat"
                    onClicked: {
                        if (root.shellModel !== null
                                && root.shellModel !== undefined) {
                            root.shellModel.showLocalChat()
                        }
                    }

                    contentItem: Label {
                        text: chatRouteButton.text
                        color: chatRouteButton.checked
                            ? CompanionTheme.textPrimary
                            : CompanionTheme.textSecondary
                        font.pixelSize: 10
                        font.weight: chatRouteButton.checked
                            ? Font.DemiBold
                            : Font.Normal
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 13
                        color: chatRouteButton.down
                            ? CompanionTheme.controlPressed
                            : chatRouteButton.checked
                                ? CompanionTheme.controlSelected
                                : chatRouteButton.hovered
                                    ? CompanionTheme.controlHover
                                    : CompanionTheme.control
                        border.width: 1
                        border.color: chatRouteButton.checked
                            ? CompanionTheme.accentMuted
                            : CompanionTheme.border
                    }
                }

                Button {
                    id: codexRouteButton
                    property string interactionId:
                        "composer.route.codex"
                    Layout.preferredWidth: 68
                    Layout.preferredHeight: 30
                    text: "Codex"
                    visible: false
                    checkable: true
                    checked: root.routeMode === "processes"
                    Accessible.name: "Codex Processes"
                    onClicked: {
                        if (root.shellModel !== null
                                && root.shellModel !== undefined) {
                            root.shellModel.showProcesses()
                        }
                    }

                    contentItem: Label {
                        text: codexRouteButton.text
                        color: codexRouteButton.checked
                            ? CompanionTheme.textPrimary
                            : CompanionTheme.textSecondary
                        font.pixelSize: 10
                        font.weight: codexRouteButton.checked
                            ? Font.DemiBold
                            : Font.Normal
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 13
                        color: codexRouteButton.down
                            ? CompanionTheme.controlPressed
                            : codexRouteButton.checked
                                ? CompanionTheme.controlSelected
                                : codexRouteButton.hovered
                                    ? CompanionTheme.controlHover
                                    : CompanionTheme.control
                        border.width: 1
                        border.color: codexRouteButton.checked
                            ? CompanionTheme.accentMuted
                            : CompanionTheme.border
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                Popup {
                        id: chatModelPopup

                        popupType: Popup.Window
                        parent: root.contentItem
                        x: root.width - width - 8
                        y: -height - 6
                        width: 238
                        implicitWidth: 238
                        contentWidth:
                            width - leftPadding - rightPadding
                        padding: 10
                        implicitHeight:
                            Math.min(
                                420,
                                chatModelPickerContent.implicitHeight
                                    + topPadding
                                    + bottomPadding)
                        closePolicy: Popup.CloseOnEscape
                            | Popup.CloseOnPressOutside
                        onOpened:
                            Qt.callLater(function() {
                                root.modelPickerWindow =
                                    chatModelPopup
                                        .contentItem.Window.window
                            })

                        background: Rectangle {
                            radius: 20
                            color:
                                CompanionTheme
                                    .popoverSurfaceForBackdrop(
                                        root.modelPickerEffectiveBackdropMode)
                            border.width: 1
                            border.color:
                                CompanionTheme
                                    .traySurfaceBorderForBackdrop(
                                        root.modelPickerEffectiveBackdropMode)
                        }

                        contentItem: Column {
                            id: chatModelPickerContent

                            width: chatModelPopup.availableWidth
                            spacing: 6

                            Label {
                                width: parent.width
                                leftPadding: 4
                                rightPadding: 4
                                text: "Chat model"
                                color: CompanionTheme.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }

                            Repeater {
                                id: chatModelOptions

                                model: root.shellModel === null
                                    || root.shellModel === undefined
                                    ? []
                                    : root.shellModel.chatModels

                                delegate: Item {
                                    id: chatModelOption

                                    required property int index
                                    required property var modelData
                                    property alias optionButton:
                                        chatModelOptionButton
                                    readonly property bool beginsGroup:
                                        index > 0
                                        && modelData.group
                                            !== root.shellModel
                                                .chatModels[index - 1]
                                                .group
                                    readonly property bool selected:
                                        root.shellModel !== null
                                        && root.shellModel !== undefined
                                        && root.shellModel
                                            .selectedChatModelId
                                            === modelData.id

                                    width: chatModelPickerContent.width
                                    height: 38
                                        + (beginsGroup ? 7 : 0)

                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        height: 1
                                        visible: chatModelOption.beginsGroup
                                        color: CompanionTheme.border
                                    }

                                    Button {
                                        id: chatModelOptionButton
                                        property string interactionId:
                                            "model.select"

                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        height: 38
                                        padding: 0
                                        Accessible.name:
                                            chatModelOption.modelData.title
                                            + ", "
                                            + chatModelOption.modelData.detail
                                        onClicked: {
                                            const accepted =
                                                root.shellModel
                                                    .chooseChatModel(
                                                        chatModelOption
                                                            .modelData.id)
                                            if (accepted) {
                                                chatModelPopup.close()
                                            }
                                        }

                                        contentItem: RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            spacing: 8

                                            Label {
                                                Layout.preferredWidth: 15
                                                text: chatModelOption.selected
                                                    ? "\u2713"
                                                    : "\u25cb"
                                                color:
                                                    chatModelOption.selected
                                                    ? CompanionTheme.accent
                                                    : CompanionTheme
                                                        .textSecondary
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                                horizontalAlignment:
                                                    Text.AlignHCenter
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 1

                                                Label {
                                                    Layout.fillWidth: true
                                                    text:
                                                        chatModelOption
                                                            .modelData
                                                            .title
                                                    color:
                                                        CompanionTheme
                                                            .textPrimary
                                                    font.pixelSize: 11
                                                    font.weight:
                                                        Font.DemiBold
                                                    elide: Text.ElideRight
                                                }

                                                Label {
                                                    Layout.fillWidth: true
                                                    text:
                                                        chatModelOption
                                                            .modelData
                                                            .detail
                                                    color:
                                                        CompanionTheme
                                                            .textSecondary
                                                    font.pixelSize: 9
                                                    font.weight:
                                                        Font.Medium
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        background: Rectangle {
                                            radius: 10
                                            color:
                                                chatModelOptionButton.down
                                                ? CompanionTheme
                                                    .controlPressed
                                                : chatModelOptionButton
                                                    .hovered
                                                    ? CompanionTheme
                                                        .controlHover
                                                    : chatModelOption
                                                        .selected
                                                        ? CompanionTheme
                                                            .controlSelected
                                                        : "transparent"
                                        }
                                    }
                                }
                            }
                        }
                }
            }
        }
    }

    GoalConfettiOverlay {
        id: goalConfettiOverlay

        anchors.fill: parent
        z: 900
        trigger: root.attentionModel !== null
                && root.attentionModel !== undefined
                && root.attentionModel
                    .goalConfettiTrigger !== undefined
            ? root.attentionModel.goalConfettiTrigger
            : 0
    }

    Popup {
        id: goalPopup

        popupType: Popup.Window
        parent: root.contentItem
        x: Math.max(
            0,
            root.goalPopupTargetOriginX())
        y: Math.max(
            0,
            root.goalPopupTargetOriginY())
        width: 286
        implicitWidth: 286
        contentWidth: width - leftPadding - rightPadding
        padding: 12
        margins: -1
        z: 1000
        modal: false
        dim: false
        focus: true
        visible: root.routeMode === "processes"
            && root.shellModel !== null
            && root.shellModel !== undefined
            && root.shellModel.goalControlVisible
        closePolicy: root.shellModel !== null
                && root.shellModel !== undefined
            ? Popup.CloseOnEscape
                | Popup.CloseOnPressOutside
            : Popup.NoAutoClose

        onOpened: {
            root.scheduleGoalPopupPosition()
            Qt.callLater(function() {
                root.goalWindow =
                    goalPopup
                        .contentItem.Window.window
            })
        }
        onHeightChanged:
            root.scheduleGoalPopupPosition()
        onClosed: {
            root.goalPopupAnchored = false
            if (root.shellModel !== null
                    && root.shellModel !== undefined
                    && root.shellModel.goalControlVisible) {
                root.shellModel.dismissGoalControls()
            }
        }

        background: Rectangle {
            radius: 22
            color:
                CompanionTheme.popoverSurfaceForBackdrop(
                    root.goalEffectiveBackdropMode)
            border.width: 1
            border.color:
                CompanionTheme.traySurfaceBorderForBackdrop(
                    root.goalEffectiveBackdropMode)
        }

        contentItem: ColumnLayout {
            id: goalPanel
            width: goalPopup.contentWidth
            implicitWidth: goalPopup.contentWidth
            spacing: 9

            RowLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: goalPopup.contentWidth
                spacing: 7

                Label {
                    id: goalStatusIcon
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    text: root.shellModel === null
                        || root.shellModel === undefined
                        ? root.goalStatusGlyph("")
                        : root.goalStatusGlyph(
                            root.shellModel.goalStatus)
                    color: root.shellModel === null
                        || root.shellModel === undefined
                        ? CompanionTheme.textMuted
                        : root.goalStatusColor(
                            root.shellModel.goalStatus)
                    font.family: "Segoe Fluent Icons"
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Accessible.name:
                        root.shellModel === null
                        || root.shellModel === undefined
                            ? "Goal"
                            : root.goalStatusDisplayTitle(
                                root.shellModel.goalStatus)
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.shellModel === null
                            || root.shellModel === undefined
                            ? "Goal"
                            : root.shellModel.goalTaskTitle
                        color: CompanionTheme.textPrimary
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Label {
                        id: goalStatusLabel
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.shellModel === null
                            || root.shellModel === undefined
                            ? ""
                            : root.goalStatusDisplayTitle(
                                root.shellModel.goalStatus)
                        color: CompanionTheme.textMuted
                        font.pixelSize: 9
                    }
                }

                ToolButton {
                    id: goalCloseButton
                    property string interactionId:
                        "goal.close"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    text: "\u00d7"
                    padding: 0
                    enabled: root.shellModel !== null
                        && root.shellModel !== undefined
                    Accessible.name: "Close goal controls"
                    ToolTip.text: "Close goal controls"
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    onClicked:
                        root.shellModel.dismissGoalControls()

                    contentItem: Label {
                        text: goalCloseButton.text
                        color: goalCloseButton.enabled
                            ? CompanionTheme.textSecondary
                            : CompanionTheme.textMuted
                        font.pixelSize: 17
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 12
                        color: goalCloseButton.down
                            ? CompanionTheme.controlPressed
                            : goalCloseButton.hovered
                                ? CompanionTheme.controlHover
                                : "transparent"
                    }
                }
            }

            Label {
                id: goalObjectiveLabel
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: goalPopup.contentWidth
                visible: root.shellModel !== null
                    && root.shellModel !== undefined
                    && !root.shellModel.goalEditing
                text: root.shellModel === null
                    || root.shellModel === undefined
                    ? ""
                    : root.shellModel.goalObjective
                color: CompanionTheme.textSecondary
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            TextArea {
                id: goalEditor
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: goalPopup.contentWidth
                Layout.preferredHeight: 84
                visible: root.shellModel !== null
                    && root.shellModel !== undefined
                    && root.shellModel.goalEditing
                enabled: visible
                    && !root.shellModel.goalMutationPending
                focus: visible
                text: root.shellModel === null
                    || root.shellModel === undefined
                    ? ""
                    : root.shellModel.goalDraftObjective
                color: CompanionTheme.textPrimary
                placeholderText: "Goal objective"
                placeholderTextColor: CompanionTheme.textMuted
                font.pixelSize: 11
                wrapMode: TextEdit.Wrap
                Accessible.name: "Goal objective"
                onVisibleChanged: {
                    if (visible) {
                        Qt.callLater(function() {
                            goalEditor.forceActiveFocus(
                                Qt.TabFocusReason)
                        })
                    }
                }
                onTextChanged: {
                    if (root.shellModel !== null
                            && root.shellModel !== undefined
                            && root.shellModel.goalDraftObjective !== text) {
                        root.shellModel.goalDraftObjective = text
                    }
                }

                background: Rectangle {
                    radius: 12
                    color: CompanionTheme.control
                    border.width: 1
                    border.color: goalEditor.activeFocus
                        ? CompanionTheme.accent
                        : CompanionTheme.border
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: goalPopup.contentWidth
                visible: text.length > 0
                text: root.shellModel === null
                    || root.shellModel === undefined
                    ? ""
                    : root.shellModel.goalErrorMessage
                color: CompanionTheme.danger
                font.pixelSize: 9
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.maximumWidth: goalPopup.contentWidth
                spacing: 6

                GoalActionButton {
                    id: goalPauseButton
                    interactionId: "goal.pause"
                    text: "Pause"
                    glyph: "\ue769"
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalCanPause
                        && !root.shellModel.goalEditing
                    enabled: visible
                        && !root.shellModel.goalMutationPending
                    onClicked: root.shellModel.pauseGoal()
                }

                GoalActionButton {
                    id: goalResumeButton
                    interactionId: "goal.resume"
                    text: "Resume"
                    glyph: "\ue768"
                    emphasized: true
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalCanResume
                        && !root.shellModel.goalEditing
                    enabled: visible
                        && !root.shellModel.goalMutationPending
                    onClicked: root.shellModel.resumeGoal()
                }

                GoalActionButton {
                    id: goalEditButton
                    interactionId: "goal.edit"
                    text: "Edit"
                    glyph: "\ue70f"
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalCanEdit
                        && !root.shellModel.goalEditing
                    enabled: visible
                        && !root.shellModel.goalMutationPending
                    onClicked:
                        root.shellModel.beginGoalEditing()
                }

                GoalActionButton {
                    id: goalCancelButton
                    interactionId: "goal.cancel"
                    text: "Cancel"
                    glyph: "\ue711"
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalEditing
                    enabled: visible
                        && !root.shellModel.goalMutationPending
                    onClicked:
                        root.shellModel.cancelGoalEditing()
                }

                GoalActionButton {
                    id: goalSaveButton
                    interactionId: "goal.save"
                    text: "Save"
                    glyph: "\ue73e"
                    emphasized: true
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalEditing
                    enabled: visible
                        && root.shellModel.goalCanEdit
                        && !root.shellModel.goalMutationPending
                    onClicked: root.shellModel.saveGoalEdit()
                }

                Item {
                    Layout.fillWidth: true
                }

                BusyIndicator {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    visible: root.shellModel !== null
                        && root.shellModel !== undefined
                        && root.shellModel.goalMutationPending
                    running: visible
                }
            }
        }
    }
}
