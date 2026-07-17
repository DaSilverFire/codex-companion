import Foundation

struct CompanionInteractionPreferences {
    private static let hidesMenuButtonUntilHoverKey = "hidesMenuButtonUntilHover"
    private static let allowsAutonomousPetMovementKey = "allowsAutonomousPetMovement"

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

    var allowsAutonomousPetMovement: Bool {
        get {
            guard defaults.object(forKey: Self.allowsAutonomousPetMovementKey) != nil else {
                return true
            }
            return defaults.bool(forKey: Self.allowsAutonomousPetMovementKey)
        }
        nonmutating set {
            defaults.set(newValue, forKey: Self.allowsAutonomousPetMovementKey)
        }
    }
}
