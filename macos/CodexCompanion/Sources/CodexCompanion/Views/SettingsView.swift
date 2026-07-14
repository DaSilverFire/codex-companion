import SwiftUI

struct SettingsView: View {
    @ObservedObject var model: CompanionAppModel
    @StateObject private var softwareUpdater = SoftwareUpdateService()

    var body: some View {
        Form {
            Section("Pet") {
                Picker("Pet", selection: petSelection) {
                    ForEach(model.petStore.pets) { pet in
                        Text("\(pet.displayName) · \(pet.source.title)")
                            .tag(pet.id)
                    }
                }

                Picker("Animation", selection: $model.selectedState) {
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

                Toggle("Hide menu button until pet hover", isOn: $model.hidesMenuButtonUntilHover)

                Button("Reload Pets") {
                    model.petStore.reload()
                }
            }

            Section("Routing") {
                Text("Codex mode shows active processes. Chat mode can answer privately with Apple's on-device model or use an OpenAI API key.")
                    .foregroundStyle(.secondary)

                Picker("Default route", selection: $model.routeMode) {
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

                if model.selectedChatGPTDeliveryMode == .openAIAPI {
                    Picker("API model", selection: $model.selectedChatGPTModel) {
                        ForEach(ChatGPTModel.allCases) { chatGPTModel in
                            Text("\(chatGPTModel.title) · \(chatGPTModel.costNote)").tag(chatGPTModel)
                        }
                    }

                    SecureField("OpenAI API key", text: $model.openAIAPIKeyInput)
                        .textContentType(.password)

                    Text("The API key is kept locally for this Mac so Companion does not ask for Keychain access every time it reopens.")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Button(model.hasOpenAIAPIKey ? "Replace API Key" : "Save API Key") {
                            model.saveOpenAIAPIKey()
                        }
                        .disabled(model.openAIAPIKeyInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)

                        Button("Remove API Key") {
                            model.clearOpenAIAPIKey()
                        }
                        .disabled(!model.hasOpenAIAPIKey)
                    }

                    Text(model.openAIAPIKeyStatus)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Rate Limits") {
                Text("Rate limits and banked reset details are read through the local Codex app-server. Applying a reset always requires an explicit confirmation.")
                    .foregroundStyle(.secondary)

                Button("Refresh Codex Rate Limits") {
                    model.rateLimitStore.refresh()
                }
            }

            Section("Updates") {
                LabeledContent("Installed version", value: softwareUpdater.currentVersion)

                Text(softwareUpdater.statusText)
                    .foregroundStyle(.secondary)

                HStack {
                    Button("Check for Updates") {
                        softwareUpdater.checkForUpdates()
                    }
                    .disabled(softwareUpdater.isBusy)

                    if softwareUpdater.installerURL == nil,
                       softwareUpdater.availableRelease != nil
                    {
                        Button("Download Installer") {
                            softwareUpdater.downloadInstaller()
                        }
                        .disabled(softwareUpdater.isBusy)
                    }

                    if softwareUpdater.installerURL != nil {
                        Button("Open Installer") {
                            softwareUpdater.openInstaller()
                        }
                    }

                    if softwareUpdater.availableRelease != nil {
                        Button("Release Notes") {
                            softwareUpdater.openReleaseNotes()
                        }
                    }
                }

                Text("Updates are prebuilt universal macOS installers. Companion verifies the published SHA-256 before opening the disk image; Xcode and Swift are not required.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 460)
    }

    private var petSelection: Binding<String> {
        Binding(
            get: { model.petStore.selectedPetID ?? model.petStore.pets.first?.id ?? "" },
            set: { model.petStore.selectedPetID = $0 }
        )
    }
}
