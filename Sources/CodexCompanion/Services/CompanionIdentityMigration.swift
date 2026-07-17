import Foundation
import LocalAuthentication
import Security

enum CompanionIdentity {
    static let bundleIdentifier = "com.silverfire.codexcompanion"
    static let legacyBundleIdentifiers: [String] = []
    static let openAIKeychainService = "com.silverfire.codexcompanion.openai-api-key"
    static let legacyOpenAIKeychainServices: [String] = []
    static let openAIKeychainAccount = "default"
}

struct CompanionIdentityMigration {
    private let defaults: UserDefaults
    private let currentBundleIdentifier: String
    private let legacyBundleIdentifiers: [String]

    init(
        defaults: UserDefaults = .standard,
        currentBundleIdentifier: String = CompanionIdentity.bundleIdentifier,
        legacyBundleIdentifiers: [String] = CompanionIdentity.legacyBundleIdentifiers
    ) {
        self.defaults = defaults
        self.currentBundleIdentifier = currentBundleIdentifier
        self.legacyBundleIdentifiers = legacyBundleIdentifiers
    }

    @discardableResult
    func run() -> Bool {
        var destination = defaults.persistentDomain(forName: currentBundleIdentifier) ?? [:]
        var changed = false

        for legacyBundleIdentifier in legacyBundleIdentifiers {
            guard let legacy = defaults.persistentDomain(forName: legacyBundleIdentifier) else {
                continue
            }
            for (key, value) in legacy where destination[key] == nil {
                destination[key] = value
                changed = true
            }
        }

        guard changed else { return false }
        defaults.setPersistentDomain(destination, forName: currentBundleIdentifier)
        return true
    }
}

protocol GenericPasswordStoring {
    func read(service: String, account: String) -> Data?
    func write(_ data: Data, service: String, account: String) -> Bool
    func delete(service: String, account: String) -> Bool
}

enum OpenAIKeychainMigrationOutcome: Equatable {
    case currentItemAlreadyPresent
    case noLegacyItem
    case migrated(fromService: String)
    case writeFailed(fromService: String)
    case verificationFailed(fromService: String)
    case legacyDeleteFailed(fromService: String)
}

struct OpenAIKeychainServiceMigrator {
    private let store: any GenericPasswordStoring

    init(store: any GenericPasswordStoring = SecurityGenericPasswordStore()) {
        self.store = store
    }

    func migrate(
        account: String,
        currentService: String,
        legacyServices: [String]
    ) -> OpenAIKeychainMigrationOutcome {
        if store.read(service: currentService, account: account) != nil {
            return .currentItemAlreadyPresent
        }

        for legacyService in legacyServices {
            guard let legacyData = store.read(service: legacyService, account: account) else {
                continue
            }
            guard store.write(legacyData, service: currentService, account: account) else {
                return .writeFailed(fromService: legacyService)
            }
            guard store.read(service: currentService, account: account) == legacyData else {
                return .verificationFailed(fromService: legacyService)
            }
            guard store.delete(service: legacyService, account: account) else {
                return .legacyDeleteFailed(fromService: legacyService)
            }
            return .migrated(fromService: legacyService)
        }

        return .noLegacyItem
    }
}

private struct SecurityGenericPasswordStore: GenericPasswordStoring {
    func read(service: String, account: String) -> Data? {
        var query = baseQuery(service: service, account: account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        let context = LAContext()
        context.interactionNotAllowed = true
        query[kSecUseAuthenticationContext as String] = context

        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess else { return nil }
        return result as? Data
    }

    func write(_ data: Data, service: String, account: String) -> Bool {
        let query = baseQuery(service: service, account: account)
        let attributes = [kSecValueData as String: data]
        let updateStatus = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if updateStatus == errSecSuccess {
            return true
        }
        guard updateStatus == errSecItemNotFound else { return false }

        var item = query
        item[kSecValueData as String] = data
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(item as CFDictionary, nil) == errSecSuccess
    }

    func delete(service: String, account: String) -> Bool {
        let status = SecItemDelete(baseQuery(service: service, account: account) as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }

    private func baseQuery(service: String, account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }
}
