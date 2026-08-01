import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionSettingsPresentationTests {
    @Test
    func settingsUseFocusedNativeTabs() {
        #expect(CompanionSettingsTab.allCases.map(\.title) == [
            "General",
            "Chat",
            "Mobile",
            "Updates",
        ])
        #expect(Set(CompanionSettingsTab.allCases.map(\.systemImage)).count == 4)
    }

    @Test
    func menuBarKeepsEssentialActionsWithoutDuplicateMaintenanceCommands() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CompanionMenuBarView.swift"))

        #expect(source.contains("Codex Processes"))
        #expect(source.contains("Local Chat"))
        #expect(source.contains("SettingsLink"))
        #expect(source.contains("Quit Codex Companion"))
        #expect(!source.contains("Handoff..."))
        #expect(!source.contains("Refresh Rate Limits"))
        #expect(!source.contains("Refresh Goals"))
        #expect(!source.contains("Menu(\"Animation\")"))
        #expect(!source.contains("menuSummary"))
        #expect(!source.contains("Reload Pets"))
    }

    @Test
    func mobileSettingsCanRestoreTheBundledAutomaticRelay() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/SettingsView.swift"))

        #expect(source.contains("Use Automatic"))
        #expect(source.contains("CompanionRelaySettings.useBundledRelay()"))
    }

    @Test
    func accountSettingsExposeOfficialProfileLoginAndProfileScopedUsage() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexAccountProfileSettingsSection.swift"))

        #expect(source.contains("Sign In to Codex"))
        #expect(source.contains("Usage for"))
        #expect(source.contains("Apply Reset"))
        #expect(source.contains("Active turns stay with the account that started them"))
        #expect(source.contains("Remove or hand off its bound tasks first"))
        #expect(source.contains("Continue quota-limited goals with another account"))
        #expect(source.contains("Continue quota-interrupted tasks without goals"))
        #expect(source.contains("explicit usage-limit turn failure"))
        #expect(source.contains("Network and relay errors are ignored"))
        #expect(source.contains("Banked resets stay untouched"))
        #expect(!source.contains("Open ChatGPT to Switch Account"))
        #expect(!source.contains("Account Changed - Reconnect"))
    }
}
