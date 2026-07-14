import Foundation
import Testing

@Suite
struct CompanionWindowLifecycleTests {
    @Test
    func petUsesOneWindowAndShowActionsReuseIt() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let appSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/App/CodexCompanionApp.swift"))
        let menuSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CompanionMenuBarView.swift"))
        let commandsSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/App/CompanionCommands.swift"))
        let modelSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Stores/CompanionAppModel.swift"))

        #expect(appSource.contains("Window(\"Codex Companion\", id: \"companion\")"))
        #expect(!appSource.contains("WindowGroup(\"Codex Companion\", id: \"companion\")"))
        #expect(!menuSource.contains("openWindow(id: \"companion\")"))
        #expect(!commandsSource.contains("openWindow(id: \"companion\")"))
        #expect(modelSource.contains("isPetHovered = false"))
        #expect(modelSource.contains("isPetPointerHovered = false"))
        #expect(modelSource.contains("PetWindowRoamer.shared.setInteractionHold(false)"))
    }
}
