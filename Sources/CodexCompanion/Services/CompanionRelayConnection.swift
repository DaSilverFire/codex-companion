import Foundation
import Network

enum CompanionRelayKeepAliveLoop {
    static func run(
        intervalNanoseconds: UInt64,
        ping: @escaping @Sendable () async throws -> Void
    ) async throws {
        while true {
            try await Task.sleep(nanoseconds: intervalNanoseconds)
            try Task.checkCancellation()
            try await ping()
        }
    }
}

enum CompanionRelayConnectionError: LocalizedError {
    case notRegistered
    case peerUnavailable
    case packetResultTimedOut
    case invalidWireMessage
    case relayRejected(String)
    case transportFailed(String)

    var errorDescription: String? {
        switch self {
        case .notRegistered:
            "The encrypted relay is not connected."
        case .peerUnavailable:
            "The paired relay endpoint is not connected."
        case .packetResultTimedOut:
            "The relay did not confirm packet delivery in time."
        case .invalidWireMessage:
            "The relay returned an invalid message."
        case .relayRejected(let message):
            message
        case .transportFailed(let message):
            message
        }
    }
}

actor CompanionRelayConnection {
    enum State: Equatable, Sendable {
        case stopped
        case connecting
        case registered
        case waitingToReconnect
    }

    typealias StateHandler = @Sendable (State) -> Void
    typealias EnvelopeHandler = @Sendable (CompanionBridgeEncryptedEnvelope) -> Void
    typealias FailureHandler = @Sendable (String) -> Void
    typealias SendOperation = @Sendable (String) async throws -> Void
    typealias PingOperation = @Sendable () async throws -> Void

    private let url: URL
    private let channelID: String
    private let endpointID: String
    private let packetResultTimeoutNanoseconds: UInt64
    private let keepAliveIntervalNanoseconds: UInt64
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let stateHandler: StateHandler
    private let envelopeHandler: EnvelopeHandler
    private let failureHandler: FailureHandler
    private let networkQueue = DispatchQueue(label: "com.silverfire.codexcompanion.relay")

    private var state: State = .stopped
    private var shouldRun = false
    private var reconnectAttempt = 0
    private var connection: NWConnection?
    private var connectionGeneration = UUID()
    private var relayRegistrationAcknowledged = false
    private var modernSendOperation: SendOperation?
    private var modernPingOperation: PingOperation?
    private var runTask: Task<Void, Never>?
    private var keepAliveTask: Task<Void, Never>?
    private var reconnectTask: Task<Void, Never>?
    private var pendingPacketResults: [
        String: CheckedContinuation<Void, Error>
    ] = [:]
    private var packetResultTimeoutTasks: [String: Task<Void, Never>] = [:]

    init(
        url: URL,
        channelID: String,
        endpointID: String,
        packetResultTimeoutNanoseconds: UInt64 = 5_000_000_000,
        keepAliveIntervalNanoseconds: UInt64 = 20_000_000_000,
        stateHandler: @escaping StateHandler,
        envelopeHandler: @escaping EnvelopeHandler,
        failureHandler: @escaping FailureHandler = { _ in }
    ) {
        self.url = Self.routedURL(url, channelID: channelID)
        self.channelID = channelID
        self.endpointID = endpointID
        self.packetResultTimeoutNanoseconds = max(
            1,
            packetResultTimeoutNanoseconds
        )
        self.keepAliveIntervalNanoseconds = max(
            1,
            keepAliveIntervalNanoseconds
        )
        self.stateHandler = stateHandler
        self.envelopeHandler = envelopeHandler
        self.failureHandler = failureHandler
        encoder = JSONEncoder()
        decoder = JSONDecoder()
    }

    static func routedURL(_ url: URL, channelID: String) -> URL {
        guard var components = URLComponents(
            url: url,
            resolvingAgainstBaseURL: false
        ) else { return url }
        var queryItems = components.queryItems ?? []
        queryItems.removeAll { $0.name == "channel" }
        queryItems.append(URLQueryItem(name: "channel", value: channelID))
        components.queryItems = queryItems
        return components.url ?? url
    }

    func start() {
        guard !shouldRun else { return }
        shouldRun = true
        reconnectAttempt = 0
        connect()
    }

    func stop() {
        shouldRun = false
        relayRegistrationAcknowledged = false
        failPendingPacketResults(
            CompanionRelayConnectionError.transportFailed(
                "The encrypted relay connection stopped."
            )
        )
        reconnectTask?.cancel()
        reconnectTask = nil
        keepAliveTask?.cancel()
        keepAliveTask = nil
        runTask?.cancel()
        runTask = nil
        modernSendOperation = nil
        modernPingOperation = nil
        connectionGeneration = UUID()
        connection?.stateUpdateHandler = nil
        connection?.cancel()
        connection = nil
        publish(.stopped)
    }

    func send(_ envelope: CompanionBridgeEncryptedEnvelope) async throws {
        guard relayRegistrationAcknowledged else {
            throw CompanionRelayConnectionError.notRegistered
        }
        guard envelope.channelID == channelID,
              envelope.senderID == endpointID
        else {
            throw CompanionRelayWireError.metadataMismatch
        }
        let packetID = UUID().uuidString
        let wire = try CompanionRelayWireMessage.packet(
            envelope: envelope,
            packetID: packetID
        )
        try await sendAndAwaitPacketResult(wire, packetID: packetID)
    }

    func currentState() -> State {
        state
    }

    private func connect() {
        guard shouldRun else { return }
        reconnectTask?.cancel()
        reconnectTask = nil
        keepAliveTask?.cancel()
        keepAliveTask = nil
        runTask?.cancel()
        runTask = nil
        relayRegistrationAcknowledged = false
        modernSendOperation = nil
        modernPingOperation = nil
        connectionGeneration = UUID()
        connection?.stateUpdateHandler = nil
        connection?.cancel()
        publish(.connecting)

        let generation = connectionGeneration
        if #available(macOS 26.0, iOS 26.0, *) {
            startModernConnection(generation: generation)
            return
        }

        let connection = makeConnection()
        self.connection = connection
        connection.stateUpdateHandler = { [weak self] state in
            Task { [weak self] in
                await self?.handleConnectionState(state, generation: generation)
            }
        }
        connection.start(queue: networkQueue)
    }

    private func makeConnection() -> NWConnection {
        let webSocket = NWProtocolWebSocket.Options()
        webSocket.autoReplyPing = true
        webSocket.maximumMessageSize = 1_048_576

        let secure = url.scheme?.lowercased() == "wss"
        let tls = secure ? NWProtocolTLS.Options() : nil
        let parameters = NWParameters(tls: tls, tcp: NWProtocolTCP.Options())
        parameters.defaultProtocolStack.applicationProtocols.insert(webSocket, at: 0)
        return NWConnection(to: .url(url), using: parameters)
    }

    @available(macOS 26.0, iOS 26.0, *)
    private func startModernConnection(generation: UUID) {
        let url = self.url
        let registration: String
        do {
            registration = String(
                decoding: try encoder.encode(
                    CompanionRelayWireMessage.registration(
                        channelID: channelID,
                        endpointID: endpointID
                    )
                ),
                as: UTF8.self
            )
        } catch {
            handleFailure(error, generation: generation)
            return
        }

        runTask = Task.detached { [weak self] in
            do {
                let webSocket: WebSocket
                if url.scheme?.lowercased() == "wss" {
                    webSocket = WebSocket { TLS() }
                } else {
                    webSocket = WebSocket { TCP() }
                }
                let configuredWebSocket = webSocket
                    .autoReplyPing(true)
                    .maximumMessageSize(1_048_576)

                try await withNetworkConnection(
                    to: .url(url),
                    using: { configuredWebSocket }
                ) { connection in
                    await self?.installModernTransport(
                        send: { payload in try await connection.send(payload) },
                        ping: { try await connection.ping(Data()) },
                        generation: generation
                    )
                    try await connection.send(registration)

                    for try await message in connection.messages {
                        try Task.checkCancellation()
                        try await self?.handleModernMessage(
                            message.content,
                            metadata: message.metadata,
                            generation: generation
                        )
                    }
                    throw CompanionRelayConnectionError.transportFailed(
                        "The encrypted relay connection ended unexpectedly."
                    )
                }
            } catch {
                guard !Task.isCancelled else { return }
                await self?.handleFailure(error, generation: generation)
            }
        }
    }

    private func installModernTransport(
        send: @escaping SendOperation,
        ping: @escaping PingOperation,
        generation: UUID
    ) {
        guard shouldRun, generation == connectionGeneration else { return }
        modernSendOperation = send
        modernPingOperation = ping
    }

    @available(macOS 26.0, iOS 26.0, *)
    private func handleModernMessage(
        _ content: Data,
        metadata: WebSocket.Metadata,
        generation: UUID
    ) async throws {
        guard shouldRun, generation == connectionGeneration else {
            throw CancellationError()
        }
        switch metadata.opcode {
        case .text, .binary:
            try await handle(content)
        case .ping, .pong, .cont:
            break
        case .close:
            throw CompanionRelayConnectionError.transportFailed(
                "The encrypted relay closed the WebSocket connection."
            )
        @unknown default:
            throw CompanionRelayConnectionError.invalidWireMessage
        }
    }

    private func handleConnectionState(
        _ next: NWConnection.State,
        generation: UUID
    ) async {
        guard shouldRun,
              generation == connectionGeneration,
              let connection
        else { return }

        switch next {
        case .ready:
            do {
                try await sendWire(
                    .registration(channelID: channelID, endpointID: endpointID),
                    through: connection
                )
                Self.armReceiveLoop(
                    from: connection,
                    owner: self,
                    generation: generation
                )
            } catch {
                handleFailure(error, generation: generation)
            }
        case .waiting(let error), .failed(let error):
            handleFailure(error, generation: generation)
        case .cancelled:
            if shouldRun {
                handleFailure(
                    CompanionRelayConnectionError.transportFailed(
                        "The encrypted relay connection closed unexpectedly."
                    ),
                    generation: generation
                )
            }
        case .setup, .preparing:
            break
        @unknown default:
            handleFailure(
                CompanionRelayConnectionError.transportFailed(
                    "The encrypted relay entered an unknown connection state."
                ),
                generation: generation
            )
        }
    }

    private nonisolated static func armReceiveLoop(
        from connection: NWConnection,
        owner: CompanionRelayConnection,
        generation: UUID
    ) {
        connection.receiveMessage { [weak owner] content, context, isComplete, error in
            guard let owner else { return }
            if error == nil {
                Self.armReceiveLoop(
                    from: connection,
                    owner: owner,
                    generation: generation
                )
            }
            Task { [weak owner] in
                await owner?.handleReceive(
                    content: content,
                    context: context,
                    isComplete: isComplete,
                    error: error,
                    from: connection,
                    generation: generation
                )
            }
        }
    }

    private func handleReceive(
        content: Data?,
        context: NWConnection.ContentContext?,
        isComplete: Bool,
        error: NWError?,
        from connection: NWConnection,
        generation: UUID
    ) async {
        guard shouldRun,
              generation == connectionGeneration,
              self.connection === connection
        else { return }

        if ProcessInfo.processInfo.environment["COMPANION_RELAY_DEBUG"] == "1" {
            let metadata = context?.protocolMetadata(
                definition: NWProtocolWebSocket.definition
            ) as? NWProtocolWebSocket.Metadata
            FileHandle.standardError.write(
                Data(
                    "relay callback endpoint=\(endpointID) opcode=\(String(describing: metadata?.opcode)) "
                        .appending("complete=\(isComplete) bytes=\(content?.count ?? -1) ")
                        .appending("error=\(String(describing: error))\n")
                        .utf8
                )
            )
        }
        if let error {
            handleFailure(error, generation: generation)
            return
        }

        do {
            let metadata = context?.protocolMetadata(
                definition: NWProtocolWebSocket.definition
            ) as? NWProtocolWebSocket.Metadata
            if metadata?.opcode == .close {
                throw CompanionRelayConnectionError.transportFailed(
                    "The encrypted relay closed the WebSocket connection."
                )
            }
            if metadata?.opcode == .ping || metadata?.opcode == .pong || content == nil {
                return
            }
            guard isComplete, let content else {
                throw CompanionRelayConnectionError.invalidWireMessage
            }
            if ProcessInfo.processInfo.environment["COMPANION_RELAY_DEBUG"] == "1" {
                FileHandle.standardError.write(
                    Data(
                        "relay receive endpoint=\(endpointID) opcode=\(String(describing: metadata?.opcode)) "
                            .appending("complete=\(isComplete) bytes=\(content.count) state=\(state)\n")
                            .utf8
                    )
                )
            }
            guard metadata == nil || metadata?.opcode == .text || metadata?.opcode == .binary else {
                throw CompanionRelayConnectionError.invalidWireMessage
            }
            try await handle(content)
        } catch {
            handleFailure(error, generation: generation)
        }
    }

    private func handleFailure(_ error: Error, generation: UUID) {
        guard shouldRun, generation == connectionGeneration else { return }
        relayRegistrationAcknowledged = false
        failPendingPacketResults(error)
        let nsError = error as NSError
        failureHandler(
            "\(error.localizedDescription) [\(nsError.domain):\(nsError.code)] "
                + "\(String(reflecting: error))"
        )
        connection?.stateUpdateHandler = nil
        connection?.cancel()
        connection = nil
        modernSendOperation = nil
        modernPingOperation = nil
        keepAliveTask?.cancel()
        keepAliveTask = nil
        runTask?.cancel()
        runTask = nil
        scheduleReconnect()
    }

    private func handle(_ data: Data) async throws {

        let wire = try decoder.decode(CompanionRelayWireMessage.self, from: data)
        guard wire.protocolVersion == CompanionRelayWireMessage.protocolVersion else {
            throw CompanionRelayConnectionError.invalidWireMessage
        }
        switch wire.type {
        case .registered:
            reconnectAttempt = 0
            relayRegistrationAcknowledged = true
            startKeepAlive(generation: connectionGeneration)
        case .peerPresence:
            guard relayRegistrationAcknowledged,
                  let peerCount = wire.peerCount,
                  peerCount >= 0
            else {
                throw CompanionRelayConnectionError.invalidWireMessage
            }
            publish(peerCount > 0 ? .registered : .connecting)
        case .packet:
            guard state == .registered else {
                throw CompanionRelayConnectionError.invalidWireMessage
            }
            let envelope = try wire.decodedEnvelope()
            guard envelope.channelID == channelID,
                  envelope.senderID != endpointID
            else {
                throw CompanionRelayWireError.metadataMismatch
            }
            envelopeHandler(envelope)
        case .packetResult:
            guard relayRegistrationAcknowledged,
                  let packetID = wire.packetID,
                  CompanionRelayWireMessage.isValidOpaqueID(packetID),
                  let status = wire.status
            else {
                throw CompanionRelayConnectionError.invalidWireMessage
            }
            switch status {
            case .accepted:
                resolvePendingPacketResult(
                    packetID: packetID,
                    result: .success(())
                )
            case .undeliverable:
                if resolvePendingPacketResult(
                    packetID: packetID,
                    result: .failure(CompanionRelayConnectionError.peerUnavailable)
                ) {
                    publish(.connecting)
                }
            }
        case .ping:
            try await sendWire(.pong())
        case .pong:
            break
        case .error:
            throw CompanionRelayConnectionError.relayRejected(
                wire.message ?? wire.code ?? "The relay rejected the connection."
            )
        case .register:
            throw CompanionRelayConnectionError.invalidWireMessage
        }
    }

    private func sendAndAwaitPacketResult(
        _ wire: CompanionRelayWireMessage,
        packetID: String
    ) async throws {
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            pendingPacketResults[packetID] = continuation
            let timeoutNanoseconds = packetResultTimeoutNanoseconds
            packetResultTimeoutTasks[packetID] = Task { [weak self] in
                do {
                    try await Task.sleep(nanoseconds: timeoutNanoseconds)
                } catch {
                    return
                }
                await self?.resolvePendingPacketResult(
                    packetID: packetID,
                    result: .failure(
                        CompanionRelayConnectionError.packetResultTimedOut
                    )
                )
            }
            Task { [weak self] in
                await self?.transmit(wire, packetID: packetID)
            }
        }
    }

    private func transmit(
        _ wire: CompanionRelayWireMessage,
        packetID: String
    ) async {
        do {
            try await sendWire(wire)
        } catch {
            resolvePendingPacketResult(
                packetID: packetID,
                result: .failure(error)
            )
        }
    }

    @discardableResult
    private func resolvePendingPacketResult(
        packetID: String,
        result: Result<Void, Error>
    ) -> Bool {
        guard let continuation = pendingPacketResults.removeValue(forKey: packetID) else {
            return false
        }
        packetResultTimeoutTasks.removeValue(forKey: packetID)?.cancel()
        continuation.resume(with: result)
        return true
    }

    private func failPendingPacketResults(_ error: Error) {
        let continuations = Array(pendingPacketResults.values)
        pendingPacketResults.removeAll()
        let timeoutTasks = Array(packetResultTimeoutTasks.values)
        packetResultTimeoutTasks.removeAll()
        for task in timeoutTasks {
            task.cancel()
        }
        for continuation in continuations {
            continuation.resume(throwing: error)
        }
    }

    private func sendWire(_ wire: CompanionRelayWireMessage) async throws {
        if let modernSendOperation {
            let data = try encoder.encode(wire)
            try await modernSendOperation(String(decoding: data, as: UTF8.self))
            return
        }
        guard let connection else {
            throw CompanionRelayConnectionError.notRegistered
        }
        try await sendWire(wire, through: connection)
    }

    private func sendWire(
        _ wire: CompanionRelayWireMessage,
        through connection: NWConnection
    ) async throws {
        let data = try encoder.encode(wire)
        let metadata = NWProtocolWebSocket.Metadata(opcode: .text)
        let context = NWConnection.ContentContext(
            identifier: "companion-relay-message",
            metadata: [metadata]
        )
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            connection.send(
                content: data,
                contentContext: context,
                isComplete: true,
                completion: .contentProcessed { error in
                    if let error {
                        continuation.resume(throwing: error)
                    } else {
                        continuation.resume()
                    }
                }
            )
        }
    }

    private func startKeepAlive(generation: UUID) {
        guard shouldRun,
              generation == connectionGeneration,
              relayRegistrationAcknowledged,
              keepAliveTask == nil
        else { return }

        let intervalNanoseconds = keepAliveIntervalNanoseconds
        keepAliveTask = Task { [weak self] in
            do {
                try await CompanionRelayKeepAliveLoop.run(
                    intervalNanoseconds: intervalNanoseconds
                ) { [weak self] in
                    guard let self else { throw CancellationError() }
                    try await self.sendPing(generation: generation)
                }
            } catch {
                guard !Task.isCancelled else { return }
                await self?.handleFailure(error, generation: generation)
            }
        }
    }

    private func sendPing(generation: UUID) async throws {
        guard shouldRun,
              generation == connectionGeneration,
              relayRegistrationAcknowledged
        else { throw CancellationError() }

        if let modernPingOperation {
            try await modernPingOperation()
            return
        }
        guard let connection else {
            throw CompanionRelayConnectionError.notRegistered
        }
        try await sendPing(through: connection)
    }

    private func sendPing(through connection: NWConnection) async throws {
        let metadata = NWProtocolWebSocket.Metadata(opcode: .ping)
        let context = NWConnection.ContentContext(
            identifier: "companion-relay-keepalive",
            metadata: [metadata]
        )
        try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<Void, Error>) in
            connection.send(
                content: Data(),
                contentContext: context,
                isComplete: true,
                completion: .contentProcessed { error in
                    if let error {
                        continuation.resume(throwing: error)
                    } else {
                        continuation.resume()
                    }
                }
            )
        }
    }

    private func scheduleReconnect() {
        guard shouldRun else { return }
        publish(.waitingToReconnect)
        reconnectAttempt += 1
        let exponent = min(reconnectAttempt - 1, 5)
        let delaySeconds = min(30, 1 << exponent)
        reconnectTask?.cancel()
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delaySeconds))
            guard let self, !Task.isCancelled else { return }
            await self.connectAfterDelay()
        }
    }

    private func connectAfterDelay() {
        reconnectTask = nil
        connect()
    }

    private func publish(_ next: State) {
        guard state != next else { return }
        state = next
        stateHandler(next)
    }
}

enum CompanionRelaySettings {
    static let relayURLKey = "CodexCompanion.relayURL.v1"
    static let remoteAccessDisabledKey = "CodexCompanion.remoteAccessDisabled.v1"
    static let bundledRelayURLInfoKey = "CodexCompanionRelayURL"
    static let didChange = Notification.Name("CodexCompanion.relaySettingsDidChange")

    static func configuredURL(
        defaults: UserDefaults = .standard,
        bundle: Bundle = .main
    ) -> URL? {
        configuredURL(
            defaults: defaults,
            bundledURLString: bundle.object(
                forInfoDictionaryKey: bundledRelayURLInfoKey
            ) as? String
        )
    }

    static func configuredURL(
        defaults: UserDefaults,
        bundledURLString: String?
    ) -> URL? {
        guard !defaults.bool(forKey: remoteAccessDisabledKey) else { return nil }
        if let override = validatedURL(defaults.string(forKey: relayURLKey)) {
            return override
        }
        return validatedURL(bundledURLString)
    }

    static func bundledURL(bundle: Bundle = .main) -> URL? {
        validatedURL(
            bundle.object(forInfoDictionaryKey: bundledRelayURLInfoKey) as? String
        )
    }

    static func setRemoteAccessEnabled(
        _ enabled: Bool,
        defaults: UserDefaults = .standard
    ) {
        defaults.set(!enabled, forKey: remoteAccessDisabledKey)
        NotificationCenter.default.post(name: didChange, object: nil)
    }

    static func useBundledRelay(defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: relayURLKey)
        defaults.set(false, forKey: remoteAccessDisabledKey)
        NotificationCenter.default.post(name: didChange, object: nil)
    }

    @discardableResult
    static func setRelayURL(
        _ value: String,
        defaults: UserDefaults = .standard
    ) -> Bool {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            defaults.removeObject(forKey: relayURLKey)
            defaults.set(true, forKey: remoteAccessDisabledKey)
            NotificationCenter.default.post(name: didChange, object: nil)
            return true
        }
        guard let url = validatedURL(trimmed) else { return false }
        defaults.set(url.absoluteString, forKey: relayURLKey)
        defaults.set(false, forKey: remoteAccessDisabledKey)
        NotificationCenter.default.post(name: didChange, object: nil)
        return true
    }

    static func validatedURL(_ value: String?) -> URL? {
        guard let value,
              let url = URL(string: value),
              let scheme = url.scheme?.lowercased(),
              let host = url.host,
              !host.isEmpty
        else { return nil }
        if scheme == "wss" { return url }
        if scheme == "ws", host == "localhost" || host == "127.0.0.1" || host == "::1" {
            return url
        }
        return nil
    }
}
