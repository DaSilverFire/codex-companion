import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var updateModel: null
    property alias installedVersionLabel: installedVersionLabel
    property alias primaryActionButton: primaryActionButton
    property alias progressBar: progressBar

    spacing: 0

    function modelValue(name, fallbackValue) {
        if (updateModel === null || updateModel === undefined) {
            return fallbackValue
        }
        const value = updateModel[name]
        return value === undefined ? fallbackValue : value
    }

    function invoke(name) {
        if (updateModel === null
                || updateModel === undefined
                || updateModel[name] === undefined) {
            return
        }
        updateModel[name]()
    }

    readonly property string phase: modelValue("phase", "unavailable")
    readonly property string fallbackPrimaryActionText: {
        switch (phase) {
        case "available":
            return "Download Verified Update"
        case "ready-to-install":
            return "Install and Relaunch"
        case "checking":
            return "Checking..."
        case "downloading":
            return "Downloading..."
        case "installing":
            return "Installing..."
        case "unavailable":
            return ""
        default:
            return "Check for Updates"
        }
    }
    readonly property string primaryActionText:
        modelValue(
            "primaryActionText",
            fallbackPrimaryActionText)
    readonly property bool fallbackPrimaryActionEnabled:
        phase === "idle"
        || phase === "up-to-date"
        || phase === "failed"
        || phase === "available"
        || phase === "ready-to-install"
    readonly property bool primaryActionEnabled:
        modelValue(
            "primaryActionEnabled",
            fallbackPrimaryActionEnabled)
    readonly property bool operationInProgress:
        phase === "checking"
        || phase === "downloading"
        || phase === "installing"

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 8
        text: "Codex Companion"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: "Installed version"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
        }

        Label {
            id: installedVersionLabel
            text: root.modelValue("installedVersion", "0")
                + " ("
                + root.modelValue("installedBuild", 0)
                + ")"
            color: CompanionTheme.textSecondary
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
        }
    }

    Separator {}

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        text: root.modelValue(
            "detail",
            "Release feed and signing key are not configured.")
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    ProgressBar {
        id: progressBar
        Layout.fillWidth: true
        Layout.topMargin: 14
        implicitHeight: 6
        visible: root.operationInProgress
        from: 0
        to: 1
        value: root.phase === "downloading"
            ? root.modelValue("downloadProgress", 0)
            : 0
        indeterminate: root.phase !== "downloading"

        background: Rectangle {
            implicitHeight: 4
            radius: 2
            color: CompanionTheme.control
        }

        contentItem: Item {
            implicitHeight: 4

            Rectangle {
                width: progressBar.indeterminate
                    ? parent.width * 0.34
                    : progressBar.visualPosition * parent.width
                height: parent.height
                radius: 2
                color: CompanionTheme.accent
                x: progressBar.indeterminate
                    ? ((Date.now() / 8) % Math.max(
                           1,
                           parent.width - width))
                    : 0
            }
        }
    }

    Button {
        id: primaryActionButton
        property string interactionId:
            "settings.update.primary"
        Layout.topMargin: 16
        Layout.preferredHeight: 32
        Layout.preferredWidth: Math.max(150, implicitWidth)
        visible: root.primaryActionText.length > 0
        enabled: root.primaryActionEnabled
        text: root.primaryActionText
        Accessible.name: text

        onClicked: {
            if (root.phase === "available") {
                root.invoke("downloadAvailableUpdate")
            } else if (root.phase === "ready-to-install") {
                root.invoke("installReadyUpdate")
            } else {
                root.invoke("checkForUpdates")
            }
        }

        contentItem: Label {
            text: primaryActionButton.text
            color: primaryActionButton.enabled
                ? CompanionTheme.accentText
                : CompanionTheme.textMuted
            font.pixelSize: 12
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: CompanionTheme.radius
            color: primaryActionButton.enabled
                ? primaryActionButton.down
                    ? CompanionTheme.accentPressed
                    : primaryActionButton.hovered
                        ? CompanionTheme.accentHover
                        : CompanionTheme.accent
                : CompanionTheme.control
            border.color: primaryActionButton.activeFocus
                ? CompanionTheme.textPrimary
                : "transparent"
        }
    }

    Item {
        Layout.fillHeight: true
    }

    component Separator: Rectangle {
        Layout.fillWidth: true
        height: 1
        color: CompanionTheme.separator
    }
}
