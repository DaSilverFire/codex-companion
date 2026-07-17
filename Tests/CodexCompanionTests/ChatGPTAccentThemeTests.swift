import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTAccentThemeTests {
    @Test
    func activeWorkspaceThemeIsReadFromChatGPTAccountSettings() {
        let domain: [String: Any] = [
            "activeUserWorkspaceID": #"{"userID":"user-1","workspaceID":"workspace-1"}"#,
            "lastAccountSettingsResponse_workspace-1": #"{"settings":{"chatTheme":"orange"}}"#,
            "lastAccountSettingsResponse_workspace-2": #"{"settings":{"chatTheme":"green"}}"#,
        ]

        #expect(ChatGPTAccentThemeReader.theme(in: domain) == .orange)
    }

    @Test
    func missingOrUnknownThemeUsesCodexBlueFallback() {
        #expect(ChatGPTAccentThemeReader.theme(in: [:]) == .blue)
        #expect(ChatGPTAccentThemeReader.theme(in: [
            "lastAccountSettingsResponse_workspace-1": #"{"settings":{"chatTheme":"unknown"}}"#,
        ]) == .blue)
    }
}
