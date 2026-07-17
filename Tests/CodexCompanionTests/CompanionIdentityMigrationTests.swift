import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionIdentityMigrationTests {
    @Test
    func shippedIdentityUsesOnlyTheSilverFireNamespace() {
        #expect(CompanionIdentity.bundleIdentifier == "com.silverfire.codexcompanion")
        #expect(CompanionIdentity.openAIKeychainService == "com.silverfire.codexcompanion.openai-api-key")
        #expect(CompanionIdentity.legacyBundleIdentifiers.isEmpty)
        #expect(CompanionIdentity.legacyOpenAIKeychainServices.isEmpty)
    }

    @Test
    func localCredentialPreventsAStartupKeychainMigrationRead() {
        #expect(!OpenAIKeychainMigrationPolicy.shouldAccessKeychain(hasLocalCredential: true))
        #expect(OpenAIKeychainMigrationPolicy.shouldAccessKeychain(hasLocalCredential: false))
    }

    @Test
    func defaultsMigrationPreservesCurrentValuesAndCopiesWindowState() throws {
        let suffix = UUID().uuidString
        let currentDomain = "CompanionIdentityTests.current.\(suffix)"
        let legacyDomain = "CompanionIdentityTests.legacy.\(suffix)"
        let defaults = try #require(UserDefaults(suiteName: currentDomain))
        defer {
            defaults.removePersistentDomain(forName: currentDomain)
            defaults.removePersistentDomain(forName: legacyDomain)
        }

        defaults.setPersistentDomain(
            [
                "selectedPetID": "custom:shadow-16",
                "animationSpeedScale": 1.15,
                "NSWindow Frame companion-AppWindow-1": "725 366 124 124 0 0 1800 1130 ",
            ],
            forName: legacyDomain
        )
        defaults.setPersistentDomain(
            ["animationSpeedScale": 1.4],
            forName: currentDomain
        )

        let migration = CompanionIdentityMigration(
            defaults: defaults,
            currentBundleIdentifier: currentDomain,
            legacyBundleIdentifiers: [legacyDomain]
        )

        #expect(migration.run())
        let migrated = try #require(defaults.persistentDomain(forName: currentDomain))
        #expect(migrated["selectedPetID"] as? String == "custom:shadow-16")
        #expect(migrated["animationSpeedScale"] as? Double == 1.4)
        #expect(
            migrated["NSWindow Frame companion-AppWindow-1"] as? String
                == "725 366 124 124 0 0 1800 1130 "
        )
        #expect(!migration.run())
    }

    @Test
    func keychainMigrationCopiesVerifiesThenDeletesLegacyItem() {
        let store = InMemoryGenericPasswordStore()
        store.entries["legacy|default"] = Data("secret-value".utf8)
        let migrator = OpenAIKeychainServiceMigrator(store: store)

        let outcome = migrator.migrate(
            account: "default",
            currentService: "current",
            legacyServices: ["legacy"]
        )

        #expect(outcome == .migrated(fromService: "legacy"))
        #expect(store.entries["current|default"] == Data("secret-value".utf8))
        #expect(store.entries["legacy|default"] == nil)
    }

    @Test
    func keychainMigrationNeverOverwritesCurrentItem() {
        let store = InMemoryGenericPasswordStore()
        store.entries["current|default"] = Data("current-value".utf8)
        store.entries["legacy|default"] = Data("legacy-value".utf8)
        let migrator = OpenAIKeychainServiceMigrator(store: store)

        let outcome = migrator.migrate(
            account: "default",
            currentService: "current",
            legacyServices: ["legacy"]
        )

        #expect(outcome == .currentItemAlreadyPresent)
        #expect(store.entries["current|default"] == Data("current-value".utf8))
        #expect(store.entries["legacy|default"] == Data("legacy-value".utf8))
    }

    @Test
    func keychainMigrationKeepsLegacyItemWhenWriteCannotBeVerified() {
        let store = InMemoryGenericPasswordStore()
        store.entries["legacy|default"] = Data("legacy-value".utf8)
        store.suppressWrites = true
        let migrator = OpenAIKeychainServiceMigrator(store: store)

        let outcome = migrator.migrate(
            account: "default",
            currentService: "current",
            legacyServices: ["legacy"]
        )

        #expect(outcome == .writeFailed(fromService: "legacy"))
        #expect(store.entries["legacy|default"] == Data("legacy-value".utf8))
        #expect(store.deletedServices.isEmpty)
    }
}

private final class InMemoryGenericPasswordStore: GenericPasswordStoring {
    var entries: [String: Data] = [:]
    var deletedServices: [String] = []
    var suppressWrites = false

    func read(service: String, account: String) -> Data? {
        entries["\(service)|\(account)"]
    }

    func write(_ data: Data, service: String, account: String) -> Bool {
        guard !suppressWrites else { return false }
        entries["\(service)|\(account)"] = data
        return true
    }

    func delete(service: String, account: String) -> Bool {
        entries.removeValue(forKey: "\(service)|\(account)")
        deletedServices.append(service)
        return true
    }
}
