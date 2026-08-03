import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CodexCompanion

Window {
    id: root

    property var shellModel: null
    property var settingsModel: null
    property var backdropState: null
    readonly property var snapshot:
        shellModel === null
        || shellModel === undefined
            ? ({})
            : shellModel.usageSnapshot
    readonly property var usageGroups:
        snapshot.groups === undefined
            ? []
            : snapshot.groups
    readonly property var resetCredits:
        snapshot.availableResetCredits === undefined
            ? []
            : snapshot.availableResetCredits
    readonly property var resetConfirmationData:
        shellModel === null
                || shellModel === undefined
            ? ({})
            : shellModel.usageResetConfirmation
    readonly property bool resetConfirmationOpen:
        resetConfirmationData.creditId !== undefined
    readonly property bool hasSnapshot:
        usageGroups.length > 0
    readonly property string effectiveBackdropMode: {
        if (backdropState !== null
                && backdropState !== undefined
                && backdropState.usageEffectiveMode !== undefined) {
            return backdropState.usageEffectiveMode
        }
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel.effectiveBackdropMode === undefined) {
            return "mica"
        }
        return settingsModel.effectiveBackdropMode
    }
    readonly property bool enhancedBackdropActive:
        effectiveBackdropMode !== "solid-black"
    readonly property bool nativeBackdropRegionEnabled: true
    readonly property real nativeBackdropRegionInsetLeft: 0
    readonly property real nativeBackdropRegionInsetTop: 0
    readonly property real nativeBackdropRegionInsetRight: 0
    readonly property real nativeBackdropRegionInsetBottom: 0
    readonly property real nativeBackdropRegionRadius: 20

    property alias closeButton: closeButton
    property alias refreshButton: refreshButton
    property alias accountSelector: accountSelector
    property alias usageScrollBar: usageScrollBar
    property alias groupRepeater: groupRepeater
    property alias statusLabel: statusLabel
    property alias resetCreditRepeater:
        resetCreditRepeater
    property alias resetConfirmation:
        resetConfirmationSurface
    property alias resetConfirmationTitle:
        resetConfirmationTitle
    property alias resetCancelButton:
        resetCancelButton
    property alias resetApplyButton:
        resetApplyButton
    property alias resetStatusLabel:
        resetStatusLabel

    signal closeRequested()

    function updatedDetail() {
        if (snapshot.updatedAt === undefined) {
            return hasSnapshot
                ? "Current limits"
                : "Not loaded"
        }
        const date = new Date(
            Number(snapshot.updatedAt))
        return "Updated "
            + date.toLocaleTimeString(
                Qt.locale(),
                Locale.ShortFormat)
    }

    objectName: "usageWindow"
    visible: false
    width: 292
    height: 396
    minimumWidth: 292
    maximumWidth: 292
    minimumHeight: 396
    maximumHeight: 396
    title: "Codex Usage"
    color: "transparent"
    flags: Qt.Tool
        | Qt.FramelessWindowHint
        | Qt.WindowStaysOnTopHint

    onVisibleChanged: {
        if (visible
                && shellModel !== null
                && shellModel !== undefined) {
            shellModel.refreshUsage()
        } else if (!visible
                && shellModel !== null
                && shellModel !== undefined) {
            shellModel.cancelUsageReset()
        }
    }

    onClosing: function(close) {
        close.accepted = false
        closeRequested()
    }

    Rectangle {
        anchors.fill: parent
        radius: root.nativeBackdropRegionRadius
        color: CompanionTheme.chromeForBackdrop(
            root.effectiveBackdropMode)
        border.width: 1
        border.color: CompanionTheme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                spacing: 8

                Label {
                    Layout.preferredWidth: 22
                    text: "\ue9d2"
                    color: CompanionTheme.textPrimary
                    font.family: "Segoe Fluent Icons"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        text: "Codex usage"
                        color: CompanionTheme.textPrimary
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.updatedDetail()
                        color: CompanionTheme.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }

                ToolButton {
                    id: refreshButton
                    property string interactionId:
                        "usage.refresh"
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    text: "\ue72c"
                    enabled: root.shellModel !== null
                        && root.shellModel !== undefined
                        && !root.shellModel.usageLoading
                    font.family: "Segoe Fluent Icons"
                    font.pixelSize: 11
                    Accessible.name: enabled
                        ? "Refresh Codex usage"
                        : "Refreshing Codex usage"
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    onClicked:
                        root.shellModel.refreshUsage()

                    contentItem: Label {
                        text: refreshButton.text
                        color: refreshButton.enabled
                            ? CompanionTheme.textPrimary
                            : CompanionTheme.textMuted
                        font: refreshButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 16
                        color: refreshButton.down
                            ? CompanionTheme.controlPressed
                            : refreshButton.hovered
                                ? CompanionTheme.controlHover
                                : CompanionTheme.control
                        border.width: 1
                        border.color:
                            CompanionTheme.border
                    }
                }

                ToolButton {
                    id: closeButton
                    property string interactionId:
                        "usage.close"
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    text: "\ue711"
                    font.family: "Segoe Fluent Icons"
                    font.pixelSize: 11
                    Accessible.name: "Close Codex usage"
                    ToolTip.text: Accessible.name
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
                        radius: 16
                        color: closeButton.down
                            ? CompanionTheme.controlPressed
                            : closeButton.hovered
                                ? CompanionTheme.controlHover
                                : "transparent"
                    }
                }
            }

            ComboBox {
                id: accountSelector
                property string interactionId:
                    "usage.account.select"
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                textRole: "label"
                valueRole: "id"
                model: root.settingsModel === null
                        || root.settingsModel === undefined
                    ? []
                    : root.settingsModel.codexAccountProfiles
                enabled: count > 0
                Accessible.name: "Codex usage account"

                Component.onCompleted: synchronizeSelection()

                function synchronizeSelection() {
                    if (root.settingsModel === null
                            || root.settingsModel === undefined) {
                        currentIndex = -1
                        return
                    }
                    const index = indexOfValue(
                        root.settingsModel
                            .selectedCodexAccountProfileId)
                    currentIndex = index >= 0 ? index : -1
                }

                onActivated: {
                    if (root.settingsModel === null
                            || root.settingsModel === undefined) {
                        return
                    }
                    root.settingsModel
                        .selectedCodexAccountProfileId = currentValue
                    if (root.shellModel !== null
                            && root.shellModel !== undefined) {
                        root.shellModel
                            .refreshUsageAfterAccountChange()
                    }
                }

                Connections {
                    target: root.settingsModel
                    ignoreUnknownSignals: true

                    function onCodexAccountsChanged() {
                        Qt.callLater(
                            accountSelector.synchronizeSelection)
                    }
                }

                contentItem: Label {
                    leftPadding: 10
                    rightPadding: 28
                    text: accountSelector.displayText.length > 0
                        ? accountSelector.displayText
                        : "Select account"
                    color: accountSelector.enabled
                        ? CompanionTheme.textPrimary
                        : CompanionTheme.textMuted
                    font.pixelSize: 10
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 15
                    color: accountSelector.down
                        ? CompanionTheme.controlPressed
                        : accountSelector.hovered
                            ? CompanionTheme.controlHover
                            : CompanionTheme.control
                    border.width: 1
                    border.color: CompanionTheme.border
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Column {
                    anchors.centerIn: parent
                    width: parent.width - 28
                    spacing: 8
                    visible: !root.hasSnapshot
                        && !root.resetConfirmationOpen

                    BusyIndicator {
                        anchors.horizontalCenter:
                            parent.horizontalCenter
                        width: 28
                        height: 28
                        running: visible
                        visible: root.shellModel !== null
                            && root.shellModel !== undefined
                            && root.shellModel.usageLoading
                    }

                    Label {
                        id: statusLabel
                        width: parent.width
                        text: {
                            if (root.shellModel === null
                                    || root.shellModel === undefined) {
                                return "Codex usage is unavailable."
                            }
                            if (root.shellModel.usageLoading) {
                                return "Checking your current limits..."
                            }
                            return root.shellModel.usageErrorMessage.length > 0
                                ? root.shellModel.usageErrorMessage
                                : "Codex usage is unavailable."
                        }
                        color: CompanionTheme.textSecondary
                        font.pixelSize: 10
                        font.weight: Font.Medium
                        horizontalAlignment:
                            Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }

                ScrollView {
                    anchors.fill: parent
                    clip: true
                    visible: root.hasSnapshot
                        && !root.resetConfirmationOpen
                    ScrollBar.vertical: ScrollBar {
                        id: usageScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy:
                        ScrollBar.AlwaysOff

                    Column {
                        width: parent.width
                        spacing: 12

                        Repeater {
                            id: groupRepeater
                            model: Math.min(
                                2,
                                root.usageGroups.length)

                            delegate: Column {
                                required property int index
                                readonly property var usageGroup:
                                    root.usageGroups[index]
                                width: parent.width
                                spacing: 8

                                Label {
                                    width: parent.width
                                    visible:
                                        root.usageGroups.length > 1
                                    text: usageGroup.title
                                    color:
                                        CompanionTheme.textSecondary
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }

                                UsageWindowRow {
                                    width: parent.width
                                    visible:
                                        usageGroup.shortWindow
                                            !== undefined
                                    title: "Hourly"
                                    glyph: "\ue823"
                                    usageWindow:
                                        usageGroup.shortWindow
                                            === undefined
                                            ? ({})
                                            : usageGroup.shortWindow
                                }

                                UsageWindowRow {
                                    width: parent.width
                                    visible:
                                        usageGroup.weeklyWindow
                                            !== undefined
                                    title: "Weekly"
                                    glyph: "\ue787"
                                    usageWindow:
                                        usageGroup.weeklyWindow
                                            === undefined
                                            ? ({})
                                            : usageGroup.weeklyWindow
                                }
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: CompanionTheme.separator
                        }

                        Row {
                            width: parent.width
                            height: 30
                            spacing: 7

                            Label {
                                width: 18
                                height: parent.height
                                text: "\ue777"
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

                            Label {
                                width: parent.width - 58
                                height: parent.height
                                text: "Banked resets"
                                color:
                                    CompanionTheme.textPrimary
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                verticalAlignment:
                                    Text.AlignVCenter
                            }

                            Label {
                                width: 26
                                height: parent.height
                                text:
                                    root.snapshot
                                        .availableResetCount
                                        === undefined
                                    ? "0"
                                    : String(
                                        root.snapshot
                                            .availableResetCount)
                                color:
                                    CompanionTheme.textSecondary
                                font.pixelSize: 10
                                font.weight: Font.Bold
                                horizontalAlignment:
                                    Text.AlignRight
                                verticalAlignment:
                                    Text.AlignVCenter
                            }
                        }

                        Label {
                            width: parent.width
                            visible:
                                root.snapshot
                                    .availableResetCount
                                    === undefined
                                || Number(
                                    root.snapshot
                                        .availableResetCount)
                                    === 0
                                || root.resetCredits.length
                                    === 0
                            text: {
                                if (root.snapshot
                                        .availableResetCount
                                        === undefined
                                        || Number(
                                            root.snapshot
                                                .availableResetCount)
                                            === 0) {
                                    return "No Codex usage resets are available."
                                }
                                return "Available resets were reported without selectable details."
                            }
                            color: CompanionTheme.textMuted
                            font.pixelSize: 9
                            font.weight: Font.Medium
                            wrapMode: Text.Wrap
                        }

                        Repeater {
                            id: resetCreditRepeater
                            model: root.resetCredits.length

                            delegate: ToolButton {
                                id: resetCreditButton
                                property string interactionId:
                                    "usage.reset.select"

                                required property int index
                                readonly property var credit:
                                    root.resetCredits[index]
                                width: parent.width
                                height: 40
                                padding: 0
                                enabled:
                                    root.shellModel !== null
                                    && root.shellModel
                                        !== undefined
                                    && !root.shellModel
                                        .usageResetBusy
                                Accessible.name:
                                    "Review "
                                    + (credit.displayTitle
                                        === undefined
                                        ? "Codex usage reset"
                                        : credit.displayTitle)
                                ToolTip.text:
                                    "Review this reset before applying it"
                                ToolTip.visible: hovered
                                ToolTip.delay: 450
                                onClicked:
                                    root.shellModel
                                        .prepareUsageReset(
                                            credit)

                                contentItem: RowLayout {
                                    spacing: 8

                                    Label {
                                        Layout.preferredWidth: 18
                                        text: "\ue777"
                                        color:
                                            CompanionTheme
                                                .textSecondary
                                        font.family:
                                            "Segoe Fluent Icons"
                                        font.pixelSize: 10
                                        horizontalAlignment:
                                            Text.AlignHCenter
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1

                                        Label {
                                            Layout.fillWidth: true
                                            text:
                                                resetCreditButton
                                                    .credit
                                                    .displayTitle
                                                    === undefined
                                                ? "Codex usage reset"
                                                : resetCreditButton
                                                    .credit
                                                    .displayTitle
                                            color:
                                                CompanionTheme
                                                    .textPrimary
                                            font.pixelSize: 10
                                            font.weight:
                                                Font.DemiBold
                                            elide:
                                                Text.ElideRight
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible:
                                                resetCreditButton
                                                    .credit
                                                    .expiresAt
                                                    !== undefined
                                            text: visible
                                                ? "Expires "
                                                    + new Date(
                                                        Number(
                                                            resetCreditButton
                                                                .credit
                                                                .expiresAt))
                                                        .toLocaleString(
                                                            Qt.locale(),
                                                            Locale.ShortFormat)
                                                : ""
                                            color:
                                                CompanionTheme
                                                    .textMuted
                                            font.pixelSize: 8
                                            elide:
                                                Text.ElideRight
                                        }
                                    }

                                    Label {
                                        Layout.preferredWidth: 12
                                        text: "\ue76c"
                                        color:
                                            CompanionTheme
                                                .textSecondary
                                        font.family:
                                            "Segoe Fluent Icons"
                                        font.pixelSize: 8
                                        horizontalAlignment:
                                            Text.AlignHCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 14
                                    color: parent.down
                                        ? CompanionTheme
                                            .controlPressed
                                        : parent.hovered
                                            ? CompanionTheme
                                                .controlHover
                                            : CompanionTheme
                                                .surfaceRaised
                                    border.width: 1
                                    border.color:
                                        CompanionTheme.border
                                }
                            }
                        }

                        Label {
                            id: resetStatusLabel
                            width: parent.width
                            visible:
                                root.shellModel !== null
                                && root.shellModel !== undefined
                                && (root.shellModel
                                        .usageResetStatusMessage
                                        .length > 0
                                    || root.shellModel
                                        .usageErrorMessage
                                        .length > 0)
                            text: {
                                if (root.shellModel === null
                                        || root.shellModel
                                            === undefined) {
                                    return ""
                                }
                                return root.shellModel
                                        .usageResetStatusMessage
                                        .length > 0
                                    ? root.shellModel
                                        .usageResetStatusMessage
                                    : root.shellModel
                                        .usageErrorMessage
                            }
                            color:
                                root.shellModel !== null
                                    && root.shellModel
                                        !== undefined
                                    && root.shellModel
                                        .usageErrorMessage
                                        .length > 0
                                    && root.shellModel
                                        .usageResetStatusMessage
                                        .length === 0
                                ? CompanionTheme.warning
                                : CompanionTheme.textSecondary
                            font.pixelSize: 9
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Item {
                    id: resetConfirmationSurface
                    anchors.fill: parent
                    visible: root.resetConfirmationOpen

                    ColumnLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: 10

                        Label {
                            id: resetConfirmationTitle
                            Layout.fillWidth: true
                            text: "Apply usage reset?"
                            color:
                                CompanionTheme.textPrimary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text:
                                "This consumes "
                                + (root.resetConfirmationData
                                        .displayTitle
                                        === undefined
                                    ? "this Codex usage reset"
                                    : root.resetConfirmationData
                                        .displayTitle)
                                + " and resets the eligible Codex limit. It cannot be undone."
                            color:
                                CompanionTheme.textSecondary
                            font.pixelSize: 10
                            font.weight: Font.Medium
                            wrapMode: Text.Wrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                id: resetCancelButton
                                property string interactionId:
                                    "usage.reset.cancel"
                                Layout.preferredWidth: 112
                                Layout.preferredHeight: 32
                                text: "Cancel"
                                enabled:
                                    root.shellModel !== null
                                    && root.shellModel
                                        !== undefined
                                    && !root.shellModel
                                        .usageResetBusy
                                Accessible.name:
                                    "Cancel usage reset"
                                onClicked:
                                    root.shellModel
                                        .cancelUsageReset()
                            }

                            Button {
                                id: resetApplyButton
                                property string interactionId:
                                    "usage.reset.apply"
                                Layout.preferredWidth: 112
                                Layout.preferredHeight: 32
                                text: "Apply Reset"
                                enabled:
                                    root.shellModel !== null
                                    && root.shellModel
                                        !== undefined
                                    && !root.shellModel
                                        .usageResetBusy
                                Accessible.name:
                                    "Apply usage reset"
                                onClicked:
                                    root.shellModel
                                        .confirmUsageReset()
                            }
                        }
                    }
                }
            }
        }
    }

    component UsageWindowRow: Rectangle {
        required property string title
        required property string glyph
        required property var usageWindow

        height: usageWindow.resetsAt
                === undefined
            ? 54
            : 68
        radius: 17
        color: CompanionTheme.surfaceRaised
        border.width: 1
        border.color: CompanionTheme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 9
            spacing: 5

            RowLayout {
                Layout.fillWidth: true
                spacing: 7

                Label {
                    Layout.preferredWidth: 16
                    text: glyph
                    color: CompanionTheme.textSecondary
                    font.family: "Segoe Fluent Icons"
                    font.pixelSize: 9
                    horizontalAlignment:
                        Text.AlignHCenter
                }

                Label {
                    Layout.fillWidth: true
                    text: title
                    color: CompanionTheme.textPrimary
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }

                Label {
                    text: Math.round(
                        Number(
                            usageWindow
                                .remainingPercent
                                || 0))
                        + "% left"
                    color: CompanionTheme.textPrimary
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
            }

            ProgressBar {
                id: usageProgress
                Layout.fillWidth: true
                Layout.preferredHeight: 5
                from: 0
                to: 100
                value: Number(
                    usageWindow.remainingPercent
                        || 0)

                background: Rectangle {
                    radius: 3
                    color: CompanionTheme.control
                }

                contentItem: Item {
                    Rectangle {
                        width: parent.width
                            * Math.max(
                                0,
                                Math.min(
                                    1,
                                    usageProgress
                                        .visualPosition))
                        height: parent.height
                        radius: 3
                        color: usageProgress.value < 20
                            ? CompanionTheme.warning
                            : CompanionTheme.accent
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible:
                    usageWindow.resetsAt
                        !== undefined
                text: visible
                    ? "Resets "
                        + new Date(
                            Number(
                                usageWindow
                                    .resetsAt))
                            .toLocaleString(
                                Qt.locale(),
                                Locale.ShortFormat)
                    : ""
                color: CompanionTheme.textMuted
                font.pixelSize: 8
                elide: Text.ElideRight
            }
        }
    }
}
