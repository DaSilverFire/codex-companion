import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTAccountRelayClientTests {
    @Test
    func pairsAndKeepsInboundMirroringSeparateFromQueuedPromptResults() async throws {
        let socket = FakeChatGPTAccountRelaySocket()
        let sink = RecordingChatGPTAccountTurnSink()
        let client = ChatGPTAccountRelayClient(
            url: URL(string: "ws://127.0.0.1:3031/companion")!,
            socket: socket,
            sink: sink
        )
        let offer = pairingOffer()
        try await client.start(with: offer)

        let registrationMessages = try await socket.sentMessages()
        let registration = try #require(registrationMessages.first)
        #expect(registration.type == .registerCompanion)
        #expect(registration.pairingCode == offer.pairingCode)
        #expect(registration.companionDeviceID == offer.companionDeviceID)

        await socket.push(try serverFrame(.registrationAccepted, [
            "companion_device_id": "mac-1",
        ]))
        let sessionID = UUID()
        await socket.push(try serverFrame(.paired, [
            "bridge_session_id": sessionID.uuidString,
            "expires_at": "2026-07-14T20:10:00Z",
        ]))
        let becamePaired = await eventually {
            if case .paired(let actualID, _) = await client.state() {
                return actualID == sessionID
            }
            return false
        }
        #expect(becamePaired)

        let stream = client.queue(ChatGPTAccountChatRequest(
            prompt: "What is √5092?"
        ))
        let collector = Task { () throws -> [ChatGPTAccountStreamEvent] in
            var events: [ChatGPTAccountStreamEvent] = []
            for try await event in stream { events.append(event) }
            return events
        }
        let queued = await eventually {
            let messages = try? await socket.sentMessages()
            return messages?.contains(where: { $0.type == .queueTurn }) == true
        }
        #expect(queued)
        let sentMessages = try await socket.sentMessages()
        let queueFrame = try #require(sentMessages.last(where: { $0.type == .queueTurn }))
        let localRequestID = try #require(queueFrame.localRequestID)

        await socket.push(try serverFrame(.pendingTurnClaimed, [
            "local_request_id": localRequestID.uuidString,
            "turn_id": "chatgpt-turn-1",
        ]))
        await socket.push(try serverFrame(.pendingTurnFinished, [
            "local_request_id": localRequestID.uuidString,
            "turn_id": "chatgpt-turn-1",
            "response_text": "√5092 ≈ 71.358",
            "terminal_status": "completed",
        ]))

        await socket.push(try serverFrame(.accountTurnBegan, [
            "turn_id": "visible-turn",
            "message": "Explain π × 2",
            "model_label": "5.6 Sol",
        ]))
        await socket.push(try serverFrame(.accountTurnStatus, [
            "turn_id": "visible-turn",
            "status": "talking",
        ]))
        await socket.push(try serverFrame(.accountTurnFinished, [
            "turn_id": "visible-turn",
            "response_text": "π × 2 = 2π",
            "terminal_status": "completed",
        ]))

        #expect(try await collector.value == [
            .conversationStarted(id: "chatgpt-turn-1"),
            .textDelta("√5092 ≈ 71.358"),
            .completed,
        ])
        let sinkReceivedAllFrames = await eventually { await sink.events().count == 3 }
        #expect(sinkReceivedAllFrames)
        #expect(await sink.events() == [
            .began(
                turnID: "visible-turn",
                message: "Explain π × 2",
                modelLabel: "5.6 Sol",
                agentLabel: nil
            ),
            .status(turnID: "visible-turn", status: .talking),
            .finished(
                turnID: "visible-turn",
                responseText: "π × 2 = 2π",
                status: .completed
            ),
        ])
        await client.stop()
    }

    @Test
    func cancellationSendsCancelAndSocketFailureEndsEveryPendingStream() async throws {
        let socket = FakeChatGPTAccountRelaySocket()
        let client = ChatGPTAccountRelayClient(
            url: URL(string: "ws://127.0.0.1:3031/companion")!,
            socket: socket
        )
        try await client.start(with: pairingOffer())
        await socket.push(try serverFrame(.registrationAccepted, [
            "companion_device_id": "mac-1",
        ]))
        await socket.push(try serverFrame(.paired, [
            "bridge_session_id": UUID().uuidString,
            "expires_at": "2026-07-14T20:10:00Z",
        ]))
        #expect(await eventually {
            if case .paired = await client.state() { return true }
            return false
        })

        let cancelledStream = client.queue(ChatGPTAccountChatRequest(prompt: "Cancel me"))
        let cancelledCollector = Task {
            do { for try await _ in cancelledStream {} } catch {}
        }
        #expect(await eventually {
            let messages = try? await socket.sentMessages()
            return messages?.contains(where: { $0.type == .queueTurn }) == true
        })
        cancelledCollector.cancel()
        _ = await cancelledCollector.result
        #expect(await eventually {
            let messages = try? await socket.sentMessages()
            return messages?.contains(where: { $0.type == .cancelTurn }) == true
        })

        let first = relayFailure(from: client.queue(.init(prompt: "first")))
        let second = relayFailure(from: client.queue(.init(prompt: "second")))
        #expect(await eventually {
            let messages = try? await socket.sentMessages()
            return messages?.filter { $0.type == .queueTurn }.count == 3
        })
        await socket.fail(ChatGPTAccountRelayError.remote(code: "socket", message: "closed"))
        #expect(await first.value == .relayUnavailable)
        #expect(await second.value == .relayUnavailable)
        await client.stop()
    }
}

private actor FakeChatGPTAccountRelaySocket: ChatGPTAccountRelaySocket {
    private var sent: [Data] = []
    private var buffered: [Result<Data, Error>] = []
    private var waiters: [CheckedContinuation<Data, Error>] = []

    func connect(to url: URL) async throws {}
    func send(_ data: Data) async throws { sent.append(data) }

    func receive() async throws -> Data {
        if !buffered.isEmpty { return try buffered.removeFirst().get() }
        return try await withCheckedThrowingContinuation { waiters.append($0) }
    }

    func close() async {
        let current = waiters
        waiters.removeAll()
        current.forEach {
            $0.resume(throwing: ChatGPTAccountRelayError.relayUnavailable)
        }
    }

    func push(_ data: Data) {
        if waiters.isEmpty { buffered.append(.success(data)) }
        else { waiters.removeFirst().resume(returning: data) }
    }

    func fail(_ error: Error) {
        if waiters.isEmpty { buffered.append(.failure(error)) }
        else { waiters.removeFirst().resume(throwing: error) }
    }

    func sentMessages() throws -> [ChatGPTAccountRelayWireMessage] {
        try sent.map { try ChatGPTAccountRelayCodec().decode($0) }
    }
}

private actor RecordingChatGPTAccountTurnSink: ChatGPTAccountTurnSink {
    private var recorded: [ChatGPTAccountMirroredTurnEvent] = []

    func receive(_ event: ChatGPTAccountMirroredTurnEvent) async { recorded.append(event) }
    func events() -> [ChatGPTAccountMirroredTurnEvent] { recorded }
}

private func pairingOffer() -> ChatGPTAccountPairingOffer {
    .init(
        pairingCode: "ABCD2345",
        companionDeviceID: "mac-1",
        clientLabel: "MacBook",
        expiresAt: Date(timeIntervalSince1970: 1_752_521_400)
    )
}

private func serverFrame(
    _ type: ChatGPTAccountRelayMessageType,
    _ fields: [String: Any]
) throws -> Data {
    var object = fields
    object["protocol_version"] = 1
    object["message_id"] = UUID().uuidString
    object["type"] = type.rawValue
    return try JSONSerialization.data(withJSONObject: object)
}

private func eventually(
    _ predicate: @escaping @Sendable () async -> Bool
) async -> Bool {
    for _ in 0 ..< 100 {
        if await predicate() { return true }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return false
}

private func relayFailure(
    from stream: AsyncThrowingStream<ChatGPTAccountStreamEvent, Error>
) -> Task<ChatGPTAccountRelayError?, Never> {
    Task {
        do {
            for try await _ in stream {}
            return nil
        } catch let error as ChatGPTAccountRelayError {
            return error
        } catch {
            return nil
        }
    }
}
