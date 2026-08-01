import Foundation
import Testing

@Suite
struct PetContextMenuPresentationTests {
    @Test
    func petContextMenuKeepsOnlyHideAndSettingsActions() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let menuStart = try #require(source.range(of: ".contextMenu {"))
        var depth = 1
        var cursor = menuStart.upperBound
        var menuEnd: String.Index?
        while cursor < source.endIndex {
            if source[cursor] == "{" {
                depth += 1
            } else if source[cursor] == "}" {
                depth -= 1
                if depth == 0 {
                    menuEnd = source.index(after: cursor)
                    break
                }
            }
            cursor = source.index(after: cursor)
        }
        let resolvedMenuEnd = try #require(menuEnd)
        let menuSource = source[menuStart.lowerBound..<resolvedMenuEnd]

        #expect(menuSource.contains("Hide Pet"))
        #expect(menuSource.contains("Open Settings"))
        #expect(menuSource.contains("SettingsLink"))
        #expect(!menuSource.contains("Open Codex"))
        #expect(!menuSource.contains("ChatGPT Menu"))
        #expect(!menuSource.contains("Quick Bar"))
        #expect(!menuSource.contains("PetAnimationState.allCases"))
    }
}
