import QtQuick
import QtQuick.Controls.Basic
import CodexCompanion

Window {
    id: root

    property var attentionModel: null
    property var settingsModel: null
    property var backdropState: null
    readonly property var message:
        attentionModel === null
        || attentionModel === undefined
        || attentionModel.attentionMessage === undefined
            ? ({})
            : attentionModel.attentionMessage
    readonly property string messageKind:
        message.kind === undefined
            ? "response"
            : String(message.kind)
    readonly property string effectiveBackdropMode: {
        if (backdropState !== null
                && backdropState !== undefined
                && backdropState.attentionEffectiveMode !== undefined) {
            return backdropState.attentionEffectiveMode
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
    readonly property real nativeBackdropRegionRadius: 16

    property alias actionSurface: actionSurface
    property alias kindGlyph: kindGlyph
    property alias titleLabel: titleLabel
    property alias processLabel: processLabel
    property alias detailLabel: detailLabel
    readonly property color messageTint:
        colorForKind(messageKind)

    signal openRequested()
    signal dismissRequested()

    function glyphForKind(kind) {
        switch (kind) {
        case "failure":
            return "\ue7ba"
        case "goal":
            return "\ue8fb"
        case "completion":
            return "\ue930"
        case "attention":
            return "\ue814"
        default:
            return "\ue8bd"
        }
    }

    function colorForKind(kind) {
        switch (kind) {
        case "failure":
            return CompanionTheme.danger
        case "completion":
            return CompanionTheme.success
        case "attention":
            return CompanionTheme.warning
        case "response":
            return CompanionTheme.textSecondary
        case "goal":
            return CompanionTheme.goal
        default:
            return CompanionTheme.border
        }
    }

    objectName: "attentionWindow"
    visible: false
    width: 268
    height: 78
    minimumWidth: 268
    maximumWidth: 268
    minimumHeight: 78
    maximumHeight: 78
    title: "Codex Companion"
    color: "transparent"
    flags: Qt.Tool
        | Qt.FramelessWindowHint
        | Qt.WindowStaysOnTopHint
        | Qt.WindowDoesNotAcceptFocus

    onClosing: function(close) {
        close.accepted = false
        dismissRequested()
    }

    Rectangle {
        id: attentionPanel

        anchors.fill: parent
        radius: root.nativeBackdropRegionRadius
        color: actionSurface.containsMouse
            ? CompanionTheme.controlHover
            : CompanionTheme.chromeForBackdrop(
                root.effectiveBackdropMode)
        border.width: 1
        border.color: Qt.alpha(
            root.messageTint,
            0.32)

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: Qt.alpha(
                root.messageTint,
                0.10)
        }

        Label {
            id: kindGlyph

            x: 9
            y: 13
            width: 16
            height: 16
            text: root.glyphForKind(
                root.messageKind)
            color: root.messageTint
            font.family: "Segoe Fluent Icons"
            font.pixelSize: 12
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Column {
            x: 34
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 59
            spacing: 2

            Label {
                id: titleLabel
                width: parent.width
                text: root.message.title === undefined
                    ? ""
                    : String(root.message.title)
                color: CompanionTheme.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }

            Label {
                id: processLabel
                width: parent.width
                text: root.message.processTitle === undefined
                    ? ""
                    : String(root.message.processTitle)
                color: CompanionTheme.textSecondary
                font.pixelSize: 9
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
        }

        Label {
            id: detailLabel

            visible: false
            text: root.message.detail === undefined
                ? ""
                : String(root.message.detail)
        }

        Label {
            anchors.right: parent.right
            anchors.rightMargin: 11
            anchors.verticalCenter: parent.verticalCenter
            text: "\ue76c"
            color: actionSurface.containsMouse
                ? CompanionTheme.textPrimary
                : CompanionTheme.textMuted
            font.family: "Segoe Fluent Icons"
            font.pixelSize: 10
        }

        MouseArea {
            id: actionSurface
            property string interactionId:
                "attention.open"
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            Accessible.role: Accessible.Button
            Accessible.name: titleLabel.text
            onClicked: root.openRequested()
        }
    }
}
