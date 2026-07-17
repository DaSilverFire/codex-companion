import Foundation

enum CompanionRelayWireMessageType: String, Codable, Sendable {
    case register
    case registered
    case peerPresence
    case packet
    case packetResult
    case ping
    case pong
    case error
}

enum CompanionRelayPacketResultStatus: String, Codable, Sendable {
    case accepted
    case undeliverable
}

struct CompanionRelayWireMessage: Codable, Equatable, Sendable {
    static let protocolVersion = 1

    var type: CompanionRelayWireMessageType
    var protocolVersion: Int = Self.protocolVersion
    var packetID: String?
    var channelID: String?
    var endpointID: String?
    var senderID: String?
    var envelope: Data?
    var peerCount: Int?
    var status: CompanionRelayPacketResultStatus?
    var code: String?
    var message: String?

    static func registration(
        channelID: String,
        endpointID: String
    ) -> CompanionRelayWireMessage {
        CompanionRelayWireMessage(
            type: .register,
            channelID: channelID,
            endpointID: endpointID
        )
    }

    static func packet(
        envelope: CompanionBridgeEncryptedEnvelope,
        packetID: String
    ) throws -> CompanionRelayWireMessage {
        guard isValidOpaqueID(packetID) else {
            throw CompanionRelayWireError.invalidPacket
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return CompanionRelayWireMessage(
            type: .packet,
            packetID: packetID,
            channelID: envelope.channelID,
            senderID: envelope.senderID,
            envelope: try encoder.encode(envelope)
        )
    }

    static func ping() -> CompanionRelayWireMessage {
        CompanionRelayWireMessage(type: .ping)
    }

    static func pong() -> CompanionRelayWireMessage {
        CompanionRelayWireMessage(type: .pong)
    }

    func decodedEnvelope() throws -> CompanionBridgeEncryptedEnvelope {
        guard type == .packet,
              protocolVersion == Self.protocolVersion,
              let packetID,
              Self.isValidOpaqueID(packetID),
              let channelID,
              let senderID,
              let envelope
        else {
            throw CompanionRelayWireError.invalidPacket
        }
        let decoded = try JSONDecoder().decode(
            CompanionBridgeEncryptedEnvelope.self,
            from: envelope
        )
        guard decoded.channelID == channelID,
              decoded.senderID == senderID
        else {
            throw CompanionRelayWireError.metadataMismatch
        }
        return decoded
    }

    static func isValidOpaqueID(_ value: String) -> Bool {
        let bytes = value.utf8
        guard (1...128).contains(bytes.count) else { return false }
        return bytes.allSatisfy { byte in
            (UInt8(ascii: "A")...UInt8(ascii: "Z")).contains(byte)
                || (UInt8(ascii: "a")...UInt8(ascii: "z")).contains(byte)
                || (UInt8(ascii: "0")...UInt8(ascii: "9")).contains(byte)
                || byte == UInt8(ascii: "_")
                || byte == UInt8(ascii: "-")
        }
    }
}

enum CompanionRelayWireError: Error, Equatable {
    case invalidPacket
    case metadataMismatch
}

enum CompanionBridgeTransportRoute: Equatable, Sendable {
    case nearby
    case relay
    case unavailable
}

enum CompanionBridgeTransportPolicy {
    static func preferredRoute(
        nearbyConnected: Bool,
        relayRegistered: Bool,
        relayHandshakeVerified: Bool
    ) -> CompanionBridgeTransportRoute {
        if nearbyConnected { return .nearby }
        if relayRegistered && relayHandshakeVerified { return .relay }
        return .unavailable
    }
}

final class CompanionRelaySequenceStore: @unchecked Sendable {
    private static let keyPrefix = "CodexCompanion.relaySequence.v1"

    private let defaults: UserDefaults
    private let lock = NSLock()

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func next(channelID: String, senderID: String) -> UInt64 {
        lock.withLock {
            let key = Self.storageKey(channelID: channelID, senderID: senderID)
            let current = defaults.string(forKey: key).flatMap(UInt64.init) ?? 0
            let next = current == UInt64.max ? UInt64.max : current + 1
            defaults.set(String(next), forKey: key)
            return next
        }
    }

    private static func storageKey(channelID: String, senderID: String) -> String {
        "\(keyPrefix).\(channelID).\(senderID)"
    }
}

private extension NSLock {
    func withLock<Value>(_ operation: () -> Value) -> Value {
        lock()
        defer { unlock() }
        return operation()
    }
}
