import Foundation

actor ChatGPTAccountRelayClient {
    typealias Stream = AsyncThrowingStream<ChatGPTAccountStreamEvent, Error>

    private let url: URL
    private let socket: any ChatGPTAccountRelaySocket
    private let sink: any ChatGPTAccountTurnSink
    private let codec: ChatGPTAccountRelayCodec
    private var currentState: ChatGPTAccountRelayState = .stopped
    private var offer: ChatGPTAccountPairingOffer?
    private var pending: [UUID: Stream.Continuation] = [:]
    private var receiveTask: Task<Void, Never>?

    init(
        url: URL,
        socket: any ChatGPTAccountRelaySocket = URLSessionChatGPTAccountRelaySocket(),
        sink: any ChatGPTAccountTurnSink = NoopChatGPTAccountTurnSink(),
        codec: ChatGPTAccountRelayCodec = ChatGPTAccountRelayCodec()
    ) {
        self.url = url
        self.socket = socket
        self.sink = sink
        self.codec = codec
    }

    func start(with offer: ChatGPTAccountPairingOffer) async throws {
        await stop()
        currentState = .connecting
        try await socket.connect(to: url)
        self.offer = offer
        try await send(.registration(
            pairingCode: offer.pairingCode,
            companionDeviceID: offer.companionDeviceID,
            clientLabel: offer.clientLabel,
            expiresAt: offer.expiresAt
        ))
        currentState = .waitingForPairing(offer)
        receiveTask = Task { [weak self] in
            guard let self else { return }
            await self.receiveLoop()
        }
    }

    func stop() async {
        receiveTask?.cancel()
        receiveTask = nil
        for continuation in pending.values {
            continuation.finish(throwing: ChatGPTAccountRelayError.relayUnavailable)
        }
        pending.removeAll()
        await socket.close()
        currentState = .stopped
    }

    func state() -> ChatGPTAccountRelayState { currentState }

    nonisolated func queue(_ request: ChatGPTAccountChatRequest) -> Stream {
        let localRequestID = UUID()
        return Stream { continuation in
            continuation.onTermination = { [weak self] _ in
                Task { await self?.cancel(localRequestID) }
            }
            Task {
                await self.enqueue(request, id: localRequestID, continuation: continuation)
            }
        }
    }

    private func enqueue(
        _ request: ChatGPTAccountChatRequest,
        id: UUID,
        continuation: Stream.Continuation
    ) async {
        guard case .paired = currentState else {
            continuation.finish(throwing: ChatGPTAccountRelayError.notLinked)
            return
        }
        pending[id] = continuation
        do {
            try await send(.queueTurn(
                localRequestID: id,
                conversationID: request.conversationID,
                prompt: request.prompt
            ))
        } catch {
            pending.removeValue(forKey: id)
            continuation.finish(throwing: error)
        }
    }

    private func cancel(_ id: UUID) async {
        guard pending.removeValue(forKey: id) != nil else { return }
        try? await send(.cancelTurn(localRequestID: id))
    }

    private func send(_ message: ChatGPTAccountRelayWireMessage) async throws {
        try await socket.send(codec.encode(message))
    }

    private func receiveLoop() async {
        do {
            while !Task.isCancelled {
                let data = try await socket.receive()
                try await route(codec.decode(data))
            }
        } catch {
            guard !Task.isCancelled else { return }
            currentState = .failed(.relayUnavailable)
            for continuation in pending.values {
                continuation.finish(throwing: ChatGPTAccountRelayError.relayUnavailable)
            }
            pending.removeAll()
        }
    }

    private func route(_ frame: ChatGPTAccountRelayWireMessage) async throws {
        switch frame.type {
        case .registrationAccepted:
            guard frame.companionDeviceID == offer?.companionDeviceID else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
        case .paired:
            guard let sessionID = frame.bridgeSessionID, let expiresAt = frame.expiresAt else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            currentState = .paired(sessionID: sessionID, expiresAt: expiresAt)
        case .pendingTurnClaimed:
            guard let id = frame.localRequestID, let turnID = frame.turnID else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            pending[id]?.yield(.conversationStarted(id: turnID))
        case .pendingTurnFinished:
            guard let id = frame.localRequestID,
                  frame.turnID != nil,
                  let text = frame.responseText,
                  let status = frame.terminalStatus else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            let continuation = pending.removeValue(forKey: id)
            continuation?.yield(.textDelta(text))
            if status == .completed {
                continuation?.yield(.completed)
                continuation?.finish()
            } else {
                continuation?.finish(throwing: ChatGPTAccountRelayError.remote(
                    code: "turn_failed",
                    message: text
                ))
            }
        case .accountTurnBegan:
            guard let turnID = frame.turnID, let message = frame.message else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            await sink.receive(.began(
                turnID: turnID,
                message: message,
                modelLabel: frame.modelLabel,
                agentLabel: frame.agentLabel
            ))
        case .accountTurnStatus:
            guard let turnID = frame.turnID, let status = frame.status else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            await sink.receive(.status(turnID: turnID, status: status))
        case .accountTurnFinished:
            guard let turnID = frame.turnID,
                  let text = frame.responseText,
                  let status = frame.terminalStatus else {
                throw ChatGPTAccountRelayError.invalidMessage
            }
            await sink.receive(.finished(
                turnID: turnID,
                responseText: text,
                status: status
            ))
        case .relayError:
            let error = ChatGPTAccountRelayError.remote(
                code: frame.errorCode ?? "invalid_message",
                message: frame.message ?? "The account relay failed."
            )
            if let id = frame.localRequestID,
               let continuation = pending.removeValue(forKey: id) {
                continuation.finish(throwing: error)
            } else {
                currentState = .failed(error)
                for continuation in pending.values {
                    continuation.finish(throwing: error)
                }
                pending.removeAll()
            }
        case .registerCompanion, .queueTurn, .cancelTurn:
            throw ChatGPTAccountRelayError.invalidMessage
        }
    }
}
