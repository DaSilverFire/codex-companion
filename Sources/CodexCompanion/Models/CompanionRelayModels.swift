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
    static let maximumWireMessageBytes = 1_048_576
    static let chunkPayloadBytes = 512 * 1_024
    static let maximumAssembledEnvelopeBytes = 16 * 1_024 * 1_024
    static let maximumChunkCount = maximumAssembledEnvelopeBytes / chunkPayloadBytes

    var type: CompanionRelayWireMessageType
    var protocolVersion: Int = Self.protocolVersion
    var packetID: String?
    var channelID: String?
    var endpointID: String?
    var senderID: String?
    var envelope: Data?
    var transferID: String?
    var chunkIndex: Int?
    var chunkCount: Int?
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

    static func packets(
        envelope: CompanionBridgeEncryptedEnvelope
    ) throws -> [CompanionRelayWireMessage] {
        try packets(
            envelope: envelope,
            transferID: UUID().uuidString,
            packetIDProvider: { _ in UUID().uuidString }
        )
    }

    static func packets(
        envelope: CompanionBridgeEncryptedEnvelope,
        transferID: String,
        packetIDProvider: (Int) -> String
    ) throws -> [CompanionRelayWireMessage] {
        let legacy = try packet(
            envelope: envelope,
            packetID: packetIDProvider(0)
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        if try encoder.encode(legacy).count < maximumWireMessageBytes {
            return [legacy]
        }

        guard isValidOpaqueID(transferID),
              let encodedEnvelope = legacy.envelope,
              encodedEnvelope.count <= maximumAssembledEnvelopeBytes
        else {
            throw CompanionRelayWireError.invalidPacket
        }
        let chunkCount = Int(
            ceil(Double(encodedEnvelope.count) / Double(chunkPayloadBytes))
        )
        guard (2...maximumChunkCount).contains(chunkCount) else {
            throw CompanionRelayWireError.invalidPacket
        }

        return try (0..<chunkCount).map { chunkIndex in
            let packetID = packetIDProvider(chunkIndex)
            guard isValidOpaqueID(packetID) else {
                throw CompanionRelayWireError.invalidPacket
            }
            let lowerBound = chunkIndex * chunkPayloadBytes
            let upperBound = min(
                lowerBound + chunkPayloadBytes,
                encodedEnvelope.count
            )
            let packet = CompanionRelayWireMessage(
                type: .packet,
                packetID: packetID,
                channelID: envelope.channelID,
                senderID: envelope.senderID,
                envelope: encodedEnvelope.subdata(in: lowerBound..<upperBound),
                transferID: transferID,
                chunkIndex: chunkIndex,
                chunkCount: chunkCount
            )
            guard try encoder.encode(packet).count < maximumWireMessageBytes else {
                throw CompanionRelayWireError.invalidPacket
            }
            return packet
        }
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
              let envelope,
              transferID == nil,
              chunkIndex == nil,
              chunkCount == nil
        else {
            throw CompanionRelayWireError.invalidPacket
        }
        return try Self.decodeEnvelope(
            envelope,
            channelID: channelID,
            senderID: senderID
        )
    }

    static func decodeEnvelope(
        _ data: Data,
        channelID: String,
        senderID: String
    ) throws -> CompanionBridgeEncryptedEnvelope {
        let decoded = try JSONDecoder().decode(
            CompanionBridgeEncryptedEnvelope.self,
            from: data
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

struct CompanionRelayEnvelopeReassembler: Sendable {
    private struct TransferKey: Hashable, Sendable {
        var channelID: String
        var senderID: String
        var transferID: String
    }

    private struct Transfer: Sendable {
        var chunkCount: Int
        var chunks: [Int: Data]
        var assembledBytes: Int
        var lastUpdatedAt: Date
    }

    private static let maximumTransferAge: TimeInterval = 60
    private static let maximumConcurrentTransfers = 8
    private static let maximumBufferedBytes = 32 * 1_024 * 1_024
    private var transfers: [TransferKey: Transfer] = [:]

    mutating func receive(
        _ wire: CompanionRelayWireMessage,
        at now: Date = Date()
    ) throws -> CompanionBridgeEncryptedEnvelope? {
        removeStaleTransfers(at: now)

        let hasChunkMetadata = wire.transferID != nil
            || wire.chunkIndex != nil
            || wire.chunkCount != nil
        guard hasChunkMetadata else {
            return try wire.decodedEnvelope()
        }
        guard wire.type == .packet,
              wire.protocolVersion == CompanionRelayWireMessage.protocolVersion,
              let packetID = wire.packetID,
              CompanionRelayWireMessage.isValidOpaqueID(packetID),
              let channelID = wire.channelID,
              let senderID = wire.senderID,
              let transferID = wire.transferID,
              CompanionRelayWireMessage.isValidOpaqueID(transferID),
              let chunkIndex = wire.chunkIndex,
              let chunkCount = wire.chunkCount,
              (2...CompanionRelayWireMessage.maximumChunkCount).contains(chunkCount),
              (0..<chunkCount).contains(chunkIndex),
              let chunk = wire.envelope,
              !chunk.isEmpty,
              chunk.count <= CompanionRelayWireMessage.chunkPayloadBytes
        else {
            throw CompanionRelayWireError.invalidPacket
        }

        let key = TransferKey(
            channelID: channelID,
            senderID: senderID,
            transferID: transferID
        )
        let existingTransfer = transfers[key]
        guard existingTransfer != nil
                || transfers.count < Self.maximumConcurrentTransfers
        else {
            throw CompanionRelayWireError.invalidPacket
        }
        var transfer = existingTransfer ?? Transfer(
            chunkCount: chunkCount,
            chunks: [:],
            assembledBytes: 0,
            lastUpdatedAt: now
        )
        guard transfer.chunkCount == chunkCount else {
            transfers.removeValue(forKey: key)
            throw CompanionRelayWireError.metadataMismatch
        }
        if let existing = transfer.chunks[chunkIndex] {
            guard existing == chunk else {
                transfers.removeValue(forKey: key)
                throw CompanionRelayWireError.metadataMismatch
            }
            transfer.lastUpdatedAt = now
            transfers[key] = transfer
            return nil
        }

        let assembledBytes = transfer.assembledBytes + chunk.count
        let bufferedBytes = transfers.values.reduce(0) {
            $0 + $1.assembledBytes
        }
        guard assembledBytes <= CompanionRelayWireMessage.maximumAssembledEnvelopeBytes,
              bufferedBytes + chunk.count <= Self.maximumBufferedBytes
        else {
            transfers.removeValue(forKey: key)
            throw CompanionRelayWireError.invalidPacket
        }
        transfer.chunks[chunkIndex] = chunk
        transfer.assembledBytes = assembledBytes
        transfer.lastUpdatedAt = now
        guard transfer.chunks.count == chunkCount else {
            transfers[key] = transfer
            return nil
        }

        var encodedEnvelope = Data(capacity: assembledBytes)
        for index in 0..<chunkCount {
            guard let next = transfer.chunks[index] else {
                transfers.removeValue(forKey: key)
                throw CompanionRelayWireError.invalidPacket
            }
            encodedEnvelope.append(next)
        }
        transfers.removeValue(forKey: key)
        return try CompanionRelayWireMessage.decodeEnvelope(
            encodedEnvelope,
            channelID: channelID,
            senderID: senderID
        )
    }

    mutating func reset() {
        transfers.removeAll(keepingCapacity: false)
    }

    private mutating func removeStaleTransfers(at now: Date) {
        transfers = transfers.filter {
            now.timeIntervalSince($0.value.lastUpdatedAt) <= Self.maximumTransferAge
        }
    }
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
