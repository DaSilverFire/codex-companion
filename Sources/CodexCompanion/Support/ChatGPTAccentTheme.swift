import Foundation

enum ChatGPTAccentTheme: String, CaseIterable {
    case blue
    case green
    case orange
    case pink
    case purple
    case red
    case teal
    case yellow
}

struct ChatGPTAccentThemeReader {
    static let chatGPTDefaultsDomain = "com.openai.chat"

    func currentTheme() -> ChatGPTAccentTheme {
        let domain = UserDefaults.standard.persistentDomain(
            forName: Self.chatGPTDefaultsDomain
        ) ?? [:]
        return Self.theme(in: domain)
    }

    static func theme(in domain: [String: Any]) -> ChatGPTAccentTheme {
        if let workspaceID = activeWorkspaceID(in: domain),
           let response = domain["lastAccountSettingsResponse_\(workspaceID)"] as? String,
           let theme = theme(inAccountSettingsResponse: response) {
            return theme
        }

        for key in domain.keys.sorted()
            where key.hasPrefix("lastAccountSettingsResponse_") {
            guard let response = domain[key] as? String,
                  let theme = theme(inAccountSettingsResponse: response)
            else { continue }
            return theme
        }

        return .blue
    }

    private static func activeWorkspaceID(in domain: [String: Any]) -> String? {
        guard let value = domain["activeUserWorkspaceID"] as? String,
              let data = value.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return object["workspaceID"] as? String
    }

    private static func theme(inAccountSettingsResponse response: String) -> ChatGPTAccentTheme? {
        guard let data = response.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let settings = object["settings"] as? [String: Any],
              let rawTheme = settings["chatTheme"] as? String
        else { return nil }
        return ChatGPTAccentTheme(rawValue: rawTheme.lowercased())
    }
}
