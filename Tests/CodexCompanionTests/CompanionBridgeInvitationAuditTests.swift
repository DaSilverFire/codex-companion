import Foundation
import Testing
@testable import CodexCompanion

@Suite("Companion bridge invitation audit")
struct CompanionBridgeInvitationAuditTests {
    @Test("a trusted secret under another device ID is rejected and diagnosed safely")
    func diagnosesExactIdentityMismatchWithoutCredentials() throws {
        let now = Date(timeIntervalSince1970: 1_750_000_000)
        let trustedDeviceID = "00000000-0000-0000-0000-000000000001"
        let invitingDeviceID = "00000000-0000-0000-0000-000000000002"
        let displayName = "Private simulator name"
        let pairingCode = "921734"
        let secret = Data(repeating: 0xA7, count: 32)
        let nonce = Data(repeating: 0xC4, count: 16)
        let authenticatorMarker = try CompanionBridgeSecurity.authenticatedInvitation(
            deviceID: invitingDeviceID,
            displayName: displayName,
            secret: secret,
            now: now,
            nonce: nonce
        ).authenticator!.base64EncodedString()
        let temporaryDirectory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: temporaryDirectory) }
        let store = CompanionPairingRecordStore(
            fileURL: temporaryDirectory.appendingPathComponent("paired-devices.json")
        )
        try store.save(CompanionPairingRecord(
            deviceID: trustedDeviceID,
            displayName: "Expected simulator",
            secret: secret,
            pairedAt: now
        ))
        let coordinator = CompanionPairingCoordinator(store: store, now: { now })
        var invitation = try CompanionBridgeSecurity.authenticatedInvitation(
            deviceID: invitingDeviceID,
            displayName: displayName,
            secret: secret,
            now: now,
            nonce: nonce
        )
        invitation.pairingCode = pairingCode

        let decision = coordinator.invitationDecision(invitation)
        let summary = CompanionBridgeInvitationAudit.rejectionSummary(
            decision: decision,
            invitation: invitation,
            trustedRecordFound: coordinator.trustedRecord(for: invitation.deviceID) != nil,
            now: now
        )

        #expect(decision == .rejectUnpaired)
        #expect(
            summary ==
                "reason=unpaired protocol=1 ageMs=0 trustedRecord=false authenticator=true pairingCode=true"
        )
        #expect(!summary.contains(secret.base64EncodedString()))
        #expect(!summary.contains(authenticatorMarker))
        #expect(!summary.contains(nonce.base64EncodedString()))
        #expect(!summary.contains(pairingCode))
        #expect(!summary.contains(displayName))
        #expect(!summary.contains(trustedDeviceID))
        #expect(!summary.contains(invitingDeviceID))
    }
}
