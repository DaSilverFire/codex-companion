import Foundation

struct CodexTaskStreamEvent: Equatable, Sendable {
    var threadID: String
    var turnID: String?
    var itemID: String?
    var kind: CompanionBridgeLiveEventKind
    var text: String?
    var item: CompanionBridgeTimelineItem?
    var taskStatus: CompanionBridgeTaskStatus?
    var errorCode: String?

    init(
        threadID: String,
        turnID: String? = nil,
        itemID: String? = nil,
        kind: CompanionBridgeLiveEventKind,
        text: String? = nil,
        item: CompanionBridgeTimelineItem? = nil,
        taskStatus: CompanionBridgeTaskStatus? = nil,
        errorCode: String? = nil
    ) {
        self.threadID = threadID
        self.turnID = turnID
        self.itemID = itemID
        self.kind = kind
        self.text = text
        self.item = item
        self.taskStatus = taskStatus
        self.errorCode = errorCode
    }
}

protocol CodexTaskEventStream: Sendable {
    func cancel()
}

protocol CodexTaskEventClient: Sendable {
    func subscribe(
        threadID: String,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) throws -> any CodexTaskEventStream
}

protocol CodexTaskStreamClock: Sendable {
    func sleep(for duration: Duration) async throws
}

protocol CodexTaskStreamBrokerServing: Sendable {
    @discardableResult
    func subscribe(
        deviceID: String,
        threadID: String,
        subscriptionID: UUID,
        onEvent: @escaping @Sendable (CompanionBridgeLiveEvent) async -> Void
    ) async throws -> UUID

    func unsubscribe(deviceID: String, subscriptionID: UUID?) async
    func stop() async
}

private struct ContinuousCodexTaskStreamClock: CodexTaskStreamClock {
    func sleep(for duration: Duration) async throws {
        try await Task.sleep(for: duration)
    }
}

actor CodexTaskStreamBroker: CodexTaskStreamBrokerServing {
    typealias ClientFactory = @Sendable (String) throws -> any CodexTaskEventClient
    typealias EventHandler = @Sendable (CompanionBridgeLiveEvent) async -> Void

    private struct PendingDelta: Sendable {
        var event: CodexTaskStreamEvent

        func canCoalesce(with other: CodexTaskStreamEvent) -> Bool {
            event.threadID == other.threadID
                && event.turnID == other.turnID
                && event.itemID == other.itemID
                && event.kind == other.kind
        }
    }

    private enum IngressMessage: Sendable {
        case event(CodexTaskStreamEvent)
        case terminated(String?)
    }

    private struct Subscription: Sendable {
        var id: UUID
        var threadID: String
        var stream: any CodexTaskEventStream
        var onEvent: EventHandler
        var nextSequence: UInt64 = 1
        var pendingDelta: PendingDelta?
        var flushTask: Task<Void, Never>?
        var ingressContinuation: AsyncStream<IngressMessage>.Continuation
        var ingressTask: Task<Void, Never>?
    }

    private let clientFactory: ClientFactory
    private let clock: any CodexTaskStreamClock
    private var subscriptionsByDevice: [String: Subscription] = [:]

    init(
        clientFactory: @escaping ClientFactory,
        clock: any CodexTaskStreamClock = ContinuousCodexTaskStreamClock()
    ) {
        self.clientFactory = clientFactory
        self.clock = clock
    }

    @discardableResult
    func subscribe(
        deviceID: String,
        threadID: String,
        subscriptionID: UUID,
        onEvent: @escaping EventHandler
    ) async throws -> UUID {
        cancelSubscription(for: deviceID)

        let client = try clientFactory(threadID)
        let ingress = AsyncStream.makeStream(of: IngressMessage.self)
        let stream = try client.subscribe(
            threadID: threadID,
            onEvent: { event in
                ingress.continuation.yield(.event(event))
            },
            onTermination: { reason in
                ingress.continuation.yield(.terminated(reason))
                ingress.continuation.finish()
            }
        )
        subscriptionsByDevice[deviceID] = Subscription(
            id: subscriptionID,
            threadID: threadID,
            stream: stream,
            onEvent: onEvent,
            ingressContinuation: ingress.continuation
        )
        let ingressTask = Task { [weak self] in
            for await message in ingress.stream {
                guard !Task.isCancelled else { break }
                switch message {
                case .event(let event):
                    await self?.ingest(
                        event,
                        deviceID: deviceID,
                        subscriptionID: subscriptionID
                    )
                case .terminated(let reason):
                    await self?.streamTerminated(
                        deviceID: deviceID,
                        subscriptionID: subscriptionID,
                        reason: reason
                    )
                    return
                }
            }
        }
        subscriptionsByDevice[deviceID]?.ingressTask = ingressTask
        return subscriptionID
    }

    func unsubscribe(deviceID: String, subscriptionID: UUID? = nil) async {
        guard let current = subscriptionsByDevice[deviceID] else { return }
        guard subscriptionID == nil || current.id == subscriptionID else { return }
        cancelSubscription(for: deviceID)
    }

    func stop() async {
        let deviceIDs = Array(subscriptionsByDevice.keys)
        for deviceID in deviceIDs {
            cancelSubscription(for: deviceID)
        }
    }

    private func ingest(
        _ event: CodexTaskStreamEvent,
        deviceID: String,
        subscriptionID: UUID
    ) async {
        guard var subscription = subscriptionsByDevice[deviceID],
              subscription.id == subscriptionID,
              subscription.threadID == event.threadID
        else { return }

        if Self.isTextDelta(event) {
            if var pending = subscription.pendingDelta,
               pending.canCoalesce(with: event)
            {
                pending.event.text = (pending.event.text ?? "") + (event.text ?? "")
                subscription.pendingDelta = pending
                subscriptionsByDevice[deviceID] = subscription
                return
            }

            if subscription.pendingDelta != nil {
                subscriptionsByDevice[deviceID] = subscription
                await flushPending(deviceID: deviceID, subscriptionID: subscriptionID)
                guard let refreshed = subscriptionsByDevice[deviceID],
                      refreshed.id == subscriptionID
                else { return }
                subscription = refreshed
            }

            subscription.pendingDelta = PendingDelta(event: event)
            subscription.flushTask?.cancel()
            let clock = self.clock
            subscription.flushTask = Task { [weak self] in
                do {
                    try await clock.sleep(for: .milliseconds(75))
                    try Task.checkCancellation()
                } catch {
                    return
                }
                await self?.flushPending(
                    deviceID: deviceID,
                    subscriptionID: subscriptionID
                )
            }
            subscriptionsByDevice[deviceID] = subscription
            return
        }

        if subscription.pendingDelta != nil {
            subscriptionsByDevice[deviceID] = subscription
            await flushPending(deviceID: deviceID, subscriptionID: subscriptionID)
        }
        await emit(event, deviceID: deviceID, subscriptionID: subscriptionID)
    }

    private func streamTerminated(
        deviceID: String,
        subscriptionID: UUID,
        reason: String?
    ) async {
        guard let subscription = subscriptionsByDevice[deviceID],
              subscription.id == subscriptionID
        else { return }

        await flushPending(deviceID: deviceID, subscriptionID: subscriptionID)
        let safeReason = reason?.trimmingCharacters(in: .whitespacesAndNewlines)
        await emit(
            CodexTaskStreamEvent(
                threadID: subscription.threadID,
                kind: .failed,
                text: safeReason?.isEmpty == false ? safeReason : nil,
                taskStatus: .failed,
                errorCode: "stream_ended"
            ),
            deviceID: deviceID,
            subscriptionID: subscriptionID
        )
        cancelSubscription(for: deviceID)
    }

    private func flushPending(deviceID: String, subscriptionID: UUID) async {
        guard var subscription = subscriptionsByDevice[deviceID],
              subscription.id == subscriptionID,
              let pending = subscription.pendingDelta
        else { return }

        subscription.flushTask?.cancel()
        subscription.flushTask = nil
        subscription.pendingDelta = nil
        subscriptionsByDevice[deviceID] = subscription
        await emit(pending.event, deviceID: deviceID, subscriptionID: subscriptionID)
    }

    private func emit(
        _ event: CodexTaskStreamEvent,
        deviceID: String,
        subscriptionID: UUID
    ) async {
        guard var subscription = subscriptionsByDevice[deviceID],
              subscription.id == subscriptionID
        else { return }

        let liveEvent = CompanionBridgeLiveEvent(
            channel: .task,
            streamID: subscriptionID,
            sequence: subscription.nextSequence,
            threadID: event.threadID,
            turnID: event.turnID,
            itemID: event.itemID,
            kind: event.kind,
            text: event.text,
            item: event.item,
            taskStatus: event.taskStatus,
            errorCode: event.errorCode
        )
        subscription.nextSequence += 1
        subscriptionsByDevice[deviceID] = subscription
        await subscription.onEvent(liveEvent)
    }

    private func cancelSubscription(for deviceID: String) {
        guard let subscription = subscriptionsByDevice.removeValue(forKey: deviceID) else {
            return
        }
        subscription.flushTask?.cancel()
        subscription.ingressContinuation.finish()
        subscription.ingressTask?.cancel()
        subscription.stream.cancel()
    }

    private static func isTextDelta(_ event: CodexTaskStreamEvent) -> Bool {
        event.kind == .assistantDelta || event.kind == .reasoningSummaryDelta
    }
}
