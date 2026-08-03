import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexRolloutTaskEventClientTests {
    @Test
    func projectsVisibleAssistantMessagesWithoutInternalContext() throws {
        let visible = Data(
            #"{"timestamp":"2026-08-02T12:00:00Z","type":"response_item","payload":{"type":"message","id":"message-1","role":"assistant","content":[{"type":"output_text","text":"Visible response"}],"phase":"commentary","internal_chat_message_metadata_passthrough":{"turn_id":"turn-1"}}}"#.utf8
        )
        let internalContext = Data(
            #"{"timestamp":"2026-08-02T12:00:01Z","type":"response_item","payload":{"type":"message","id":"message-private","role":"assistant","content":[{"type":"output_text","text":"<codex_internal_context>PRIVATE</codex_internal_context>"}],"phase":"commentary"}}"#.utf8
        )

        let event = try #require(
            CodexRolloutTaskEventParser.event(
                from: visible,
                threadID: "thread-1",
                fallbackID: "offset-1"
            )
        )

        #expect(event.kind == .assistantDelta)
        #expect(event.threadID == "thread-1")
        #expect(event.turnID == "turn-1")
        #expect(event.itemID == "message-1")
        #expect(event.text == "Visible response")
        #expect(
            CodexRolloutTaskEventParser.event(
                from: internalContext,
                threadID: "thread-1",
                fallbackID: "offset-private"
            ) == nil
        )
    }

    @Test
    func projectsOnlyVisibleReasoningSummaries() throws {
        let visibleSummary = Data(
            #"{"timestamp":"2026-08-02T12:00:00Z","type":"event_msg","payload":{"type":"agent_reasoning","text":"**Checking the live stream**"}}"#.utf8
        )
        let privateReasoning = Data(
            #"{"timestamp":"2026-08-02T12:00:01Z","type":"response_item","payload":{"type":"reasoning","id":"reasoning-1","summary":[],"encrypted_content":"PRIVATE_CHAIN_OF_THOUGHT"}}"#.utf8
        )

        let event = try #require(
            CodexRolloutTaskEventParser.event(
                from: visibleSummary,
                threadID: "thread-1",
                fallbackID: "offset-2"
            )
        )

        #expect(event.kind == .reasoningSummaryDelta)
        #expect(event.itemID == "offset-2")
        #expect(event.text == "Checking the live stream")
        #expect(
            CodexRolloutTaskEventParser.event(
                from: privateReasoning,
                threadID: "thread-1",
                fallbackID: "offset-private"
            ) == nil
        )
    }

    @Test
    func projectsTaskLifecycleImmediately() throws {
        let started = Data(
            #"{"timestamp":"2026-08-02T12:00:00Z","type":"event_msg","payload":{"type":"task_started","turn_id":"turn-1"}}"#.utf8
        )
        let completed = Data(
            #"{"timestamp":"2026-08-02T12:01:00Z","type":"event_msg","payload":{"type":"task_complete","turn_id":"turn-1","last_agent_message":"PRIVATE DUPLICATE"}}"#.utf8
        )

        let startedEvent = try #require(
            CodexRolloutTaskEventParser.event(
                from: started,
                threadID: "thread-1",
                fallbackID: "offset-3"
            )
        )
        let completedEvent = try #require(
            CodexRolloutTaskEventParser.event(
                from: completed,
                threadID: "thread-1",
                fallbackID: "offset-4"
            )
        )

        #expect(startedEvent.kind == .turnStarted)
        #expect(startedEvent.turnID == "turn-1")
        #expect(startedEvent.taskStatus == .running)
        #expect(completedEvent.kind == .turnCompleted)
        #expect(completedEvent.taskStatus == .completed)
        #expect(completedEvent.text == nil)
    }

    @Test
    func rolloutTailBufferPreservesSplitLinesAndDropsOversizedRecords() {
        var buffer = CodexRolloutTailBuffer(maximumLineBytes: 12)

        #expect(buffer.append(Data("first".utf8), startingAt: 0).isEmpty)
        #expect(buffer.append(Data(" line\nsecond\n".utf8), startingAt: 5).map { String(decoding: $0.data, as: UTF8.self) }
            == ["first line", "second"])

        #expect(buffer.append(Data("this record is too long".utf8), startingAt: 18).isEmpty)
        #expect(buffer.append(Data(" and ends\nvalid\n".utf8), startingAt: 41).map { String(decoding: $0.data, as: UTF8.self) }
            == ["valid"])
    }

    @Test
    func tailsOnlyRecordsAppendedAfterSubscription() async throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? FileManager.default.removeItem(at: directory) }
        let rolloutURL = directory.appendingPathComponent("rollout.jsonl")
        try Data("old history\n".utf8).write(to: rolloutURL)

        let collector = RolloutTaskEventCollector()
        let client = CodexRolloutTaskEventClient(
            rolloutURL: rolloutURL,
            hasActiveTurn: true
        )
        let stream = try client.subscribe(
            threadID: "thread-1",
            onEvent: { event in
                Task { await collector.append(event) }
            },
            onTermination: { _ in }
        )
        defer { stream.cancel() }

        let appended = Data(
            (#"{"timestamp":"2026-08-02T12:00:00Z","type":"response_item","payload":{"type":"message","id":"message-live","role":"assistant","content":[{"type":"output_text","text":"Live now"}],"phase":"commentary","internal_chat_message_metadata_passthrough":{"turn_id":"turn-live"}}}"# + "\n").utf8
        )
        let writer = try FileHandle(forWritingTo: rolloutURL)
        defer { try? writer.close() }
        try writer.seekToEnd()
        try writer.write(contentsOf: appended)
        try writer.synchronize()

        let events = try await collector.waitForCount(2)
        #expect(events.map(\.kind) == [.turnStarted, .assistantDelta])
        #expect(events.last?.itemID == "message-live")
        #expect(events.last?.text == "Live now")
        #expect(!events.contains { $0.text == "old history" })
    }
}

private actor RolloutTaskEventCollector {
    private var events: [CodexTaskStreamEvent] = []

    func append(_ event: CodexTaskStreamEvent) {
        events.append(event)
    }

    func waitForCount(_ count: Int) async throws -> [CodexTaskStreamEvent] {
        for _ in 0..<100 {
            if events.count >= count { return events }
            try await Task.sleep(for: .milliseconds(20))
        }
        return events
    }
}
