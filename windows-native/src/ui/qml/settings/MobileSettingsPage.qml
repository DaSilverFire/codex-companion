import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property var settingsModel: null
    property alias mobileEnabledSwitch: mobileEnabledSwitch
    property alias mobileRuntimeStatusLabel: mobileRuntimeStatusLabel
    property alias keepAvailableSwitch: keepAvailableSwitch
    property alias publicNetworkSwitch: publicNetworkSwitch
    property alias pairButton: pairButton
    property alias cancelPairingButton: cancelPairingButton
    property alias pairingCodeLabel: pairingCodeLabel
    property alias pairingQrImage: pairingQrImage
    property alias pairingLinkField: pairingLinkField
    property alias copyPairingLinkButton: copyPairingLinkButton
    property alias pairingLinkStatusLabel: pairingLinkStatusLabel
    property alias nearbyStatusLabel: nearbyStatusLabel
    property alias pairedDeviceRepeater: pairedDeviceRepeater
    property alias relayUrlField: relayUrlField
    property alias relaySaveButton: relaySaveButton
    property alias relayAutomaticButton: relayAutomaticButton
    property alias relayDisableButton: relayDisableButton

    spacing: 0
    readonly property bool runtimeEnabled: root.modelValue(
        "mobileEnabled",
        true)

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

    function expiryText() {
        const milliseconds = root.modelValue(
            "mobilePairingExpiresAtMilliseconds",
            0)
        if (milliseconds <= 0) {
            return ""
        }
        return "Expires " + new Date(milliseconds).toLocaleTimeString(
            Qt.locale(),
            Locale.ShortFormat)
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 8
        text: "Mobile Companion"
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
            text: "Enable mobile access"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
        }

        CompactSwitch {
            id: mobileEnabledSwitch
            interactionId:
                "settings.mobile.enabled"
            Accessible.name: "Enable mobile access"
            checked: root.modelValue("mobileEnabled", true)
            onToggled: root.setModelValue("mobileEnabled", checked)
        }
    }

    Label {
        id: mobileRuntimeStatusLabel
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 12
        text: root.runtimeEnabled
            ? "Nearby discovery and encrypted access for paired devices are active."
            : "Discovery, relay connections, mobile task services, and display-sleep availability are stopped and released."
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Separator {
        visible: root.runtimeEnabled
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        visible: root.runtimeEnabled
        text: "Availability"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 38
        spacing: 12
        visible: root.runtimeEnabled

        Label {
            Layout.fillWidth: true
            text: "Keep Windows available while display is off"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
        }

        CompactSwitch {
            id: keepAvailableSwitch
            visible: root.runtimeEnabled
            interactionId:
                "settings.mobile.keep-available"
            Accessible.name: "Keep Windows available while display is off"
            checked: root.modelValue(
                "keepAvailableWhileDisplayOff",
                true)
            onToggled: root.setModelValue(
                "keepAvailableWhileDisplayOff",
                checked)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: 38
        spacing: 12
        visible: root.runtimeEnabled

        Label {
            Layout.fillWidth: true
            text: "Allow nearby access on Public networks"
            color: CompanionTheme.textPrimary
            font.pixelSize: 13
            elide: Text.ElideRight
        }

        CompactSwitch {
            id: publicNetworkSwitch
            visible: root.runtimeEnabled
            interactionId:
                "settings.mobile.public-network"
            Accessible.name: "Allow nearby access on Public networks"
            checked: root.modelValue(
                "allowNearbyOnPublicNetworks",
                false)
            enabled: root.modelValue(
                "mobileEnabled",
                true)
            onToggled: root.setModelValue(
                "allowNearbyOnPublicNetworks",
                checked)
        }
    }

    Label {
        id: nearbyStatusLabel
        Layout.fillWidth: true
        Layout.bottomMargin: 12
        visible: root.runtimeEnabled
        text: root.modelValue(
            "mobileNearbyAccessStatusText",
            "Nearby Wi-Fi status is unavailable.")
        color: root.modelValue(
            "mobileNearbyAccessAvailable",
            false)
            ? CompanionTheme.textSecondary
            : CompanionTheme.warning
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    Separator {
        visible: root.runtimeEnabled
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        visible: root.runtimeEnabled
        text: "Paired Devices"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: root.modelValue(
            "mobilePairingActive",
            false) ? 58 : 34
        spacing: 10
        visible: root.runtimeEnabled

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            visible: root.modelValue(
                "mobilePairingActive",
                false)

            Label {
                text: "Pairing code"
                color: CompanionTheme.textMuted
                font.pixelSize: 11
            }

            Label {
                id: pairingCodeLabel
                text: root.modelValue(
                    "mobilePairingCode",
                    "")
                color: CompanionTheme.textPrimary
                font.family: "Cascadia Mono"
                font.pixelSize: 22
                font.weight: Font.DemiBold
                font.letterSpacing: 0
            }

            Label {
                text: root.expiryText()
                color: CompanionTheme.textMuted
                font.pixelSize: 10
            }
        }

        Button {
            id: pairButton
            property string interactionId:
                "settings.mobile.pair"
            implicitHeight: 30
            text: "Pair iPhone"
            visible: root.runtimeEnabled
                && !root.modelValue(
                "mobilePairingActive",
                false)
            enabled: root.modelValue(
                "mobileEnabled",
                true)
                && root.modelValue(
                    "mobilePairingAvailable",
                    false)
            Accessible.name: "Pair iPhone"
            Accessible.description: enabled
                ? ""
                : root.modelValue(
                      "mobileNearbyAccessStatusText",
                      "Nearby Wi-Fi is unavailable.")
            onClicked: root.invoke(
                "beginMobilePairing")
            ToolTip.visible: hovered && !enabled
            ToolTip.text: root.modelValue(
                "mobileNearbyAccessStatusText",
                "Nearby Wi-Fi is unavailable.")
            background: ButtonBackground {
                control: pairButton
                primary: true
            }
            contentItem: ButtonLabel {
                control: pairButton
                primary: true
            }
        }

        Button {
            id: cancelPairingButton
            property string interactionId:
                "settings.mobile.cancel-pairing"
            implicitHeight: 30
            text: "Cancel Pairing"
            visible: root.runtimeEnabled
                && root.modelValue(
                "mobilePairingActive",
                false)
            Accessible.name: "Cancel iPhone pairing"
            onClicked: root.invoke(
                "cancelMobilePairing")
            background: ButtonBackground {
                control: cancelPairingButton
            }
            contentItem: ButtonLabel {
                control: cancelPairingButton
            }
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        visible: root.runtimeEnabled
            && text.length > 0
        text: root.modelValue(
            "mobilePairingStatusText",
            "")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 6
        visible: root.runtimeEnabled
            && root.modelValue(
                "hasMobilePairingLink",
                false)

        Label {
            Layout.fillWidth: true
            visible: pairingQrFrame.visible
            text: "Scan with Companion Mobile"
            color: CompanionTheme.textMuted
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
        }

        Rectangle {
            id: pairingQrFrame
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 232
            Layout.preferredHeight: 232
            visible: root.runtimeEnabled
                && root.modelValue(
                    "mobilePairingQrSource",
                    "").length > 0
            radius: 8
            color: "#ffffff"
            border.color: "#d4d4d4"
            border.width: 1

            Image {
                id: pairingQrImage
                anchors.fill: parent
                anchors.margins: 10
                source: root.modelValue(
                    "mobilePairingQrSource",
                    "")
                fillMode: Image.PreserveAspectFit
                smooth: false
                cache: false
                Accessible.name:
                    "Scan to pair this iPhone"
            }
        }

        Label {
            Layout.fillWidth: true
            text: "Secure short-lived pairing link"
            color: CompanionTheme.textMuted
            font.pixelSize: 11
        }

        TextField {
            id: pairingLinkField
            Layout.fillWidth: true
            implicitHeight: 32
            visible: root.runtimeEnabled
                && root.modelValue(
                    "hasMobilePairingLink",
                    false)
            readOnly: true
            selectByMouse: true
            text: root.modelValue(
                "mobilePairingLink",
                "")
            color: CompanionTheme.textPrimary
            selectionColor: CompanionTheme.accent
            selectedTextColor:
                CompanionTheme.accentText
            font.family: "Cascadia Mono"
            font.pixelSize: 11
            Accessible.name:
                "Secure short-lived pairing link"

            background: Rectangle {
                radius: CompanionTheme.radius
                color: pairingLinkField.activeFocus
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
                border.color:
                    pairingLinkField.activeFocus
                    ? CompanionTheme.accent
                    : CompanionTheme.border
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                id: copyPairingLinkButton
                property string interactionId:
                    "settings.mobile.copy-pairing-link"
                implicitHeight: 30
                visible: root.runtimeEnabled
                    && root.modelValue(
                        "hasMobilePairingLink",
                        false)
                enabled: root.modelValue(
                    "hasMobilePairingLink",
                    false)
                text: "Copy Link"
                Accessible.name:
                    "Copy secure pairing link"
                onClicked: root.invoke(
                    "copyMobilePairingLink")
                background: ButtonBackground {
                    control: copyPairingLinkButton
                }
                contentItem: ButtonLabel {
                    control: copyPairingLinkButton
                }
            }

            Label {
                id: pairingLinkStatusLabel
                Layout.fillWidth: true
                text: root.modelValue(
                    "mobilePairingLinkStatus",
                    "")
                color: CompanionTheme.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 6
        Layout.bottomMargin: 6
        visible: root.runtimeEnabled
            && root.modelValue(
            "pairedMobileDevices",
            []).length === 0
        text: "No iPhones are paired."
        color: CompanionTheme.textMuted
        font.pixelSize: 12
    }

    Repeater {
        id: pairedDeviceRepeater
        model: root.modelValue(
            "pairedMobileDevices",
            [])

        delegate: RowLayout {
            id: deviceRow
            required property var modelData
            property alias forgetButton: forgetButton

            Layout.fillWidth: true
            width: root.width
            height: 34
            spacing: 10
            visible: root.runtimeEnabled

            Label {
                Layout.fillWidth: true
                text: deviceRow.modelData.displayName
                color: CompanionTheme.textPrimary
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Button {
                id: forgetButton
                property string interactionId:
                    "settings.mobile.forget"
                implicitHeight: 28
                text: "Forget"
                Accessible.name: "Forget " + deviceRow.modelData.displayName
                onClicked: root.invoke(
                    "forgetMobileDevice",
                    deviceRow.modelData.deviceId)
                background: ButtonBackground {
                    control: forgetButton
                    destructive: true
                }
                contentItem: ButtonLabel {
                    control: forgetButton
                    destructive: true
                }
            }
        }
    }

    Separator {
        Layout.topMargin: 8
        visible: root.runtimeEnabled
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        Layout.bottomMargin: 8
        visible: root.runtimeEnabled
        text: "Remote Access"
        color: CompanionTheme.textSecondary
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    TextField {
        id: relayUrlField
        Layout.fillWidth: true
        implicitHeight: 32
        visible: root.runtimeEnabled
        text: root.modelValue(
            "relayUrl",
            "")
        placeholderText: "Secure relay URL"
        color: CompanionTheme.textPrimary
        placeholderTextColor: CompanionTheme.textMuted
        selectionColor: CompanionTheme.accent
        selectedTextColor: CompanionTheme.accentText
        font.pixelSize: 12
        selectByMouse: true
        Accessible.name: "Secure relay URL"
        onAccepted: root.invoke(
            "saveRelayUrl",
            text)

        background: Rectangle {
            radius: CompanionTheme.radius
            color: relayUrlField.activeFocus
                ? CompanionTheme.controlHover
                : CompanionTheme.control
            border.color: relayUrlField.activeFocus
                ? CompanionTheme.accent
                : CompanionTheme.border
        }

        Connections {
            target: root.settingsModel
            ignoreUnknownSignals: true

            function onRelayUrlChanged() {
                if (!relayUrlField.activeFocus) {
                    relayUrlField.text = root.modelValue(
                        "relayUrl",
                        "")
                }
            }

            function onRelayConfigurationChanged() {
                relayUrlField.text = root.modelValue(
                    "relayUrl",
                    "")
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 8
        visible: root.runtimeEnabled

        Button {
            id: relaySaveButton
            property string interactionId:
                "settings.relay.save"
            implicitHeight: 30
            visible: root.runtimeEnabled
            text: "Save"
            enabled: relayUrlField.text.trim().length > 0
            Accessible.name: "Save secure relay URL"
            onClicked: root.invoke(
                "saveRelayUrl",
                relayUrlField.text)
            background: ButtonBackground {
                control: relaySaveButton
                primary: true
            }
            contentItem: ButtonLabel {
                control: relaySaveButton
                primary: true
            }
        }

        Button {
            id: relayAutomaticButton
            property string interactionId:
                "settings.relay.automatic"
            implicitHeight: 30
            visible: root.runtimeEnabled
            text: "Use Automatic"
            enabled: root.modelValue(
                "relayAutomaticAvailable",
                false)
            Accessible.name: "Use automatic secure relay"
            onClicked: root.invoke(
                "useAutomaticRelay")
            background: ButtonBackground {
                control: relayAutomaticButton
            }
            contentItem: ButtonLabel {
                control: relayAutomaticButton
            }
        }

        Button {
            id: relayDisableButton
            property string interactionId:
                "settings.relay.disable"
            implicitHeight: 30
            visible: root.runtimeEnabled
            text: "Disable"
            enabled: root.modelValue(
                "relayDisableAvailable",
                false)
            Accessible.name: "Disable remote access"
            onClicked: root.invoke(
                "disableRelay")
            background: ButtonBackground {
                control: relayDisableButton
                destructive: true
            }
            contentItem: ButtonLabel {
                control: relayDisableButton
                destructive: true
            }
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 7
        Layout.bottomMargin: 4
        visible: root.runtimeEnabled
        text: root.modelValue(
            "relayStatusText",
            "Nearby access is available.")
        color: CompanionTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
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
            y: parent.topPadding
                + (parent.availableHeight - height) / 2
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

    component ButtonBackground: Rectangle {
        required property Button control
        property bool primary: false
        property bool destructive: false

        radius: CompanionTheme.radius
        color: primary
            ? control.down
                ? CompanionTheme.accentPressed
                : control.hovered
                    ? CompanionTheme.accentMuted
                    : CompanionTheme.accent
            : control.down
                ? CompanionTheme.controlPressed
                : control.hovered
                    ? CompanionTheme.controlHover
                    : CompanionTheme.control
        border.color: control.activeFocus
            ? CompanionTheme.accent
            : destructive
                ? CompanionTheme.danger
                : primary
                    ? "transparent"
                    : CompanionTheme.border
        opacity: control.enabled ? 1 : 0.45
    }

    component ButtonLabel: Label {
        required property Button control
        property bool primary: false
        property bool destructive: false

        text: control.text
        color: primary
            ? CompanionTheme.accentText
            : destructive
                ? CompanionTheme.danger
                : CompanionTheme.textPrimary
        font.pixelSize: 12
        font.weight: primary
            ? Font.DemiBold
            : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
