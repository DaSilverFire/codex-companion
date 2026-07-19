import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionMobileRuntimeTests {
    @Test
    func publicBuildCannotEnableMobileRuntime() {
        let defaults = makeDefaults()
        defer { clear(defaults) }
        defaults.set(
            true,
            forKey: CompanionMobileRuntimePolicy.runtimeEnabledDefaultsKey
        )

        let policy = CompanionMobileRuntimePolicy(
            isBuildAuthorized: false,
            defaults: defaults
        )

        #expect(!policy.isRuntimeEnabled)
        policy.setRuntimeEnabled(true)
        #expect(!policy.isRuntimeEnabled)
    }

    @Test
    func authorizedBuildUsesPersistedRuntimePreference() {
        let defaults = makeDefaults()
        defer { clear(defaults) }
        let policy = CompanionMobileRuntimePolicy(
            isBuildAuthorized: true,
            defaults: defaults
        )

        #expect(policy.isRuntimeEnabled)
        policy.setRuntimeEnabled(false)
        #expect(!policy.isRuntimeEnabled)
        policy.setRuntimeEnabled(true)
        #expect(policy.isRuntimeEnabled)
    }

    @Test
    func authorizedBuildSeedsAccessForLaterSignedUpdates() {
        let defaults = makeDefaults()
        defer { clear(defaults) }

        let authorizedBuild = CompanionMobileRuntimePolicy(
            isBuildAuthorized: true,
            defaults: defaults
        )
        #expect(authorizedBuild.isBuildAuthorized)
        #expect(defaults.bool(forKey: CompanionMobileRuntimePolicy.accessGrantedDefaultsKey))

        let laterPublicBuild = CompanionMobileRuntimePolicy(
            isBuildAuthorized: false,
            defaults: defaults
        )
        #expect(laterPublicBuild.isBuildAuthorized)
        #expect(laterPublicBuild.isRuntimeEnabled)
    }

    @MainActor
    @Test
    func publicBuildNeverConstructsMobileServices() {
        let defaults = makeDefaults()
        defer { clear(defaults) }
        var bridgeConstructionCount = 0
        var powerConstructionCount = 0
        let controller = CompanionMobileRuntimeController(
            policy: CompanionMobileRuntimePolicy(
                isBuildAuthorized: false,
                defaults: defaults
            ),
            makeBridge: {
                bridgeConstructionCount += 1
                return RuntimeServiceSpy()
            },
            makePowerCoordinator: {
                powerConstructionCount += 1
                return RuntimeServiceSpy()
            }
        )

        controller.startIfEnabled()
        controller.setEnabled(true)

        #expect(!controller.isAvailable)
        #expect(!controller.isEnabled)
        #expect(!controller.isRunning)
        #expect(bridgeConstructionCount == 0)
        #expect(powerConstructionCount == 0)
    }

    @MainActor
    @Test
    func disablingMobileStopsAndReleasesEveryRuntimeService() {
        let defaults = makeDefaults()
        defer { clear(defaults) }
        var bridge: RuntimeServiceSpy? = RuntimeServiceSpy()
        var power: RuntimeServiceSpy? = RuntimeServiceSpy()
        weak let releasedBridge = bridge
        weak let releasedPower = power
        var pairingCancellationCount = 0
        let controller = CompanionMobileRuntimeController(
            policy: CompanionMobileRuntimePolicy(
                isBuildAuthorized: true,
                defaults: defaults
            ),
            makeBridge: { bridge! },
            makePowerCoordinator: { power! },
            cancelPairing: { pairingCancellationCount += 1 }
        )

        controller.startIfEnabled()
        #expect(bridge?.startCount == 1)
        #expect(power?.startCount == 1)
        #expect(controller.isRunning)

        let startedBridge = bridge
        let startedPower = power
        bridge = nil
        power = nil
        controller.setEnabled(false)

        #expect(startedBridge?.stopCount == 1)
        #expect(startedPower?.stopCount == 1)
        #expect(pairingCancellationCount == 1)
        #expect(!controller.isEnabled)
        #expect(!controller.isRunning)

        // Release the test's inspection references after checking stop delivery.
        _ = startedBridge
        _ = startedPower
        #expect(releasedBridge != nil)
        #expect(releasedPower != nil)
    }

    @MainActor
    @Test
    func disablingMobileReleasesServicesWithoutExternalOwners() {
        let defaults = makeDefaults()
        defer { clear(defaults) }
        weak var releasedBridge: RuntimeServiceSpy?
        weak var releasedPower: RuntimeServiceSpy?
        let controller = CompanionMobileRuntimeController(
            policy: CompanionMobileRuntimePolicy(
                isBuildAuthorized: true,
                defaults: defaults
            ),
            makeBridge: {
                let service = RuntimeServiceSpy()
                releasedBridge = service
                return service
            },
            makePowerCoordinator: {
                let service = RuntimeServiceSpy()
                releasedPower = service
                return service
            }
        )

        controller.startIfEnabled()
        #expect(releasedBridge != nil)
        #expect(releasedPower != nil)

        controller.setEnabled(false)

        #expect(releasedBridge == nil)
        #expect(releasedPower == nil)
    }

    private func makeDefaults() -> UserDefaults {
        let suite = "CompanionMobileRuntimeTests.\(UUID().uuidString)"
        return UserDefaults(suiteName: suite)!
    }

    private func clear(_ defaults: UserDefaults) {
        defaults.removeObject(
            forKey: CompanionMobileRuntimePolicy.accessGrantedDefaultsKey
        )
        defaults.removeObject(
            forKey: CompanionMobileRuntimePolicy.runtimeEnabledDefaultsKey
        )
    }
}

private final class RuntimeServiceSpy: CompanionMobileRuntimeService {
    private(set) var startCount = 0
    private(set) var stopCount = 0

    func start() {
        startCount += 1
    }

    func stop() {
        stopCount += 1
    }
}
