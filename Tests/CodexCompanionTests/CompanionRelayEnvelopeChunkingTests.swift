import Foundation
import Testing
@testable import CodexCompanion

@Suite("Companion relay envelope chunking")
struct CompanionRelayEnvelopeChunkingTests {
    @Test("small encrypted envelopes preserve the legacy one-packet wire format")
    func smallEnvelopeUsesLegacyPacket() throws {
        let envelope = makeEnvelope(payloadBytes: 512)

        let packets = try CompanionRelayWireMessage.packets(
            envelope: envelope,
            transferID: "transfer-small",
            packetIDProvider: { "packet-\($0)" }
        )

        let packet = try #require(packets.first)
        #expect(packets.count == 1)
        #expect(packet.transferID == nil)
        #expect(packet.chunkIndex == nil)
        #expect(packet.chunkCount == nil)
        #expect(try packet.decodedEnvelope() == envelope)
    }

    @Test("large encrypted envelopes stay under the deployed relay limit and reassemble")
    func largeEnvelopeChunksAndReassembles() throws {
        let envelope = makeEnvelope(payloadBytes: 900_000)

        let packets = try CompanionRelayWireMessage.packets(
            envelope: envelope,
            transferID: "transfer-large",
            packetIDProvider: { "packet-\($0)" }
        )

        #expect(packets.count > 1)
        for packet in packets {
            #expect(
                try JSONEncoder().encode(packet).count
                    < CompanionRelayWireMessage.maximumWireMessageBytes
            )
        }

        var reassembler = CompanionRelayEnvelopeReassembler()
        var outputs: [CompanionBridgeEncryptedEnvelope] = []
        for packet in packets.reversed() {
            if let output = try reassembler.receive(packet, at: Date(timeIntervalSince1970: 10)) {
                outputs.append(output)
            }
        }

        #expect(outputs == [envelope])
    }

    @Test("an incomplete transfer never emits a partial encrypted envelope")
    func incompleteTransferDoesNotEmit() throws {
        let envelope = makeEnvelope(payloadBytes: 900_000)
        let packets = try CompanionRelayWireMessage.packets(
            envelope: envelope,
            transferID: "transfer-incomplete",
            packetIDProvider: { "packet-\($0)" }
        )
        var reassembler = CompanionRelayEnvelopeReassembler()

        let output = try reassembler.receive(
            try #require(packets.first),
            at: Date(timeIntervalSince1970: 20)
        )

        #expect(output == nil)
    }

    @Test("conflicting duplicate chunks are rejected")
    func conflictingDuplicateIsRejected() throws {
        let envelope = makeEnvelope(payloadBytes: 900_000)
        let packets = try CompanionRelayWireMessage.packets(
            envelope: envelope,
            transferID: "transfer-duplicate",
            packetIDProvider: { "packet-\($0)" }
        )
        var reassembler = CompanionRelayEnvelopeReassembler()
        let first = try #require(packets.first)
        _ = try reassembler.receive(first, at: Date(timeIntervalSince1970: 30))

        var conflicting = first
        conflicting.envelope = Data(repeating: 0xFF, count: first.envelope?.count ?? 1)

        #expect(throws: CompanionRelayWireError.metadataMismatch) {
            try reassembler.receive(
                conflicting,
                at: Date(timeIntervalSince1970: 31)
            )
        }
    }

    @Test("incomplete transfer IDs cannot grow reassembly memory without bound")
    func concurrentIncompleteTransfersAreBounded() throws {
        let packets = try CompanionRelayWireMessage.packets(
            envelope: makeEnvelope(payloadBytes: 900_000),
            transferID: "transfer-template",
            packetIDProvider: { "packet-template-\($0)" }
        )
        let template = try #require(packets.first)
        var reassembler = CompanionRelayEnvelopeReassembler()

        for index in 0..<8 {
            var packet = template
            packet.packetID = "packet-bounded-\(index)"
            packet.transferID = "transfer-bounded-\(index)"
            #expect(
                try reassembler.receive(
                    packet,
                    at: Date(timeIntervalSince1970: 40)
                ) == nil
            )
        }

        var overflow = template
        overflow.packetID = "packet-bounded-overflow"
        overflow.transferID = "transfer-bounded-overflow"
        #expect(throws: CompanionRelayWireError.invalidPacket) {
            try reassembler.receive(
                overflow,
                at: Date(timeIntervalSince1970: 41)
            )
        }
    }

    private func makeEnvelope(payloadBytes: Int) -> CompanionBridgeEncryptedEnvelope {
        CompanionBridgeEncryptedEnvelope(
            channelID: "channel-test",
            senderID: "mac-test",
            sequence: 42,
            sentAtMilliseconds: 1_750_000_000_000,
            sealedPayload: Data(repeating: 0xA5, count: payloadBytes)
        )
    }
}
