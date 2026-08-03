import QtQuick
import QtQuick.Controls.Basic
import CodexCompanion

Window {
    id: root

    property var petModel: null
    property var shellModel: null
    property var reactionModel: null
    property var movementController: null
    property bool liveAnimation: true
    property bool reduceMotion: false
    property bool dragGestureActive: false
    property bool pendingPointerHovered: false
    property bool pendingControlsHovered: false
    property var animationPlaybackModel: null
    readonly property int hoverSettleDuration: 24

    property alias artwork: artwork
    property alias spriteImage: artwork.image
    property alias dragSurface: dragSurface
    property alias controlCluster: controlCluster
    property alias controlHoverSurface:
        controlHoverSurface
    property alias usageButton: usageButton
    property alias processesButton: processesButton
    property alias menuButton: menuButton
    property alias menuGlyphItem:
        menuButton.glyphItem
    property alias menuBadgeItem:
        menuButton.badgeItem
    property alias contextMenu: petContextMenu
    property alias contextHideItem: contextHideItem
    property alias contextSettingsItem:
        contextSettingsItem
    property alias goalConfettiOverlay:
        goalConfettiOverlay
    readonly property bool showsProcesses:
        shellModel === null
        || shellModel === undefined
        || shellModel.routeMode === "processes"
    readonly property int activeProcessCount:
        shellModel === null
                || shellModel === undefined
            ? 0
            : shellModel.activeProcessCount
    readonly property bool showsActiveProcessBadge:
        petModel !== null
        && petModel !== undefined
        && !petModel.menuOpen
        && activeProcessCount > 0
    readonly property string activeProcessBadgeText:
        activeProcessCount > 9
            ? "9+"
            : String(activeProcessCount)
    readonly property color activeProcessAccent:
        shellModel !== null
                && shellModel !== undefined
                && shellModel.chatAccentColor !== undefined
            ? shellModel.chatAccentColor
            : CompanionTheme.accent
    readonly property color activeProcessAccentFill:
        Qt.rgba(
            activeProcessAccent.r,
            activeProcessAccent.g,
            activeProcessAccent.b,
            0.92)
    readonly property bool controlsRequestedVisible:
        petModel === null
        || petModel === undefined
        || petModel.controlsVisible
    readonly property bool menuExpanded:
        petModel !== null
        && petModel !== undefined
        && petModel.menuOpen
    readonly property bool animationEnabled:
        liveAnimation
        && visible
        && petModel !== null
        && petModel !== undefined
    readonly property int menuMotionDuration:
        reduceMotion ? 120 : 620
    property bool menuMotionReady: false
    property real controlsMotionClock: 1
    property real controlsMotionStart:
        controlsRequestedVisible ? 1 : 0
    property real controlsMotionTarget:
        controlsRequestedVisible ? 1 : 0
    property real expansionMotionClock: 1
    property real expansionMotionStart:
        menuExpanded ? 1 : 0
    property real expansionMotionTarget:
        menuExpanded ? 1 : 0
    property real badgeMotionClock: 1
    property real badgeMotionStart:
        showsActiveProcessBadge ? 1 : 0
    property real badgeMotionTarget:
        showsActiveProcessBadge ? 1 : 0
    readonly property real controlsMotionProgress:
        motionValue(
            controlsMotionStart,
            controlsMotionTarget,
            controlsMotionClock)
    readonly property real expansionMotionProgress:
        motionValue(
            expansionMotionStart,
            expansionMotionTarget,
            expansionMotionClock)
    readonly property real badgeMotionProgress:
        motionValue(
            badgeMotionStart,
            badgeMotionTarget,
            badgeMotionClock)
    readonly property real controlsVisualProgress:
        clampedProgress(controlsMotionProgress)
    readonly property real expansionVisualProgress:
        clampedProgress(expansionMotionProgress)
    readonly property real badgeVisualProgress:
        clampedProgress(badgeMotionProgress)

    signal usageRequested()
    signal modelPickerRequested()
    signal processesRequested()
    signal chatRequested()
    signal settingsRequested()
    signal hideRequested()

    objectName: "petWindow"
    visible: false
    width: 124
    height: 164
    minimumWidth: 124
    maximumWidth: 124
    minimumHeight: 164
    maximumHeight: 164
    title: "Codex Companion"
    color: "transparent"
    flags: Qt.Tool
        | Qt.FramelessWindowHint
        | Qt.WindowStaysOnTopHint

    onClosing: function(close) {
        close.accepted = false
        hideRequested()
    }

    onAnimationEnabledChanged:
        synchronizeAnimationPlayback()
    onPetModelChanged: {
        synchronizePendingHoverState()
        synchronizeAnimationPlayback()
    }
    onControlsRequestedVisibleChanged: {
        if (menuMotionReady) {
            startControlsMotion(
                controlsRequestedVisible ? 1 : 0)
        }
        Qt.callLater(reconcileWindowHover)
    }
    onMenuExpandedChanged: {
        if (menuMotionReady) {
            startExpansionMotion(
                menuExpanded ? 1 : 0)
        }
        Qt.callLater(reconcileWindowHover)
    }
    onShowsActiveProcessBadgeChanged: {
        if (menuMotionReady) {
            startBadgeMotion(
                showsActiveProcessBadge ? 1 : 0)
        }
    }

    function synchronizeAnimationPlayback() {
        if (animationPlaybackModel !== null
                && animationPlaybackModel !== undefined
                && animationPlaybackModel !== petModel
                && animationPlaybackModel.animationPlaybackEnabled
                    !== undefined) {
            animationPlaybackModel.animationPlaybackEnabled =
                false
        }
        animationPlaybackModel = petModel
        if (animationPlaybackModel !== null
                && animationPlaybackModel !== undefined
                && animationPlaybackModel.animationPlaybackEnabled
                    !== undefined) {
            animationPlaybackModel.animationPlaybackEnabled =
                animationEnabled
        }
    }

    function petModelValue(name, fallbackValue) {
        if (petModel === null
                || petModel === undefined) {
            return fallbackValue
        }
        const value = petModel[name]
        return value === undefined
            ? fallbackValue
            : value
    }

    function synchronizePendingHoverState() {
        pendingPointerHovered =
            petModel !== null
                && petModel !== undefined
            ? petModel.pointerHovered
            : false
        pendingControlsHovered =
            petModel !== null
                && petModel !== undefined
            ? petModel.controlsHovered
            : false
    }

    function publishPendingHoverState() {
        if (petModel === null
                || petModel === undefined
                || dragGestureActive
                || petModel.dragging
                || (petModel.pointerHovered
                        === pendingPointerHovered
                    && petModel.controlsHovered
                        === pendingControlsHovered)) {
            return
        }

        if (movementController !== null
                && movementController !== undefined
                && movementController
                    .publishHoverState !== undefined) {
            movementController.publishHoverState(
                pendingPointerHovered,
                pendingControlsHovered)
        } else {
            petModel.setHoverState(
                pendingPointerHovered,
                pendingControlsHovered)
        }
    }

    function queueHoverState(
        pointerHovered,
        controlsHovered) {
        const pendingChanged =
            pendingPointerHovered !== pointerHovered
            || pendingControlsHovered !== controlsHovered
        pendingPointerHovered = pointerHovered
        pendingControlsHovered = controlsHovered
        const modelMatches =
            petModel !== null
            && petModel !== undefined
            && petModel.pointerHovered === pointerHovered
            && petModel.controlsHovered === controlsHovered
        if (pointerHovered || controlsHovered) {
            hoverReconciliationTimer.stop()
            if (!modelMatches) {
                publishPendingHoverState()
            }
            return
        }
        if (pendingChanged || !modelMatches) {
            hoverReconciliationTimer.restart()
        }
    }

    function reconcileWindowHover() {
        if (petModel === null
                || petModel === undefined
                || dragGestureActive
                || petModel.dragging) {
            return
        }

        if (!windowHoverHandler.hovered) {
            queueHoverState(false, false)
            return
        }

        const position =
            windowHoverHandler.point.position
        const pointerHovered =
            position.x >= dragSurface.x
            && position.x
                < dragSurface.x + dragSurface.width
            && position.y >= dragSurface.y
            && position.y
                < dragSurface.y + dragSurface.height
        const controlsHovered =
            controlsRequestedVisible
            && position.x >= controlHoverSurface.x
            && position.x
                < controlHoverSurface.x
                    + controlHoverSurface.width
            && position.y >= controlHoverSurface.y
            && position.y
                < controlHoverSurface.y
                    + controlHoverSurface.height
        queueHoverState(
            pointerHovered,
            controlsHovered)
    }

    function clampedProgress(progress) {
        return Math.max(0, Math.min(1, progress))
    }

    function springProgress(progress) {
        const normalized =
            clampedProgress(progress)
        if (reduceMotion) {
            const remainder = 1 - normalized
            return 1 - remainder
                * remainder
                * remainder
        }
        const response = 0.42
        const dampingFraction = 0.88
        const angularFrequency =
            2 * Math.PI / response
        const dampedScale = Math.sqrt(
            1 - dampingFraction
                * dampingFraction)
        const dampedFrequency =
            angularFrequency * dampedScale
        const elapsedSeconds =
            normalized
            * menuMotionDuration
            / 1000
        const decay = Math.exp(
            -dampingFraction
            * angularFrequency
            * elapsedSeconds)
        const displacement =
            decay
            * (Math.cos(
                    dampedFrequency
                    * elapsedSeconds)
                + dampingFraction
                    / dampedScale
                    * Math.sin(
                        dampedFrequency
                        * elapsedSeconds))
        return 1 - displacement
    }

    function motionValue(start, target, clock) {
        return start
            + (target - start)
                * springProgress(clock)
    }

    function mixedColor(from, to, progress) {
        const amount =
            clampedProgress(progress)
        return Qt.rgba(
            from.r + (to.r - from.r) * amount,
            from.g + (to.g - from.g) * amount,
            from.b + (to.b - from.b) * amount,
            from.a + (to.a - from.a) * amount)
    }

    function startControlsMotion(target) {
        const current = controlsVisualProgress
        controlsMotionAnimation.stop()
        controlsMotionStart = current
        controlsMotionTarget = target
        controlsMotionClock = 0
        controlsMotionAnimation.start()
    }

    function startExpansionMotion(target) {
        const current = expansionVisualProgress
        expansionMotionAnimation.stop()
        expansionMotionStart = current
        expansionMotionTarget = target
        expansionMotionClock = 0
        expansionMotionAnimation.start()
    }

    function startBadgeMotion(target) {
        const current = badgeVisualProgress
        badgeMotionAnimation.stop()
        badgeMotionStart = current
        badgeMotionTarget = target
        badgeMotionClock = 0
        badgeMotionAnimation.start()
    }

    function synchronizeMenuMotion() {
        controlsMotionAnimation.stop()
        expansionMotionAnimation.stop()
        badgeMotionAnimation.stop()
        controlsMotionStart =
            controlsRequestedVisible ? 1 : 0
        controlsMotionTarget = controlsMotionStart
        controlsMotionClock = 1
        expansionMotionStart =
            menuExpanded ? 1 : 0
        expansionMotionTarget =
            expansionMotionStart
        expansionMotionClock = 1
        badgeMotionStart =
            showsActiveProcessBadge ? 1 : 0
        badgeMotionTarget = badgeMotionStart
        badgeMotionClock = 1
    }

    Component.onCompleted: {
        synchronizePendingHoverState()
        synchronizeMenuMotion()
        menuMotionReady = true
        synchronizeAnimationPlayback()
    }

    Component.onDestruction: {
        if (animationPlaybackModel !== null
                && animationPlaybackModel !== undefined
                && animationPlaybackModel.animationPlaybackEnabled
                    !== undefined) {
            animationPlaybackModel.animationPlaybackEnabled =
                false
        }
    }

    NumberAnimation {
        id: controlsMotionAnimation

        target: root
        property: "controlsMotionClock"
        from: 0
        to: 1
        duration: root.menuMotionDuration
        easing.type: Easing.Linear
        onFinished: {
            root.controlsMotionStart =
                root.controlsMotionTarget
            root.controlsMotionClock = 1
        }
    }

    NumberAnimation {
        id: expansionMotionAnimation

        target: root
        property: "expansionMotionClock"
        from: 0
        to: 1
        duration: root.menuMotionDuration
        easing.type: Easing.Linear
        onFinished: {
            root.expansionMotionStart =
                root.expansionMotionTarget
            root.expansionMotionClock = 1
        }
    }

    NumberAnimation {
        id: badgeMotionAnimation

        target: root
        property: "badgeMotionClock"
        from: 0
        to: 1
        duration: root.menuMotionDuration
        easing.type: Easing.Linear
        onFinished: {
            root.badgeMotionStart =
                root.badgeMotionTarget
            root.badgeMotionClock = 1
        }
    }

    Timer {
        id: hoverReconciliationTimer

        interval: root.hoverSettleDuration
        repeat: false
        onTriggered:
            root.publishPendingHoverState()
    }

    HoverHandler {
        id: windowHoverHandler

        acceptedDevices: PointerDevice.Mouse
        onHoveredChanged:
            root.reconcileWindowHover()
        onPointChanged:
            root.reconcileWindowHover()
    }

    Connections {
        target: root.petModel
        enabled: root.petModel !== null
            && root.petModel !== undefined

        function onDraggingChanged() {
            root.dragGestureActive =
                root.petModel.dragging
            if (!root.dragGestureActive) {
                Qt.callLater(
                    root.reconcileWindowHover)
            }
        }

        function onHoverChanged() {
            if (!hoverReconciliationTimer.running) {
                root.synchronizePendingHoverState()
            }
        }
    }

    PetSpriteSheet {
        id: artwork

        x: 12
        y: 48
        width: 100
        height: 108
        source: root.petModelValue(
            "spriteSheetSource",
            "")
        atlasColumns: root.petModelValue(
            "spriteColumns",
            16)
        atlasRows: root.petModelValue(
            "spriteRows",
            12)
        sourceFrameWidth: root.petModelValue(
            "sourceFrameWidth",
            192)
        sourceFrameHeight: root.petModelValue(
            "sourceFrameHeight",
            208)
        frameRow: root.petModel === null
                || root.petModel === undefined
            ? 0
            : root.petModel.frameRow
        frameColumn: root.petModel === null
                || root.petModel === undefined
            ? 0
            : root.petModel.frameColumn
    }

    GoalConfettiOverlay {
        id: goalConfettiOverlay

        x: 0
        y: root.height - height
        width: 124
        height: 124
        z: 4
        liveAnimation: root.liveAnimation
        trigger: root.reactionModel !== null
                && root.reactionModel !== undefined
                && root.reactionModel
                    .goalConfettiTrigger !== undefined
            ? root.reactionModel.goalConfettiTrigger
            : 0
    }

    Menu {
        id: petContextMenu

        popupType: Popup.Window
        width: 184
        padding: 6
        modal: false
        dim: false
        closePolicy: Popup.CloseOnEscape
            | Popup.CloseOnPressOutside

        PetContextMenuItem {
            id: contextHideItem
            interactionId: "pet.context.hide"
            text: "Hide Pet"
            onTriggered: root.hideRequested()
        }

        PetContextMenuItem {
            id: contextSettingsItem
            interactionId: "pet.context.settings"
            text: "Open Settings"
            onTriggered: root.settingsRequested()
        }

        background: Rectangle {
            radius: 8
            color: CompanionTheme.materialChrome
            border.width: 1
            border.color: CompanionTheme.border
        }
    }

    MouseArea {
        id: dragSurface
        property string interactionId: "pet.context.open"

        x: 12
        y: 48
        width: 100
        height: 108
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
            | Qt.RightButton
        preventStealing: true
        cursorShape: Qt.ArrowCursor

        onPressed: function(mouse) {
            if (mouse.button !== Qt.LeftButton) {
                return
            }
            root.dragGestureActive = true
            if (root.movementController !== null
                    && root.movementController !== undefined) {
                root.movementController.beginDrag()
            } else if (root.petModel !== null
                    && root.petModel !== undefined) {
                root.petModel.beginDrag()
            }
        }

        onPositionChanged: function(mouse) {
            if (!root.dragGestureActive
                    || !(mouse.buttons & Qt.LeftButton)) {
                return
            }
            if (root.movementController !== null
                    && root.movementController !== undefined) {
                root.movementController.dragTo()
            }
        }

        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton
                    || !root.dragGestureActive) {
                return
            }
            root.dragGestureActive = false
            if (root.movementController !== null
                    && root.movementController !== undefined) {
                root.movementController.endDrag()
            } else if (root.petModel !== null
                    && root.petModel !== undefined) {
                root.petModel.endDrag()
            }
        }

        onCanceled: {
            if (!root.dragGestureActive) {
                return
            }
            if (root.movementController !== null
                    && root.movementController !== undefined) {
                return
            }
            root.dragGestureActive = false
            if (root.petModel !== null
                    && root.petModel !== undefined) {
                root.petModel.endDrag()
            }
        }

        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                petContextMenu.popup(
                    dragSurface,
                    mouse.x,
                    mouse.y)
            }
        }
    }

    Item {
        id: controlHoverSurface
        x: root.width - width - 4
        y: 8
        width: root.petModel !== null
                && root.petModel !== undefined
                && root.petModel.menuOpen
            ? 116
            : 36
        height: 48
        enabled: root.controlsRequestedVisible
        z: 3
    }

    Item {
        id: controlCluster

        x: 4
        y: 12 - 4 * root.controlsVisualProgress
        width: 116
        height: 48
        visible: root.controlsRequestedVisible
            || root.controlsVisualProgress > 0.001
            || controlsMotionAnimation.running
        enabled: root.controlsRequestedVisible
        opacity: root.controlsVisualProgress
        scale: 0.94
            + 0.06
                * root.controlsVisualProgress
        transformOrigin: Item.BottomRight
        z: 2

        PetControlButton {
            id: usageButton
            interactionId: root.showsProcesses
                ? "pet.usage.toggle"
                : "pet.model.toggle"

            x: 25.92
                * (1 - root.expansionVisualProgress)
            visible: root.menuExpanded
                || root.expansionVisualProgress > 0.001
                || expansionMotionAnimation.running
            enabled: root.menuExpanded
            opacity: root.expansionVisualProgress
            scale: 0.72
                + 0.28
                    * root.expansionVisualProgress
            transformOrigin: Item.Right
            glyph: root.showsProcesses
                ? root.shellModel !== null
                    && root.shellModel !== undefined
                    && root.shellModel.usageLoading
                    ? "\ue917"
                    : "\ue9d2"
                : "\ue945"
            accessibleName: root.showsProcesses
                ? "Codex usage and resets"
                : "Chat service and model"
            toolTipText: accessibleName
            onClicked: {
                if (root.showsProcesses) {
                    root.usageRequested()
                } else {
                    root.modelPickerRequested()
                }
            }
        }

        PetControlButton {
            id: processesButton
            interactionId: "pet.route.toggle"

            x: 40
                + 25.92
                    * (1 - root.expansionVisualProgress)
            visible: root.menuExpanded
                || root.expansionVisualProgress > 0.001
                || expansionMotionAnimation.running
            enabled: root.menuExpanded
            opacity: root.expansionVisualProgress
            scale: 0.72
                + 0.28
                    * root.expansionVisualProgress
            transformOrigin: Item.Right
            glyph: root.showsProcesses
                ? "\ue8bd"
                : "\ue8fd"
            accessibleName: root.showsProcesses
                ? "Open local chat"
                : "Show Codex processes"
            toolTipText: accessibleName
            onClicked: {
                if (root.showsProcesses) {
                    root.chatRequested()
                } else {
                    root.processesRequested()
                }
            }
        }

        PetControlButton {
            id: menuButton
            interactionId: "pet.menu.toggle"

            x: 80
            glyph: "\ue70d"
            selected: root.menuExpanded
            glyphRotation:
                180 * root.expansionVisualProgress
            badgeText: root.showsActiveProcessBadge
                ? root.activeProcessBadgeText
                : ""
            accessibleName: root.petModel !== null
                    && root.petModel !== undefined
                    && root.petModel.menuOpen
                ? "Hide menu"
                : root.activeProcessCount === 1
                    ? "Show menu - 1 active Codex process"
                    : root.activeProcessCount > 1
                        ? "Show menu - "
                            + root.activeProcessCount
                            + " active Codex processes"
                : "Show menu"
            toolTipText: root.petModel !== null
                    && root.petModel !== undefined
                    && root.petModel.menuOpen
                ? "Hide menu"
                : root.activeProcessCount === 1
                    ? "Show menu - 1 active Codex process"
                    : root.activeProcessCount > 1
                        ? "Show menu - "
                            + root.activeProcessCount
                            + " active Codex processes"
                : "Show menu"
            onClicked: {
                if (root.petModel !== null
                        && root.petModel !== undefined) {
                    if (root.petModel.menuOpen) {
                        root.petModel.menuOpen = false
                    } else {
                        root.processesRequested()
                    }
                }
            }
        }
    }

    component PetControlButton: ToolButton {
        id: control

        required property string interactionId
        required property string glyph
        required property string accessibleName
        required property string toolTipText
        property string badgeText: ""
        property real glyphRotation: 0
        property bool selected: false
        property real horizontalScale:
            control.hovered ? 1.014 : 1
        property real verticalScale:
            control.hovered ? 0.992 : 1
        property alias glyphItem: glyphLabel
        property alias badgeItem: badgeLabel
        readonly property bool showsBadge:
            badgeText.length > 0

        width: 36
        height: 48
        padding: 4
        text: showsBadge ? badgeText : glyph
        font.family: showsBadge
            ? "Segoe UI"
            : "Segoe Fluent Icons"
        font.pixelSize: showsBadge
                && badgeText.length > 1
            ? 11
            : 12
        font.weight: showsBadge
            ? Font.DemiBold
            : Font.Normal
        Accessible.name: accessibleName
        ToolTip.text: toolTipText
        ToolTip.visible: hovered
        ToolTip.delay: 450

        Behavior on horizontalScale {
            NumberAnimation {
                duration: 160
                easing.type: Easing.OutCubic
            }
        }

        Behavior on verticalScale {
            NumberAnimation {
                duration: 160
                easing.type: Easing.OutCubic
            }
        }

        contentItem: Item {
            id: controlContent

            transform: Scale {
                origin.x: controlContent.width / 2
                origin.y: controlContent.height / 2
                xScale: control.horizontalScale
                yScale: control.verticalScale
            }

            Label {
                id: glyphLabel

                anchors.fill: parent
                text: control.glyph
                color: CompanionTheme.textPrimary
                font.family: "Segoe Fluent Icons"
                font.pixelSize: 12
                font.weight: Font.Normal
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                opacity:
                    1 - root.badgeVisualProgress
                scale: 1
                    - 0.28
                        * root.badgeVisualProgress
                rotation: control.glyphRotation
            }

            Label {
                id: badgeLabel

                anchors.fill: parent
                anchors.verticalCenterOffset: -0.5
                text: control.badgeText
                color: "white"
                font.family: "Segoe UI"
                font.pixelSize:
                    control.badgeText.length > 1
                        ? 11
                        : 12
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                opacity: root.badgeVisualProgress
                scale: 0.72
                    + 0.28
                        * root.badgeVisualProgress
                rotation: -18
                    * (1 - root.badgeVisualProgress)
            }
        }

        background: Rectangle {
            id: controlBackground

            anchors.centerIn: parent
            width: 28
            height: 28
            radius: 14
            readonly property color restingColor:
                control.down
                ? CompanionTheme.controlPressed
                : control.selected
                    ? CompanionTheme.controlSelected
                : control.hovered
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
            readonly property color restingBorderColor:
                control.activeFocus
                ? CompanionTheme.accentMuted
                : control.selected
                    ? CompanionTheme.accentMuted
                    : CompanionTheme.border
            color: root.mixedColor(
                restingColor,
                root.activeProcessAccentFill,
                root.badgeVisualProgress)
            border.width: 1
            border.color: root.mixedColor(
                restingBorderColor,
                root.activeProcessAccent,
                root.badgeVisualProgress)

            transform: Scale {
                origin.x: controlBackground.width / 2
                origin.y: controlBackground.height / 2
                xScale: control.horizontalScale
                yScale: control.verticalScale
            }
        }
    }

    component PetContextMenuItem: MenuItem {
        id: contextItem

        required property string interactionId
        implicitWidth: 172
        implicitHeight: 30
        leftPadding: 10
        rightPadding: 10

        contentItem: Label {
            text: contextItem.text
            color: contextItem.enabled
                ? CompanionTheme.textPrimary
                : CompanionTheme.textMuted
            font.pixelSize: 11
            font.weight: Font.Medium
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: 6
            color: contextItem.down
                ? CompanionTheme.controlPressed
                : contextItem.highlighted
                    ? CompanionTheme.controlHover
                    : "transparent"
        }
    }
}
