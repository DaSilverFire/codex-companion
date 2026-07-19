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
}
