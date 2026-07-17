import Foundation

struct CodexWebSocketCodec {
    enum Opcode: UInt8, Equatable {
        case continuation = 0x0
        case text = 0x1
        case binary = 0x2
        case close = 0x8
        case ping = 0x9
        case pong = 0xA
    }

    enum Event: Equatable {
        case upgraded
        case text(Data)
        case ping(Data)
        case close
    }

    enum CodecError: Error {
        case invalidHandshake
        case unsupportedFrame
        case messageTooLarge
    }

    private static let headerTerminator = Data("\r\n\r\n".utf8)
    private static let maximumPayloadSize = 4 * 1_024 * 1_024

    private var buffer = Data()
    private var isUpgraded = false
    private var fragmentedOpcode: Opcode?
    private var fragmentedPayload = Data()

    static func handshakeRequest(key: String) -> Data {
        Data(
            """
            GET /rpc HTTP/1.1\r
            Host: localhost\r
            Upgrade: websocket\r
            Connection: Upgrade\r
            Sec-WebSocket-Key: \(key)\r
            Sec-WebSocket-Version: 13\r
            \r

            """.utf8
        )
    }

    static func randomHandshakeKey() -> String {
        var uuid = UUID().uuid
        return withUnsafeBytes(of: &uuid) { Data($0).base64EncodedString() }
    }

    static func randomMask() -> [UInt8] {
        var generator = SystemRandomNumberGenerator()
        return (0..<4).map { _ in UInt8.random(in: .min ... .max, using: &generator) }
    }

    static func clientFrame(
        opcode: Opcode,
        payload: Data,
        mask: [UInt8] = randomMask()
    ) -> Data {
        precondition(mask.count == 4)

        var frame = Data([0x80 | opcode.rawValue])
        if payload.count < 126 {
            frame.append(0x80 | UInt8(payload.count))
        } else if payload.count <= Int(UInt16.max) {
            frame.append(0x80 | 126)
            frame.append(UInt8((payload.count >> 8) & 0xFF))
            frame.append(UInt8(payload.count & 0xFF))
        } else {
            frame.append(0x80 | 127)
            let length = UInt64(payload.count)
            for shift in stride(from: 56, through: 0, by: -8) {
                frame.append(UInt8((length >> UInt64(shift)) & 0xFF))
            }
        }

        frame.append(contentsOf: mask)
        frame.append(contentsOf: payload.enumerated().map { offset, byte in
            byte ^ mask[offset % mask.count]
        })
        return frame
    }

    mutating func receive<S: DataProtocol>(_ incoming: S) throws -> [Event] {
        buffer.append(contentsOf: incoming)
        var events: [Event] = []

        if !isUpgraded {
            guard let headerRange = buffer.firstRange(of: Self.headerTerminator) else {
                return []
            }
            let headerData = buffer.subdata(in: buffer.startIndex..<headerRange.lowerBound)
            guard
                let header = String(data: headerData, encoding: .utf8),
                header.hasPrefix("HTTP/1.1 101 "),
                header.localizedCaseInsensitiveContains("upgrade: websocket")
            else {
                throw CodecError.invalidHandshake
            }
            buffer.removeSubrange(buffer.startIndex..<headerRange.upperBound)
            isUpgraded = true
            events.append(.upgraded)
        }

        while let frame = try nextFrame() {
            switch frame.opcode {
            case .text:
                if frame.isFinal {
                    events.append(.text(frame.payload))
                } else {
                    fragmentedOpcode = .text
                    fragmentedPayload = frame.payload
                }
            case .continuation:
                guard fragmentedOpcode == .text else {
                    throw CodecError.unsupportedFrame
                }
                fragmentedPayload.append(frame.payload)
                guard fragmentedPayload.count <= Self.maximumPayloadSize else {
                    throw CodecError.messageTooLarge
                }
                if frame.isFinal {
                    events.append(.text(fragmentedPayload))
                    fragmentedOpcode = nil
                    fragmentedPayload.removeAll(keepingCapacity: true)
                }
            case .ping:
                events.append(.ping(frame.payload))
            case .pong:
                break
            case .close:
                events.append(.close)
            case .binary:
                throw CodecError.unsupportedFrame
            }
        }
        return events
    }

    private mutating func nextFrame() throws -> Frame? {
        guard buffer.count >= 2 else { return nil }
        let first = buffer[buffer.startIndex]
        let second = buffer[buffer.index(after: buffer.startIndex)]
        guard let opcode = Opcode(rawValue: first & 0x0F) else {
            throw CodecError.unsupportedFrame
        }

        var cursor = 2
        var payloadLength = Int(second & 0x7F)
        if payloadLength == 126 {
            guard buffer.count >= cursor + 2 else { return nil }
            payloadLength = Int(buffer[cursor]) << 8 | Int(buffer[cursor + 1])
            cursor += 2
        } else if payloadLength == 127 {
            guard buffer.count >= cursor + 8 else { return nil }
            var length: UInt64 = 0
            for byte in buffer[cursor..<(cursor + 8)] {
                length = (length << 8) | UInt64(byte)
            }
            guard length <= UInt64(Self.maximumPayloadSize) else {
                throw CodecError.messageTooLarge
            }
            payloadLength = Int(length)
            cursor += 8
        }
        guard payloadLength <= Self.maximumPayloadSize else {
            throw CodecError.messageTooLarge
        }

        let isMasked = second & 0x80 != 0
        var mask: [UInt8] = []
        if isMasked {
            guard buffer.count >= cursor + 4 else { return nil }
            mask = Array(buffer[cursor..<(cursor + 4)])
            cursor += 4
        }
        guard buffer.count >= cursor + payloadLength else { return nil }

        var payload = Data(buffer[cursor..<(cursor + payloadLength)])
        if isMasked {
            payload = Data(payload.enumerated().map { offset, byte in
                byte ^ mask[offset % mask.count]
            })
        }
        buffer.removeSubrange(buffer.startIndex..<(cursor + payloadLength))
        return Frame(
            opcode: opcode,
            payload: payload,
            isFinal: first & 0x80 != 0
        )
    }

    private struct Frame {
        var opcode: Opcode
        var payload: Data
        var isFinal: Bool
    }
}
