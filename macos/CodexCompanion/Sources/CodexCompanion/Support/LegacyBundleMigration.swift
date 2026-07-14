import Foundation

struct LegacyBundleMigration {
    static let legacyBundleIdentifier = "com.harlin.codex-companion"
    static let currentBundleIdentifier = "com.silverfire.codexcompanion"
    static let migrationMarker = "silverFireBundleMigrationVersion"
    static let migrationVersion = 1

    private let defaults: UserDefaults
    private let legacyBundleIdentifier: String

    init(
        defaults: UserDefaults = .standard,
        legacyBundleIdentifier: String = Self.legacyBundleIdentifier
    ) {
        self.defaults = defaults
        self.legacyBundleIdentifier = legacyBundleIdentifier
    }

    func run() {
        guard defaults.integer(forKey: Self.migrationMarker) < Self.migrationVersion else {
            return
        }

        if let legacyDomain = defaults.persistentDomain(forName: legacyBundleIdentifier) {
            for (key, value) in legacyDomain where defaults.object(forKey: key) == nil {
                defaults.set(value, forKey: key)
            }
        }

        defaults.set(Self.migrationVersion, forKey: Self.migrationMarker)
    }
}
