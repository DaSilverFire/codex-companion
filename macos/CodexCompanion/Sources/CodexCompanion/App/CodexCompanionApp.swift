import AppKit
import SwiftUI

@main
struct CodexCompanionApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model: CompanionAppModel

    init() {
        LegacyBundleMigration().run()
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
                        isChatGPTAppHandoffVisible: model.shouldShowChatGPTAppHandoff,
                        isPetVisible: model.isPetVisible,
                        hasAttentionMessage: model.attentionMessage != nil,
                        model: model
                    )
                )
        }
        .windowResizability(.contentSize)

        MenuBarExtra("Codex Companion", systemImage: "pawprint") {
            CompanionMenuBarView(model: model)
        }

        Window("Handoff", id: "handoff") {
            HandoffView(model: model)
                .frame(minWidth: 390, idealWidth: 430, minHeight: 360, idealHeight: 440)
                .background(WindowConfigurator(style: .utility))
        }
        .windowResizability(.contentSize)

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

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.global(qos: .utility).async {
            LegacySharedAppServerEnvironmentCleanup().run()
        }
        let bridge = CodexCompanionMobileBridgeServer()
        mobileBridgeServer = bridge
        bridge.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        mobileBridgeServer?.stop()
    }
}
