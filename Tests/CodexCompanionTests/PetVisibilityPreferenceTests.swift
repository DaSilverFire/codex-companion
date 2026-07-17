import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct PetVisibilityPreferenceTests {
    @Test
    func defaultsToVisibleAndPersistsChanges() throws {
        let suiteName = "PetVisibilityPreferenceTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let preference = PetVisibilityPreference(defaults: defaults)
        #expect(preference.isVisible)

        preference.isVisible = false
        #expect(!PetVisibilityPreference(defaults: defaults).isVisible)

        preference.isVisible = true
        #expect(PetVisibilityPreference(defaults: defaults).isVisible)
    }
}
