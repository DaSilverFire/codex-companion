import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var settingsModel: null
    property alias profileSelector: profileSelector
    property alias signInButton: signInButton
    property alias refreshButton: refreshButton
    property alias removeButton: removeButton
    property alias profileLabelField: profileLabelField
    property alias addButton: addButton
    property alias continuationSwitch: continuationSwitch
    property alias statusLabel: statusLabel
    property alias selectionSummaryLabel: selectionSummaryLabel

    spacing: 0

    function modelValue(name, fallbackValue) {
        if (settingsModel === null
                || settingsModel === undefined) {
            return fallbackValue
        }
        const value = settingsModel[name]
        return value === undefined
            ? fallbackValue
            : value
    }

    function setModelValue(name, value) {
        if (settingsModel !== null
                && settingsModel !== undefined) {
            settingsModel[name] = value
        }
    }

    function invoke(name, argument) {
        if (settingsModel === null
                || settingsModel === undefined
                || settingsModel[name] === undefined) {
            return false
        }
        return argument === undefined
            ? settingsModel[name]()
            : settingsModel[name](argument)
    }

    function synchronizeProfileSelection() {
        const selected = root.modelValue(
            "selectedCodexAccountProfileId",
            "")
        const index =
            profileSelector.indexOfValue(
                selected)
        profileSelector.currentIndex =
            index >= 0 ? index : -1
    }

    function addProfile() {
        if (root.invoke(
                "addCodexAccount",
                profileLabelField.text)) {
            profileLabelField.clear()
            Qt.callLater(
                root.synchronizeProfileSelection)
        }
    }

    onSettingsModelChanged:
        Qt.callLater(
            synchronizeProfileSelection)

    Connections {
        target: root.settingsModel
        ignoreUnknownSignals: true

        function onCodexAccountsChanged() {
            Qt.callLater(
                root.synchronizeProfileSelection)
        }

        function onCodexAccountProfilesChanged() {
            Qt.callLater(
                root.synchronizeProfileSelection)
        }

        function onSelectedCodexAccountProfileIdChanged() {
            Qt.callLater(
                root.synchronizeProfileSelection)
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 8
        text: "Codex Accounts"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    Label {
        Layout.fillWidth: true
        Layout.bottomMargin: 12
        text: "The selected signed-in profile applies to new Codex work. Active and approval-pending tasks stay with their original account."
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        spacing: 12

        Label {
            Layout.preferredWidth: 144
            text: "New work account"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
        }

        ComboBox {
            id: profileSelector
            property string interactionId:
                "settings.accounts.select"
            Layout.fillWidth: true
            implicitHeight: 30
            textRole: "label"
            valueRole: "id"
            model: root.modelValue(
                "codexAccountProfiles",
                [])
            enabled: root.modelValue(
                "codexAccountsAvailable",
                false)
                && count > 0
            Accessible.name: "New work account"
            onActivated: root.setModelValue(
                "selectedCodexAccountProfileId",
                currentValue)

            delegate: ItemDelegate {
                required property int index
                width: profileSelector.width
                height: 30
                highlighted:
                    profileSelector
                        .highlightedIndex
                    === index

                contentItem: Label {
                    text: profileSelector.textAt(
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
                text: profileSelector.displayText.length > 0
                    ? profileSelector.displayText
                    : "No profile selected"
                color: profileSelector.enabled
                    ? CompanionTheme.textPrimary
                    : CompanionTheme.textMuted
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            indicator: Canvas {
                x: profileSelector.width
                    - width - 10
                y: (profileSelector.height
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
                    context.lineTo(
                        width - 1,
                        1)
                    context.stroke()
                }
            }

            background: Rectangle {
                radius: CompanionTheme.radius
                color: profileSelector.down
                    ? CompanionTheme.controlPressed
                    : profileSelector.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
                border.color:
                    profileSelector.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
                opacity:
                    profileSelector.enabled
                    ? 1
                    : 0.55
            }

            popup: Popup {
                y: profileSelector.height + 4
                width: profileSelector.width
                implicitHeight:
                    Math.min(
                        contentItem.contentHeight,
                        180) + 2
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: profileSelector.popup.visible
                        ? profileSelector.delegateModel
                        : null
                    currentIndex:
                        profileSelector
                            .highlightedIndex
                    ScrollBar.vertical.policy:
                        ScrollBar.AlwaysOff
                }

                background: Rectangle {
                    radius: CompanionTheme.radius
                    color:
                        CompanionTheme.surfaceRaised
                    border.color:
                        CompanionTheme.border
                }
            }
        }
    }

    Label {
        id: selectionSummaryLabel
        Layout.fillWidth: true
        Layout.leftMargin: 156
        Layout.topMargin: 4
        Layout.bottomMargin: 10
        text: root.modelValue(
            "codexAccountSelectionSummary",
            "No Codex account profile is selected.")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 156
        Layout.bottomMargin: 16
        spacing: 8

        ActionButton {
            id: signInButton
            interactionId:
                "settings.accounts.sign-in"
            text: "Official Codex Sign In"
            primary: true
            enabled: profileSelector.enabled
            Accessible.name: "Official Codex Sign In"
            onClicked: root.invoke(
                "beginSelectedCodexAccountLogin")
        }

        ActionButton {
            id: refreshButton
            interactionId:
                "settings.accounts.refresh"
            text: root.modelValue(
                "codexAccountRefreshInProgress",
                false)
                ? "Checking..."
                : "Refresh Account"
            enabled: profileSelector.enabled
                && !root.modelValue(
                    "codexAccountRefreshInProgress",
                    false)
            Accessible.name: text
            onClicked: root.invoke(
                "refreshSelectedCodexAccount")
        }

        ActionButton {
            id: removeButton
            interactionId:
                "settings.accounts.remove"
            text: "Remove"
            destructive: true
            enabled: profileSelector.enabled
                && profileSelector.currentValue
                    !== "current-codex-account"
            Accessible.name: "Remove selected Codex account profile"
            onClicked: root.invoke(
                "removeSelectedCodexAccount")
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Separator {}

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "Add Profile"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.preferredWidth: 144
            text: "Display name"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
        }

        TextField {
            id: profileLabelField
            Layout.fillWidth: true
            implicitHeight: 32
            enabled: root.modelValue(
                "codexAccountsAvailable",
                false)
            placeholderText: "Account name"
            color: CompanionTheme.textPrimary
            placeholderTextColor:
                CompanionTheme.textMuted
            selectionColor:
                CompanionTheme.accent
            selectedTextColor:
                CompanionTheme.accentText
            font.pixelSize: 12
            selectByMouse: true
            Accessible.name:
                "Codex account profile display name"
            onAccepted: root.addProfile()

            background: Rectangle {
                radius: CompanionTheme.radius
                color: profileLabelField.activeFocus
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
                border.color:
                    profileLabelField.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
                opacity:
                    profileLabelField.enabled
                    ? 1
                    : 0.55
            }
        }

        ActionButton {
            id: addButton
            interactionId:
                "settings.accounts.add"
            text: "Add"
            primary: true
            enabled: profileLabelField.enabled
                && profileLabelField.text.trim()
                    .length > 0
            Accessible.name:
                "Add Codex account profile"
            onClicked: root.addProfile()
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.leftMargin: 156
        Layout.topMargin: 6
        Layout.bottomMargin: 16
        text: "Companion stores only the profile name and routing ID. Codex owns sign-in credentials."
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Separator {}

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        text: "Quota Recovery"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 38
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: "Continue stopped work with another eligible Codex account"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        CompactSwitch {
            id: continuationSwitch
            interactionId:
                "settings.accounts.automatic-continuation"
            Accessible.name:
                "Continue stopped work with another eligible Codex account"
            checked: root.modelValue(
                "automaticallyContinuesAcrossCodexAccounts",
                false)
            onToggled: root.setModelValue(
                "automaticallyContinuesAcrossCodexAccounts",
                checked)
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 14
        text: "Off by default. It runs only after a confirmed usage-limit stop, never moves active or approval-pending work, and never consumes a banked reset."
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Label {
        id: statusLabel
        Layout.fillWidth: true
        Layout.bottomMargin: 4
        text: root.modelValue(
            "codexAccountStatus",
            root.modelValue(
                "codexAccountsAvailable",
                false)
                ? "Select a Codex account profile."
                : "Codex account profiles are unavailable.")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
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
            y: parent.topPadding
                + (parent.availableHeight
                    - height) / 2
            radius: height / 2
            color: parent.checked
                ? CompanionTheme.accent
                : CompanionTheme.control
            border.color: parent.activeFocus
                ? CompanionTheme.textPrimary
                : CompanionTheme.border

            Rectangle {
                x: parent.parent.checked
                    ? parent.width - width - 3
                    : 3
                y: 3
                width: 16
                height: 16
                radius: 8
                color: parent.parent.checked
                    ? CompanionTheme.accentText
                    : CompanionTheme.textPrimary
            }
        }
        contentItem: Item {}
    }

    component ActionButton: Button {
        required property string interactionId
        property bool primary: false
        property bool destructive: false

        implicitHeight: 30

        background: Rectangle {
            radius: CompanionTheme.radius
            color: parent.primary
                ? parent.down
                    ? CompanionTheme.accentPressed
                    : parent.hovered
                        ? CompanionTheme.accentHover
                        : CompanionTheme.accent
                : parent.down
                    ? CompanionTheme.controlPressed
                    : parent.hovered
                        ? CompanionTheme.controlHover
                        : CompanionTheme.control
            border.color: parent.activeFocus
                ? CompanionTheme.accent
                : parent.destructive
                    ? CompanionTheme.danger
                    : parent.primary
                        ? "transparent"
                        : CompanionTheme.border
            opacity: parent.enabled ? 1 : 0.45
        }

        contentItem: Label {
            text: parent.text
            color: parent.primary
                ? CompanionTheme.accentText
                : parent.destructive
                    ? CompanionTheme.danger
                    : CompanionTheme.textPrimary
            font.pixelSize: 12
            font.weight: parent.primary
                ? Font.DemiBold
                : Font.Normal
            horizontalAlignment:
                Text.AlignHCenter
            verticalAlignment:
                Text.AlignVCenter
        }
    }
}
