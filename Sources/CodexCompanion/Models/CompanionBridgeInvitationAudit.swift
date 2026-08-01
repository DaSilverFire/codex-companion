import Foundation

enum CompanionBridgeInvitationAudit {
    static func rejectionSummary(
        decision: CompanionBridgeInvitationDecision,
        invitation: CompanionBridgeInvitation,
        trustedRecordFound: Bool,
        now: Date
    ) -> String {
        let ageMilliseconds = CompanionBridgeSecurity.milliseconds(since1970: now)
            - invitation.issuedAtMilliseconds
        return [
            "reason=\(reason(for: decision))",
            "protocol=\(invitation.version)",
            "ageMs=\(ageMilliseconds)",
            "trustedRecord=\(trustedRecordFound)",
            "authenticator=\(invitation.authenticator != nil)",
            "pairingCode=\(CompanionBridgeSecurity.normalizedPairingCode(invitation.pairingCode) != nil)",
        ].joined(separator: " ")
    }

    private static func reason(for decision: CompanionBridgeInvitationDecision) -> String {
        switch decision {
        case .acceptTrusted:
            "accepted-trusted"
        case .acceptPairing:
            "accepted-pairing"
        case .rejectVersion:
            "version"
        case .rejectExpired:
            "expired"
        case .rejectAuthentication:
            "authentication"
        case .rejectUnpaired:
            "unpaired"
        }
    }
}
