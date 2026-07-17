import CryptoKit
import Foundation

enum CompanionBridgeSecurityError: Error, Equatable {
    case invalidInvitationVersion
    case invalidSecret
    case invalidEnvelopeChannel
}

struct CompanionBridgeInvitation: Codable, Equatable, Sendable {
    var version: Int = CompanionBridgeSecurity.invitationVersion
    var deviceID: String
    var displayName: String
    var issuedAtMilliseconds: Int64
    var nonce: Data
    var authenticator: Data?
    var pairingCode: String?
}

struct CompanionBridgeActivePairing: Equatable, Sendable {
    var code: String
    var expiresAt: Date
}

enum CompanionBridgeInvitationDecision: Equatable, Sendable {
    case acceptTrusted
    case acceptPairing
    case rejectVersion
    case rejectExpired
    case rejectAuthentication
    case rejectUnpaired
}

struct CompanionBridgeEncryptedEnvelope: Codable, Equatable, Sendable {
    var version: Int = CompanionBridgeSecurity.envelopeVersion
    var channelID: String
    var senderID: String
    var sequence: UInt64
    var sentAtMilliseconds: Int64
    var sealedPayload: Data
}

struct CompanionBridgeReplayWindow: Sendable {
    private var highestSequenceBySender: [String: UInt64] = [:]

    mutating func accept(sequence: UInt64, from senderID: String) -> Bool {
        if let highest = highestSequenceBySender[senderID], sequence <= highest {
            return false
        }
        highestSequenceBySender[senderID] = sequence
        return true
    }
}

struct CompanionBridgeChannelReplayWindows: Sendable {
    private var windowsByChannelID: [String: CompanionBridgeReplayWindow] = [:]

    mutating func accept(
        sequence: UInt64,
        from senderID: String,
        channelID: String
    ) -> Bool {
        var window = windowsByChannelID[channelID] ?? CompanionBridgeReplayWindow()
        guard window.accept(sequence: sequence, from: senderID) else { return false }
        windowsByChannelID[channelID] = window
        return true
    }
}

enum CompanionBridgeSecurity {
    static let invitationVersion = 1
    static let envelopeVersion = 1
    static let invitationClockSkew: TimeInterval = 120

    private struct EnvelopeHeader: Codable {
        var version: Int
        var channelID: String
        var senderID: String
        var sequence: UInt64
        var sentAtMilliseconds: Int64
    }

    static func milliseconds(since1970 date: Date) -> Int64 {
        Int64((date.timeIntervalSince1970 * 1_000).rounded())
    }

    static func authenticatedInvitation(
        deviceID: String,
        displayName: String,
        secret: Data,
        now: Date = Date(),
        nonce: Data = randomBytes(count: 16)
    ) throws -> CompanionBridgeInvitation {
        guard secret.count >= 32 else { throw CompanionBridgeSecurityError.invalidSecret }
        var invitation = CompanionBridgeInvitation(
            deviceID: deviceID,
            displayName: displayName,
            issuedAtMilliseconds: milliseconds(since1970: now),
            nonce: nonce
        )
        invitation.authenticator = invitationAuthenticator(for: invitation, secret: secret)
        return invitation
    }

    static func invitationDecision(
        _ invitation: CompanionBridgeInvitation,
        trustedSecret: Data?,
        activePairing: CompanionBridgeActivePairing?,
        now: Date = Date()
    ) -> CompanionBridgeInvitationDecision {
        guard invitation.version == invitationVersion else { return .rejectVersion }

        let issuedAt = Date(
            timeIntervalSince1970: TimeInterval(invitation.issuedAtMilliseconds) / 1_000
        )
        guard abs(now.timeIntervalSince(issuedAt)) <= invitationClockSkew else {
            return .rejectExpired
        }

        if let trustedSecret {
            guard trustedSecret.count >= 32,
                  let authenticator = invitation.authenticator,
                  HMAC<SHA256>.isValidAuthenticationCode(
                    authenticator,
                    authenticating: invitationAuthenticationData(invitation),
                    using: SymmetricKey(data: trustedSecret)
                  )
            else { return .rejectAuthentication }
            return .acceptTrusted
        }

        if let activePairing,
           activePairing.expiresAt >= now,
           normalizedPairingCode(activePairing.code) == normalizedPairingCode(invitation.pairingCode),
           normalizedPairingCode(activePairing.code)?.count == 6 {
            return .acceptPairing
        }

        return .rejectUnpaired
    }

    static func channelID(secret: Data) -> String {
        let key = SymmetricKey(data: secret)
        let digest = HMAC<SHA256>.authenticationCode(
            for: Data("codex-companion-relay-channel-v1".utf8),
            using: key
        )
        return Data(digest.prefix(24)).base64URLEncodedString()
    }

    static func seal<Value: Encodable>(
        _ value: Value,
        secret: Data,
        senderID: String,
        sequence: UInt64,
        now: Date = Date()
    ) throws -> CompanionBridgeEncryptedEnvelope {
        guard secret.count >= 32 else { throw CompanionBridgeSecurityError.invalidSecret }
        let header = EnvelopeHeader(
            version: envelopeVersion,
            channelID: channelID(secret: secret),
            senderID: senderID,
            sequence: sequence,
            sentAtMilliseconds: milliseconds(since1970: now)
        )
        let payload = try makeEncoder().encode(value)
        let sealed = try ChaChaPoly.seal(
            payload,
            using: SymmetricKey(data: secret),
            authenticating: try makeEncoder().encode(header)
        )
        return CompanionBridgeEncryptedEnvelope(
            channelID: header.channelID,
            senderID: senderID,
            sequence: sequence,
            sentAtMilliseconds: header.sentAtMilliseconds,
            sealedPayload: sealed.combined
        )
    }

    static func open<Value: Decodable>(
        _ envelope: CompanionBridgeEncryptedEnvelope,
        secret: Data,
        as type: Value.Type
    ) throws -> Value {
        guard secret.count >= 32 else { throw CompanionBridgeSecurityError.invalidSecret }
        guard envelope.version == envelopeVersion,
              envelope.channelID == channelID(secret: secret)
        else { throw CompanionBridgeSecurityError.invalidEnvelopeChannel }
        let header = EnvelopeHeader(
            version: envelope.version,
            channelID: envelope.channelID,
            senderID: envelope.senderID,
            sequence: envelope.sequence,
            sentAtMilliseconds: envelope.sentAtMilliseconds
        )
        let box = try ChaChaPoly.SealedBox(combined: envelope.sealedPayload)
        let payload = try ChaChaPoly.open(
            box,
            using: SymmetricKey(data: secret),
            authenticating: try makeEncoder().encode(header)
        )
        return try makeDecoder().decode(type, from: payload)
    }

    static func randomSecret() -> Data {
        randomBytes(count: 32)
    }

    static func normalizedPairingCode(_ code: String?) -> String? {
        guard let code else { return nil }
        let digits = code.filter(\.isNumber)
        return digits.isEmpty ? nil : digits
    }

    private static func invitationAuthenticator(
        for invitation: CompanionBridgeInvitation,
        secret: Data
    ) -> Data {
        Data(HMAC<SHA256>.authenticationCode(
            for: invitationAuthenticationData(invitation),
            using: SymmetricKey(data: secret)
        ))
    }

    private static func invitationAuthenticationData(
        _ invitation: CompanionBridgeInvitation
    ) -> Data {
        var data = Data()
        data.append(contentsOf: withUnsafeBytes(of: Int64(invitation.version).bigEndian, Array.init))
        appendLengthPrefixed(Data(invitation.deviceID.utf8), to: &data)
        appendLengthPrefixed(Data(invitation.displayName.utf8), to: &data)
        data.append(contentsOf: withUnsafeBytes(of: invitation.issuedAtMilliseconds.bigEndian, Array.init))
        appendLengthPrefixed(invitation.nonce, to: &data)
        return data
    }

    private static func appendLengthPrefixed(_ value: Data, to data: inout Data) {
        let count = UInt32(value.count).bigEndian
        data.append(contentsOf: withUnsafeBytes(of: count, Array.init))
        data.append(value)
    }

    private static func randomBytes(count: Int) -> Data {
        let key = SymmetricKey(size: .bits256)
        let bytes = key.withUnsafeBytes { Data($0) }
        if count == bytes.count { return bytes }
        if count < bytes.count { return Data(bytes.prefix(count)) }
        var result = Data()
        while result.count < count {
            result.append(SymmetricKey(size: .bits256).withUnsafeBytes { Data($0) })
        }
        return Data(result.prefix(count))
    }

    private static func makeEncoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }

    private static func makeDecoder() -> JSONDecoder {
        JSONDecoder()
    }
}

private extension Data {
    func base64URLEncodedString() -> String {
        base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}
