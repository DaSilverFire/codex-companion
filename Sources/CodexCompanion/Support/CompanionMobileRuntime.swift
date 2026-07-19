import Combine
import Foundation

struct CompanionMobileRuntimePolicy {
    static let buildAuthorizationInfoKey = "CodexCompanionMobileBetaAuthorized"
    static let accessGrantedDefaultsKey = "CodexCompanion.mobileBetaAccessGranted.v1"
    static let runtimeEnabledDefaultsKey = "CodexCompanion.mobileRuntimeEnabled.v1"

    let isBuildAuthorized: Bool
    private let defaults: UserDefaults
    private let defaultEnabled: Bool

    init(
        bundle: Bundle = .main,
        defaults: UserDefaults = .standard,
        defaultEnabled: Bool = true
    ) {
        let bundleAuthorization = bundle.object(
            forInfoDictionaryKey: Self.buildAuthorizationInfoKey
        ) as? Bool ?? false
        if bundleAuthorization {
            defaults.set(true, forKey: Self.accessGrantedDefaultsKey)
        }
        isBuildAuthorized = bundleAuthorization || defaults.bool(
            forKey: Self.accessGrantedDefaultsKey
        )
        self.defaults = defaults
        self.defaultEnabled = defaultEnabled
    }

    init(
        isBuildAuthorized: Bool,
        defaults: UserDefaults,
        defaultEnabled: Bool = true
    ) {
        if isBuildAuthorized {
            defaults.set(true, forKey: Self.accessGrantedDefaultsKey)
        }
        self.isBuildAuthorized = isBuildAuthorized || defaults.bool(
            forKey: Self.accessGrantedDefaultsKey
        )
        self.defaults = defaults
        self.defaultEnabled = defaultEnabled
    }

    var isRuntimeEnabled: Bool {
        guard isBuildAuthorized else { return false }
        guard defaults.object(forKey: Self.runtimeEnabledDefaultsKey) != nil else {
            return defaultEnabled
        }
        return defaults.bool(forKey: Self.runtimeEnabledDefaultsKey)
    }

    func setRuntimeEnabled(_ isEnabled: Bool) {
        defaults.set(
            isBuildAuthorized && isEnabled,
            forKey: Self.runtimeEnabledDefaultsKey
        )
    }
}

protocol CompanionMobileRuntimeService: AnyObject {
    func start()
    func stop()
}

extension CodexCompanionMobileBridgeServer: CompanionMobileRuntimeService {}
extension CompanionPowerAvailabilityCoordinator: CompanionMobileRuntimeService {}

@MainActor
final class CompanionMobileRuntimeController: ObservableObject {
    static let shared = CompanionMobileRuntimeController()

    @Published private(set) var isEnabled: Bool

    let isAvailable: Bool

    private let policy: CompanionMobileRuntimePolicy
    private let makeBridge: () -> any CompanionMobileRuntimeService
    private let makePowerCoordinator: () -> any CompanionMobileRuntimeService
    private let cancelPairing: () -> Void
    private var bridge: (any CompanionMobileRuntimeService)?
    private var powerCoordinator: (any CompanionMobileRuntimeService)?

    init(
        policy: CompanionMobileRuntimePolicy = CompanionMobileRuntimePolicy(),
        makeBridge: @escaping () -> any CompanionMobileRuntimeService = {
            CodexCompanionMobileBridgeServer()
        },
        makePowerCoordinator: @escaping () -> any CompanionMobileRuntimeService = {
            CompanionPowerAvailabilityCoordinator()
        },
        cancelPairing: @escaping () -> Void = {
            CompanionPairingCoordinator.shared.cancelPairing()
        }
    ) {
        self.policy = policy
        self.makeBridge = makeBridge
        self.makePowerCoordinator = makePowerCoordinator
        self.cancelPairing = cancelPairing
        isAvailable = policy.isBuildAuthorized
        isEnabled = policy.isRuntimeEnabled
    }

    var isRunning: Bool {
        bridge != nil
    }

    func startIfEnabled() {
        guard isEnabled, isAvailable else {
            stopRuntime()
            return
        }
        guard bridge == nil else { return }

        let nextBridge = makeBridge()
        let nextPowerCoordinator = makePowerCoordinator()
        bridge = nextBridge
        powerCoordinator = nextPowerCoordinator
        nextBridge.start()
        nextPowerCoordinator.start()
    }

    func setEnabled(_ requestedValue: Bool) {
        let effectiveValue = isAvailable && requestedValue
        policy.setRuntimeEnabled(effectiveValue)
        guard effectiveValue != isEnabled else {
            if effectiveValue {
                startIfEnabled()
            }
            return
        }

        isEnabled = effectiveValue
        if effectiveValue {
            startIfEnabled()
        } else {
            cancelPairing()
            stopRuntime()
        }
    }

    func shutdown() {
        stopRuntime()
    }

    private func stopRuntime() {
        powerCoordinator?.stop()
        powerCoordinator = nil
        bridge?.stop()
        bridge = nil
    }
}
