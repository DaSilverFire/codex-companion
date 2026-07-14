import Foundation

struct CompanionInteractionPreferences {
    private static let hidesMenuButtonUntilHoverKey = "hidesMenuButtonUntilHover"

    let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var hidesMenuButtonUntilHover: Bool {
        get {
            defaults.bool(forKey: Self.hidesMenuButtonUntilHoverKey)
        }
        nonmutating set {
            defaults.set(newValue, forKey: Self.hidesMenuButtonUntilHoverKey)
        }
    }
}
