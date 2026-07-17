import Foundation

struct PetVisibilityPreference {
    private static let key = "isPetVisible"

    let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var isVisible: Bool {
        get {
            guard defaults.object(forKey: Self.key) != nil else { return true }
            return defaults.bool(forKey: Self.key)
        }
        nonmutating set {
            defaults.set(newValue, forKey: Self.key)
        }
    }
}
