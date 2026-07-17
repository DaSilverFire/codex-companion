import Foundation

protocol ProcessActivityManaging: AnyObject {
    func beginActivity(
        options: ProcessInfo.ActivityOptions,
        reason: String
    ) -> NSObjectProtocol

    func endActivity(_ activity: NSObjectProtocol)
}

extension ProcessInfo: ProcessActivityManaging {}

struct CompanionPowerAvailabilityPreferences {
    static let didChange = Notification.Name(
        "CodexCompanion.powerAvailabilityPreferenceDidChange"
    )

    private static let keepMacAvailableWhileDisplayOffKey =
        "keepMacAvailableWhileDisplayOff"

    let defaults: UserDefaults
    let notificationCenter: NotificationCenter

    init(
        defaults: UserDefaults = .standard,
        notificationCenter: NotificationCenter = .default
    ) {
        self.defaults = defaults
        self.notificationCenter = notificationCenter
    }

    var keepMacAvailableWhileDisplayOff: Bool {
        guard defaults.object(forKey: Self.keepMacAvailableWhileDisplayOffKey) != nil else {
            return true
        }
        return defaults.bool(forKey: Self.keepMacAvailableWhileDisplayOffKey)
    }

    func setKeepMacAvailableWhileDisplayOff(_ isEnabled: Bool) {
        defaults.set(isEnabled, forKey: Self.keepMacAvailableWhileDisplayOffKey)
        notificationCenter.post(name: Self.didChange, object: nil)
    }
}

final class CompanionPowerAvailabilityController {
    private let activityManager: any ProcessActivityManaging
    private var activity: NSObjectProtocol?

    init(activityManager: any ProcessActivityManaging = ProcessInfo.processInfo) {
        self.activityManager = activityManager
    }

    func setEnabled(_ isEnabled: Bool) {
        if isEnabled {
            guard activity == nil else { return }
            activity = activityManager.beginActivity(
                options: .idleSystemSleepDisabled,
                reason: "Keep Codex Companion available to paired devices"
            )
        } else {
            stop()
        }
    }

    func stop() {
        guard let activity else { return }
        activityManager.endActivity(activity)
        self.activity = nil
    }

    deinit {
        stop()
    }
}

final class CompanionPowerAvailabilityCoordinator {
    private let preferences: CompanionPowerAvailabilityPreferences
    private let controller: CompanionPowerAvailabilityController
    private let notificationCenter: NotificationCenter
    private var preferenceObserver: NSObjectProtocol?

    init(
        preferences: CompanionPowerAvailabilityPreferences = .init(),
        controller: CompanionPowerAvailabilityController = .init(),
        notificationCenter: NotificationCenter = .default
    ) {
        self.preferences = preferences
        self.controller = controller
        self.notificationCenter = notificationCenter
    }

    func start() {
        guard preferenceObserver == nil else { return }

        controller.setEnabled(preferences.keepMacAvailableWhileDisplayOff)
        preferenceObserver = notificationCenter.addObserver(
            forName: CompanionPowerAvailabilityPreferences.didChange,
            object: nil,
            queue: nil
        ) { [weak self] _ in
            guard let self else { return }
            self.controller.setEnabled(
                self.preferences.keepMacAvailableWhileDisplayOff
            )
        }
    }

    func stop() {
        if let preferenceObserver {
            notificationCenter.removeObserver(preferenceObserver)
            self.preferenceObserver = nil
        }
        controller.stop()
    }

    deinit {
        stop()
    }
}
