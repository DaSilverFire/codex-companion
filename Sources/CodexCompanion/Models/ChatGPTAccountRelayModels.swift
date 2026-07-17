import Foundation

enum ChatGPTAccountRelayProtocol {
    static let version = 1
    static let pairingCodeLength = 8
    static let pairingLifetime: TimeInterval = 600
    static let maximumTextLength = 100_000
    static let maximumFrameBytes = 131_072
    static let alphabet = Array("ABCDEFGHJKLMNPQRSTUVWXYZ23456789")
}

struct ChatGPTAccountPairingOffer: Equatable, Sendable {
    var pairingCode: String
    var companionDeviceID: String
    var clientLabel: String
    var expiresAt: Date

    static func make(
        companionDeviceID: String,
        clientLabel: String,
        now: Date = Date(),
        randomIndex: (Int) -> Int = { Int.random(in: 0 ..< $0) }
    ) -> Self {
        let code = String((0 ..< ChatGPTAccountRelayProtocol.pairingCodeLength).map { _ in
            ChatGPTAccountRelayProtocol.alphabet[
                randomIndex(ChatGPTAccountRelayProtocol.alphabet.count)
            ]
        })
        return Self(
            pairingCode: code,
            companionDeviceID: companionDeviceID,
            clientLabel: clientLabel,
            expiresAt: now.addingTimeInterval(ChatGPTAccountRelayProtocol.pairingLifetime)
        )
    }
}

enum ChatGPTAccountRelayMessageType: String, Codable, Sendable {
    case registerCompanion = "register_companion"
    case queueTurn = "queue_turn"
    case cancelTurn = "cancel_turn"
    case registrationAccepted = "registration_accepted"
    case paired
    case accountTurnBegan = "account_turn_began"
    case accountTurnStatus = "account_turn_status"
    case accountTurnFinished = "account_turn_finished"
    case pendingTurnClaimed = "pending_turn_claimed"
    case pendingTurnFinished = "pending_turn_finished"
    case relayError = "relay_error"
}

enum ChatGPTAccountRelayTurnStatus: String, Codable, Sendable {
    case thinking
    case talking
    case waiting
    case completed
    case failed
}

enum ChatGPTAccountRelayTerminalStatus: String, Codable, Sendable {
    case completed
    case failed
}

struct ChatGPTAccountRelayWireMessage: Codable, Equatable, Sendable {
    var protocolVersion = ChatGPTAccountRelayProtocol.version
    var messageID: UUID
    var type: ChatGPTAccountRelayMessageType
    var pairingCode: String?
    var companionDeviceID: String?
    var clientLabel: String?
    var expiresAt: Date?
    var bridgeSessionID: UUID?
    var localRequestID: UUID?
    var conversationID: String?
    var turnID: String?
    var prompt: String?
    var message: String?
    var responseText: String?
    var status: ChatGPTAccountRelayTurnStatus?
    var terminalStatus: ChatGPTAccountRelayTerminalStatus?
    var modelLabel: String?
    var agentLabel: String?
    var errorCode: String?

    static func registration(
        messageID: UUID = UUID(),
        pairingCode: String,
        companionDeviceID: String,
        clientLabel: String,
        expiresAt: Date
    ) -> Self {
        Self(
            messageID: messageID,
            type: .registerCompanion,
            pairingCode: pairingCode,
            companionDeviceID: companionDeviceID,
            clientLabel: clientLabel,
            expiresAt: expiresAt
        )
    }

    static func queueTurn(
        messageID: UUID = UUID(),
        localRequestID: UUID,
        conversationID: String?,
        prompt: String
    ) -> Self {
        Self(
            messageID: messageID,
            type: .queueTurn,
            localRequestID: localRequestID,
            conversationID: conversationID,
            prompt: prompt
        )
    }

    static func cancelTurn(messageID: UUID = UUID(), localRequestID: UUID) -> Self {
        Self(messageID: messageID, type: .cancelTurn, localRequestID: localRequestID)
    }

    enum CodingKeys: String, CodingKey {
        case protocolVersion = "protocol_version"
        case messageID = "message_id"
        case type
        case pairingCode = "pairing_code"
        case companionDeviceID = "companion_device_id"
        case clientLabel = "client_label"
        case expiresAt = "expires_at"
        case bridgeSessionID = "bridge_session_id"
        case localRequestID = "local_request_id"
        case conversationID = "conversation_id"
        case turnID = "turn_id"
        case prompt
        case message
        case responseText = "response_text"
        case status
        case terminalStatus = "terminal_status"
        case modelLabel = "model_label"
        case agentLabel = "agent_label"
        case errorCode = "error_code"
    }
}

enum ChatGPTAccountMirroredTurnEvent: Equatable, Sendable {
    case began(turnID: String, message: String, modelLabel: String?, agentLabel: String?)
    case status(turnID: String, status: ChatGPTAccountRelayTurnStatus)
    case finished(
        turnID: String,
        responseText: String,
        status: ChatGPTAccountRelayTerminalStatus
    )
}

enum ChatGPTAccountRelayState: Equatable, Sendable {
    case stopped
    case connecting
    case waitingForPairing(ChatGPTAccountPairingOffer)
    case paired(sessionID: UUID, expiresAt: Date)
    case failed(ChatGPTAccountRelayError)
}

enum ChatGPTAccountRelayError: Error, Equatable, Sendable {
    case notLinked
    case pairingExpired
    case pairingReplayed
    case unauthorizedSession
    case invalidMessage
    case relayUnavailable
    case unsupportedCapability
    case textTooLarge
    case frameTooLarge
    case remote(code: String, message: String)
}

struct ChatGPTAccountRelayCodec: Sendable {
    func encode(_ message: ChatGPTAccountRelayWireMessage) throws -> Data {
        for text in [message.prompt, message.message, message.responseText].compactMap({ $0 }) {
            guard text.unicodeScalars.count <= ChatGPTAccountRelayProtocol.maximumTextLength else {
                throw ChatGPTAccountRelayError.textTooLarge
            }
        }

        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(message)
        guard data.count <= ChatGPTAccountRelayProtocol.maximumFrameBytes else {
            throw ChatGPTAccountRelayError.frameTooLarge
        }
        return data
    }

    func decode(_ data: Data) throws -> ChatGPTAccountRelayWireMessage {
        guard data.count <= ChatGPTAccountRelayProtocol.maximumFrameBytes else {
            throw ChatGPTAccountRelayError.frameTooLarge
        }

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let message = try decoder.decode(ChatGPTAccountRelayWireMessage.self, from: data)
        guard message.protocolVersion == ChatGPTAccountRelayProtocol.version else {
            throw ChatGPTAccountRelayError.invalidMessage
        }
        return message
    }
}
