import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionPowerAvailabilityTests {
    @Test
    func availabilityDefaultsToEnabledAndPersistsChanges() throws {
        let suiteName = "CompanionPowerAvailabilityTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let preferences = CompanionPowerAvailabilityPreferences(
            defaults: defaults,
            notificationCenter: NotificationCenter()
        )

        #expect(preferences.keepMacAvailableWhileDisplayOff)

        preferences.setKeepMacAvailableWhileDisplayOff(false)
        #expect(!preferences.keepMacAvailableWhileDisplayOff)

        preferences.setKeepMacAvailableWhileDisplayOff(true)
        #expect(preferences.keepMacAvailableWhileDisplayOff)
    }

    @Test
    func controllerUsesOnlyIdleSystemSleepPreventionAndIsIdempotent() {
        let manager = RecordingProcessActivityManager()
        let controller = CompanionPowerAvailabilityController(activityManager: manager)

        controller.setEnabled(true)
        controller.setEnabled(true)

        #expect(manager.beginCalls.count == 1)
        #expect(
            manager.beginCalls[0].options.rawValue
                == ProcessInfo.ActivityOptions.idleSystemSleepDisabled.rawValue
        )
        #expect(!manager.beginCalls[0].reason.isEmpty)
        #expect(manager.endedTokens.isEmpty)

        controller.setEnabled(false)
        controller.setEnabled(false)

        #expect(manager.endedTokens.count == 1)
    }

    @Test
    func coordinatorTracksPreferenceChangesAndReleasesActivityWhenStopped() throws {
        let suiteName = "CompanionPowerAvailabilityCoordinatorTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let notificationCenter = NotificationCenter()
        let preferences = CompanionPowerAvailabilityPreferences(
            defaults: defaults,
            notificationCenter: notificationCenter
        )
        let manager = RecordingProcessActivityManager()
        let controller = CompanionPowerAvailabilityController(activityManager: manager)
        let coordinator = CompanionPowerAvailabilityCoordinator(
            preferences: preferences,
            controller: controller,
            notificationCenter: notificationCenter
        )

        coordinator.start()
        #expect(manager.beginCalls.count == 1)

        preferences.setKeepMacAvailableWhileDisplayOff(false)
        #expect(manager.endedTokens.count == 1)

        preferences.setKeepMacAvailableWhileDisplayOff(true)
        #expect(manager.beginCalls.count == 2)

        coordinator.stop()
        #expect(manager.endedTokens.count == 2)

        preferences.setKeepMacAvailableWhileDisplayOff(false)
        preferences.setKeepMacAvailableWhileDisplayOff(true)
        #expect(manager.beginCalls.count == 2)
        #expect(manager.endedTokens.count == 2)
    }
}

private final class RecordingProcessActivityManager: ProcessActivityManaging {
    struct BeginCall {
        let options: ProcessInfo.ActivityOptions
        let reason: String
    }

    var beginCalls: [BeginCall] = []
    var endedTokens: [ObjectIdentifier] = []

    func beginActivity(
        options: ProcessInfo.ActivityOptions,
        reason: String
    ) -> NSObjectProtocol {
        beginCalls.append(BeginCall(options: options, reason: reason))
        return NSObject()
    }

    func endActivity(_ activity: NSObjectProtocol) {
        endedTokens.append(ObjectIdentifier(activity as AnyObject))
    }
}
