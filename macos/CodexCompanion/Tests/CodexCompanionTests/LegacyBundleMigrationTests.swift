import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct LegacyBundleMigrationTests {
    @Test
    func copiesLegacyPreferencesWithoutOverwritingCurrentValues() throws {
        let currentSuite = "com.silverfire.codexcompanion.tests.current.\(UUID().uuidString)"
        let legacySuite = "com.silverfire.codexcompanion.tests.legacy.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: currentSuite))
        defer {
            defaults.removePersistentDomain(forName: currentSuite)
            defaults.removePersistentDomain(forName: legacySuite)
        }

        defaults.setPersistentDomain(
            [
                "selectedPetID": "custom:shadow-16",
                "routeMode": "codex",
                "isPetVisible": true,
            ],
            forName: legacySuite
        )
        defaults.set("chatGPT", forKey: "routeMode")

        LegacyBundleMigration(
            defaults: defaults,
            legacyBundleIdentifier: legacySuite
        ).run()

        #expect(defaults.string(forKey: "selectedPetID") == "custom:shadow-16")
        #expect(defaults.string(forKey: "routeMode") == "chatGPT")
        #expect(defaults.bool(forKey: "isPetVisible"))
        #expect(defaults.integer(forKey: LegacyBundleMigration.migrationMarker) == 1)
    }

    @Test
    func onlyRunsOnce() throws {
        let currentSuite = "com.silverfire.codexcompanion.tests.current.\(UUID().uuidString)"
        let legacySuite = "com.silverfire.codexcompanion.tests.legacy.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: currentSuite))
        defer {
            defaults.removePersistentDomain(forName: currentSuite)
            defaults.removePersistentDomain(forName: legacySuite)
        }

        defaults.setPersistentDomain(["selectedPetID": "first"], forName: legacySuite)
        let migration = LegacyBundleMigration(defaults: defaults, legacyBundleIdentifier: legacySuite)
        migration.run()

        defaults.setPersistentDomain(["selectedPetID": "second"], forName: legacySuite)
        defaults.removeObject(forKey: "selectedPetID")
        migration.run()

        #expect(defaults.string(forKey: "selectedPetID") == nil)
    }
}
