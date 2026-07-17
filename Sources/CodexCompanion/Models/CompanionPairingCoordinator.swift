import Foundation

enum CompanionPairingCoordinatorError: Error {
    case pairingNotActive
    case invitationRejected
}

final class CompanionPairingCoordinator: @unchecked Sendable {
    static let shared = CompanionPairingCoordinator()
    static let pairingStateDidChange = Notification.Name(
        "CodexCompanion.pairingStateDidChange"
    )

    private let store: CompanionPairingRecordStore
    private let now: () -> Date
    private let codeGenerator: () -> String
    private let secretGenerator: () -> Data
    private let lock = NSLock()
    private var pairing: CompanionBridgeActivePairing?

    init(
        store: CompanionPairingRecordStore = CompanionPairingRecordStore(),
        now: @escaping () -> Date = Date.init,
        codeGenerator: @escaping () -> String = CompanionPairingCoordinator.makeCode,
        secretGenerator: @escaping () -> Data = CompanionBridgeSecurity.randomSecret
    ) {
        self.store = store
        self.now = now
        self.codeGenerator = codeGenerator
        self.secretGenerator = secretGenerator
    }

    @discardableResult
    func beginPairing(validFor duration: TimeInterval = 5 * 60) -> CompanionBridgeActivePairing {
        let next = CompanionBridgeActivePairing(
            code: codeGenerator(),
            expiresAt: now().addingTimeInterval(duration)
        )
        lock.withLock { pairing = next }
        postStateChanged()
        return next
    }

    func cancelPairing() {
        lock.withLock { pairing = nil }
        postStateChanged()
    }

    func activePairing() -> CompanionBridgeActivePairing? {
        lock.withLock {
            guard let pairing, pairing.expiresAt >= now() else {
                self.pairing = nil
                return nil
            }
            return pairing
        }
    }

    func trustedRecords() -> [CompanionPairingRecord] {
        store.records()
    }

    func trustedRecord(for deviceID: String) -> CompanionPairingRecord? {
        store.record(for: deviceID)
    }

    func remember(_ record: CompanionPairingRecord) throws {
        try store.save(record)
        postStateChanged()
    }

    func invitationDecision(
        _ invitation: CompanionBridgeInvitation
    ) -> CompanionBridgeInvitationDecision {
        CompanionBridgeSecurity.invitationDecision(
            invitation,
            trustedSecret: store.record(for: invitation.deviceID)?.secret,
            activePairing: activePairing(),
            now: now()
        )
    }

    func completePairing(
        _ invitation: CompanionBridgeInvitation
    ) throws -> CompanionPairingRecord {
        guard activePairing() != nil else {
            throw CompanionPairingCoordinatorError.pairingNotActive
        }
        guard invitationDecision(invitation) == .acceptPairing else {
            throw CompanionPairingCoordinatorError.invitationRejected
        }
        let record = CompanionPairingRecord(
            deviceID: invitation.deviceID,
            displayName: invitation.displayName,
            secret: secretGenerator(),
            pairedAt: now()
        )
        try store.save(record)
        cancelPairing()
        return record
    }

    func forget(deviceID: String) throws {
        try store.remove(deviceID: deviceID)
        postStateChanged()
    }

    private static func makeCode() -> String {
        String(format: "%06d", Int.random(in: 0...999_999))
    }

    private func postStateChanged() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(
                name: Self.pairingStateDidChange,
                object: self
            )
        }
    }
}

private extension NSLock {
    func withLock<Value>(_ operation: () throws -> Value) rethrows -> Value {
        lock()
        defer { unlock() }
        return try operation()
    }
}
