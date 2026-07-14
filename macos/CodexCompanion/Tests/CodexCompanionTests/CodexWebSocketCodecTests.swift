import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexWebSocketCodecTests {
    @Test
    func handshakeTargetsTheDaemonRPCWebSocket() {
        let request = String(decoding: CodexWebSocketCodec.handshakeRequest(key: "test-key"), as: UTF8.self)

        #expect(request.hasPrefix("GET /rpc HTTP/1.1\r\n"))
        #expect(request.contains("Host: localhost\r\n"))
        #expect(request.contains("Upgrade: websocket\r\n"))
        #expect(request.contains("Connection: Upgrade\r\n"))
        #expect(request.contains("Sec-WebSocket-Key: test-key\r\n"))
        #expect(request.contains("Sec-WebSocket-Version: 13\r\n"))
        #expect(request.hasSuffix("\r\n\r\n"))
    }

    @Test
    func clientTextFrameMasksExtendedPayload() throws {
        let payload = Data(repeating: 0x41, count: 140)
        let mask: [UInt8] = [0x11, 0x22, 0x33, 0x44]
        let frame = CodexWebSocketCodec.clientFrame(
            opcode: .text,
            payload: payload,
            mask: mask
        )

        #expect(frame[0] == 0x81)
        #expect(frame[1] == 0xFE)
        #expect(frame[2] == 0)
        #expect(frame[3] == 140)
        #expect(Array(frame[4..<8]) == mask)

        let decoded = Data(frame[8...].enumerated().map { offset, byte in
            byte ^ mask[offset % mask.count]
        })
        #expect(decoded == payload)
    }

    @Test
    func parserWaitsForCompleteHandshakeAndTextFrame() throws {
        let response = Data("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n".utf8)
        let payload = Data("{\"id\":1,\"result\":{}}".utf8)
        let frame = serverFrame(opcode: .text, payload: payload)
        let combined = response + frame
        var codec = CodexWebSocketCodec()

        #expect(try codec.receive(combined.prefix(24)).isEmpty)
        let events = try codec.receive(combined.dropFirst(24))

        #expect(events == [.upgraded, .text(payload)])
    }

    @Test
    func parserSurfacesPingPayloadForPongReply() throws {
        let response = Data("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n".utf8)
        let payload = Data("keepalive".utf8)
        var codec = CodexWebSocketCodec()

        let events = try codec.receive(response + serverFrame(opcode: .ping, payload: payload))

        #expect(events == [.upgraded, .ping(payload)])
    }

    private func serverFrame(
        opcode: CodexWebSocketCodec.Opcode,
        payload: Data
    ) -> Data {
        var frame = Data([0x80 | opcode.rawValue])
        if payload.count < 126 {
            frame.append(UInt8(payload.count))
        } else {
            frame.append(126)
            frame.append(UInt8((payload.count >> 8) & 0xFF))
            frame.append(UInt8(payload.count & 0xFF))
        }
        frame.append(payload)
        return frame
    }
}
