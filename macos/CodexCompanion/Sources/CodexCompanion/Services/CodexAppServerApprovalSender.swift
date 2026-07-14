import Foundation

enum CodexAppServerApprovalOutcome: Equatable, Sendable {
    case approved
    case declined
    case requestNotFound
    case sharedDaemonUnavailable
    case timedOut
    case failed
}

enum CodexApprovalDecision: Equatable, Sendable {
    case approveOnce
    case approveSimilarCommands
    case decline
}

struct CodexPendingApproval: Equatable, Sendable {
    enum Method: String, Sendable {
        case commandExecution = "item/commandExecution/requestApproval"
        case fileChange = "item/fileChange/requestApproval"
    }

    var threadID: String
    var requestID: Int
    var method: Method
    var proposedExecpolicyAmendment: [String]? = nil
}

enum CodexApprovalRequestParser {
    static func pendingApproval(from message: [String: Any]) -> CodexPendingApproval? {
        guard
            let rawMethod = message["method"] as? String,
            let method = CodexPendingApproval.Method(rawValue: rawMethod),
            let requestID = numericID(in: message),
            let params = message["params"] as? [String: Any],
            let threadID = params["threadId"] as? String
        else {
            return nil
        }

        let amendment = (params["proposedExecpolicyAmendment"] as? [String])
            ?? (params["proposed_execpolicy_amendment"] as? [String])
        return CodexPendingApproval(
            threadID: threadID,
            requestID: requestID,
            method: method,
            proposedExecpolicyAmendment: amendment?.isEmpty == false ? amendment : nil
        )
    }

    private static func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? NSNumber { return id.intValue }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }
}

enum CodexApprovalResponseFactory {
    static func result(
        for decision: CodexApprovalDecision,
        request: CodexPendingApproval
    ) -> [String: Any] {
        let responseDecision: Any
        switch decision {
        case .approveOnce:
            responseDecision = "accept"
        case .approveSimilarCommands:
            if request.method == .commandExecution,
               let amendment = request.proposedExecpolicyAmendment,
               !amendment.isEmpty
            {
                responseDecision = [
                    "acceptWithExecpolicyAmendment": [
                        "execpolicy_amendment": amendment,
                    ],
                ]
            } else {
                responseDecision = "acceptForSession"
            }
        case .decline:
            responseDecision = "decline"
        }
        return ["decision": responseDecision]
    }
}

enum CodexDesktopApprovalLogParser {
    static func pendingApprovals(
        lines: some Sequence<String>
    ) -> [String: CodexPendingApproval] {
        var pendingByThread: [String: CodexPendingApproval] = [:]
        var threadByRequestID: [Int: String] = [:]

        for line in lines {
            if line.contains("[desktop-notifications] show approval"),
               let threadID = token(named: "conversationId", in: line),
               let rawRequestID = token(named: "requestId", in: line),
               let requestID = Int(rawRequestID),
               let rawKind = token(named: "kind", in: line),
               let method = method(forLogKind: rawKind)
            {
                if let previous = pendingByThread[threadID] {
                    threadByRequestID.removeValue(forKey: previous.requestID)
                }
                pendingByThread[threadID] = CodexPendingApproval(
                    threadID: threadID,
                    requestID: requestID,
                    method: method
                )
                threadByRequestID[requestID] = threadID
                continue
            }

            if line.contains("Sending server response"),
               isApprovalResponse(line),
               let rawRequestID = token(named: "id", in: line),
               let requestID = Int(rawRequestID),
               let threadID = threadByRequestID.removeValue(forKey: requestID),
               pendingByThread[threadID]?.requestID == requestID
            {
                pendingByThread.removeValue(forKey: threadID)
            }
        }

        return pendingByThread
    }

    static func pendingApproval(
        for threadID: String,
        lines: some Sequence<String>
    ) -> CodexPendingApproval? {
        pendingApprovals(lines: lines)[threadID]
    }

    static func currentVisibleThreadID(
        lines: some Sequence<String>
    ) -> String? {
        var currentThreadID: String?

        for line in lines {
            guard
                line.contains("browser sidebar owner sync"),
                let route = token(named: "ownerRoutePath", in: line),
                route.hasPrefix("/local/")
            else {
                continue
            }

            let remainder = route.dropFirst("/local/".count)
            let threadID = remainder.prefix {
                $0.isLetter || $0.isNumber || $0 == "-"
            }
            if !threadID.isEmpty {
                currentThreadID = String(threadID)
            }
        }

        return currentThreadID
    }

    private static func method(forLogKind kind: String) -> CodexPendingApproval.Method? {
        switch kind {
        case "commandExecution": return .commandExecution
        case "fileChange": return .fileChange
        default: return nil
        }
    }

    private static func isApprovalResponse(_ line: String) -> Bool {
        line.contains("method=\(CodexPendingApproval.Method.commandExecution.rawValue)")
            || line.contains("method=\(CodexPendingApproval.Method.fileChange.rawValue)")
    }

    private static func token(named name: String, in line: String) -> String? {
        let marker = "\(name)="
        guard let markerRange = line.range(of: marker) else { return nil }
        let remainder = line[markerRange.upperBound...]
        let token = remainder.prefix { !$0.isWhitespace }
        return token.isEmpty ? nil : String(token)
    }
}

struct CodexDesktopApprovalLogReader: Sendable {
    var maximumFileAge: TimeInterval = 2 * 24 * 60 * 60
    var maximumTailBytes = 4 * 1_024 * 1_024
    var logsRootURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/com.openai.codex", isDirectory: true)

    func pendingApproval(for threadID: String) -> CodexPendingApproval? {
        pendingApprovals()[threadID]
    }

    func pendingApprovals() -> [String: CodexPendingApproval] {
        let lines = currentPrimaryLogURLs()
            .flatMap(CodexDesktopApprovalEventCache.shared.lines)
            .sorted()
        return CodexDesktopApprovalLogParser.pendingApprovals(lines: lines)
    }

    func currentVisibleThreadID() -> String? {
        let lines = currentPrimaryLogURLs().flatMap(tailLines)
        return CodexDesktopApprovalLogParser.currentVisibleThreadID(lines: lines)
    }

    private func currentPrimaryLogURLs() -> [URL] {
        guard let enumerator = FileManager.default.enumerator(
            at: logsRootURL,
            includingPropertiesForKeys: [.contentModificationDateKey, .isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        let cutoff = Date().addingTimeInterval(-maximumFileAge)
        let candidates = enumerator.compactMap { element -> (URL, Date, String)? in
            guard let url = element as? URL, url.pathExtension == "log" else { return nil }
            let filename = url.lastPathComponent
            guard let primaryMarker = filename.range(of: "-t0-") else { return nil }
            guard
                let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .isRegularFileKey]),
                values.isRegularFile == true,
                let modifiedAt = values.contentModificationDate,
                modifiedAt >= cutoff
            else {
                return nil
            }
            return (url, modifiedAt, String(filename[..<primaryMarker.lowerBound]))
        }
        guard let currentSession = candidates.max(by: { $0.1 < $1.1 })?.2 else {
            return []
        }
        return candidates
            .filter { $0.2 == currentSession }
            .sorted { $0.1 < $1.1 }
            .map(\.0)
    }

    private func tailLines(at url: URL) -> [String] {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? handle.close() }

        let fileSize = (try? handle.seekToEnd()) ?? 0
        guard fileSize > 0 else { return [] }
        let readLength = min(fileSize, UInt64(maximumTailBytes))
        let startOffset = fileSize - readLength
        guard (try? handle.seek(toOffset: startOffset)) != nil else { return [] }
        var data = handle.readDataToEndOfFile()
        if startOffset > 0, let newline = data.firstIndex(of: 0x0A) {
            data.removeSubrange(data.startIndex...newline)
        }
        guard let text = String(data: data, encoding: .utf8) else { return [] }
        return text.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
    }
}

private final class CodexDesktopApprovalEventCache: @unchecked Sendable {
    static let shared = CodexDesktopApprovalEventCache()

    private struct Entry {
        var parsedByteCount: UInt64 = 0
        var unfinishedLine = Data()
        var eventLines: [String] = []
    }

    private let lock = NSLock()
    private var entriesByPath: [String: Entry] = [:]

    func lines(at url: URL) -> [String] {
        lock.lock()
        defer { lock.unlock() }

        guard let handle = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? handle.close() }
        guard let fileSize = try? handle.seekToEnd() else { return [] }

        var entry = entriesByPath[url.path] ?? Entry()
        if fileSize < entry.parsedByteCount {
            entry = Entry()
        }
        guard fileSize > entry.parsedByteCount else { return entry.eventLines }
        guard (try? handle.seek(toOffset: entry.parsedByteCount)) != nil else {
            return entry.eventLines
        }

        var data = entry.unfinishedLine
        data.append(handle.readDataToEndOfFile())
        entry.parsedByteCount = fileSize

        let endsWithNewline = data.last == 0x0A
        let fragments = data.split(separator: 0x0A, omittingEmptySubsequences: false)
        let completeCount = endsWithNewline ? max(0, fragments.count - 1) : max(0, fragments.count - 1)
        entry.unfinishedLine = endsWithNewline
            ? Data()
            : fragments.last.map { Data($0) } ?? Data()

        for fragment in fragments.prefix(completeCount) {
            guard let line = String(data: Data(fragment), encoding: .utf8),
                  Self.isApprovalEvent(line)
            else { continue }
            entry.eventLines.append(line)
        }
        if entry.eventLines.count > 4_096 {
            entry.eventLines.removeFirst(entry.eventLines.count - 4_096)
        }
        entriesByPath[url.path] = entry
        return entry.eventLines
    }

    private static func isApprovalEvent(_ line: String) -> Bool {
        line.contains("[desktop-notifications] show approval")
            || line.contains("Sending server response")
    }
}

final class CodexPendingApprovalBroker: @unchecked Sendable {
    static let shared = CodexPendingApprovalBroker()

    private let stateQueue = DispatchQueue(label: "com.silverfire.codexcompanion.approval-broker")
    private var session: CodexPendingApprovalMonitorSession?
    private var pendingByThread: [String: CodexPendingApproval] = [:]
    private var retryWorkItem: DispatchWorkItem?
    private var shouldMonitor = false

    func start() {
        stateQueue.async { [weak self] in
            guard let self else { return }
            shouldMonitor = true
            launchMonitorIfNeeded()
        }
    }

    func details(matching request: CodexPendingApproval) -> CodexPendingApproval {
        stateQueue.sync {
            guard
                let cached = pendingByThread[request.threadID],
                cached.requestID == request.requestID,
                cached.method == request.method
            else {
                return request
            }
            return cached
        }
    }

    func remove(_ request: CodexPendingApproval) {
        stateQueue.async { [weak self] in
            guard let self, pendingByThread[request.threadID]?.requestID == request.requestID else {
                return
            }
            pendingByThread.removeValue(forKey: request.threadID)
        }
    }

    private func launchMonitorIfNeeded() {
        guard shouldMonitor, session == nil else { return }
        guard FileManager.default.fileExists(atPath: CodexAppServerSender.sharedDaemonSocketURL.path) else {
            scheduleRetry()
            return
        }
        guard let executableURL = CodexAppServerSender.runningCodexExecutableURL()
            ?? WorkspacePaths.codexExecutableURLs.first(where: {
                FileManager.default.isExecutableFile(atPath: $0.path)
            })
        else {
            scheduleRetry()
            return
        }

        retryWorkItem?.cancel()
        retryWorkItem = nil
        let monitor = CodexPendingApprovalMonitorSession(
            executableURL: executableURL,
            didReceive: { [weak self] request in
                self?.stateQueue.async { [weak self] in
                    self?.pendingByThread[request.threadID] = request
                }
            },
            didEnd: { [weak self] in
                self?.stateQueue.async { [weak self] in
                    guard let self else { return }
                    session = nil
                    scheduleRetry()
                }
            }
        )
        session = monitor
        monitor.start()
    }

    private func scheduleRetry() {
        guard shouldMonitor, retryWorkItem == nil else { return }
        let workItem = DispatchWorkItem { [weak self] in
            guard let self else { return }
            retryWorkItem = nil
            launchMonitorIfNeeded()
        }
        retryWorkItem = workItem
        stateQueue.asyncAfter(deadline: .now() + .seconds(2), execute: workItem)
    }
}

final class CodexAppServerApprovalSender {
    func respond(
        threadID: String,
        decision: CodexApprovalDecision
    ) async -> CodexAppServerApprovalOutcome {
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedThreadID.isEmpty else { return .failed }
        guard let loggedRequest = CodexDesktopApprovalLogReader().pendingApproval(for: trimmedThreadID) else {
            return .requestNotFound
        }
        let request = CodexPendingApprovalBroker.shared.details(matching: loggedRequest)

        CodexSendLog.append(
            "approval send started thread=\(trimmedThreadID) request=\(request.requestID) method=\(request.method.rawValue) decision=\(String(describing: decision))"
        )
        let outcome = await CodexFollowerIPCTransport().respond(
            to: request,
            decision: decision
        )

        CodexSendLog.append(
            "approval send finished thread=\(trimmedThreadID) request=\(request.requestID) outcome=\(String(describing: outcome))"
        )
        if outcome == .approved || outcome == .declined {
            CodexPendingApprovalBroker.shared.remove(request)
        }
        return outcome
    }

    func approve(threadID: String) async -> CodexAppServerApprovalOutcome {
        await respond(threadID: threadID, decision: .approveOnce)
    }
}

private final class CodexAppServerApprovalSession {
    private enum RequestID {
        static let initialize = 1
        static let threadList = 2
    }

    private let executableURL: URL
    private let request: CodexPendingApproval
    private let decision: CodexApprovalDecision
    private let completion: @Sendable (CodexAppServerApprovalOutcome) -> Void
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let stateQueue = DispatchQueue(label: "com.silverfire.codexcompanion.approval-session")
    private var codec = CodexWebSocketCodec()
    private var isUpgraded = false
    private var isFinished = false
    private var didCleanUp = false
    private var pollCount = 0
    private var timeoutWorkItem: DispatchWorkItem?
    private var pollWorkItem: DispatchWorkItem?

    init(
        executableURL: URL,
        request: CodexPendingApproval,
        decision: CodexApprovalDecision,
        completion: @escaping @Sendable (CodexAppServerApprovalOutcome) -> Void
    ) {
        self.executableURL = executableURL
        self.request = request
        self.decision = decision
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
            self?.stateQueue.async { self?.read(data) }
        }
        process.terminationHandler = { [weak self] _ in
            self?.stateQueue.async {
                guard let self, !self.isFinished else { return }
                self.finish(.sharedDaemonUnavailable)
            }
        }

        do {
            try process.run()
        } catch {
            finish(.sharedDaemonUnavailable)
            return
        }
        stateQueue.async { [weak self] in
            guard let self else { return }
            scheduleTimeout()
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }
    }

    private func read(_ data: Data) {
        do {
            for event in try codec.receive(data) {
                switch event {
                case .upgraded:
                    isUpgraded = true
                    sendInitialize()
                case .text(let payload):
                    handle(payload)
                case .ping(let payload):
                    writeRaw(CodexWebSocketCodec.clientFrame(opcode: .pong, payload: payload))
                case .close:
                    finish(.sharedDaemonUnavailable)
                }
            }
        } catch {
            finish(.failed)
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

    private func handle(_ payload: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: payload),
            let message = object as? [String: Any]
        else { return }

        if let method = message["method"] as? String {
            handleNotification(method: method, message: message)
            return
        }
        if message["error"] != nil {
            finish(.failed)
            return
        }
        guard let id = numericID(in: message) else { return }
        switch id {
        case RequestID.initialize:
            send(["method": "initialized"])
            send([
                "id": request.requestID,
                "result": CodexApprovalResponseFactory.result(
                    for: decision,
                    request: request
                ),
            ])
            schedulePoll()
        case RequestID.threadList:
            guard let statuses = CodexSharedThreadStatusParser.statuses(from: message) else {
                finish(.failed)
                return
            }
            if statuses[request.threadID] != .waitingOnApproval {
                finish(successOutcome)
            } else if pollCount < 10 {
                schedulePoll()
            } else {
                finish(.timedOut)
            }
        default:
            break
        }
    }

    private func handleNotification(method: String, message: [String: Any]) {
        guard method == "thread/status/changed" else { return }
        guard
            let params = message["params"] as? [String: Any],
            params["threadId"] as? String == request.threadID,
            let status = params["status"] as? [String: Any],
            let type = status["type"] as? String
        else { return }
        let flags = Set(status["activeFlags"] as? [String] ?? [])
        if type != "active" || !flags.contains("waitingOnApproval") {
            finish(successOutcome)
        }
    }

    private var successOutcome: CodexAppServerApprovalOutcome {
        decision == .decline ? .declined : .approved
    }

    private func schedulePoll() {
        pollWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            guard let self, !isFinished else { return }
            pollCount += 1
            send([
                "id": RequestID.threadList,
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
        pollWorkItem = workItem
        stateQueue.asyncAfter(deadline: .now() + .milliseconds(300), execute: workItem)
    }

    private func scheduleTimeout() {
        let workItem = DispatchWorkItem { [weak self] in
            guard let self, !isFinished else { return }
            finish(.timedOut)
        }
        timeoutWorkItem = workItem
        stateQueue.asyncAfter(deadline: .now() + .seconds(8), execute: workItem)
    }

    private func send(_ object: [String: Any]) {
        guard
            isUpgraded,
            let payload = try? JSONSerialization.data(withJSONObject: object)
        else { return }
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: .text, payload: payload))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            finish(.sharedDaemonUnavailable)
        }
    }

    private func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? NSNumber { return id.intValue }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private func finish(_ outcome: CodexAppServerApprovalOutcome) {
        guard !isFinished else { return }
        isFinished = true
        timeoutWorkItem?.cancel()
        pollWorkItem?.cancel()
        completion(outcome)
        cleanup()
    }

    private func cleanup() {
        guard !didCleanUp else { return }
        didCleanUp = true
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
        if process.isRunning { process.terminate() }
        CodexAppServerApprovalSessionRegistry.shared.release(self)
    }
}

private final class CodexPendingApprovalMonitorSession {
    private enum RequestID {
        static let initialize = 1
    }

    private let executableURL: URL
    private let didReceive: @Sendable (CodexPendingApproval) -> Void
    private let didEnd: @Sendable () -> Void
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let stateQueue = DispatchQueue(label: "com.silverfire.codexcompanion.approval-monitor")
    private var codec = CodexWebSocketCodec()
    private var isUpgraded = false
    private var isFinished = false

    init(
        executableURL: URL,
        didReceive: @escaping @Sendable (CodexPendingApproval) -> Void,
        didEnd: @escaping @Sendable () -> Void
    ) {
        self.executableURL = executableURL
        self.didReceive = didReceive
        self.didEnd = didEnd
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
            self?.stateQueue.async { self?.read(data) }
        }
        process.terminationHandler = { [weak self] _ in
            self?.stateQueue.async { self?.finish() }
        }

        do {
            try process.run()
        } catch {
            finish()
            return
        }
        stateQueue.async { [weak self] in
            guard let self else { return }
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }
    }

    private func read(_ data: Data) {
        do {
            for event in try codec.receive(data) {
                switch event {
                case .upgraded:
                    isUpgraded = true
                    sendInitialize()
                case .text(let payload):
                    handle(payload)
                case .ping(let payload):
                    writeRaw(CodexWebSocketCodec.clientFrame(opcode: .pong, payload: payload))
                case .close:
                    finish()
                }
            }
        } catch {
            finish()
        }
    }

    private func sendInitialize() {
        send([
            "id": RequestID.initialize,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "codex-companion-approval-monitor",
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

    private func handle(_ payload: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: payload),
            let message = object as? [String: Any]
        else { return }

        if let request = CodexApprovalRequestParser.pendingApproval(from: message) {
            didReceive(request)
            return
        }
        guard numericID(in: message) == RequestID.initialize, message["error"] == nil else {
            return
        }
        send(["method": "initialized"])
    }

    private func send(_ object: [String: Any]) {
        guard
            isUpgraded,
            let payload = try? JSONSerialization.data(withJSONObject: object)
        else { return }
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: .text, payload: payload))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            finish()
        }
    }

    private func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? NSNumber { return id.intValue }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private func finish() {
        guard !isFinished else { return }
        isFinished = true
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
        if process.isRunning { process.terminate() }
        didEnd()
    }
}

private final class CodexAppServerApprovalSessionRegistry: @unchecked Sendable {
    static let shared = CodexAppServerApprovalSessionRegistry()

    private let lock = NSLock()
    private var sessions: [ObjectIdentifier: CodexAppServerApprovalSession] = [:]

    func retain(_ session: CodexAppServerApprovalSession) {
        lock.lock()
        sessions[ObjectIdentifier(session)] = session
        lock.unlock()
    }

    func release(_ session: CodexAppServerApprovalSession) {
        lock.lock()
        sessions.removeValue(forKey: ObjectIdentifier(session))
        lock.unlock()
    }
}
