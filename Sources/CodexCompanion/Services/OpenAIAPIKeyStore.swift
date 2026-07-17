import Foundation
import LocalAuthentication
import Security

enum OpenAIKeychainMigrationPolicy {
    static func shouldAccessKeychain(hasLocalCredential: Bool) -> Bool {
        !hasLocalCredential
    }
}

struct OpenAIAPIKeyStore {
    private let service = CompanionIdentity.openAIKeychainService
    private let account = CompanionIdentity.openAIKeychainAccount

    @discardableResult
    func migrateLegacyKeychainItemIfNeeded() -> OpenAIKeychainMigrationOutcome {
        guard OpenAIKeychainMigrationPolicy.shouldAccessKeychain(
            hasLocalCredential: loadLocalKey() != nil
        ) else {
            return .currentItemAlreadyPresent
        }

        return OpenAIKeychainServiceMigrator().migrate(
            account: account,
            currentService: service,
            legacyServices: CompanionIdentity.legacyOpenAIKeychainServices
        )
    }

    func load() -> String? {
        if let localKey = loadLocalKey() {
            return localKey
        }

        return loadKeychainKey(allowPrompt: true)
    }

    private func loadKeychainKey(allowPrompt: Bool) -> String? {
        var query = baseQuery()
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        if !allowPrompt {
            preventAuthenticationUI(for: &query)
        }

        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return nil }
        try? saveLocalKey(data)
        return String(data: data, encoding: .utf8)
    }

    func save(_ key: String) throws {
        let trimmed = key.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, let data = trimmed.data(using: .utf8) else {
            clear()
            return
        }

        try saveLocalKey(data)
        saveKeychainKey(data)
    }

    private func saveKeychainKey(_ data: Data) {
        let attributes = [kSecValueData as String: data]
        let updateStatus = SecItemUpdate(baseQuery() as CFDictionary, attributes as CFDictionary)
        if updateStatus == errSecSuccess {
            return
        }

        var item = baseQuery()
        item[kSecValueData as String] = data
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let addStatus = SecItemAdd(item as CFDictionary, nil)
        if addStatus != errSecSuccess {
            NSLog("Codex Companion could not mirror OpenAI API key to Keychain: \(KeychainError(status: addStatus).localizedDescription)")
        }
    }

    func clear() {
        SecItemDelete(baseQuery() as CFDictionary)
        if let url = localKeyURL {
            try? FileManager.default.removeItem(at: url)
        }
    }

    var hasKey: Bool {
        loadLocalKey() != nil || hasKeychainKey
    }

    private func baseQuery() -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }

    private var hasKeychainKey: Bool {
        var query = baseQuery()
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        query[kSecReturnAttributes as String] = true
        preventAuthenticationUI(for: &query)

        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        return status == errSecSuccess || status == errSecInteractionNotAllowed
    }

    private var localKeyURL: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first?
            .appendingPathComponent("Codex Companion", isDirectory: true)
            .appendingPathComponent("openai-api-key", isDirectory: false)
    }

    private func loadLocalKey() -> String? {
        guard
            let url = localKeyURL,
            let data = try? Data(contentsOf: url),
            let key = String(data: data, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines),
            !key.isEmpty
        else {
            return nil
        }
        return key
    }

    private func saveLocalKey(_ data: Data) throws {
        guard let url = localKeyURL else {
            throw KeychainError(status: errSecNotAvailable)
        }

        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try data.write(to: url, options: [.atomic])
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o600],
            ofItemAtPath: url.path
        )
    }

    private func preventAuthenticationUI(for query: inout [String: Any]) {
        let context = LAContext()
        context.interactionNotAllowed = true
        query[kSecUseAuthenticationContext as String] = context
    }
}

struct KeychainError: LocalizedError {
    var status: OSStatus

    var errorDescription: String? {
        SecCopyErrorMessageString(status, nil) as String? ?? "Keychain error \(status)"
    }
}
