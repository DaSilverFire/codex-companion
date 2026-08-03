import Foundation
import MultipeerConnectivity
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeStreamTests {
    @Test
    func nearbyRequestContextUsesTheAuthorizedDeviceID() {
        let broker = BridgeStreamBrokerSpy()
        let server = CodexCompanionMobileBridgeServer(taskStreamBroker: broker)
        let peer = MCPeerID(displayName: "Test iPhone")
        server.authorize(peer, deviceID: "phone-nearby")

        let context = server.requestContext(for: peer)

        #expect(context.deviceID == "phone-nearby")
        #expect(context.nearbyPeer?.displayName == peer.displayName)
        #expect(context.relayGeneration == nil)
    }

    @Test
    func subscribeRejectsMissingTrustedDeviceContext() async {
        let broker = BridgeStreamBrokerSpy()
        let server = CodexCompanionMobileBridgeServer(taskStreamBroker: broker)
        let request = CompanionBridgeRequest(
            operation: .subscribeTaskStream,
            threadID: "thread-1",
            subscriptionID: UUID()
        )

        let response = await server.handle(request)

        #expect(!response.succeeded)
        #expect(response.errorCode == "unauthorized_device")
        #expect(await broker.subscribeCalls.isEmpty)
    }

    @Test
    func relayContextKeepsItsEndpointDeviceAndGeneration() async throws {
        let broker = BridgeStreamBrokerSpy()
        let transport = BridgeLiveEventTransportSpy(nearbyResult: false)
        let server = CodexCompanionMobileBridgeServer(
            taskStreamBroker: broker,
            nearbyLiveEventSender: transport.sendNearby,
            relayLiveEventSender: transport.sendRelay
        )
        let generation = UUID()
        let subscriptionID = UUID()
        let request = CompanionBridgeRequest(
            operation: .subscribeTaskStream,
            threadID: "thread-relay",
            subscriptionID: subscriptionID
        )

        let response = await server.handle(
            request,
            context: CompanionBridgeRequestContext(
                deviceID: "phone-relay",
                relayGeneration: generation
            )
        )
        try await broker.emit(
            CompanionBridgeLiveEvent(
                channel: .task,
                streamID: subscriptionID,
                sequence: 1,
                threadID: "thread-relay",
                kind: .turnStarted,
                taskStatus: .running
            ),
            deviceID: "phone-relay"
        )

        #expect(response.succeeded)
        #expect(response.subscriptionID == subscriptionID)
        #expect(await broker.subscribeCalls.map(\.deviceID) == ["phone-relay"])
        let relayCall = try #require(transport.relayCalls.first)
        #expect(relayCall.deviceID == "phone-relay")
        #expect(relayCall.generation == generation)
    }

    @Test
    func canonicalSubscriptionUsesPhysicalStreamAndEmitsCanonicalEvents() async throws {
        let defaultsName = "CodexCompanionMobileBridgeStreamLineageTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let lineages = CodexThreadLineageStore(defaults: defaults)
        #expect(lineages.registerFork(
            sourceThreadID: "canonical-thread",
            destinationThreadID: "physical-fork"
        ) == "canonical-thread")
        let broker = BridgeStreamBrokerSpy()
        let transport = BridgeLiveEventTransportSpy(nearbyResult: true)
        let server = CodexCompanionMobileBridgeServer(
            archive: CodexMobileTaskArchive(lineageStore: lineages),
            taskStreamBroker: broker,
            nearbyLiveEventSender: transport.sendNearby
        )
        let subscriptionID = UUID()

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .subscribeTaskStream,
                threadID: "canonical-thread",
                subscriptionID: subscriptionID
            ),
            context: CompanionBridgeRequestContext(deviceID: "phone-lineage")
        )
        try await broker.emit(
            CompanionBridgeLiveEvent(
                channel: .task,
                streamID: subscriptionID,
                sequence: 1,
                threadID: "physical-fork",
                kind: .turnStarted
            ),
            deviceID: "phone-lineage"
        )

        #expect(response.threadID == "canonical-thread")
        #expect(await broker.subscribeCalls.map(\.threadID) == ["physical-fork"])
        #expect(transport.nearbyCalls.first?.event.liveEvent.threadID == "canonical-thread")
    }

    @Test
    func replacementUsesOneStableDeviceSlot() async {
        let broker = BridgeStreamBrokerSpy()
        let server = CodexCompanionMobileBridgeServer(taskStreamBroker: broker)
        let context = CompanionBridgeRequestContext(deviceID: "phone-1")

        _ = await server.handle(
            CompanionBridgeRequest(
                operation: .subscribeTaskStream,
                threadID: "thread-old",
                subscriptionID: UUID()
            ),
            context: context
        )
        _ = await server.handle(
            CompanionBridgeRequest(
                operation: .subscribeTaskStream,
                threadID: "thread-new",
                subscriptionID: UUID()
            ),
            context: context
        )

        let calls = await broker.subscribeCalls
        #expect(calls.map(\.deviceID) == ["phone-1", "phone-1"])
        #expect(calls.map(\.threadID) == ["thread-old", "thread-new"])
        #expect(await broker.activeThread(deviceID: "phone-1") == "thread-new")
    }

    @Test
    func eventDeliveryPrefersNearbyAndFallsBackToRelay() async throws {
        let broker = BridgeStreamBrokerSpy()
        let nearbyTransport = BridgeLiveEventTransportSpy(nearbyResult: true)
        let server = CodexCompanionMobileBridgeServer(
            taskStreamBroker: broker,
            nearbyLiveEventSender: nearbyTransport.sendNearby,
            relayLiveEventSender: nearbyTransport.sendRelay
        )
        let streamID = UUID()
        _ = await server.handle(
            CompanionBridgeRequest(
                operation: .subscribeTaskStream,
                threadID: "thread-1",
                subscriptionID: streamID
            ),
            context: CompanionBridgeRequestContext(
                deviceID: "phone-1",
                nearbyPeer: MCPeerID(displayName: "Phone"),
                relayGeneration: UUID()
            )
        )

        try await broker.emit(Self.event(streamID: streamID), deviceID: "phone-1")

        #expect(nearbyTransport.nearbyCalls.count == 1)
        #expect(nearbyTransport.relayCalls.isEmpty)

        nearbyTransport.nearbyResult = false
        try await broker.emit(Self.event(streamID: streamID), deviceID: "phone-1")

        #expect(nearbyTransport.nearbyCalls.count == 2)
        #expect(nearbyTransport.relayCalls.count == 1)
    }

    @Test
    func unsubscribeIsIdempotentAndStopCancelsAllStreams() async {
        let broker = BridgeStreamBrokerSpy()
        let server = CodexCompanionMobileBridgeServer(taskStreamBroker: broker)
        let streamID = UUID()
        let context = CompanionBridgeRequestContext(deviceID: "phone-1")

        _ = await server.handle(
            CompanionBridgeRequest(
                operation: .unsubscribeTaskStream,
                streamID: streamID
            ),
            context: context
        )
        _ = await server.handle(
            CompanionBridgeRequest(
                operation: .unsubscribeTaskStream,
                streamID: streamID
            ),
            context: context
        )
        server.stop()
        await broker.waitForStop()

        #expect(await broker.unsubscribeCalls.count == 2)
        #expect(await broker.stopCount == 1)
    }

    @Test
    func normalRequestsNeverCreateAnUnsolicitedEventRoute() async {
        let broker = BridgeStreamBrokerSpy()
        let transport = BridgeLiveEventTransportSpy(nearbyResult: true)
        let server = CodexCompanionMobileBridgeServer(
            taskStreamBroker: broker,
            nearbyLiveEventSender: transport.sendNearby,
            relayLiveEventSender: transport.sendRelay
        )

        _ = await server.handle(CompanionBridgeRequest(operation: .handshake))

        #expect(await broker.subscribeCalls.isEmpty)
        #expect(transport.nearbyCalls.isEmpty)
        #expect(transport.relayCalls.isEmpty)
    }

    private static func event(streamID: UUID) -> CompanionBridgeLiveEvent {
        CompanionBridgeLiveEvent(
            channel: .task,
            streamID: streamID,
            sequence: 1,
            threadID: "thread-1",
            kind: .assistantDelta,
            text: "Hi"
        )
    }
}

private actor BridgeStreamBrokerSpy: CodexTaskStreamBrokerServing {
    struct SubscribeCall: Equatable, Sendable {
        var deviceID: String
        var threadID: String
        var subscriptionID: UUID
    }

    struct UnsubscribeCall: Equatable, Sendable {
        var deviceID: String
        var subscriptionID: UUID?
    }

    private(set) var subscribeCalls: [SubscribeCall] = []
    private(set) var unsubscribeCalls: [UnsubscribeCall] = []
    private(set) var stopCount = 0
    private var handlers: [String: @Sendable (CompanionBridgeLiveEvent) async -> Void] = [:]
    private var activeThreads: [String: String] = [:]

    func subscribe(
        deviceID: String,
        threadID: String,
        subscriptionID: UUID,
        onEvent: @escaping @Sendable (CompanionBridgeLiveEvent) async -> Void
    ) async throws -> UUID {
        subscribeCalls.append(.init(
            deviceID: deviceID,
            threadID: threadID,
            subscriptionID: subscriptionID
        ))
        handlers[deviceID] = onEvent
        activeThreads[deviceID] = threadID
        return subscriptionID
    }

    func unsubscribe(deviceID: String, subscriptionID: UUID?) async {
        unsubscribeCalls.append(.init(deviceID: deviceID, subscriptionID: subscriptionID))
        handlers.removeValue(forKey: deviceID)
        activeThreads.removeValue(forKey: deviceID)
    }

    func stop() async {
        stopCount += 1
        handlers.removeAll()
        activeThreads.removeAll()
    }

    func activeThread(deviceID: String) -> String? {
        activeThreads[deviceID]
    }

    func emit(_ event: CompanionBridgeLiveEvent, deviceID: String) async throws {
        let handler = try #require(handlers[deviceID])
        await handler(event)
    }

    func waitForStop() async {
        for _ in 0..<200 where stopCount == 0 {
            await Task.yield()
            try? await Task.sleep(for: .milliseconds(1))
        }
    }
}

private final class BridgeLiveEventTransportSpy: @unchecked Sendable {
    struct Call: Sendable {
        var event: CompanionBridgeServerEvent
        var deviceID: String
        var generation: UUID?
    }

    private let lock = NSLock()
    private var recordedNearbyCalls: [Call] = []
    private var recordedRelayCalls: [Call] = []
    private var storedNearbyResult: Bool

    init(nearbyResult: Bool) {
        storedNearbyResult = nearbyResult
    }

    var nearbyResult: Bool {
        get { lock.withLock { storedNearbyResult } }
        set { lock.withLock { storedNearbyResult = newValue } }
    }

    var nearbyCalls: [Call] { lock.withLock { recordedNearbyCalls } }
    var relayCalls: [Call] { lock.withLock { recordedRelayCalls } }

    func sendNearby(_ event: CompanionBridgeServerEvent, deviceID: String) -> Bool {
        lock.withLock {
            recordedNearbyCalls.append(.init(
                event: event,
                deviceID: deviceID,
                generation: nil
            ))
            return storedNearbyResult
        }
    }

    func sendRelay(
        _ event: CompanionBridgeServerEvent,
        deviceID: String,
        generation: UUID?
    ) async -> Bool {
        lock.withLock {
            recordedRelayCalls.append(.init(
                event: event,
                deviceID: deviceID,
                generation: generation
            ))
        }
        return true
    }
}
