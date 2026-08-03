import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexTaskStreamBrokerTests {
    @Test
    func replacingADeviceStreamCancelsThePreviousThread() async throws {
        let factory = FakeTaskEventClientFactory()
        let broker = CodexTaskStreamBroker(
            clientFactory: factory.makeClient,
            clock: ImmediateTaskStreamClock()
        )

        _ = try await broker.subscribe(
            deviceID: "phone-1",
            threadID: "thread-old",
            subscriptionID: UUID(),
            onEvent: { _ in }
        )
        let oldStream = try #require(factory.latestStream(threadID: "thread-old"))

        _ = try await broker.subscribe(
            deviceID: "phone-1",
            threadID: "thread-new",
            subscriptionID: UUID(),
            onEvent: { _ in }
        )

        #expect(oldStream.isCancelled)
        #expect(factory.subscriptionCount(threadID: "thread-new") == 1)
    }

    @Test
    func textDeltasCoalesceAndSequenceFromOne() async throws {
        let factory = FakeTaskEventClientFactory()
        let clock = ManualTaskStreamClock()
        let collector = TaskStreamEventCollector()
        let broker = CodexTaskStreamBroker(
            clientFactory: factory.makeClient,
            clock: clock
        )
        let subscriptionID = UUID()

        let streamID = try await broker.subscribe(
            deviceID: "phone-1",
            threadID: "thread-1",
            subscriptionID: subscriptionID,
            onEvent: { event in await collector.append(event) }
        )
        let stream = try #require(factory.latestStream(threadID: "thread-1"))

        stream.emit(.init(
            threadID: "thread-1",
            turnID: "turn-1",
            itemID: "message-1",
            kind: .assistantDelta,
            text: "Hel"
        ))
        stream.emit(.init(
            threadID: "thread-1",
            turnID: "turn-1",
            itemID: "message-1",
            kind: .assistantDelta,
            text: "lo"
        ))

        await clock.waitForSleeper()
        #expect(await collector.events.isEmpty)
        await clock.advance()
        await collector.waitForCount(1)

        let event = try #require(await collector.events.first)
        #expect(streamID == subscriptionID)
        #expect(event.streamID == subscriptionID)
        #expect(event.sequence == 1)
        #expect(event.text == "Hello")
    }

    @Test
    func lifecycleBoundaryFlushesPendingTextBeforeItself() async throws {
        let factory = FakeTaskEventClientFactory()
        let clock = ManualTaskStreamClock()
        let collector = TaskStreamEventCollector()
        let broker = CodexTaskStreamBroker(
            clientFactory: factory.makeClient,
            clock: clock
        )

        _ = try await broker.subscribe(
            deviceID: "phone-1",
            threadID: "thread-1",
            subscriptionID: UUID(),
            onEvent: { event in await collector.append(event) }
        )
        let stream = try #require(factory.latestStream(threadID: "thread-1"))
        stream.emit(.init(
            threadID: "thread-1",
            turnID: "turn-1",
            itemID: "message-1",
            kind: .assistantDelta,
            text: "Done"
        ))
        stream.emit(.init(
            threadID: "thread-1",
            turnID: "turn-1",
            itemID: "tool-1",
            kind: .itemStarted,
            item: CompanionBridgeTimelineItem(
                id: "tool-1",
                kind: .tool,
                status: .inProgress,
                title: "Read files",
                turnID: "turn-1"
            )
        ))

        await collector.waitForCount(2)
        let events = await collector.events
        #expect(events.map(\.sequence) == [1, 2])
        #expect(events.map(\.kind) == [.assistantDelta, .itemStarted])
        #expect(events.first?.text == "Done")
    }
}

private final class FakeTaskEventClientFactory: @unchecked Sendable {
    private let lock = NSLock()
    private var streamsByThread: [String: [FakeTaskEventStream]] = [:]

    func makeClient(_ threadID: String) throws -> any CodexTaskEventClient {
        FakeTaskEventClient(factory: self)
    }

    func add(_ stream: FakeTaskEventStream, threadID: String) {
        lock.withLock { streamsByThread[threadID, default: []].append(stream) }
    }

    func latestStream(threadID: String) -> FakeTaskEventStream? {
        lock.withLock { streamsByThread[threadID]?.last }
    }

    func subscriptionCount(threadID: String) -> Int {
        lock.withLock { streamsByThread[threadID]?.count ?? 0 }
    }
}

private struct FakeTaskEventClient: CodexTaskEventClient {
    var factory: FakeTaskEventClientFactory

    func subscribe(
        threadID: String,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) throws -> any CodexTaskEventStream {
        let stream = FakeTaskEventStream(
            onEvent: onEvent,
            onTermination: onTermination
        )
        factory.add(stream, threadID: threadID)
        return stream
    }
}

private final class FakeTaskEventStream: CodexTaskEventStream, @unchecked Sendable {
    private let lock = NSLock()
    private let onEvent: @Sendable (CodexTaskStreamEvent) -> Void
    private let onTermination: @Sendable (String?) -> Void
    private var cancelled = false

    init(
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) {
        self.onEvent = onEvent
        self.onTermination = onTermination
    }

    var isCancelled: Bool { lock.withLock { cancelled } }

    func emit(_ event: CodexTaskStreamEvent) {
        guard !isCancelled else { return }
        onEvent(event)
    }

    func terminate(_ reason: String?) {
        guard !isCancelled else { return }
        onTermination(reason)
    }

    func cancel() {
        lock.withLock { cancelled = true }
    }
}

private actor TaskStreamEventCollector {
    private(set) var events: [CompanionBridgeLiveEvent] = []

    func append(_ event: CompanionBridgeLiveEvent) {
        events.append(event)
    }

    func waitForCount(_ count: Int) async {
        for _ in 0..<200 where events.count < count {
            await Task.yield()
            try? await Task.sleep(for: .milliseconds(1))
        }
    }
}

private actor ManualTaskStreamClock: CodexTaskStreamClock {
    private var sleepers: [CheckedContinuation<Void, Never>] = []

    func sleep(for duration: Duration) async throws {
        _ = duration
        await withCheckedContinuation { sleepers.append($0) }
    }

    func waitForSleeper() async {
        for _ in 0..<200 where sleepers.isEmpty {
            await Task.yield()
            try? await Task.sleep(for: .milliseconds(1))
        }
    }

    func advance() {
        let pending = sleepers
        sleepers.removeAll()
        pending.forEach { $0.resume() }
    }
}

private struct ImmediateTaskStreamClock: CodexTaskStreamClock {
    func sleep(for duration: Duration) async throws {
        _ = duration
    }
}
