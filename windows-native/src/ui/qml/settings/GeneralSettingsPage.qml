import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var settingsModel: null
    property var usageModel: null
    property var petModel: null
    property alias backdropSelector: backdropSelector
    property alias petSelector: petSelector
    property alias previewAnimationSelector: previewAnimationSelector
    property alias previewAnimationLabel: previewAnimationLabel
    property alias hideControlsSwitch: hideControlsSwitch
    property alias autonomousMovementSwitch: autonomousMovementSwitch
    property alias animationPacingSlider: animationPacingSlider
    property alias animationPacingLabel: animationPacingLabel
    property alias hideControlsLabel: hideControlsLabel
    property alias autonomousMovementLabel: autonomousMovementLabel
    property alias appearanceRowLabel: appearanceRowLabel
    property alias petSectionLabel: petSectionLabel
    property alias backdropSelectorBackground: backdropSelectorBackground
    property alias usageSectionLabel: usageSectionLabel
    property alias usageRemainingLabel: usageRemainingLabel
    property alias usageSummaryLabel: usageSummaryLabel
    property alias usageDisclosureLabel: usageDisclosureLabel
    property alias usageRefreshButton: usageRefreshButton
    property alias reloadPetsButton: reloadPetsButton

    spacing: 0

    ListModel {
        id: petOptionsModel
    }

    function modelValue(name, fallbackValue) {
        if (settingsModel === null || settingsModel === undefined) {
            return fallbackValue
        }
        const value = settingsModel[name]
        return value === undefined ? fallbackValue : value
    }

    function setModelValue(name, value) {
        if (settingsModel !== null && settingsModel !== undefined) {
            settingsModel[name] = value
        }
    }

    function usageModelValue(name, fallbackValue) {
        if (usageModel === null || usageModel === undefined) {
            return fallbackValue
        }
        const value = usageModel[name]
        return value === undefined ? fallbackValue : value
    }

    function usageSummary() {
        const snapshot = usageModelValue(
            "usageSnapshot",
            ({}))
        const snapshotPresent = snapshot !== null
            && Object.keys(snapshot).length > 0
        if (usageModelValue("usageLoading", false)
                && !snapshotPresent) {
            return "Checking usage..."
        }

        const groups = snapshot !== null
                && snapshot.groups !== undefined
            ? snapshot.groups
            : []
        if (groups.length === 0) {
            const errorMessage = usageModelValue(
                "usageErrorMessage",
                "")
            return errorMessage.length > 0
                ? errorMessage
                : "Rate limits unavailable"
        }

        const main = groups[0]
        const parts = []
        if (main.shortWindow !== undefined
                && main.shortWindow !== null) {
            const remaining = Number(
                main.shortWindow.remainingPercent)
            if (isFinite(remaining)) {
                parts.push(
                    Math.round(remaining)
                    + "% short left")
            }
        }
        if (main.weeklyWindow !== undefined
                && main.weeklyWindow !== null) {
            const remaining = Number(
                main.weeklyWindow.remainingPercent)
            if (isFinite(remaining)) {
                parts.push(
                    Math.round(remaining)
                    + "% weekly left")
            }
        }

        const resetCount = Number(
            snapshot.availableResetCount)
        if (isFinite(resetCount) && resetCount > 0) {
            parts.push(
                Math.round(resetCount)
                + (Math.round(resetCount) === 1
                    ? " reset"
                    : " resets"))
        }
        return parts.join(" \u00b7 ")
    }

    function petModelValue(name, fallbackValue) {
        if (petModel === null || petModel === undefined) {
            return fallbackValue
        }
        const value = petModel[name]
        return value === undefined ? fallbackValue : value
    }

    function synchronizePreviewAnimation() {
        const selected = petModelValue(
            "selectedAnimation",
            "idle")
        const index =
            previewAnimationSelector.indexOfValue(
                selected)
        previewAnimationSelector.currentIndex =
            index >= 0 ? index : 0
    }

    function synchronizePetSelection() {
        const selected = petModelValue(
            "selectedPetId",
            "")
        let index = -1
        for (let candidate = 0;
             candidate < petOptionsModel.count;
             ++candidate) {
            if (petOptionsModel.get(
                    candidate).id === selected) {
                index = candidate
                break
            }
        }
        petSelector.currentIndex =
            index >= 0 ? index : 0
    }

    function refreshPetOptions() {
        const pets = petModelValue(
            "availablePets",
            [])
        petOptionsModel.clear()
        for (let index = 0;
             index < pets.length;
             ++index) {
            petOptionsModel.append({
                "id": String(pets[index].id),
                "label": String(
                    pets[index].label)
            })
        }
        synchronizePetSelection()
    }

    onPetModelChanged: {
        refreshPetOptions()
        Qt.callLater(synchronizePreviewAnimation)
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: appearanceRowLabel
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Appearance"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        ComboBox {
            id: backdropSelector
            property string interactionId:
                "settings.appearance"
            Accessible.name: "Appearance"
            textRole: "text"
            valueRole: "value"
            implicitHeight: 28
            implicitWidth: 168
            model: [
                { text: "Mica", value: "mica" },
                { text: "Windows Glass", value: "windows-glass" },
                { text: "Solid Black", value: "solid-black" }
            ]
            Component.onCompleted: currentIndex = indexOfValue(root.modelValue("backdropMode", "mica"))
            onActivated: root.setModelValue("backdropMode", currentValue)
            Connections {
                target: root.settingsModel
                ignoreUnknownSignals: true
                function onBackdropModeChanged() {
                    backdropSelector.currentIndex = backdropSelector.indexOfValue(root.modelValue("backdropMode", "mica"))
                }
            }

            delegate: ItemDelegate {
                required property int index
                width: backdropSelector.width
                height: 28
                highlighted: backdropSelector.highlightedIndex === index

                contentItem: Label {
                    text: backdropSelector.textAt(parent.index)
                    color: parent.highlighted ? CompanionTheme.accentText : CompanionTheme.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 4
                    color: parent.highlighted ? CompanionTheme.accent : (parent.hovered ? CompanionTheme.controlHover : CompanionTheme.surfaceRaised)
                }
            }

            contentItem: Label {
                leftPadding: 10
                rightPadding: 28
                text: backdropSelector.displayText
                color: CompanionTheme.textPrimary
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: backdropSelector.width - width - 10
                y: (backdropSelector.height - height) / 2
                width: 9
                height: 6
                contextType: "2d"

                onPaint: {
                    context.reset()
                    context.strokeStyle = CompanionTheme.textSecondary
                    context.lineWidth = 1.5
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    context.beginPath()
                    context.moveTo(1, 1)
                    context.lineTo(width / 2, height - 1)
                    context.lineTo(width - 1, 1)
                    context.stroke()
                }
            }

            background: Rectangle {
                id: backdropSelectorBackground
                radius: CompanionTheme.radius
                color: backdropSelector.down ? CompanionTheme.controlPressed : backdropSelector.hovered ? CompanionTheme.controlHover : CompanionTheme.control
                border.color: backdropSelector.activeFocus ? CompanionTheme.accent : CompanionTheme.border
            }

            popup: Popup {
                y: backdropSelector.height + 4
                width: backdropSelector.width
                implicitHeight: contentItem.implicitHeight + 2
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: backdropSelector.popup.visible ? backdropSelector.delegateModel : null
                    currentIndex: backdropSelector.highlightedIndex
                }

                background: Rectangle {
                    radius: CompanionTheme.radius
                    color: CompanionTheme.surfaceRaised
                    border.color: CompanionTheme.border
                }
            }
        }
    }

    Separator {}

    Label {
        id: petSectionLabel
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "Pet"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Pet"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        ComboBox {
            id: petSelector

            property string interactionId:
                "settings.pet.select"
            Accessible.name: "Pet"
            textRole: "label"
            valueRole: "id"
            implicitHeight: 28
            implicitWidth: 168
            enabled: root.petModel !== null
                && root.petModel !== undefined
                && typeof root.petModel.selectPet
                    === "function"
                && count > 0
            model: petOptionsModel
            onCountChanged:
                root.synchronizePetSelection()
            Component.onCompleted:
                root.refreshPetOptions()
            onActivated: {
                if (!enabled
                        || root.petModel.selectPet(
                            currentValue)) {
                    return
                }
                root.synchronizePetSelection()
            }

            Connections {
                target: root.petModel
                ignoreUnknownSignals: true

                function onAvailablePetsChanged() {
                    root.refreshPetOptions()
                }

                function onSelectedPetChanged() {
                    root.synchronizePetSelection()
                }

                function onSelectedPetIdChanged() {
                    root.synchronizePetSelection()
                }
            }

            delegate: ItemDelegate {
                required property int index

                width: petSelector.width
                height: 28
                highlighted:
                    petSelector.highlightedIndex
                        === index

                contentItem: Label {
                    text: petSelector.textAt(
                        parent.index)
                    color: parent.highlighted
                        ? CompanionTheme.accentText
                        : CompanionTheme.textPrimary
                    font.pixelSize: 12
                    verticalAlignment:
                        Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 4
                    color: parent.highlighted
                        ? CompanionTheme.accent
                        : parent.hovered
                            ? CompanionTheme.controlHover
                            : CompanionTheme.surfaceRaised
                }
            }

            contentItem: Label {
                leftPadding: 10
                rightPadding: 28
                text: petSelector.displayText
                color: petSelector.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: petSelector.width
                    - width - 10
                y: (petSelector.height
                    - height) / 2
                width: 9
                height: 6
                contextType: "2d"

                onPaint: {
                    context.reset()
                    context.strokeStyle =
                        CompanionTheme.textSecondary
                    context.lineWidth = 1.5
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    context.beginPath()
                    context.moveTo(1, 1)
                    context.lineTo(
                        width / 2,
                        height - 1)
                    context.lineTo(width - 1, 1)
                    context.stroke()
                }
            }

            background: Rectangle {
                radius: CompanionTheme.radius
                color: petSelector.down
                    ? CompanionTheme.controlPressed
                    : petSelector.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color:
                    petSelector.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
            }

            popup: Popup {
                y: petSelector.height + 4
                width: petSelector.width
                implicitHeight:
                    Math.min(
                        contentItem.contentHeight,
                        8 * 28)
                    + 2
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight:
                        Math.min(
                            contentHeight,
                            8 * 28)
                    model: petSelector.popup.visible
                        ? petSelector.delegateModel
                        : null
                    currentIndex:
                        petSelector.highlightedIndex
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOff
                    }
                }

                background: Rectangle {
                    radius: CompanionTheme.radius
                    color: CompanionTheme.surfaceRaised
                    border.color: CompanionTheme.border
                }
            }
        }
    }

    Separator {}

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: previewAnimationLabel
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Preview animation"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        ComboBox {
            id: previewAnimationSelector
            property string interactionId:
                "settings.pet.preview-animation"
            Accessible.name: "Preview animation"
            textRole: "text"
            valueRole: "value"
            implicitHeight: 28
            implicitWidth: 168
            enabled: root.petModel !== null
                && root.petModel !== undefined
                && typeof root.petModel
                    .setSelectedAnimation === "function"
            model: [
                { text: "Idle", value: "idle" },
                { text: "Run Right", value: "running-right" },
                { text: "Run Left", value: "running-left" },
                { text: "Wave", value: "waving" },
                { text: "Jump", value: "jumping" },
                { text: "Failed", value: "failed" },
                { text: "Waiting", value: "waiting" },
                { text: "Running", value: "running" },
                { text: "Review", value: "review" },
                { text: "Goal Complete", value: "goal-complete" },
                { text: "Thinking", value: "thinking" },
                { text: "Talking", value: "talking" }
            ]
            Component.onCompleted:
                root.synchronizePreviewAnimation()
            onActivated: {
                if (enabled) {
                    root.petModel.setSelectedAnimation(
                        currentValue)
                }
            }
            Connections {
                target: root.petModel
                ignoreUnknownSignals: true
                function onSelectedAnimationChanged() {
                    root.synchronizePreviewAnimation()
                }
            }

            delegate: ItemDelegate {
                required property int index
                width: previewAnimationSelector.width
                height: 28
                highlighted:
                    previewAnimationSelector
                        .highlightedIndex === index

                contentItem: Label {
                    text: previewAnimationSelector
                        .textAt(parent.index)
                    color: parent.highlighted
                        ? CompanionTheme.accentText
                        : CompanionTheme.textPrimary
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 4
                    color: parent.highlighted
                        ? CompanionTheme.accent
                        : parent.hovered
                            ? CompanionTheme.controlHover
                            : CompanionTheme.surfaceRaised
                }
            }

            contentItem: Label {
                leftPadding: 10
                rightPadding: 28
                text: previewAnimationSelector.displayText
                color: previewAnimationSelector.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: previewAnimationSelector.width
                    - width - 10
                y: (previewAnimationSelector.height
                    - height) / 2
                width: 9
                height: 6
                contextType: "2d"

                onPaint: {
                    context.reset()
                    context.strokeStyle =
                        CompanionTheme.textSecondary
                    context.lineWidth = 1.5
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    context.beginPath()
                    context.moveTo(1, 1)
                    context.lineTo(
                        width / 2,
                        height - 1)
                    context.lineTo(width - 1, 1)
                    context.stroke()
                }
            }

            background: Rectangle {
                radius: CompanionTheme.radius
                color: previewAnimationSelector.down
                    ? CompanionTheme.controlPressed
                    : previewAnimationSelector.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color:
                    previewAnimationSelector.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
            }

            popup: Popup {
                y: previewAnimationSelector.height + 4
                width: previewAnimationSelector.width
                implicitHeight:
                    Math.min(
                        contentItem.contentHeight,
                        12 * 28)
                    + 2
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight:
                        Math.min(
                            contentHeight,
                            12 * 28)
                    model:
                        previewAnimationSelector
                            .popup.visible
                        ? previewAnimationSelector
                            .delegateModel
                        : null
                    currentIndex:
                        previewAnimationSelector
                            .highlightedIndex
                }

                background: Rectangle {
                    radius: CompanionTheme.radius
                    color: CompanionTheme.surfaceRaised
                    border.color: CompanionTheme.border
                }
            }
        }
    }

    Separator {}

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: animationPacingLabel
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Animation pacing"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        RowLayout {
            spacing: 8

            Label {
                text: "Faster"
                color: CompanionTheme.textMuted
                font.pixelSize: 11
            }

            Slider {
                id: animationPacingSlider
                property string interactionId:
                    "settings.animation.pacing"
                Accessible.name: "Animation pacing"
                Layout.preferredWidth: 148
                implicitHeight: 24
                from: 0.75
                to: 2.5
                stepSize: 0.05
                snapMode: Slider.SnapAlways
                value: root.modelValue("animationSpeedScale", 1.15)
                onMoved: root.setModelValue("animationSpeedScale", value)
                Connections {
                    target: root.settingsModel
                    ignoreUnknownSignals: true
                    function onAnimationSpeedScaleChanged() {
                        animationPacingSlider.value = root.modelValue("animationSpeedScale", 1.15)
                    }
                }

                background: Rectangle {
                    x: animationPacingSlider.leftPadding
                    y: animationPacingSlider.topPadding + animationPacingSlider.availableHeight / 2 - height / 2
                    width: animationPacingSlider.availableWidth
                    height: 3
                    radius: 2
                    color: CompanionTheme.control

                    Rectangle {
                        width: animationPacingSlider.visualPosition * parent.width
                        height: parent.height
                        radius: 2
                        color: CompanionTheme.accent
                    }
                }

                handle: Rectangle {
                    x: animationPacingSlider.leftPadding + animationPacingSlider.visualPosition * (animationPacingSlider.availableWidth - width)
                    y: animationPacingSlider.topPadding + animationPacingSlider.availableHeight / 2 - height / 2
                    width: 10
                    height: 18
                    radius: 3
                    color: CompanionTheme.accent
                }
            }

            Label {
                text: "Slower"
                color: CompanionTheme.textMuted
                font.pixelSize: 11
            }
        }
    }

    Separator {}

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: hideControlsLabel
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Hide tray controls until pet hover"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        CompactSwitch {
            id: hideControlsSwitch
            interactionId:
                "settings.controls.hover-only"
            Accessible.name: "Hide tray controls until pet hover"
            checked: root.modelValue("hideControlsUntilHover", false)
            onToggled: root.setModelValue("hideControlsUntilHover", checked)
            Connections {
                target: root.settingsModel
                ignoreUnknownSignals: true
                function onHideControlsUntilHoverChanged() {
                    hideControlsSwitch.checked = root.modelValue("hideControlsUntilHover", false)
                }
            }
        }
    }

    Separator {}

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: autonomousMovementLabel
            Layout.fillWidth: true
            Layout.maximumWidth: 292
            text: "Allow autonomous movement"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        CompactSwitch {
            id: autonomousMovementSwitch
            interactionId:
                "settings.movement.autonomous"
            Accessible.name: "Allow autonomous movement"
            checked: root.modelValue("allowAutonomousMovement", true)
            onToggled: root.setModelValue("allowAutonomousMovement", checked)
            Connections {
                target: root.settingsModel
                ignoreUnknownSignals: true
                function onAllowAutonomousMovementChanged() {
                    autonomousMovementSwitch.checked = root.modelValue("allowAutonomousMovement", true)
                }
            }
        }
    }

    Separator {}

    Button {
        id: reloadPetsButton

        property string interactionId:
            "settings.pet.reload"
        Layout.topMargin: 10
        Layout.preferredWidth: 124
        Layout.preferredHeight: 30
        text: "Reload Pets"
        icon.name: "view-refresh"
        icon.width: 14
        icon.height: 14
        Accessible.name: "Reload Pets"
        enabled: root.petModel !== null
            && root.petModel !== undefined
            && typeof root.petModel.reloadPets
                === "function"
        onClicked: {
            if (root.petModel.reloadPets()) {
                Qt.callLater(
                    root.refreshPetOptions)
            }
        }

        contentItem: RowLayout {
            spacing: 6

            Image {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                source:
                    reloadPetsButton.icon.source
                sourceSize.width: 14
                sourceSize.height: 14
                visible: status === Image.Ready
            }

            Label {
                Layout.fillWidth: true
                text: reloadPetsButton.text
                color: reloadPetsButton.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: 12
                font.weight: Font.Medium
                horizontalAlignment:
                    Text.AlignHCenter
                verticalAlignment:
                    Text.AlignVCenter
            }
        }

        background: Rectangle {
            radius: CompanionTheme.radius
            color: reloadPetsButton.down
                ? CompanionTheme.controlPressed
                : reloadPetsButton.hovered
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
            border.color:
                reloadPetsButton.activeFocus
                ? CompanionTheme.accent
                : CompanionTheme.border
        }
    }

    Separator {
        Layout.topMargin: 10
    }

    Label {
        id: usageSectionLabel
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "Codex Usage"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            id: usageRemainingLabel
            Layout.preferredWidth: 92
            text: "Remaining"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        Label {
            id: usageSummaryLabel
            Layout.fillWidth: true
            text: root.usageSummary()
            color: CompanionTheme.textSecondary
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }

    Label {
        id: usageDisclosureLabel
        Layout.fillWidth: true
        Layout.topMargin: 6
        text:
            "Banked resets are only applied after explicit confirmation."
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Button {
        id: usageRefreshButton
        property string interactionId:
            "settings.usage.refresh"
        Layout.topMargin: 12
        Layout.preferredWidth: 132
        Layout.preferredHeight: 30
        text: "Refresh Usage"
        enabled: root.usageModel !== null
            && root.usageModel !== undefined
            && typeof root.usageModel.refreshUsage
                === "function"
            && !root.usageModelValue(
                "usageLoading",
                false)
        Accessible.name: enabled
            ? "Refresh Usage"
            : "Refreshing Usage"
        onClicked: root.usageModel.refreshUsage()

        contentItem: Label {
            text: usageRefreshButton.text
            color: usageRefreshButton.enabled
                ? CompanionTheme.accentText
                : CompanionTheme.textMuted
            font.pixelSize: 12
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: CompanionTheme.radius
            color: usageRefreshButton.enabled
                ? usageRefreshButton.down
                    ? CompanionTheme.accentPressed
                    : usageRefreshButton.hovered
                        ? CompanionTheme.accentHover
                        : CompanionTheme.accent
                : CompanionTheme.control
            border.color: usageRefreshButton.activeFocus
                ? CompanionTheme.textPrimary
                : "transparent"
        }
    }

    Item {
        Layout.fillHeight: true
        Layout.fillWidth: true
    }

    component Separator: Rectangle {
        Layout.fillWidth: true
        height: 1
        color: CompanionTheme.separator
    }

    component CompactSwitch: Switch {
        required property string interactionId
        implicitWidth: 40
        implicitHeight: 24
        indicator: Rectangle {
            implicitWidth: 40
            implicitHeight: 22
            x: parent.leftPadding
            y: parent.topPadding + (parent.availableHeight - height) / 2
            radius: height / 2
            color: parent.checked ? CompanionTheme.accent : CompanionTheme.control
            border.color: parent.activeFocus ? CompanionTheme.textPrimary : CompanionTheme.border

            Rectangle {
                x: parent.parent.checked ? parent.width - width - 3 : 3
                y: 3
                width: 16
                height: 16
                radius: 8
                color: parent.parent.checked ? CompanionTheme.accentText : CompanionTheme.textPrimary
            }
        }

        contentItem: Item {}
    }
}
