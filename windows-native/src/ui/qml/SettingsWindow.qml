import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import CodexCompanion

Window {
    id: root

    property var settingsModel: null
    property var routingModel: null
    property var petModel: null
    property var updateModel: null
    property alias closeButton: closeButton
    property alias backdropSelector: generalPage.backdropSelector
    property alias navigationTabs: navigationTabs
    property alias contentStack: contentStack
    property alias generalContentScrollView: contentScrollView
    property alias verticalScrollBar: verticalScrollBar
    property alias accountsContentScrollView: accountsContentScrollView
    property alias accountsVerticalScrollBar: accountsVerticalScrollBar
    property alias chatVerticalScrollBar: chatVerticalScrollBar
    property alias mobileContentScrollView: mobileContentScrollView
    property alias mobileVerticalScrollBar: mobileVerticalScrollBar
    property alias updateVerticalScrollBar: updateVerticalScrollBar
    property alias hideControlsSwitch: generalPage.hideControlsSwitch
    property alias autonomousMovementSwitch: generalPage.autonomousMovementSwitch
    property alias petSelector: generalPage.petSelector
    property alias previewAnimationSelector: generalPage.previewAnimationSelector
    property alias previewAnimationLabel: generalPage.previewAnimationLabel
    property alias animationPacingSlider: generalPage.animationPacingSlider
    property alias animationPacingLabel: generalPage.animationPacingLabel
    property alias hideControlsLabel: generalPage.hideControlsLabel
    property alias autonomousMovementLabel: generalPage.autonomousMovementLabel
    property alias appearanceRowLabel: generalPage.appearanceRowLabel
    property alias petSectionLabel: generalPage.petSectionLabel
    property alias materialSurface: materialSurface
    property alias backdropSelectorBackground: generalPage.backdropSelectorBackground
    property alias usageSectionLabel: generalPage.usageSectionLabel
    property alias usageRemainingLabel: generalPage.usageRemainingLabel
    property alias usageSummaryLabel: generalPage.usageSummaryLabel
    property alias usageDisclosureLabel: generalPage.usageDisclosureLabel
    property alias usageRefreshButton: generalPage.usageRefreshButton
    property alias reloadPetsButton: generalPage.reloadPetsButton
    property alias openAIKeyField: chatPage.openAIKeyField
    property alias defaultModeSelector: chatPage.defaultModeSelector
    property alias chatDeliverySelector: chatPage.chatDeliverySelector
    property alias chatDeliveryDescription: chatPage.chatDeliveryDescription
    property alias openAISectionLabel: chatPage.openAISectionLabel
    property alias openAIModelSelector: chatPage.openAIModelSelector
    property alias openAISaveButton: chatPage.openAISaveButton
    property alias openAIRemoveButton: chatPage.openAIRemoveButton
    property alias lumoSectionLabel: chatPage.lumoSectionLabel
    property alias lumoModelSelector: chatPage.lumoModelSelector
    property alias lumoKeyField: chatPage.lumoKeyField
    property alias lumoSaveButton: chatPage.lumoSaveButton
    property alias lumoRemoveButton: chatPage.lumoRemoveButton
    property alias accountProfileSelector: accountsPage.profileSelector
    property alias accountSignInButton: accountsPage.signInButton
    property alias accountRefreshButton: accountsPage.refreshButton
    property alias accountRemoveButton: accountsPage.removeButton
    property alias accountLabelField: accountsPage.profileLabelField
    property alias accountAddButton: accountsPage.addButton
    property alias accountContinuationSwitch: accountsPage.continuationSwitch
    property alias accountStatusLabel: accountsPage.statusLabel
    property alias accountSelectionSummaryLabel: accountsPage.selectionSummaryLabel
    property alias mobileEnabledSwitch: mobilePage.mobileEnabledSwitch
    property alias mobileRuntimeStatusLabel: mobilePage.mobileRuntimeStatusLabel
    property alias keepAvailableSwitch: mobilePage.keepAvailableSwitch
    property alias publicNetworkSwitch: mobilePage.publicNetworkSwitch
    property alias pairButton: mobilePage.pairButton
    property alias cancelPairingButton: mobilePage.cancelPairingButton
    property alias pairingCodeLabel: mobilePage.pairingCodeLabel
    property alias pairingQrImage: mobilePage.pairingQrImage
    property alias pairingLinkField: mobilePage.pairingLinkField
    property alias copyPairingLinkButton: mobilePage.copyPairingLinkButton
    property alias pairingLinkStatusLabel: mobilePage.pairingLinkStatusLabel
    property alias nearbyStatusLabel: mobilePage.nearbyStatusLabel
    property alias pairedDeviceRepeater: mobilePage.pairedDeviceRepeater
    property alias relayUrlField: mobilePage.relayUrlField
    property alias relaySaveButton: mobilePage.relaySaveButton
    property alias relayAutomaticButton: mobilePage.relayAutomaticButton
    property alias relayDisableButton: mobilePage.relayDisableButton
    property alias updateInstalledVersionLabel: updatesPage.installedVersionLabel
    property alias updatePrimaryActionButton: updatesPage.primaryActionButton
    property alias updateProgressBar: updatesPage.progressBar
    readonly property string effectiveBackdropMode: {
        if (settingsModel === null || settingsModel === undefined || settingsModel.backdropMode === undefined) {
            return "mica"
        }
        if (settingsModel.effectiveBackdropMode === undefined) {
            return settingsModel.backdropMode
        }
        return settingsModel.effectiveBackdropMode
    }
    readonly property bool enhancedBackdropActive: effectiveBackdropMode !== "solid-black"

    signal closeRequested()

    width: 560
    height: 520
    minimumWidth: 560
    maximumWidth: 560
    minimumHeight: 520
    maximumHeight: 520
    title: "Codex Companion Settings"
    color: enhancedBackdropActive ? "transparent" : CompanionTheme.window
    flags: Qt.Window | Qt.CustomizeWindowHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint

    onClosing: function(close) {
        close.accepted = false
        closeRequested()
    }

    Rectangle {
        id: materialSurface
        anchors.fill: parent
        color: CompanionTheme.chromeForBackdrop(
            root.effectiveBackdropMode)
        radius: CompanionTheme.radius

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                spacing: CompanionTheme.gap

                Label {
                    Layout.fillWidth: true
                    text: "Settings"
                    color: CompanionTheme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                ToolButton {
                    id: closeButton
                    property string interactionId:
                        "settings.close"
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    text: ""
                    display: AbstractButton.IconOnly
                    icon.name: "window-close"
                    icon.width: 14
                    icon.height: 14
                    icon.color: hovered ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                    padding: 0
                    Accessible.name: "Close Settings"
                    ToolTip.text: "Close Settings"
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    onClicked: root.closeRequested()

                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: closeButton.down ? CompanionTheme.control : closeButton.hovered ? CompanionTheme.controlHover : "transparent"
                        border.color: closeButton.activeFocus ? CompanionTheme.accent : "transparent"
                    }
                }
            }

            TabBar {
                id: navigationTabs
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                background: Rectangle {
                    color: "transparent"
                }

                TabButton {
                    property string interactionId:
                        "settings.tab.general"
                    text: "General"
                    width: implicitWidth + 20
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: parent.checked ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: parent.checked ? CompanionTheme.surfaceRaised : "transparent"
                        border.color: parent.checked ? CompanionTheme.border : "transparent"
                    }
                }

                TabButton {
                    property string interactionId:
                        "settings.tab.accounts"
                    text: "Accounts"
                    width: implicitWidth + 20
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: parent.checked ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: parent.checked ? CompanionTheme.surfaceRaised : "transparent"
                        border.color: parent.checked ? CompanionTheme.border : "transparent"
                    }
                }

                TabButton {
                    property string interactionId:
                        "settings.tab.chat"
                    text: "Chat"
                    width: implicitWidth + 20
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: parent.checked ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: parent.checked ? CompanionTheme.surfaceRaised : "transparent"
                        border.color: parent.checked ? CompanionTheme.border : "transparent"
                    }
                }

                TabButton {
                    property string interactionId:
                        "settings.tab.mobile"
                    text: "Mobile"
                    width: implicitWidth + 20
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: parent.checked ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: parent.checked ? CompanionTheme.surfaceRaised : "transparent"
                        border.color: parent.checked ? CompanionTheme.border : "transparent"
                    }
                }

                TabButton {
                    property string interactionId:
                        "settings.tab.updates"
                    text: "Updates"
                    width: implicitWidth + 20
                    contentItem: Label {
                        text: parent.text
                        color: parent.checked ? CompanionTheme.textPrimary : CompanionTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: parent.checked ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: CompanionTheme.radius
                        color: parent.checked ? CompanionTheme.surfaceRaised : "transparent"
                        border.color: parent.checked ? CompanionTheme.border : "transparent"
                    }
                }
            }

            StackLayout {
                id: contentStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: navigationTabs.currentIndex

                ScrollView {
                    id: contentScrollView
                    clip: true
                    ScrollBar.vertical: ScrollBar {
                        id: verticalScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    GeneralSettingsPage {
                        id: generalPage
                        width: contentScrollView.availableWidth
                        settingsModel: root.settingsModel
                        usageModel: root.routingModel
                        petModel: root.petModel
                    }
                }

                ScrollView {
                    id: accountsContentScrollView
                    clip: true
                    ScrollBar.vertical: ScrollBar {
                        id: accountsVerticalScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    AccountsSettingsPage {
                        id: accountsPage
                        width: accountsContentScrollView.availableWidth
                        settingsModel: root.settingsModel
                    }
                }

                ScrollView {
                    id: chatContentScrollView
                    clip: true
                    ScrollBar.vertical: ScrollBar {
                        id: chatVerticalScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ChatSettingsPage {
                        id: chatPage
                        width: chatContentScrollView.availableWidth
                        settingsModel: root.settingsModel
                        routingModel: root.routingModel
                    }
                }

                ScrollView {
                    id: mobileContentScrollView
                    clip: true
                    ScrollBar.vertical: ScrollBar {
                        id: mobileVerticalScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    MobileSettingsPage {
                        id: mobilePage
                        width: mobileContentScrollView.availableWidth
                        settingsModel: root.settingsModel
                    }
                }

                ScrollView {
                    id: updatesContentScrollView
                    clip: true
                    ScrollBar.vertical: ScrollBar {
                        id: updateVerticalScrollBar
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    UpdatesSettingsPage {
                        id: updatesPage
                        width: updatesContentScrollView.availableWidth
                        updateModel: root.updateModel
                    }
                }
            }
        }
    }
}
