import AppKit
import SwiftUI

@main
struct CodexCompanionApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model: CompanionAppModel

    init() {
        CompanionIdentityMigration().run()
        OpenAIAPIKeyStore().migrateLegacyKeychainItemIfNeeded()
        _model = StateObject(wrappedValue: CompanionAppModel())
    }

    var body: some Scene {
        Window("Codex Companion", id: "companion") {
            ContentView(model: model)
                .clearWindowContainerBackground()
                .background(
                    WindowConfigurator(
                        style: .pet,
                        isQuickBarOpen: model.isQuickBarOpen,
                        isCodexProcessTrayVisible: model.isCodexProcessTrayVisible,
                        isChatGPTModelPickerExpanded: model.isChatGPTModelPickerExpanded,
                        isChatGPTMenuResponseVisible: model.shouldShowChatGPTMenuResponse,
                        isPetVisible: model.isPetVisible,
                        hasAttentionMessage: model.attentionMessage != nil,
                        model: model
                    )
                )
        }

        MenuBarExtra("Codex Companion", systemImage: "pawprint") {
            CompanionMenuBarView(model: model)
        }

        Window("Codex Rate Limits", id: "rate-limits") {
            RateLimitsView(store: model.rateLimitStore)
                .frame(minWidth: 420, idealWidth: 480, minHeight: 320, idealHeight: 460)
                .background(WindowConfigurator(style: .utility))
        }
        .windowResizability(.contentSize)

        Settings {
            SettingsView(model: model)
        }

        .commands {
            CompanionCommands(model: model)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var mobileBridgeServer: CodexCompanionMobileBridgeServer?
    private let powerAvailabilityCoordinator = CompanionPowerAvailabilityCoordinator()

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        NSApp.activate(ignoringOtherApps: true)
        powerAvailabilityCoordinator.start()
        let bridge = CodexCompanionMobileBridgeServer()
        mobileBridgeServer = bridge
        bridge.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        powerAvailabilityCoordinator.stop()
        mobileBridgeServer?.stop()
    }
}
