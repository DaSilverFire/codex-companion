import Foundation

struct CompanionInteractionPreferences {
    private static let hidesMenuButtonUntilHoverKey = "hidesMenuButtonUntilHover"
    private static let allowsAutonomousPetMovementKey = "allowsAutonomousPetMovement"
    private static let automaticallyContinuesGoalsAcrossAccountsKey =
        "automaticallyContinuesGoalsAcrossAccounts"
    private static let automaticallyContinuesQuotaInterruptedTasksAcrossAccountsKey =
        "automaticallyContinuesQuotaInterruptedTasksAcrossAccounts"

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

    var automaticallyContinuesGoalsAcrossAccounts: Bool {
        get {
            defaults.bool(forKey: Self.automaticallyContinuesGoalsAcrossAccountsKey)
        }
        nonmutating set {
            defaults.set(
                newValue,
                forKey: Self.automaticallyContinuesGoalsAcrossAccountsKey
            )
        }
    }

    var automaticallyContinuesQuotaInterruptedTasksAcrossAccounts: Bool {
        get {
            defaults.bool(
                forKey: Self.automaticallyContinuesQuotaInterruptedTasksAcrossAccountsKey
            )
        }
        nonmutating set {
            defaults.set(
                newValue,
                forKey: Self.automaticallyContinuesQuotaInterruptedTasksAcrossAccountsKey
            )
        }
    }
}
