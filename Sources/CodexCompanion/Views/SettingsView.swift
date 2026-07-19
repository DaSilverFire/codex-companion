import AppKit
import SwiftUI

enum CompanionSettingsTab: String, CaseIterable, Identifiable {
    case general
    case chat
    case mobile
    case updates

    var id: Self { self }

    var title: String {
        switch self {
        case .general: "General"
        case .chat: "Chat"
        case .mobile: "Mobile"
        case .updates: "Updates"
        }
    }

    var systemImage: String {
        switch self {
        case .general: "slider.horizontal.3"
        case .chat: "bubble.left.and.bubble.right"
        case .mobile: "iphone"
        case .updates: "arrow.triangle.2.circlepath"
        }
    }
}

struct SettingsView: View {
    @ObservedObject var model: CompanionAppModel
    @ObservedObject private var mobileRuntime = CompanionMobileRuntimeController.shared
    @StateObject private var updateService = CompanionUpdateService()
    @State private var selectedTab: CompanionSettingsTab = .general
    @State private var mobilePairing: CompanionBridgeActivePairing?
    @State private var pairedMobileDevices: [CompanionPairingRecord] = []
    @State private var relayURLInput = ""
    @State private var relayStatusText = ""
    @State private var keepMacAvailableWhileDisplayOff =
        CompanionPowerAvailabilityPreferences().keepMacAvailableWhileDisplayOff

    var body: some View {
        TabView(selection: $selectedTab) {
            generalSettings
                .tabItem { Label(CompanionSettingsTab.general.title, systemImage: CompanionSettingsTab.general.systemImage) }
                .tag(CompanionSettingsTab.general)

            chatSettings
                .tabItem { Label(CompanionSettingsTab.chat.title, systemImage: CompanionSettingsTab.chat.systemImage) }
                .tag(CompanionSettingsTab.chat)

            if mobileRuntime.isAvailable {
                mobileSettings
                    .tabItem { Label(CompanionSettingsTab.mobile.title, systemImage: CompanionSettingsTab.mobile.systemImage) }
                    .tag(CompanionSettingsTab.mobile)
            }

            updateSettings
                .tabItem { Label(CompanionSettingsTab.updates.title, systemImage: CompanionSettingsTab.updates.systemImage) }
                .tag(CompanionSettingsTab.updates)
        }
        .frame(width: 560, height: 520)
        .onAppear {
            reloadMobileSettingsIfEnabled()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: CompanionPairingCoordinator.pairingStateDidChange
            )
        ) { _ in
            guard mobileRuntime.isEnabled else { return }
            mobilePairing = CompanionPairingCoordinator.shared.activePairing()
            reloadPairedMobileDevices()
        }
        .onChange(of: mobileRuntime.isEnabled) { _, isEnabled in
            if isEnabled {
                reloadMobileSettingsIfEnabled()
            } else {
                mobilePairing = nil
                pairedMobileDevices = []
                relayStatusText = ""
            }
        }
        .task(id: mobilePairing?.expiresAt) {
            guard let expiresAt = mobilePairing?.expiresAt else { return }
            let delay = max(0, expiresAt.timeIntervalSinceNow)
            try? await Task.sleep(for: .seconds(delay))
            if CompanionPairingCoordinator.shared.activePairing() == nil {
                mobilePairing = nil
                reloadPairedMobileDevices()
            }
        }
    }

    private var generalSettings: some View {
        Form {
            Section("Pet") {
                Picker("Pet", selection: petSelection) {
                    ForEach(model.petStore.pets) { pet in
                        Text("\(pet.displayName) · \(pet.source.title)")
                            .tag(pet.id)
                    }
                }

                Picker("Preview animation", selection: $model.selectedState) {
                    ForEach(PetAnimationState.allCases) { state in
                        Text(state.title).tag(state)
                    }
                }

                Slider(value: $model.animationSpeedScale, in: 0.75...2.5) {
                    Text("Animation pacing")
                } minimumValueLabel: {
                    Text("Faster")
                } maximumValueLabel: {
                    Text("Slower")
                }

                Toggle("Hide tray controls until pet hover", isOn: $model.hidesMenuButtonUntilHover)
                Toggle("Allow autonomous movement", isOn: $model.allowsAutonomousPetMovement)

                Button {
                    model.petStore.reload()
                } label: {
                    Label("Reload Pets", systemImage: "arrow.clockwise")
                }
            }

            Section("Codex Usage") {
                LabeledContent("Remaining") {
                    Text(model.rateLimitStore.menuSummary)
                        .foregroundStyle(.secondary)
                }

                Text("Banked resets are only applied after explicit confirmation.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Button {
                    model.rateLimitStore.refresh()
                } label: {
                    Label("Refresh Usage", systemImage: "arrow.clockwise")
                }
            }
        }
        .formStyle(.grouped)
    }

    private var chatSettings: some View {
        Form {
            Section("Routing") {
                Picker("Default mode", selection: $model.routeMode) {
                    ForEach(model.selectableRouteModes) { mode in
                        Text(mode.title).tag(mode)
                    }
                }

                Picker("Chat delivery", selection: $model.selectedChatGPTDeliveryMode) {
                    ForEach(model.selectableChatGPTDeliveryModes) { deliveryMode in
                        Text(deliveryMode.title).tag(deliveryMode)
                    }
                }

                Text(model.selectedChatGPTDeliveryMode.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if model.selectedChatGPTDeliveryMode == .openAIAPI {
                Section("OpenAI API") {
                    Picker("Model", selection: $model.selectedChatGPTModel) {
                        ForEach(ChatGPTModel.allCases) { chatGPTModel in
                            Text("\(chatGPTModel.title) · \(chatGPTModel.costNote)").tag(chatGPTModel)
                        }
                    }

                    SecureField("API key", text: $model.openAIAPIKeyInput)
                        .textContentType(.password)

                    HStack {
                        Button(model.hasOpenAIAPIKey ? "Replace Key" : "Save Key") {
                            model.saveOpenAIAPIKey()
                        }
                        .disabled(model.openAIAPIKeyInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

                        Button("Remove Key", role: .destructive) {
                            model.clearOpenAIAPIKey()
                        }
                        .disabled(!model.hasOpenAIAPIKey)
                    }

                    Text(model.openAIAPIKeyStatus)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if model.selectedChatGPTDeliveryMode == .lumoAPI {
                Section("Lumo API") {
                    Picker("Model", selection: $model.selectedLumoModel) {
                        ForEach(LumoModel.allCases) { lumoModel in
                            Text("\(lumoModel.title) · \(lumoModel.capabilityNote)").tag(lumoModel)
                        }
                    }

                    SecureField("API key", text: $model.lumoAPIKeyInput)
                        .textContentType(.password)

                    HStack {
                        Button(model.hasLumoAPIKey ? "Replace Key" : "Save Key") {
                            model.saveLumoAPIKey()
                        }
                        .disabled(model.lumoAPIKeyInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

                        Button("Remove Key", role: .destructive) {
                            model.clearLumoAPIKey()
                        }
                        .disabled(!model.hasLumoAPIKey)
                    }

                    Text(model.lumoAPIKeyStatus)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .animation(.smooth(duration: 0.22), value: model.selectedChatGPTDeliveryMode)
    }

    private var mobileSettings: some View {
        Form {
            Section("Mobile Companion") {
                Toggle("Enable Mobile Companion", isOn: mobileRuntimeBinding)

                Text(
                    mobileRuntime.isEnabled
                        ? "Nearby discovery and encrypted access for paired devices are active."
                        : "Discovery, relay connections, mobile task services, and display-sleep availability are stopped and released."
                )
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if mobileRuntime.isEnabled {
                Section("Availability") {
                    Toggle(
                        "Keep Mac available while display is off",
                        isOn: $keepMacAvailableWhileDisplayOff
                    )
                    .onChange(of: keepMacAvailableWhileDisplayOff) { _, isEnabled in
                        CompanionPowerAvailabilityPreferences()
                            .setKeepMacAvailableWhileDisplayOff(isEnabled)
                    }

                    Text("Keeps nearby and remote Companion access available through screen saver, display sleep, and lock. Closing a Mac laptop still puts it to sleep.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section("Paired Devices") {
                    if let activePairing = mobilePairing {
                        LabeledContent("Pairing code") {
                            Text(formattedPairingCode(activePairing.code))
                                .font(.system(.title2, design: .monospaced, weight: .semibold))
                                .textSelection(.enabled)
                        }

                        Text("Expires \(activePairing.expiresAt.formatted(date: .omitted, time: .standard))")
                            .font(.caption)
                            .foregroundStyle(.secondary)

                        Button("Cancel Pairing", role: .cancel) {
                            CompanionPairingCoordinator.shared.cancelPairing()
                            mobilePairing = nil
                        }
                    } else {
                        Button {
                            mobilePairing = CompanionPairingCoordinator.shared.beginPairing()
                        } label: {
                            Label("Pair iPhone", systemImage: "iphone.and.arrow.forward")
                        }
                    }

                    if pairedMobileDevices.isEmpty {
                        Text("No iPhones are paired.")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(pairedMobileDevices) { device in
                            LabeledContent {
                                Button("Forget", role: .destructive) {
                                    try? CompanionPairingCoordinator.shared.forget(deviceID: device.deviceID)
                                    reloadPairedMobileDevices()
                                }
                            } label: {
                                Label(device.displayName, systemImage: "iphone")
                            }
                        }
                    }
                }

                Section("Remote Access") {
                    TextField("Secure relay URL", text: $relayURLInput)
                        .textFieldStyle(.roundedBorder)
                        .help("Use a wss:// endpoint. ws:// is accepted only for localhost testing.")

                    HStack {
                        Button("Save") {
                            saveRelayURL()
                        }

                        Button("Use Automatic") {
                            CompanionRelaySettings.useBundledRelay()
                            relayURLInput = CompanionRelaySettings.configuredURL()?.absoluteString ?? ""
                            relayStatusText = "Automatic secure relay restored. Reconnect the paired phone nearby once to synchronize it."
                        }
                        .disabled(CompanionRelaySettings.bundledURL() == nil)

                        Button("Disable", role: .destructive) {
                            relayURLInput = ""
                            saveRelayURL()
                        }
                        .disabled(CompanionRelaySettings.configuredURL() == nil)
                    }

                    Text(
                        relayStatusText.isEmpty
                            ? "A secure relay lets a paired phone reach this Mac from cellular or another network."
                            : relayStatusText
                    )
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
    }

    private var updateSettings: some View {
        Form {
            Section("Codex Companion") {
                LabeledContent("Installed version") {
                    Text("\(updateService.configuration.currentVersion) (\(updateService.configuration.currentBuild))")
                        .foregroundStyle(.secondary)
                }

                Text(updateStatusText)
                    .foregroundStyle(.secondary)

                Button {
                    Task {
                        await updateService.checkForUpdates()
                    }
                } label: {
                    Label(
                        updateService.state == .checking ? "Checking..." : "Check for Updates",
                        systemImage: "arrow.triangle.2.circlepath"
                    )
                }
                .disabled(updateService.state == .checking)

                if case .available = updateService.state {
                    Button {
                        Task {
                            await updateService.downloadAvailableUpdate()
                        }
                    } label: {
                        Label("Download Verified Update", systemImage: "arrow.down.circle")
                    }
                }

                if case .downloading = updateService.state {
                    ProgressView("Downloading and verifying update...")
                }

                if case .readyToInstall = updateService.state {
                    Button {
                        updateService.installReadyUpdate()
                    } label: {
                        Label("Install and Relaunch", systemImage: "arrow.down.app")
                    }
                }

                if case .installing = updateService.state {
                    ProgressView("Installing update and relaunching Companion...")
                }
            }
        }
        .formStyle(.grouped)
    }

    private var petSelection: Binding<String> {
        Binding(
            get: { model.petStore.selectedPetID ?? model.petStore.pets.first?.id ?? "" },
            set: { model.petStore.selectedPetID = $0 }
        )
    }

    private var mobileRuntimeBinding: Binding<Bool> {
        Binding(
            get: { mobileRuntime.isEnabled },
            set: { mobileRuntime.setEnabled($0) }
        )
    }

    private var updateStatusText: String {
        switch updateService.state {
        case .idle:
            "Updates are checked against the configured signed release channel."
        case .checking:
            "Checking the signed release channel..."
        case .unavailable(let reason):
            reason
        case .upToDate:
            "Codex Companion is up to date."
        case .available(let release):
            "Version \(release.version) is available."
        case .downloading(let release):
            "Downloading and verifying version \(release.version)..."
        case .readyToInstall(let release, _):
            "Version \(release.version) is verified and ready to install."
        case .installing(let release):
            "Installing version \(release.version). Companion will reopen automatically."
        case .failed(let message):
            "Update failed: \(message)"
        }
    }

    private func reloadPairedMobileDevices() {
        pairedMobileDevices = CompanionPairingCoordinator.shared.trustedRecords()
    }

    private func reloadMobileSettingsIfEnabled() {
        guard mobileRuntime.isAvailable, mobileRuntime.isEnabled else {
            mobilePairing = nil
            pairedMobileDevices = []
            return
        }
        mobilePairing = CompanionPairingCoordinator.shared.activePairing()
        reloadPairedMobileDevices()
        relayURLInput = CompanionRelaySettings.configuredURL()?.absoluteString ?? ""
    }

    private func formattedPairingCode(_ code: String) -> String {
        let digits = code.filter(\.isNumber)
        guard digits.count == 6 else { return digits }
        let split = digits.index(digits.startIndex, offsetBy: 3)
        return "\(digits[..<split]) \(digits[split...])"
    }

    private func saveRelayURL() {
        guard CompanionRelaySettings.setRelayURL(relayURLInput) else {
            relayStatusText = "Enter a valid wss:// URL. Unencrypted ws:// is limited to localhost testing."
            return
        }
        relayURLInput = CompanionRelaySettings.configuredURL()?.absoluteString ?? ""
        relayStatusText = relayURLInput.isEmpty
            ? "Remote access is disabled. Nearby access still works."
            : "Remote access saved. Reconnect the paired phone nearby once to synchronize it."
    }
}
