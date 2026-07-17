import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTAccountRelayModelsTests {
    @Test
    func pairingCodesExcludeAmbiguousCharactersAndExpireAfterTenMinutes() {
        let now = Date(timeIntervalSince1970: 1_752_520_800)
        let offer = ChatGPTAccountPairingOffer.make(
            companionDeviceID: "mac-1",
            clientLabel: "MacBook",
            now: now,
            randomIndex: { _ in 0 }
        )
        #expect(offer.pairingCode == "AAAAAAAA")
        #expect(offer.expiresAt == now.addingTimeInterval(600))
    }

    @Test
    func registrationEncodesWithTheNodeFieldNames() throws {
        let message = ChatGPTAccountRelayWireMessage.registration(
            messageID: UUID(),
            pairingCode: "ABCD2345",
            companionDeviceID: "mac-1",
            clientLabel: "MacBook",
            expiresAt: Date(timeIntervalSince1970: 1_752_521_400)
        )
        let object = try #require(
            JSONSerialization.jsonObject(with: ChatGPTAccountRelayCodec().encode(message))
                as? [String: Any]
        )
        #expect(object["protocol_version"] as? Int == 1)
        #expect(object["type"] as? String == "register_companion")
        #expect(object["pairing_code"] as? String == "ABCD2345")
        #expect(object["companion_device_id"] as? String == "mac-1")
    }

    @Test
    func oversizedTextIsRejectedBeforeEncoding() {
        let message = ChatGPTAccountRelayWireMessage.queueTurn(
            messageID: UUID(),
            localRequestID: UUID(),
            conversationID: nil,
            prompt: String(repeating: "x", count: 100_001)
        )
        #expect(throws: ChatGPTAccountRelayError.textTooLarge) {
            try ChatGPTAccountRelayCodec().encode(message)
        }
    }

    @Test
    func decodesEveryServerFrameKindAndPreservesUnicode() throws {
        let messageIDs = (0..<8).map { _ in UUID().uuidString.lowercased() }
        let bridgeSessionID = UUID().uuidString.lowercased()
        let localRequestID = UUID().uuidString.lowercased()
        let fixtures: [(String, ChatGPTAccountRelayMessageType)] = [
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[0])","type":"registration_accepted","companion_device_id":"mac-1"}"#, .registrationAccepted),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[1])","type":"paired","bridge_session_id":"\#(bridgeSessionID)","expires_at":"2026-07-14T20:10:00Z"}"#, .paired),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[2])","type":"account_turn_began","turn_id":"turn-1","message":"Explain √5092","model_label":"5.6 Sol","agent_label":"Math"}"#, .accountTurnBegan),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[3])","type":"account_turn_status","turn_id":"turn-1","status":"thinking"}"#, .accountTurnStatus),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[4])","type":"account_turn_finished","turn_id":"turn-1","response_text":"√5092 ≈ 71.358","terminal_status":"completed"}"#, .accountTurnFinished),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[5])","type":"pending_turn_claimed","local_request_id":"\#(localRequestID)","turn_id":"turn-2"}"#, .pendingTurnClaimed),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[6])","type":"pending_turn_finished","local_request_id":"\#(localRequestID)","turn_id":"turn-2","response_text":"π × 2 = 2π","terminal_status":"completed"}"#, .pendingTurnFinished),
            (#"{"protocol_version":1,"message_id":"\#(messageIDs[7])","type":"relay_error","local_request_id":"\#(localRequestID)","error_code":"turn_failed","message":"The turn failed."}"#, .relayError),
        ]
        let codec = ChatGPTAccountRelayCodec()
        var decoded: [ChatGPTAccountRelayWireMessage] = []

        for (json, expectedType) in fixtures {
            let message = try codec.decode(Data(json.utf8))
            #expect(message.type == expectedType)
            decoded.append(message)
        }

        #expect(
            decoded[1].expiresAt
                == ISO8601DateFormatter().date(from: "2026-07-14T20:10:00Z")
        )
        #expect(decoded[4].responseText == "√5092 ≈ 71.358")
        #expect(decoded[6].responseText == "π × 2 = 2π")
    }

    @Test
    func rejectsAProtocolVersionTheClientDoesNotImplement() {
        let messageID = UUID().uuidString.lowercased()
        let json = #"{"protocol_version":2,"message_id":"\#(messageID)","type":"registration_accepted","companion_device_id":"mac-1"}"#
        #expect(throws: ChatGPTAccountRelayError.invalidMessage) {
            try ChatGPTAccountRelayCodec().decode(Data(json.utf8))
        }
    }
}
