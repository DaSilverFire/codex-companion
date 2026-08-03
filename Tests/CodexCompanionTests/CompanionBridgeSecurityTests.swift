import CryptoKit
import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionBridgeSecurityTests {
    private let secret = Data((0..<32).map(UInt8.init))

    @Test
    func authenticatedRawPayloadRoundTripsBeforeWireDecoding() throws {
        let payload = Data(#"{"messageType":"server_event","sequence":7}"#.utf8)
        let envelope = try CompanionBridgeSecurity.seal(
            RawPayloadFixture(data: payload),
            secret: secret,
            senderID: "mac-security-test",
            sequence: 7
        )
        let encodedFixture = try JSONEncoder().encode(RawPayloadFixture(data: payload))

        #expect(try CompanionBridgeSecurity.openData(envelope, secret: secret) == encodedFixture)
    }

    @Test
    func authenticatedRawPayloadRejectsTamperedHeader() throws {
        var envelope = try CompanionBridgeSecurity.seal(
            RawPayloadFixture(data: Data("secure".utf8)),
            secret: secret,
            senderID: "mac-security-test",
            sequence: 8
        )
        envelope.sequence += 1

        #expect(throws: (any Error).self) {
            _ = try CompanionBridgeSecurity.openData(envelope, secret: secret)
        }
    }

    @Test
    func authenticatedDatePayloadUsesUnixMilliseconds() throws {
        let createdAt = Date(timeIntervalSince1970: 1_785_661_234.567)
        let envelope = try CompanionBridgeSecurity.seal(
            DatePayloadFixture(createdAt: createdAt),
            secret: secret,
            senderID: "mac-security-test",
            sequence: 9
        )
        let payload = try CompanionBridgeSecurity.openData(envelope, secret: secret)
        let object = try #require(
            JSONSerialization.jsonObject(with: payload) as? [String: Any]
        )
        let encodedMilliseconds = try #require(object["createdAt"] as? Double)

        #expect(abs(encodedMilliseconds - 1_785_661_234_567) < 1)
    }
}

private struct RawPayloadFixture: Codable {
    var data: Data
}

private struct DatePayloadFixture: Codable {
    var createdAt: Date
}
