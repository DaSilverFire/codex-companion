import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTAccountRelayTransportTests {
    @Test
    func exposesConservativeCapabilitiesAndForwardsOneCompleteTextEvent() async throws {
        let recorder = RelayRequestRecorder()
        let request = ChatGPTAccountChatRequest(
            prompt: "What is √5092?"
        )
        let transport = ChatGPTAccountRelayTransport(client: StubRelayStreaming(
            events: [
                .conversationStarted(id: "turn-1"),
                .textDelta("√5092 ≈ 71.358"),
                .completed,
            ],
            recorder: recorder
        ))
        let expectedCapabilities = ChatGPTAccountCapabilities(
            transportID: "chatgpt-apps-sdk-relay-v1",
            transportName: "ChatGPT account relay",
            supportsStreaming: false,
            supportsConversationContinuation: false,
            models: [],
            agents: []
        )
        #expect(transport.capabilities == expectedCapabilities)

        let service = ChatGPTAccountConnectionService(bridge: transport)
        #expect(service.availability() == .available(expectedCapabilities))
        var events: [ChatGPTAccountStreamEvent] = []
        for try await event in service.stream(request) { events.append(event) }

        #expect(events == [
            .conversationStarted(id: "turn-1"),
            .textDelta("√5092 ≈ 71.358"),
            .completed,
        ])
        #expect(events.compactMap {
            if case .textDelta(let text) = $0 { return text }
            return nil
        } == ["√5092 ≈ 71.358"])
        #expect(recorder.requests == [request])
    }

    @Test
    func rejectsUnprovedContinuationModelAndAgentSelection() async {
        let transport = ChatGPTAccountRelayTransport(client: StubRelayStreaming(
            events: [],
            recorder: RelayRequestRecorder()
        ))
        let request = ChatGPTAccountChatRequest(
            conversationID: "existing-chat",
            prompt: "continue with a selected agent",
            modelID: "subscription-model",
            agentID: "subscription-agent"
        )
        do {
            for try await _ in transport.stream(request) {
                Issue.record("Unsupported selections must not produce relay events.")
            }
            Issue.record("Unsupported selections must finish by throwing.")
        } catch let error as ChatGPTAccountRelayError {
            #expect(error == .unsupportedCapability)
        } catch {
            Issue.record("Expected ChatGPTAccountRelayError, got \(error).")
        }
    }
}

private struct StubRelayStreaming: ChatGPTAccountRelayStreaming {
    var events: [ChatGPTAccountStreamEvent]
    var recorder: RelayRequestRecorder

    func queue(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error> {
        recorder.record(request)
        return AsyncThrowingStream { continuation in
            events.forEach { continuation.yield($0) }
            continuation.finish()
        }
    }
}

private final class RelayRequestRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recorded: [ChatGPTAccountChatRequest] = []

    var requests: [ChatGPTAccountChatRequest] { lock.withLock { recorded } }

    func record(_ request: ChatGPTAccountChatRequest) {
        lock.withLock { recorded.append(request) }
    }
}
