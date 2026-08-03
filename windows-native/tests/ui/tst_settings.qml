import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtTest
import CodexCompanion

TestCase {
    name: "SettingsWindow"
    when: windowShown

    Component {
        id: signalSpy
        SignalSpy {}
    }

    Component {
        id: settingsComponent
        SettingsWindow {
            visible: true
        }
    }

    Component {
        id: fakeSettingsModelComponent
        QtObject {
            property string backdropMode: "mica"
            property string effectiveBackdropMode: "mica"
            property real animationSpeedScale: 1.0
            property bool hideControlsUntilHover: false
            property bool allowAutonomousMovement: true
            property bool hasOpenAIAPIKey: false
            property string openAIAPIKeyStatus: "No OpenAI API key saved."
            property bool hasLumoAPIKey: false
            property string lumoAPIKeyStatus: "No Lumo API key saved."
            property bool codexAccountsAvailable: true
            property var codexAccountProfiles: [{
                id: "11111111-1111-1111-1111-111111111111",
                label: "Main",
                selected: true
            }]
            property string selectedCodexAccountProfileId:
                "11111111-1111-1111-1111-111111111111"
            property string codexAccountSelectionSummary:
                "Main applies to new Codex work. Active and approval-pending tasks stay with their original account."
            property string codexAccountStatus:
                "Signed in using ChatGPT"
            property bool codexAccountRefreshInProgress: false
            property bool automaticallyContinuesAcrossCodexAccounts: false
            property bool mobileEnabled: true
            property bool keepAvailableWhileDisplayOff: true
            property bool allowNearbyOnPublicNetworks: false
            property bool mobilePairingAvailable: true
            property bool mobilePairingActive: false
            property string mobilePairingCode: ""
            property double mobilePairingExpiresAtMilliseconds: 0
            property string mobilePairingLink: ""
            property string mobilePairingQrSource: ""
            property bool hasMobilePairingLink: false
            property string mobilePairingLinkStatus: ""
            property string mobilePairingStatusText:
                "Secure relay pairing is ready."
            property var pairedMobileDevices: []
            property bool mobileNearbyAccessAvailable: false
            property string mobileNearbyAccessStatusText:
                "Nearby Wi-Fi is blocked on this Public network."
            property string relayUrl: ""
            property string relayStatusText: "Nearby access is available."
            property bool relayAutomaticAvailable: true
            property bool relayDisableAvailable: false
            property int beginPairingCount: 0
            property int cancelPairingCount: 0
            property int copyPairingLinkCount: 0
            property int forgottenDeviceCount: 0
            property int savedRelayCount: 0
            property int automaticRelayCount: 0
            property int disabledRelayCount: 0
            property int addedCodexAccountCount: 0
            property int removedCodexAccountCount: 0
            property int codexLoginCount: 0
            property int codexRefreshCount: 0

            function addCodexAccount(label) {
                const normalized = label.trim()
                if (normalized.length === 0) {
                    return false
                }
                ++addedCodexAccountCount
                const id =
                    "22222222-2222-2222-2222-222222222222"
                codexAccountProfiles =
                    codexAccountProfiles.concat([{
                        id: id,
                        label: normalized,
                        selected: true
                    }])
                selectedCodexAccountProfileId = id
                codexAccountSelectionSummary =
                    normalized
                    + " applies to new Codex work. Active and "
                    + "approval-pending tasks stay with their original account."
                codexAccountStatus =
                    normalized + " was added."
                return true
            }

            function removeSelectedCodexAccount() {
                ++removedCodexAccountCount
                codexAccountProfiles = codexAccountProfiles.slice(
                    0,
                    Math.max(0, codexAccountProfiles.length - 1))
                selectedCodexAccountProfileId =
                    codexAccountProfiles.length > 0
                    ? codexAccountProfiles[0].id
                    : ""
                return true
            }

            function beginSelectedCodexAccountLogin() {
                ++codexLoginCount
                codexAccountStatus =
                    "Official Codex sign-in opened."
                return true
            }

            function refreshSelectedCodexAccount() {
                ++codexRefreshCount
                codexAccountStatus =
                    "Signed in using ChatGPT"
                return true
            }

            function beginMobilePairing() {
                ++beginPairingCount
                mobilePairingActive = true
                mobilePairingCode = "123 456"
                mobilePairingExpiresAtMilliseconds = Date.now() + 300000
                mobilePairingLink =
                    "codex-companion://pair?payload=test-payload"
                mobilePairingQrSource =
                    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
                hasMobilePairingLink = true
                mobilePairingLinkStatus = ""
                mobilePairingStatusText =
                    "Open or paste the secure short-lived pairing link in Companion Mobile."
                return true
            }

            function cancelMobilePairing() {
                ++cancelPairingCount
                mobilePairingActive = false
                mobilePairingCode = ""
                mobilePairingExpiresAtMilliseconds = 0
                mobilePairingLink = ""
                mobilePairingQrSource = ""
                hasMobilePairingLink = false
                mobilePairingLinkStatus = ""
                mobilePairingStatusText =
                    "Secure relay pairing is ready."
            }

            function copyMobilePairingLink() {
                if (!hasMobilePairingLink) {
                    return false
                }
                ++copyPairingLinkCount
                mobilePairingLinkStatus =
                    "Pairing link copied."
                return true
            }

            function forgetMobileDevice(deviceId) {
                if (deviceId.length === 0) {
                    return false
                }
                ++forgottenDeviceCount
                pairedMobileDevices = []
                return true
            }

            function saveRelayUrl(value) {
                if (value.length === 0) {
                    return false
                }
                ++savedRelayCount
                relayUrl = value
                relayDisableAvailable = true
                return true
            }

            function disableRelay() {
                ++disabledRelayCount
                relayUrl = ""
                relayDisableAvailable = false
                return true
            }

            function useAutomaticRelay() {
                ++automaticRelayCount
                relayUrl =
                    "wss://codex-companion-relay."
                    + "silverfire-codex-companion."
                    + "workers.dev/relay"
                relayStatusText =
                    "Automatic secure relay restored. "
                    + "Reconnect the paired phone nearby once to synchronize it."
                relayDisableAvailable = true
                return true
            }
        }
    }

    Component {
        id: fakeUpdateModelComponent
        QtObject {
            property string phase: "idle"
            property string installedVersion: "0.3.4"
            property int installedBuild: 1
            property string detail: "Updates are checked against the configured signed release channel."
            property string availableVersion: ""
            property int availableBuild: 0
            property real downloadProgress: 0
            property int checkCount: 0
            property int downloadCount: 0
            property int installCount: 0

            function checkForUpdates() {
                ++checkCount
            }

            function downloadAvailableUpdate() {
                ++downloadCount
            }

            function installReadyUpdate() {
                ++installCount
            }
        }
    }

    Component {
        id: fakeRoutingModelComponent
        QtObject {
            property string routeMode: "local-chat"
            property string selectedChatModelId: "on-device"
            property int showLocalChatCount: 0
            property int showProcessesCount: 0
            property int chatModelSelectionCount: 0
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
            property bool usageLoading: false
            property var usageSnapshot: ({
                "groups": [{
                    "shortWindow": {
                        "remainingPercent": 67.6
                    },
                    "weeklyWindow": {
                        "remainingPercent": 41.5
                    }
                }],
                "availableResetCount": 2
            })
            property string usageErrorMessage: ""
            property int usageRefreshCount: 0

            function showLocalChat() {
                ++showLocalChatCount
                routeMode = "local-chat"
            }

            function showProcesses() {
                ++showProcessesCount
                routeMode = "processes"
            }

            function chooseChatModel(value) {
                ++chatModelSelectionCount
                selectedChatModelId = value
                return true
            }

            function refreshUsage() {
                if (usageLoading) {
                    return
                }
                ++usageRefreshCount
                usageLoading = true
            }
        }
    }

    Component {
        id: fakePetModelComponent
        QtObject {
            property var availablePets: [
                {
                    "id": "shadow-16",
                    "displayName": "Shadow",
                    "sourceTitle": "Built-in",
                    "label": "Shadow \u00b7 Built-in"
                },
                {
                    "id": "custom-pet",
                    "displayName": "Custom Pet",
                    "sourceTitle": "Custom",
                    "label": "Custom Pet \u00b7 Custom"
                }
            ]
            property string selectedPetId: "shadow-16"
            property string selectedAnimation: "idle"
            property int selectPetRequestCount: 0
            property string lastSelectedPetId: ""
            property int reloadPetsRequestCount: 0
            property int selectedAnimationRequestCount: 0
            property string lastSelectedAnimation: ""

            function selectPet(petId) {
                ++selectPetRequestCount
                lastSelectedPetId = petId
                selectedPetId = petId
                return true
            }

            function reloadPets() {
                ++reloadPetsRequestCount
                availablePets = availablePets.concat([{
                    "id": "late-pet",
                    "displayName": "Late Pet",
                    "sourceTitle": "Custom",
                    "label": "Late Pet \u00b7 Custom"
                }])
                return true
            }

            function setSelectedAnimation(animation) {
                ++selectedAnimationRequestCount
                lastSelectedAnimation = animation
                selectedAnimation = animation
            }
        }
    }

    function createWindow() {
        const window = createTemporaryObject(settingsComponent, null)
        verify(window)
        tryCompare(window, "visible", true)
        return window
    }

    function scrollItemIntoView(scrollView, item) {
        const flickable = scrollView.contentItem
        const topInContent = item.mapToItem(
            flickable.contentItem,
            0,
            0)
        const maximumContentY = Math.max(
            0,
            flickable.contentHeight - scrollView.availableHeight)
        flickable.contentY = Math.max(
            0,
            Math.min(
                maximumContentY,
                topInContent.y - 12))
        tryVerify(function() {
            const center = item.mapToItem(
                scrollView,
                item.width / 2,
                item.height / 2)
            return item.visible
                && item.enabled
                && center.x >= 0
                && center.x <= scrollView.availableWidth
                && center.y >= 0
                && center.y <= scrollView.availableHeight
        })
    }

    function clickItemThroughWindow(window, item) {
        const centerInWindow = item.mapToItem(
            window.contentItem,
            item.width / 2,
            item.height / 2)
        mouseClick(
            window.contentItem,
            Math.round(centerInWindow.x),
            Math.round(centerInWindow.y))
    }

    function logViewportGeometry(label, window, scrollView, item) {
        const flickable = scrollView.contentItem
        const centerInScrollView = item.mapToItem(
            scrollView,
            item.width / 2,
            item.height / 2)
        const centerInWindow = item.mapToItem(
            window.contentItem,
            item.width / 2,
            item.height / 2)
        const topInContent = item.mapToItem(
            flickable.contentItem,
            0,
            0)
        console.info(
            "mobile-viewport " + label
            + " availableHeight=" + scrollView.availableHeight
            + " contentY=" + flickable.contentY
            + " contentHeight=" + flickable.contentHeight
            + " itemTopInContent=(" + topInContent.x
            + "," + topInContent.y + ")"
            + " centerInScrollView=(" + centerInScrollView.x
            + "," + centerInScrollView.y + ")"
            + " centerInWindow=(" + centerInWindow.x
            + "," + centerInWindow.y + ")"
            + " visible=" + item.visible
            + " enabled=" + item.enabled)
    }

    function test_window_uses_fixed_utility_size() {
        const window = createWindow()
        compare(window.width, 560)
        compare(window.height, 520)
        verify(window.flags & Qt.Window)
    }

    function test_close_requests_window_only() {
        const window = createWindow()
        const spy = signalSpy.createObject(window, {
            target: window,
            signalName: "closeRequested"
        })
        compare(
            window.closeButton.interactionId,
            "settings.close")
        mouseClick(window.closeButton)
        compare(spy.count, 1)
        compare(Qt.application.state, Qt.ApplicationActive)
    }

    function test_close_control_accessibility_and_tooltip() {
        const window = createWindow()
        compare(window.closeButton.text, "")
        compare(window.closeButton.display, AbstractButton.IconOnly)
        compare(window.closeButton.Accessible.name, "Close Settings")
        compare(window.closeButton.ToolTip.text, "Close Settings")
    }

    function test_native_close_request_hides_nothing_and_reuses_window() {
        const window = createWindow()
        const spy = signalSpy.createObject(window, {
            target: window,
            signalName: "closeRequested"
        })

        window.close()
        tryCompare(spy, "count", 1)
        compare(window.visible, true)

        window.hide()
        compare(window.visible, false)
        window.show()
        tryCompare(window, "visible", true)
    }

    function test_backdrop_selector_has_exact_windows_options() {
        const window = createWindow()
        compare(window.backdropSelector.count, 3)
        compare(window.backdropSelector.textAt(0), "Mica")
        compare(window.backdropSelector.textAt(1), "Windows Glass")
        compare(window.backdropSelector.textAt(2), "Solid Black")
    }

    function test_client_fill_follows_effective_backdrop_not_requested_backdrop() {
        const settingsModel = createTemporaryObject(fakeSettingsModelComponent, null)
        const window = createWindow()
        window.settingsModel = settingsModel

        settingsModel.backdropMode = "mica"
        settingsModel.effectiveBackdropMode = "solid-black"
        compare(window.color.a, 1)
        compare(window.materialSurface.color.a, 1)

        settingsModel.backdropMode = "windows-glass"
        settingsModel.effectiveBackdropMode = "solid-black"
        compare(window.color.a, 1)
        compare(window.materialSurface.color.a, 1)

        settingsModel.effectiveBackdropMode = "mica"
        verify(window.color.a < 1)
        verify(window.materialSurface.color.a < 1)
        const micaChromeAlpha =
            window.materialSurface.color.a

        settingsModel.effectiveBackdropMode = "windows-glass"
        verify(window.color.a < 1)
        verify(window.materialSurface.color.a < 1)
        verify(window.materialSurface.color.a > 0.60)
        verify(window.materialSurface.color.a
               < micaChromeAlpha)
        verify(window.materialSurface.color
               !== CompanionTheme.micaChrome)
    }

    function test_backdrop_selector_uses_compact_dark_chrome() {
        const window = createWindow()
        verify(window.backdropSelectorBackground.color.r < 0.3)
        verify(window.backdropSelectorBackground.color.g < 0.3)
        verify(window.backdropSelectorBackground.color.b < 0.3)
        verify(window.backdropSelectorBackground.radius <= 8)
        verify(window.backdropSelector.implicitHeight <= 32)
    }

    function test_general_accounts_chat_mobile_and_updates_tabs_are_available() {
        const window = createWindow()
        compare(window.navigationTabs.count, 5)
        compare(window.navigationTabs.itemAt(0).text, "General")
        compare(window.navigationTabs.itemAt(1).text, "Accounts")
        compare(window.navigationTabs.itemAt(2).text, "Chat")
        compare(window.navigationTabs.itemAt(3).text, "Mobile")
        compare(window.navigationTabs.itemAt(4).text, "Updates")
        compare(
            window.navigationTabs.itemAt(0).interactionId,
            "settings.tab.general")
        compare(
            window.navigationTabs.itemAt(1).interactionId,
            "settings.tab.accounts")
        compare(
            window.navigationTabs.itemAt(2).interactionId,
            "settings.tab.chat")
        compare(
            window.navigationTabs.itemAt(3).interactionId,
            "settings.tab.mobile")
        compare(
            window.navigationTabs.itemAt(4).interactionId,
            "settings.tab.updates")
    }

    function test_accounts_tab_controls_call_the_model() {
        const settingsModel = createTemporaryObject(
            fakeSettingsModelComponent,
            null)
        const window = createWindow()
        window.settingsModel = settingsModel
        window.navigationTabs.currentIndex = 1
        tryCompare(window.contentStack, "currentIndex", 1)

        verify(window.accountProfileSelector instanceof ComboBox)
        verify(window.accountLabelField instanceof TextField)
        verify(window.accountSignInButton instanceof Button)
        verify(window.accountRefreshButton instanceof Button)
        verify(window.accountRemoveButton instanceof Button)
        verify(window.accountAddButton instanceof Button)
        verify(window.accountContinuationSwitch instanceof Switch)
        compare(window.accountProfileSelector.count, 1)
        compare(
            window.accountProfileSelector.currentValue,
            "11111111-1111-1111-1111-111111111111")
        compare(
            window.accountProfileSelector.Accessible.name,
            "New work account")
        compare(
            window.accountSignInButton.interactionId,
            "settings.accounts.sign-in")
        compare(
            window.accountRefreshButton.interactionId,
            "settings.accounts.refresh")
        compare(
            window.accountRemoveButton.interactionId,
            "settings.accounts.remove")
        compare(
            window.accountAddButton.interactionId,
            "settings.accounts.add")
        compare(
            window.accountContinuationSwitch.interactionId,
            "settings.accounts.automatic-continuation")
        compare(
            window.accountSelectionSummaryLabel.text,
            settingsModel.codexAccountSelectionSummary)
        compare(
            window.accountStatusLabel.text,
            "Signed in using ChatGPT")

        mouseClick(window.accountSignInButton)
        compare(settingsModel.codexLoginCount, 1)
        mouseClick(window.accountRefreshButton)
        compare(settingsModel.codexRefreshCount, 1)

        window.accountsContentScrollView.contentItem.contentY =
            Math.max(
                0,
                window.accountsContentScrollView.contentItem.contentHeight
                    - window.accountsContentScrollView.availableHeight)
        wait(50)
        window.accountLabelField.text = " Account 2 "
        mouseClick(window.accountAddButton)
        compare(settingsModel.addedCodexAccountCount, 1)
        compare(window.accountLabelField.text, "")
        tryCompare(window.accountProfileSelector, "count", 2)
        tryCompare(
            window.accountProfileSelector,
            "currentValue",
            "22222222-2222-2222-2222-222222222222")

        mouseClick(window.accountContinuationSwitch)
        tryCompare(
            settingsModel,
            "automaticallyContinuesAcrossCodexAccounts",
            true)

        mouseClick(window.accountRemoveButton)
        compare(settingsModel.removedCodexAccountCount, 1)
        tryCompare(window.accountProfileSelector, "count", 1)
    }

    function test_account_continuation_switch_persists_through_real_model() {
        const model = settingsTestSupport.createViewModel()
        verify(model)
        const window = createWindow()
        window.settingsModel = model
        window.navigationTabs.currentIndex = 1
        tryCompare(window.contentStack, "currentIndex", 1)

        window.accountsContentScrollView.contentItem.contentY =
            Math.max(
                0,
                window.accountsContentScrollView.contentItem.contentHeight
                    - window.accountsContentScrollView.availableHeight)
        wait(50)
        compare(window.accountContinuationSwitch.checked, false)
        mouseClick(window.accountContinuationSwitch)
        tryCompare(
            model,
            "automaticallyContinuesAcrossCodexAccounts",
            true)
        compare(
            settingsTestSupport
                .persistedAutomaticCodexAccountContinuation(
                    model),
            true)
    }

    function test_updates_tab_dispatches_state_appropriate_actions() {
        const updateModel = createTemporaryObject(
            fakeUpdateModelComponent,
            null)
        const window = createWindow()
        window.updateModel = updateModel
        window.navigationTabs.currentIndex = 4
        tryCompare(window.contentStack, "currentIndex", 4)

        compare(
            window.updateInstalledVersionLabel.text,
            "0.3.4 (1)")
        compare(
            window.updatePrimaryActionButton.text,
            "Check for Updates")
        compare(
            window.updatePrimaryActionButton.interactionId,
            "settings.update.primary")
        mouseClick(window.updatePrimaryActionButton)
        compare(updateModel.checkCount, 1)

        updateModel.phase = "available"
        updateModel.availableVersion = "0.3.5"
        updateModel.availableBuild = 2
        updateModel.detail = "Version 0.3.5 is available."
        tryCompare(
            window.updatePrimaryActionButton,
            "text",
            "Download Verified Update")
        mouseClick(window.updatePrimaryActionButton)
        compare(updateModel.downloadCount, 1)

        updateModel.phase = "ready-to-install"
        updateModel.detail =
            "Version 0.3.5 is verified and ready to install."
        tryCompare(
            window.updatePrimaryActionButton,
            "text",
            "Install and Relaunch")
        mouseClick(window.updatePrimaryActionButton)
        compare(updateModel.installCount, 1)
    }

    function test_updates_tab_shows_progress_without_visible_scrollbar() {
        const updateModel = createTemporaryObject(
            fakeUpdateModelComponent,
            null)
        updateModel.phase = "downloading"
        updateModel.downloadProgress = 0.45
        updateModel.detail =
            "Downloading and verifying version 0.3.5..."

        const window = createWindow()
        window.updateModel = updateModel
        window.navigationTabs.currentIndex = 4
        tryCompare(window.contentStack, "currentIndex", 4)

        compare(window.updateProgressBar.visible, true)
        compare(window.updateProgressBar.value, 0.45)
        compare(window.updatePrimaryActionButton.enabled, false)
        compare(
            window.updateVerticalScrollBar.policy,
            ScrollBar.AlwaysOff)
        compare(window.updateVerticalScrollBar.visible, false)
    }

    function test_appearance_is_separate_from_pet_group() {
        const window = createWindow()
        compare(window.appearanceRowLabel.text, "Appearance")
        compare(window.petSectionLabel.text, "Pet")
        const appearanceY = window.appearanceRowLabel.mapToItem(window.contentItem, 0, 0).y
        const backdropY = window.backdropSelector.mapToItem(window.contentItem, 0, 0).y
        const petY = window.petSectionLabel.mapToItem(window.contentItem, 0, 0).y
        const pacingY = window.animationPacingSlider.mapToItem(window.contentItem, 0, 0).y
        verify(appearanceY < petY)
        verify(backdropY < petY)
        verify(petY < pacingY)
    }

    function test_scrollbar_chrome_is_hidden() {
        const window = createWindow()
        compare(window.verticalScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.verticalScrollBar.visible, false)
        compare(window.accountsVerticalScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.accountsVerticalScrollBar.visible, false)
        compare(window.chatVerticalScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.chatVerticalScrollBar.visible, false)
        compare(window.mobileVerticalScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.mobileVerticalScrollBar.visible, false)
        compare(window.updateVerticalScrollBar.policy, ScrollBar.AlwaysOff)
        compare(window.updateVerticalScrollBar.visible, false)
    }

    function test_general_controls_use_native_control_types() {
        const window = createWindow()
        verify(window.backdropSelector instanceof ComboBox)
        verify(window.petSelector instanceof ComboBox)
        verify(window.previewAnimationSelector instanceof ComboBox)
        verify(window.reloadPetsButton instanceof Button)
        verify(window.hideControlsSwitch instanceof Switch)
        verify(window.autonomousMovementSwitch instanceof Switch)
        verify(window.animationPacingSlider instanceof Slider)
    }

    function test_general_controls_have_accessible_names() {
        const window = createWindow()
        compare(window.backdropSelector.Accessible.name, "Appearance")
        compare(window.petSelector.Accessible.name, "Pet")
        compare(
            window.previewAnimationSelector.Accessible.name,
            "Preview animation")
        compare(window.animationPacingSlider.Accessible.name, "Animation pacing")
        compare(window.hideControlsSwitch.Accessible.name, "Hide tray controls until pet hover")
        compare(window.autonomousMovementSwitch.Accessible.name, "Allow autonomous movement")
        compare(
            window.backdropSelector.interactionId,
            "settings.appearance")
        compare(
            window.petSelector.interactionId,
            "settings.pet.select")
        compare(
            window.previewAnimationSelector.interactionId,
            "settings.pet.preview-animation")
        compare(
            window.reloadPetsButton.interactionId,
            "settings.pet.reload")
        compare(
            window.reloadPetsButton.Accessible.name,
            "Reload Pets")
        compare(
            window.animationPacingSlider.interactionId,
            "settings.animation.pacing")
        compare(
            window.hideControlsSwitch.interactionId,
            "settings.controls.hover-only")
        compare(
            window.autonomousMovementSwitch.interactionId,
            "settings.movement.autonomous")
    }

    function test_pet_selector_and_reload_control_live_catalog() {
        const petModel = createTemporaryObject(
            fakePetModelComponent,
            null)
        const window = createWindow()
        window.petModel = petModel

        const selector = window.petSelector
        compare(selector.count, 2)
        compare(
            selector.textAt(0),
            "Shadow \u00b7 Built-in")
        compare(
            selector.textAt(1),
            "Custom Pet \u00b7 Custom")
        compare(selector.currentValue, "shadow-16")

        mouseClick(selector)
        tryCompare(selector.popup, "visible", true)
        tryVerify(function() {
            selector.popup.contentItem.forceLayout()
            return selector.popup.contentItem
                .itemAtIndex(1) !== null
        })
        const customDelegate =
            selector.popup.contentItem.itemAtIndex(1)
        mouseClick(
            customDelegate,
            customDelegate.width / 2,
            customDelegate.height / 2)
        tryCompare(selector.popup, "visible", false)
        compare(petModel.selectPetRequestCount, 1)
        compare(
            petModel.lastSelectedPetId,
            "custom-pet")
        compare(petModel.selectedPetId, "custom-pet")
        compare(selector.currentValue, "custom-pet")

        mouseClick(window.reloadPetsButton)
        compare(petModel.reloadPetsRequestCount, 1)
        tryCompare(selector, "count", 3)
        compare(selector.currentValue, "custom-pet")
    }

    function test_preview_animation_selector_controls_live_pet_model() {
        const petModel = createTemporaryObject(
            fakePetModelComponent,
            null)
        const window = createWindow()
        window.petModel = petModel

        const selector = window.previewAnimationSelector
        compare(selector.count, 12)
        compare(selector.textAt(0), "Idle")
        compare(selector.textAt(1), "Run Right")
        compare(selector.textAt(2), "Run Left")
        compare(selector.textAt(3), "Wave")
        compare(selector.textAt(4), "Jump")
        compare(selector.textAt(5), "Failed")
        compare(selector.textAt(6), "Waiting")
        compare(selector.textAt(7), "Running")
        compare(selector.textAt(8), "Review")
        compare(selector.textAt(9), "Goal Complete")
        compare(selector.textAt(10), "Thinking")
        compare(selector.textAt(11), "Talking")
        compare(selector.currentValue, "idle")

        mouseClick(selector)
        tryCompare(selector.popup, "visible", true)
        tryVerify(function() {
            selector.popup.contentItem.forceLayout()
            return selector.popup.contentItem.itemAtIndex(3) !== null
        })
        const waveDelegate =
            selector.popup.contentItem.itemAtIndex(3)
        mouseClick(
            waveDelegate,
            waveDelegate.width / 2,
            waveDelegate.height / 2)
        tryCompare(selector.popup, "visible", false)
        compare(
            petModel.selectedAnimationRequestCount,
            1)
        compare(
            petModel.lastSelectedAnimation,
            "waving")
        compare(petModel.selectedAnimation, "waving")
        compare(selector.currentValue, "waving")

        petModel.selectedAnimation = "talking"
        tryCompare(selector, "currentValue", "talking")
    }

    function test_general_codex_usage_matches_macos_summary_and_refresh() {
        const routingModel = createTemporaryObject(
            fakeRoutingModelComponent,
            null)
        const window = createWindow()
        window.routingModel = routingModel

        compare(window.usageSectionLabel.text, "Codex Usage")
        compare(window.usageRemainingLabel.text, "Remaining")
        compare(
            window.usageSummaryLabel.text,
            "68% short left \u00b7 42% weekly left \u00b7 2 resets")
        compare(
            window.usageDisclosureLabel.text,
            "Banked resets are only applied after explicit confirmation.")
        verify(window.usageRefreshButton instanceof Button)
        compare(window.usageRefreshButton.text, "Refresh Usage")
        compare(
            window.usageRefreshButton.interactionId,
            "settings.usage.refresh")
        compare(
            window.usageRefreshButton.Accessible.name,
            "Refresh Usage")

        routingModel.usageSnapshot = ({})
        routingModel.usageLoading = true
        tryCompare(
            window.usageSummaryLabel,
            "text",
            "Checking usage...")

        routingModel.usageLoading = false
        routingModel.usageErrorMessage =
            "Codex usage service is unavailable."
        tryCompare(
            window.usageSummaryLabel,
            "text",
            "Codex usage service is unavailable.")

        routingModel.usageErrorMessage = ""
        tryCompare(
            window.usageSummaryLabel,
            "text",
            "Rate limits unavailable")

        routingModel.usageSnapshot = ({
            "groups": [{
                "shortWindow": {
                    "remainingPercent": 99.5
                }
            }],
            "availableResetCount": 1
        })
        tryCompare(
            window.usageSummaryLabel,
            "text",
            "100% short left \u00b7 1 reset")

        window.generalContentScrollView.contentItem.contentY = Math.max(
            0,
            window.generalContentScrollView.contentItem.contentHeight
                - window.generalContentScrollView.availableHeight)
        wait(50)
        mouseClick(window.usageRefreshButton)
        compare(routingModel.usageRefreshCount, 1)
        tryCompare(window.usageRefreshButton, "enabled", false)
        compare(
            window.usageRefreshButton.Accessible.name,
            "Refreshing Usage")
    }

    function test_general_controls_persist_through_real_settings_view_model() {
        const model = settingsTestSupport.createViewModel()
        verify(model)
        const window = createWindow()
        window.settingsModel = model
        settingsTestSupport.watchModel(model)

        mouseClick(window.backdropSelector)
        tryCompare(window.backdropSelector.popup, "visible", true)
        tryVerify(function() {
            window.backdropSelector.popup.contentItem.forceLayout()
            return window.backdropSelector.popup.contentItem.itemAtIndex(1) !== null
        })
        const windowsGlassDelegate = window.backdropSelector.popup.contentItem.itemAtIndex(1)
        mouseClick(windowsGlassDelegate,
                   windowsGlassDelegate.width / 2,
                   windowsGlassDelegate.height / 2)
        tryCompare(window.backdropSelector.popup, "visible", false)
        tryCompare(settingsTestSupport, "lastPersistedBackdropMode", "windows-glass")

        settingsTestSupport.watchModel(model)
        const beforeSpeed = settingsTestSupport.persistedAnimationSpeedScale(model)
        mouseDrag(window.animationPacingSlider,
                  window.animationPacingSlider.width * 0.2,
                  window.animationPacingSlider.height / 2,
                  window.animationPacingSlider.width * 0.45,
                  0,
                  Qt.LeftButton)
        verify(settingsTestSupport.persistedAnimationSpeedScale(model) > beforeSpeed)

        mouseClick(window.hideControlsSwitch)
        compare(settingsTestSupport.persistedHideControlsUntilHover(model), true)

        mouseClick(window.autonomousMovementSwitch)
        compare(settingsTestSupport.persistedAllowAutonomousMovement(model), false)
    }

    function test_chat_credentials_use_secure_fields_and_real_actions() {
        const model = settingsTestSupport.createViewModel()
        const routingModel = createTemporaryObject(
            fakeRoutingModelComponent,
            null)
        verify(model)
        const window = createWindow()
        window.settingsModel = model
        window.routingModel = routingModel
        window.navigationTabs.currentIndex = 2
        tryCompare(window.contentStack, "currentIndex", 2)

        compare(window.openAIKeyField.echoMode, TextInput.Password)
        compare(window.lumoKeyField.echoMode, TextInput.Password)
        compare(window.openAIKeyField.accessibleName, "OpenAI API key")
        compare(window.lumoKeyField.accessibleName, "Lumo API key")
        compare(window.openAIRemoveButton.enabled, false)
        compare(window.lumoRemoveButton.enabled, false)
        compare(
            window.openAISaveButton.interactionId,
            "settings.openai.key.save")
        compare(
            window.openAIRemoveButton.interactionId,
            "settings.openai.key.remove")
        compare(
            window.lumoSaveButton.interactionId,
            "settings.lumo.key.save")
        compare(
            window.lumoRemoveButton.interactionId,
            "settings.lumo.key.remove")

        window.chatDeliverySelector.currentIndex = 1
        window.chatDeliverySelector.activated(1)
        tryCompare(window.openAISectionLabel, "visible", true)
        window.openAIKeyField.text = "  qml-openai-secret  "
        compare(window.openAISaveButton.enabled, true)
        mouseClick(window.openAISaveButton)
        tryCompare(model, "hasOpenAIAPIKey", true)
        compare(window.openAIKeyField.text, "")
        compare(window.openAISaveButton.text, "Replace Key")
        compare(window.openAIRemoveButton.enabled, true)

        window.chatDeliverySelector.currentIndex = 2
        window.chatDeliverySelector.activated(2)
        tryCompare(window.lumoSectionLabel, "visible", true)
        window.lumoKeyField.text = "qml-lumo-secret"
        compare(window.lumoSaveButton.enabled, true)
        mouseClick(window.lumoSaveButton)
        tryCompare(model, "hasLumoAPIKey", true)
        compare(window.lumoKeyField.text, "")
        compare(window.lumoSaveButton.text, "Replace Key")
        compare(window.lumoRemoveButton.enabled, true)

        window.chatDeliverySelector.currentIndex = 1
        window.chatDeliverySelector.activated(1)
        tryCompare(window.openAISectionLabel, "visible", true)
        mouseClick(window.openAIRemoveButton)
        tryCompare(model, "hasOpenAIAPIKey", false)
        compare(window.openAIRemoveButton.enabled, false)

        window.chatDeliverySelector.currentIndex = 2
        window.chatDeliverySelector.activated(2)
        tryCompare(window.lumoSectionLabel, "visible", true)
        mouseClick(window.lumoRemoveButton)
        tryCompare(model, "hasLumoAPIKey", false)
        compare(window.lumoRemoveButton.enabled, false)
    }

    function test_chat_default_mode_controls_live_shell_route() {
        const routingModel = createTemporaryObject(
            fakeRoutingModelComponent,
            null)
        const window = createWindow()
        window.routingModel = routingModel
        window.navigationTabs.currentIndex = 2
        tryCompare(window.contentStack, "currentIndex", 2)

        const selector = window.defaultModeSelector
        verify(selector instanceof ComboBox)
        compare(selector.Accessible.name, "Default mode")
        compare(selector.count, 2)
        compare(selector.textAt(0), "Chat")
        compare(selector.textAt(1), "Codex")
        compare(selector.currentIndex, 0)
        compare(
            selector.interactionId,
            "settings.route.default")

        mouseClick(selector)
        tryCompare(selector.popup, "visible", true)
        tryVerify(function() {
            selector.popup.contentItem.forceLayout()
            return selector.popup.contentItem.itemAtIndex(1) !== null
        })
        const codexDelegate =
            selector.popup.contentItem.itemAtIndex(1)
        mouseClick(
            codexDelegate,
            codexDelegate.width / 2,
            codexDelegate.height / 2)
        tryCompare(routingModel, "routeMode", "processes")
        compare(routingModel.showProcessesCount, 1)
        compare(selector.currentIndex, 1)

        routingModel.showLocalChat()
        tryCompare(selector, "currentIndex", 0)
        compare(routingModel.showLocalChatCount, 1)
    }

    function test_chat_delivery_and_models_follow_live_shell_selection() {
        const routingModel = createTemporaryObject(
            fakeRoutingModelComponent,
            null)
        const window = createWindow()
        window.routingModel = routingModel
        window.navigationTabs.currentIndex = 2
        tryCompare(window.contentStack, "currentIndex", 2)

        const delivery = window.chatDeliverySelector
        verify(delivery instanceof ComboBox)
        compare(delivery.Accessible.name, "Chat delivery")
        compare(delivery.interactionId, "settings.chat.delivery")
        compare(delivery.count, 3)
        compare(delivery.textAt(0), "On-device Windows model")
        compare(delivery.textAt(1), "OpenAI API")
        compare(delivery.textAt(2), "Lumo API")
        compare(delivery.currentValue, "on-device")
        compare(
            window.chatDeliveryDescription.text,
            "Reasons on this PC without an API key or Codex usage; "
            + "live tools contact their data sources when needed.")
        compare(window.openAISectionLabel.visible, false)
        compare(window.lumoSectionLabel.visible, false)

        delivery.currentIndex = 1
        delivery.activated(1)
        tryCompare(
            routingModel,
            "selectedChatModelId",
            "openai:gpt56Luna")
        compare(routingModel.chatModelSelectionCount, 1)
        compare(window.openAISectionLabel.visible, true)
        compare(window.lumoSectionLabel.visible, false)
        compare(window.openAIModelSelector.count, 3)
        compare(window.openAIModelSelector.currentValue,
                "openai:gpt56Luna")
        compare(window.openAIModelSelector.interactionId,
                "settings.openai.model")
        compare(
            window.chatDeliveryDescription.text,
            "Answers inside Companion through your OpenAI API key.")

        window.openAIModelSelector.currentIndex = 2
        window.openAIModelSelector.activated(2)
        tryCompare(
            routingModel,
            "selectedChatModelId",
            "openai:gpt56Sol")

        delivery.currentIndex = 2
        delivery.activated(2)
        tryCompare(
            routingModel,
            "selectedChatModelId",
            "lumo:automatic")
        compare(window.openAISectionLabel.visible, false)
        compare(window.lumoSectionLabel.visible, true)
        compare(window.lumoModelSelector.count, 3)
        compare(window.lumoModelSelector.currentValue,
                "lumo:automatic")
        compare(window.lumoModelSelector.interactionId,
                "settings.lumo.model")
        compare(
            window.chatDeliveryDescription.text,
            "Answers inside Companion through an API key included with Lumo+.")

        window.lumoModelSelector.currentIndex = 2
        window.lumoModelSelector.activated(2)
        tryCompare(
            routingModel,
            "selectedChatModelId",
            "lumo:thinking")

        delivery.currentIndex = 0
        delivery.activated(0)
        tryCompare(
            routingModel,
            "selectedChatModelId",
            "on-device")
        compare(window.openAISectionLabel.visible, false)
        compare(window.lumoSectionLabel.visible, false)
    }

    function test_mobile_pairing_and_relay_controls_call_the_model() {
        const settingsModel = createTemporaryObject(fakeSettingsModelComponent, null)
        settingsModel.pairedMobileDevices = [{
            deviceId: "iphone-alpha",
            displayName: "Harlin iPhone",
            pairedAtMilliseconds: Date.now()
        }]
        const window = createWindow()
        window.settingsModel = settingsModel
        window.navigationTabs.currentIndex = 3
        tryCompare(window.contentStack, "currentIndex", 3)

        verify(window.mobileEnabledSwitch instanceof Switch)
        verify(window.keepAvailableSwitch instanceof Switch)
        verify(window.publicNetworkSwitch instanceof Switch)
        compare(window.mobileEnabledSwitch.Accessible.name, "Enable mobile access")
        compare(window.keepAvailableSwitch.Accessible.name,
                "Keep Windows available while display is off")
        compare(window.publicNetworkSwitch.Accessible.name,
                "Allow nearby access on Public networks")
        compare(window.publicNetworkSwitch.checked, false)
        compare(window.pairButton.visible, true)
        compare(window.pairButton.enabled, true)
        compare(window.cancelPairingButton.visible, false)
        compare(
            window.mobileEnabledSwitch.interactionId,
            "settings.mobile.enabled")
        compare(
            window.keepAvailableSwitch.interactionId,
            "settings.mobile.keep-available")
        compare(
            window.publicNetworkSwitch.interactionId,
            "settings.mobile.public-network")
        compare(
            window.pairButton.interactionId,
            "settings.mobile.pair")
        compare(
            window.cancelPairingButton.interactionId,
            "settings.mobile.cancel-pairing")
        compare(
            window.relaySaveButton.interactionId,
            "settings.relay.save")
        compare(
            window.relayAutomaticButton.interactionId,
            "settings.relay.automatic")
        compare(
            window.relayDisableButton.interactionId,
            "settings.relay.disable")
        compare(
            window.nearbyStatusLabel.text,
            "Nearby Wi-Fi is blocked on this Public network.")
        compare(
            window.nearbyStatusLabel.color,
            CompanionTheme.warning)

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.pairButton)
        clickItemThroughWindow(
            window,
            window.pairButton)
        compare(settingsModel.beginPairingCount, 1)
        tryCompare(window.pairingCodeLabel, "text", "123 456")
        compare(window.pairButton.visible, false)
        compare(window.cancelPairingButton.visible, true)
        compare(window.pairingQrImage.visible, true)
        compare(
            window.pairingQrImage.source.toString(),
            settingsModel.mobilePairingQrSource)
        tryCompare(
            window.pairingQrImage,
            "status",
            Image.Ready)
        logViewportGeometry(
            "copy-link-before-scroll",
            window,
            window.mobileContentScrollView,
            window.copyPairingLinkButton)
        compare(
            window.pairingQrImage.Accessible.name,
            "Scan to pair this iPhone")
        compare(window.pairingLinkField.visible, true)
        compare(window.pairingLinkField.readOnly, true)
        compare(window.pairingLinkField.selectByMouse, true)
        compare(
            window.pairingLinkField.text,
            "codex-companion://pair?payload=test-payload")
        compare(window.copyPairingLinkButton.enabled, true)
        compare(
            window.copyPairingLinkButton.interactionId,
            "settings.mobile.copy-pairing-link")

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.copyPairingLinkButton)
        logViewportGeometry(
            "copy-link-after-scroll",
            window,
            window.mobileContentScrollView,
            window.copyPairingLinkButton)
        clickItemThroughWindow(
            window,
            window.copyPairingLinkButton)
        compare(settingsModel.copyPairingLinkCount, 1)
        compare(
            window.pairingLinkStatusLabel.text,
            "Pairing link copied.")

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.cancelPairingButton)
        clickItemThroughWindow(
            window,
            window.cancelPairingButton)
        compare(settingsModel.cancelPairingCount, 1)
        tryCompare(window.pairButton, "visible", true)
        compare(window.pairingQrImage.visible, false)
        compare(window.pairingLinkField.visible, false)
        compare(window.copyPairingLinkButton.enabled, false)

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.publicNetworkSwitch)
        clickItemThroughWindow(
            window,
            window.publicNetworkSwitch)
        tryCompare(settingsModel, "allowNearbyOnPublicNetworks", true)
        settingsModel.mobileNearbyAccessAvailable = true
        settingsModel.mobileNearbyAccessStatusText =
            "Nearby Wi-Fi is available on this Public network by your explicit setting."
        tryCompare(window.pairButton, "enabled", true)

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.pairButton)
        clickItemThroughWindow(
            window,
            window.pairButton)
        compare(settingsModel.beginPairingCount, 2)
        tryCompare(window.pairingCodeLabel, "text", "123 456")
        compare(window.pairButton.visible, false)
        compare(window.cancelPairingButton.visible, true)

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.cancelPairingButton)
        clickItemThroughWindow(
            window,
            window.cancelPairingButton)
        compare(settingsModel.cancelPairingCount, 2)
        tryCompare(window.pairButton, "visible", true)

        compare(window.pairedDeviceRepeater.count, 1)
        const deviceRow = window.pairedDeviceRepeater.itemAt(0)
        verify(deviceRow)
        compare(
            deviceRow.forgetButton.interactionId,
            "settings.mobile.forget")
        logViewportGeometry(
            "forget-before-scroll",
            window,
            window.mobileContentScrollView,
            deviceRow.forgetButton)
        scrollItemIntoView(
            window.mobileContentScrollView,
            deviceRow.forgetButton)
        logViewportGeometry(
            "forget-after-scroll",
            window,
            window.mobileContentScrollView,
            deviceRow.forgetButton)
        clickItemThroughWindow(
            window,
            deviceRow.forgetButton)
        compare(settingsModel.forgottenDeviceCount, 1)
        tryCompare(window.pairedDeviceRepeater, "count", 0)

        window.relayUrlField.text = "wss://relay.example.test/socket"
        scrollItemIntoView(
            window.mobileContentScrollView,
            window.relaySaveButton)
        clickItemThroughWindow(
            window,
            window.relaySaveButton)
        compare(settingsModel.savedRelayCount, 1)
        compare(window.relayDisableButton.enabled, true)

        compare(window.relayAutomaticButton.text, "Use Automatic")
        compare(
            window.relayAutomaticButton.Accessible.name,
            "Use automatic secure relay")
        compare(window.relayAutomaticButton.enabled, true)
        scrollItemIntoView(
            window.mobileContentScrollView,
            window.relayAutomaticButton)
        clickItemThroughWindow(
            window,
            window.relayAutomaticButton)
        compare(settingsModel.automaticRelayCount, 1)
        compare(
            window.relayUrlField.text,
            "wss://codex-companion-relay."
            + "silverfire-codex-companion."
            + "workers.dev/relay")

        scrollItemIntoView(
            window.mobileContentScrollView,
            window.relayDisableButton)
        clickItemThroughWindow(
            window,
            window.relayDisableButton)
        compare(settingsModel.disabledRelayCount, 1)
        compare(window.relayUrlField.text, "")
        compare(window.relayDisableButton.enabled, false)
    }

    function test_mobile_disabled_hides_runtime_sections() {
        const settingsModel = createTemporaryObject(
            fakeSettingsModelComponent,
            null)
        settingsModel.mobileEnabled = false
        settingsModel.pairedMobileDevices = [{
            deviceId: "iphone-alpha",
            displayName: "Harlin iPhone",
            pairedAtMilliseconds: Date.now()
        }]

        const window = createWindow()
        window.settingsModel = settingsModel
        window.navigationTabs.currentIndex = 3
        tryCompare(window.contentStack, "currentIndex", 3)

        compare(window.mobileEnabledSwitch.checked, false)
        compare(
            window.mobileRuntimeStatusLabel.text,
            "Discovery, relay connections, mobile task services, "
            + "and display-sleep availability are stopped and released.")
        compare(window.keepAvailableSwitch.visible, false)
        compare(window.publicNetworkSwitch.visible, false)
        compare(window.nearbyStatusLabel.visible, false)
        compare(window.pairButton.visible, false)
        compare(window.relayUrlField.visible, false)
        compare(window.relaySaveButton.visible, false)
        compare(window.relayAutomaticButton.visible, false)
        compare(window.relayDisableButton.visible, false)
        compare(
            window.pairedDeviceRepeater.itemAt(0).visible,
            false)

        mouseClick(window.mobileEnabledSwitch)
        tryCompare(settingsModel, "mobileEnabled", true)
        compare(
            window.mobileRuntimeStatusLabel.text,
            "Nearby discovery and encrypted access for paired devices "
            + "are active.")
        tryCompare(window.keepAvailableSwitch, "visible", true)
        compare(window.publicNetworkSwitch.visible, true)
        compare(window.nearbyStatusLabel.visible, true)
        compare(window.pairButton.visible, true)
        compare(window.relayUrlField.visible, true)
        compare(window.relaySaveButton.visible, true)
        compare(window.relayAutomaticButton.visible, true)
        compare(window.relayDisableButton.visible, true)
        compare(
            window.pairedDeviceRepeater.itemAt(0).visible,
            true)
    }

    function test_mobile_public_network_switch_persists_through_real_model() {
        const model = settingsTestSupport.createViewModel()
        verify(model)
        const window = createWindow()
        window.settingsModel = model
        window.navigationTabs.currentIndex = 3
        tryCompare(window.contentStack, "currentIndex", 3)

        compare(window.publicNetworkSwitch.checked, false)
        mouseClick(window.publicNetworkSwitch)
        tryCompare(model, "allowNearbyOnPublicNetworks", true)
        compare(
            settingsTestSupport
                .persistedAllowNearbyOnPublicNetworks(model),
            true)

        mouseClick(window.publicNetworkSwitch)
        tryCompare(model, "allowNearbyOnPublicNetworks", false)
        compare(
            settingsTestSupport
                .persistedAllowNearbyOnPublicNetworks(model),
            false)
    }

    function test_longest_labels_fit_at_target_size() {
        const window = createWindow()
        const labels = [
            window.animationPacingLabel,
            window.hideControlsLabel,
            window.autonomousMovementLabel
        ]

        for (let index = 0; index < labels.length; ++index) {
            verify(labels[index].paintedWidth <= labels[index].width)
        }
    }
}
