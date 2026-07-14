import AppKit
import Foundation

enum CodexAppServerSendOutcome: Equatable, Sendable {
    case sent
    case sharedDaemonUnavailable
    case threadNotLoaded
    case noActiveTurn
    case timedOut
    case failed

    var succeeded: Bool {
        self == .sent
    }
}

enum CodexAppServerTurnsListState: Equatable, Sendable {
    case idle
    case active(turnID: String)
}

enum CodexAppServerThreadState: Equatable, Sendable {
    case idle
    case active
    case unavailable
}

enum CodexAppServerRequestPolicy {
    static let turnItemsView = "notLoaded"
    static let turnDiscoveryTimeoutSeconds = 30

    static func requiresTurnDiscovery(before action: CodexSendAction) -> Bool {
        action == .steer
    }
}

struct CodexAppServerResponseParser {
    static func turnsListState(from message: [String: Any]) -> CodexAppServerTurnsListState? {
        guard
            let result = message["result"] as? [String: Any],
            let turns = result["data"] as? [[String: Any]]
        else {
            return nil
        }

        if let activeTurnID = turns.first(where: { turn in
            turn["status"] as? String == "inProgress"
        })?["id"] as? String {
            return .active(turnID: activeTurnID)
        }
        return .idle
    }

    static func threadState(
        from message: [String: Any],
        threadID: String
    ) -> CodexAppServerThreadState? {
        guard
            let result = message["result"] as? [String: Any],
            let threads = result["data"] as? [[String: Any]]
        else {
            return nil
        }
        guard let thread = threads.first(where: { $0["id"] as? String == threadID }) else {
            return .unavailable
        }
        guard let status = thread["status"] as? [String: Any], let type = status["type"] as? String else {
            return nil
        }
        return type == "active" ? .active : .idle
    }
}

typealias CodexFollowerSubmitter = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ action: CodexSendAction,
    _ clientMessageID: String,
    _ cwd: String?
) async -> CodexAppServerSendOutcome

typealias CodexThreadLoader = @Sendable (_ threadID: String) async -> Bool
typealias CodexSendRetryWaiter = @Sendable (_ retryIndex: Int) async -> Void

final class CodexAppServerSender {
    private let submitter: CodexFollowerSubmitter
    private let threadLoader: CodexThreadLoader
    private let retryWaiter: CodexSendRetryWaiter
    private let maximumLoadRetries: Int

    init(
        submitter: @escaping CodexFollowerSubmitter = { prompt, threadID, action, clientMessageID, cwd in
            await CodexFollowerIPCTransport().submit(
                prompt: prompt,
                threadID: threadID,
                action: action,
                clientMessageID: clientMessageID,
                cwd: cwd
            )
        },
        threadLoader: @escaping CodexThreadLoader = { threadID in
            await CodexBackgroundThreadLoader().load(threadID: threadID)
        },
        retryWaiter: @escaping CodexSendRetryWaiter = { retryIndex in
            let delayMilliseconds = min(250 + (retryIndex * 250), 1_000)
            try? await Task.sleep(for: .milliseconds(delayMilliseconds))
        },
        maximumLoadRetries: Int = 4
    ) {
        self.submitter = submitter
        self.threadLoader = threadLoader
        self.retryWaiter = retryWaiter
        self.maximumLoadRetries = max(0, maximumLoadRetries)
    }

    func submit(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        onQueued: @escaping @Sendable () -> Void
    ) async -> CodexAppServerSendOutcome {
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedPrompt.isEmpty, !trimmedThreadID.isEmpty else { return .failed }

        _ = expectedTurnID
        _ = onQueued
        Self.log(
            "submit native-ipc action=\(action.logName) thread=\(trimmedThreadID) socket=\(CodexFollowerIPCProtocol.socketURL.path)"
        )
        var outcome = await submitter(
            trimmedPrompt,
            trimmedThreadID,
            action,
            clientMessageID,
            cwd
        )
        if outcome == .threadNotLoaded, maximumLoadRetries > 0 {
            Self.log("native-ipc loading unowned thread in background thread=\(trimmedThreadID)")
            guard await threadLoader(trimmedThreadID) else {
                Self.log("native-ipc background thread load failed thread=\(trimmedThreadID)")
                return .threadNotLoaded
            }

            for retryIndex in 0..<maximumLoadRetries {
                guard !Task.isCancelled else { return .failed }
                await retryWaiter(retryIndex)
                outcome = await submitter(
                    trimmedPrompt,
                    trimmedThreadID,
                    action,
                    clientMessageID,
                    cwd
                )
                Self.log(
                    "native-ipc post-load retry=\(retryIndex + 1) thread=\(trimmedThreadID) outcome=\(String(describing: outcome))"
                )
                if outcome != .threadNotLoaded {
                    break
                }
            }
        }
        Self.log(
            "native-ipc finished action=\(action.logName) thread=\(trimmedThreadID) outcome=\(String(describing: outcome))"
        )
        return outcome
    }

    static var sharedDaemonSocketURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("app-server-control", isDirectory: true)
            .appendingPathComponent("app-server-control.sock")
    }

    private static func log(_ message: String) {
        NSLog("CodexAppServerSender: %@", message)
        CodexSendLog.append("app-server \(message)")
    }

    static func runningCodexExecutableURL() -> URL? {
        preferredRunningCodexApplications()
            .compactMap { app -> URL? in
                guard let bundleURL = app.bundleURL else { return nil }
                let executableURL = bundleURL.appendingPathComponent("Contents/Resources/codex")
                guard FileManager.default.isExecutableFile(atPath: executableURL.path) else { return nil }
                return executableURL
            }
            .first
    }

    private static func preferredRunningCodexApplications() -> [NSRunningApplication] {
        let apps = NSRunningApplication.runningApplications(withBundleIdentifier: "com.openai.codex")
        return apps.sorted { lhs, rhs in
            appPreferenceRank(lhs.bundleURL) < appPreferenceRank(rhs.bundleURL)
        }
    }

    private static func appPreferenceRank(_ bundleURL: URL?) -> Int {
        guard let path = bundleURL?.standardizedFileURL.path else { return Int.max }
        return WorkspacePaths.codexAppURLs
            .map { $0.standardizedFileURL.path }
            .firstIndex(of: path) ?? Int.max
    }
}

private struct CodexBackgroundThreadLoader {
    func load(threadID: String) async -> Bool {
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedThreadID.isEmpty,
              let encodedThreadID = trimmedThreadID.addingPercentEncoding(
                  withAllowedCharacters: .urlPathAllowed
              ),
              let url = URL(string: "codex://threads/\(encodedThreadID)")
        else {
            return false
        }

        return await withCheckedContinuation { continuation in
            DispatchQueue.main.async {
                let configuration = NSWorkspace.OpenConfiguration()
                configuration.activates = false
                configuration.addsToRecentItems = false
                configuration.promptsUserIfNeeded = false
                NSWorkspace.shared.open(url, configuration: configuration) { _, error in
                    if let error {
                        CodexSendLog.append(
                            "app-server background thread load failed thread=\(trimmedThreadID) error=\(error.localizedDescription)"
                        )
                    }
                    continuation.resume(returning: error == nil)
                }
            }
        }
    }
}

private final class CodexAppServerSession {
    private enum PendingTurnStart: Equatable {
        case queuedReply

        var logName: String {
            "queued reply"
        }
    }

    private enum RequestID {
        static let initialize = 1
        static let loadedThreads = 2
        static let recentTurns = 3
        static let sendAction = 4
        static let pendingTurnStartRecheck = 5
    }

    private let executableURL: URL
    private let prompt: String
    private let threadID: String
    private let cwd: String?
    private let action: CodexSendAction
    private let expectedTurnID: String?
    private let clientMessageID: String
    private let onQueued: @Sendable () -> Void
    private let completion: @Sendable (CodexAppServerSendOutcome) -> Void
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let stateQueue = DispatchQueue(label: "com.silverfire.codexcompanion.app-server-session")
    private var webSocketCodec = CodexWebSocketCodec()
    private var hasUpgradedWebSocket = false
    private var isFinished = false
    private var didCleanUp = false
    private var pendingTurnStart: PendingTurnStart?
    private var isPendingTurnStartRecheckPending = false
    private var phaseTimeoutWorkItem: DispatchWorkItem?
    private var pendingTurnStartPollWorkItem: DispatchWorkItem?
    private var pendingTurnStartExpiryWorkItem: DispatchWorkItem?

    init(
        executableURL: URL,
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        onQueued: @escaping @Sendable () -> Void,
        completion: @escaping @Sendable (CodexAppServerSendOutcome) -> Void
    ) {
        self.executableURL = executableURL
        self.prompt = prompt
        self.threadID = threadID
        self.cwd = cwd
        self.action = action
        self.expectedTurnID = expectedTurnID
        self.clientMessageID = clientMessageID
        self.onQueued = onQueued
        self.completion = completion
    }

    func start() {
        process.executableURL = executableURL
        process.arguments = ["app-server", "proxy"]
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            self?.stateQueue.async { [weak self] in
                self?.readStdout(data)
            }
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            if let text = String(data: data, encoding: .utf8) {
                self?.stateQueue.async { [weak self] in
                    self?.appendLog("stderr \(text.trimmingCharacters(in: .whitespacesAndNewlines))")
                }
            }
        }
        process.terminationHandler = { [weak self] process in
            self?.stateQueue.async { [weak self] in
                self?.handleTermination(status: process.terminationStatus)
            }
        }

        CodexAppServerSessionRegistry.shared.retain(self)
        do {
            try process.run()
        } catch {
            appendLog("launch failed: \(error.localizedDescription)")
            stateQueue.sync {
                finish(outcome: .sharedDaemonUnavailable)
                cleanup()
            }
            return
        }

        appendLog("launched \(executableURL.path) \(process.arguments?.joined(separator: " ") ?? "")")
        stateQueue.async { [weak self] in
            guard let self else { return }
            schedulePhaseTimeout(message: "WebSocket handshake timed out")
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }
    }

    func cancel() {
        stateQueue.async { [weak self] in
            guard let self, !isFinished else { return }
            appendLog("session canceled")
            finish(outcome: .failed)
            terminate()
        }
    }

    private func readStdout(_ data: Data) {
        guard !data.isEmpty else { return }
        do {
            for event in try webSocketCodec.receive(data) {
                switch event {
                case .upgraded:
                    hasUpgradedWebSocket = true
                    appendLog("WebSocket upgraded")
                    schedulePhaseTimeout(message: "initialization timed out")
                    sendInitialize()
                case .text(let payload):
                    handleLine(payload)
                case .ping(let payload):
                    writeWebSocketFrame(opcode: .pong, payload: payload)
                case .close:
                    appendLog("WebSocket closed")
                    finish(outcome: .sharedDaemonUnavailable)
                    terminate()
                }
            }
        } catch {
            appendLog("WebSocket decode failed: \(error)")
            finish(outcome: .sharedDaemonUnavailable)
            terminate()
        }
    }

    private func sendInitialize() {
        send([
            "id": RequestID.initialize,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "codex-companion",
                    "title": "Codex Companion",
                    "version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0",
                ],
                "capabilities": [
                    "experimentalApi": true,
                    "optOutNotificationMethods": [],
                ],
            ],
        ])
    }

    private func handleLine(_ data: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let message = object as? [String: Any]
        else {
            return
        }

        if let error = message["error"] as? [String: Any] {
            handleError(error, requestID: numericID(in: message))
            return
        }

        if let id = numericID(in: message) {
            appendLog("response id=\(id)")
            handleResponse(id: id, message: message)
            return
        }

        guard let method = message["method"] as? String else { return }
        appendLog("notification \(method)")
        handleNotification(method: method, message: message)
    }

    private func handleResponse(id: Int, message: [String: Any]) {
        switch id {
        case RequestID.initialize:
            schedulePhaseTimeout(message: "loaded-thread discovery timed out")
            send(["method": "initialized"])
            requestLoadedThreads()
        case RequestID.loadedThreads:
            let loadedThreadIDs = loadedThreadIDs(from: message)
            guard !loadedThreadIDs.isEmpty else {
                appendLog("shared daemon has no ChatGPT-loaded threads")
                finish(outcome: .sharedDaemonUnavailable)
                terminate()
                return
            }
            guard loadedThreadIDs.contains(threadID) else {
                appendLog("target thread is not loaded by the shared ChatGPT app-server")
                finish(outcome: .threadNotLoaded)
                terminate()
                return
            }
            if action == .steer, let expectedTurnID {
                appendLog("using rollout turn id \(expectedTurnID)")
                steerTurn(expectedTurnID: expectedTurnID)
            } else if CodexAppServerRequestPolicy.requiresTurnDiscovery(before: action) {
                schedulePhaseTimeout(
                    message: "turn discovery timed out",
                    after: .seconds(CodexAppServerRequestPolicy.turnDiscoveryTimeoutSeconds)
                )
                requestRecentTurns(id: RequestID.recentTurns)
            } else {
                startTurn()
            }
        case RequestID.recentTurns:
            routeInitialAction(from: message)
        case RequestID.sendAction:
            appendLog("\(action.logName) accepted")
            finish(outcome: .sent)
            terminate()
        case RequestID.pendingTurnStartRecheck:
            cancelPhaseTimeout()
            isPendingTurnStartRecheckPending = false
            guard pendingTurnStart != nil else { return }
            guard let state = CodexAppServerResponseParser.threadState(
                from: message,
                threadID: threadID
            ) else {
                appendLog("pending turn-start status response was malformed")
                finish(outcome: .failed)
                terminate()
                return
            }
            if state == .idle {
                startPendingTurn()
            } else if state == .active {
                schedulePendingTurnStartPoll()
            } else {
                appendLog("pending turn-start target became unavailable")
                finish(outcome: .threadNotLoaded)
                terminate()
            }
        default:
            break
        }
    }

    private func routeInitialAction(from message: [String: Any]) {
        guard let state = CodexAppServerResponseParser.turnsListState(from: message) else {
            appendLog("turn discovery response was malformed")
            finish(outcome: .failed)
            terminate()
            return
        }
        switch action {
        case .reply:
            if case .idle = state {
                startTurn()
            } else {
                queueReply()
            }
        case .steer:
            guard case let .active(activeTurnID) = state else {
                appendLog("steer failed: no in-progress turn")
                finish(outcome: .noActiveTurn)
                terminate()
                return
            }
            steerTurn(expectedTurnID: activeTurnID)
        }
    }

    private func handleNotification(method: String, message: [String: Any]) {
        guard pendingTurnStart != nil, notificationMatchesTargetThread(message) else { return }
        switch method {
        case "turn/completed", "thread/status/changed":
            requestPendingTurnStartRecheck()
        case "thread/closed", "thread/deleted":
            appendLog("pending turn-start cancelled because the thread closed")
            finish(outcome: .failed)
            terminate()
        default:
            break
        }
    }

    private func handleError(_ error: [String: Any], requestID: Int?) {
        appendLog("error id=\(requestID.map(String.init) ?? "nil") \(String(describing: error))")

        let message = errorMessage(error)
        if requestID == RequestID.sendAction,
           pendingTurnStart == nil,
           message.localizedCaseInsensitiveContains("active turn") {
            if action == .reply {
                queueReply()
            } else {
                finish(outcome: .noActiveTurn)
                terminate()
            }
            return
        }

        finish(outcome: .failed)
        terminate()
    }

    private func requestLoadedThreads() {
        appendLog("thread/loaded/list")
        send([
            "id": RequestID.loadedThreads,
            "method": "thread/loaded/list",
            "params": [:],
        ])
    }

    private func requestRecentTurns(id: Int) {
        appendLog("thread/turns/list id=\(id)")
        send([
            "id": id,
            "method": "thread/turns/list",
            "params": [
                "threadId": threadID,
                "limit": 1,
                "sortDirection": "desc",
                "itemsView": CodexAppServerRequestPolicy.turnItemsView,
            ],
        ])
    }

    private func requestThreadState(id: Int) {
        appendLog("thread/list id=\(id)")
        send([
            "id": id,
            "method": "thread/list",
            "params": [
                "limit": 100,
                "sortKey": "updated_at",
                "sortDirection": "desc",
                "archived": false,
                "useStateDbOnly": true,
            ],
        ])
    }

    private func startTurn() {
        appendLog("turn/start")
        let params: [String: Any] = [
            "threadId": threadID,
            "input": userInput,
            "clientUserMessageId": clientMessageID,
        ]
        schedulePhaseTimeout(message: "turn/start acknowledgement timed out")
        send([
            "id": RequestID.sendAction,
            "method": "turn/start",
            "params": params,
        ])
    }

    private func steerTurn(expectedTurnID: String) {
        appendLog("turn/steer expectedTurnID=\(expectedTurnID)")
        schedulePhaseTimeout(message: "turn/steer acknowledgement timed out")
        send([
            "id": RequestID.sendAction,
            "method": "turn/steer",
            "params": [
                "threadId": threadID,
                "expectedTurnId": expectedTurnID,
                "input": userInput,
                "clientUserMessageId": clientMessageID,
            ],
        ])
    }

    private func queueReply() {
        beginWaitingForTurnStart(.queuedReply)
    }

    private func beginWaitingForTurnStart(_ pending: PendingTurnStart) {
        guard pendingTurnStart == nil else { return }
        pendingTurnStart = pending
        appendLog("\(pending.logName) waiting until the active turn completes")
        cancelPhaseTimeout()
        if pending == .queuedReply {
            onQueued()
        }

        let workItem = DispatchWorkItem { [weak self] in
            guard let self, pendingTurnStart == pending else { return }
            appendLog("\(pending.logName) expired")
            finish(outcome: .timedOut)
            terminate()
        }
        pendingTurnStartExpiryWorkItem = workItem
        stateQueue.asyncAfter(
            deadline: .now() + .hours(24),
            execute: workItem
        )
        requestPendingTurnStartRecheck()
    }

    private func requestPendingTurnStartRecheck() {
        guard !isPendingTurnStartRecheckPending else { return }
        pendingTurnStartPollWorkItem?.cancel()
        pendingTurnStartPollWorkItem = nil
        isPendingTurnStartRecheckPending = true
        schedulePhaseTimeout(
            message: "pending turn-start status check timed out",
            after: .seconds(CodexAppServerRequestPolicy.turnDiscoveryTimeoutSeconds)
        )
        requestThreadState(id: RequestID.pendingTurnStartRecheck)
    }

    private func schedulePendingTurnStartPoll() {
        guard pendingTurnStart != nil else { return }
        pendingTurnStartPollWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            guard let self, pendingTurnStart != nil else { return }
            requestPendingTurnStartRecheck()
        }
        pendingTurnStartPollWorkItem = workItem
        stateQueue.asyncAfter(deadline: .now() + .seconds(2), execute: workItem)
    }

    private func startPendingTurn() {
        guard let pending = pendingTurnStart else { return }
        pendingTurnStart = nil
        isPendingTurnStartRecheckPending = false
        pendingTurnStartPollWorkItem?.cancel()
        pendingTurnStartPollWorkItem = nil
        pendingTurnStartExpiryWorkItem?.cancel()
        pendingTurnStartExpiryWorkItem = nil
        appendLog("active turn completed; starting \(pending.logName)")
        startTurn()
    }

    private var userInput: [[String: Any]] {
        [
            [
                "type": "text",
                "text": prompt,
                "text_elements": [],
            ],
        ]
    }

    private func loadedThreadIDs(from message: [String: Any]) -> Set<String> {
        guard
            let result = message["result"] as? [String: Any],
            let data = result["data"] as? [String]
        else {
            return []
        }
        return Set(data)
    }

    private func notificationMatchesTargetThread(_ message: [String: Any]) -> Bool {
        guard let params = message["params"] as? [String: Any] else { return false }
        return params["threadId"] as? String == threadID
    }

    private func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let rawID = message["id"] as? String { return Int(rawID) }
        return nil
    }

    private func errorMessage(_ error: [String: Any]) -> String {
        error["message"] as? String ?? ""
    }

    private func send(_ object: [String: Any]) {
        guard
            hasUpgradedWebSocket,
            JSONSerialization.isValidJSONObject(object),
            let data = try? JSONSerialization.data(withJSONObject: object)
        else {
            return
        }
        writeWebSocketFrame(opcode: .text, payload: data)
    }

    private func writeWebSocketFrame(
        opcode: CodexWebSocketCodec.Opcode,
        payload: Data
    ) {
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: opcode, payload: payload))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            appendLog("write failed: \(error.localizedDescription)")
            finish(outcome: .sharedDaemonUnavailable)
            terminate()
        }
    }

    private func appendLog(_ message: String) {
        CodexSendLog.append("app-server-session \(message)")
    }

    private func schedulePhaseTimeout(
        message: String,
        after delay: DispatchTimeInterval = .seconds(8)
    ) {
        cancelPhaseTimeout()
        let workItem = DispatchWorkItem { [weak self] in
            guard let self, !isFinished else { return }
            appendLog(message)
            finish(outcome: .timedOut)
            terminate()
        }
        phaseTimeoutWorkItem = workItem
        stateQueue.asyncAfter(deadline: .now() + delay, execute: workItem)
    }

    private func cancelPhaseTimeout() {
        phaseTimeoutWorkItem?.cancel()
        phaseTimeoutWorkItem = nil
    }

    private func finish(outcome: CodexAppServerSendOutcome) {
        guard !isFinished else { return }
        isFinished = true
        cancelPhaseTimeout()
        pendingTurnStartPollWorkItem?.cancel()
        pendingTurnStartPollWorkItem = nil
        pendingTurnStartExpiryWorkItem?.cancel()
        pendingTurnStartExpiryWorkItem = nil
        completion(outcome)
    }

    private func terminate() {
        cleanup()
        if process.isRunning {
            process.terminate()
        }
    }

    private func handleTermination(status: Int32) {
        appendLog("process terminated status=\(status)")
        finish(outcome: .sharedDaemonUnavailable)
        cleanup()
    }

    private func cleanup() {
        guard !didCleanUp else { return }
        didCleanUp = true
        cancelPhaseTimeout()
        pendingTurnStartPollWorkItem?.cancel()
        pendingTurnStartPollWorkItem = nil
        pendingTurnStartExpiryWorkItem?.cancel()
        pendingTurnStartExpiryWorkItem = nil
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
        CodexAppServerSessionRegistry.shared.release(self)
    }
}

private final class CodexAppServerSessionHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var session: CodexAppServerSession?
    private var isCanceled = false

    func install(_ session: CodexAppServerSession) -> Bool {
        lock.withLock {
            guard !isCanceled else { return false }
            self.session = session
            return true
        }
    }

    func cancel() {
        let session = lock.withLock { () -> CodexAppServerSession? in
            isCanceled = true
            return self.session
        }
        session?.cancel()
    }
}

private final class CodexAppServerSessionRegistry: @unchecked Sendable {
    static let shared = CodexAppServerSessionRegistry()

    private let lock = NSLock()
    private var sessions: [ObjectIdentifier: CodexAppServerSession] = [:]

    func retain(_ session: CodexAppServerSession) {
        lock.withLock {
            sessions[ObjectIdentifier(session)] = session
        }
    }

    func release(_ session: CodexAppServerSession) {
        _ = lock.withLock {
            sessions.removeValue(forKey: ObjectIdentifier(session))
        }
    }
}

enum CodexSendLog {
    static func append(_ message: String) {
        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Logs", isDirectory: true)
            .appendingPathComponent("CodexCompanion", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent("send.log")
        let line = "\(ISO8601DateFormatter().string(from: Date())) \(message)\n"
        guard let data = line.data(using: .utf8) else { return }

        if FileManager.default.fileExists(atPath: url.path),
           let handle = try? FileHandle(forWritingTo: url) {
            defer {
                try? handle.close()
            }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
            return
        }

        try? data.write(to: url, options: .atomic)
    }
}

private extension DispatchTimeInterval {
    static func hours(_ hours: Int) -> DispatchTimeInterval {
        .seconds(hours * 60 * 60)
    }
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }
}
