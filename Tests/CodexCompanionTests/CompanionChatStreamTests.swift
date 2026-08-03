import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionChatStreamTests {
    @Test
    func cumulativeSnapshotsBecomeSuffixDeltas() {
        var accumulator = CompanionCumulativeTextAccumulator()

        #expect(accumulator.delta(for: "Hel") == "Hel")
        #expect(accumulator.delta(for: "Hello") == "lo")
        #expect(accumulator.delta(for: "Hello") == "")
        #expect(accumulator.text == "Hello")
    }

    @Test
    func finalOnlyOnDeviceAdapterStillUsesTheStreamContract() async throws {
        let service = FinalOnlyOnDeviceChatService()
        var events: [CompanionChatStreamEvent] = []

        for try await event in service.stream(prompt: "Hello", attachments: []) {
            events.append(event)
        }

        #expect(events == [
            .started,
            .assistantDelta("Final response"),
            .completed(.init(text: "Final response")),
        ])
    }

    @Test
    func finalOnlyOpenAIAdapterPreservesUsageInCompletion() async throws {
        let service = FinalOnlyOpenAIChatService()
        var events: [CompanionChatStreamEvent] = []

        for try await event in service.stream(
            prompt: "Hello",
            model: .gpt56Luna,
            apiKey: "test-key"
        ) {
            events.append(event)
        }

        #expect(events == [
            .started,
            .assistantDelta("OpenAI final"),
            .completed(.init(text: "OpenAI final", inputTokens: 7, outputTokens: 3)),
        ])
    }

    @Test
    func finalOnlyLumoAdapterPreservesUsageInCompletion() async throws {
        let service = FinalOnlyLumoChatService()
        var events: [CompanionChatStreamEvent] = []

        for try await event in service.stream(
            prompt: "Hello",
            model: .thinking,
            apiKey: "test-key"
        ) {
            events.append(event)
        }

        #expect(events == [
            .started,
            .assistantDelta("Lumo final"),
            .completed(.init(text: "Lumo final", inputTokens: 5, outputTokens: 2)),
        ])
    }
}

private struct FinalOnlyOnDeviceChatService: OnDeviceChatServing {
    func prewarm() async {}

    func send(prompt: String) async throws -> String {
        "Final response"
    }
}

private struct FinalOnlyOpenAIChatService: OpenAIChatServing {
    func send(prompt: String, model: ChatGPTModel, apiKey: String) async throws -> OpenAIChatResult {
        OpenAIChatResult(text: "OpenAI final", inputTokens: 7, outputTokens: 3)
    }
}

private struct FinalOnlyLumoChatService: LumoChatServing {
    func send(prompt: String, model: LumoModel, apiKey: String) async throws -> LumoChatResult {
        LumoChatResult(text: "Lumo final", inputTokens: 5, outputTokens: 2)
    }
}
