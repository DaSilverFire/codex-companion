import Foundation

enum CodexThreadRuntimeStatus: String, Codable, Equatable, Hashable, Sendable {
    case notLoaded
    case idle
    case active
    case waitingOnApproval
    case waitingOnUserInput
    case systemError
}

enum CodexSharedThreadStatusParser {
    static func statuses(from message: [String: Any]) -> [String: CodexThreadRuntimeStatus]? {
        guard
            let result = message["result"] as? [String: Any],
            let threads = result["data"] as? [[String: Any]]
        else {
            return nil
        }

        return threads.reduce(into: [:]) { statuses, thread in
            guard
                let id = thread["id"] as? String,
                let rawStatus = thread["status"] as? [String: Any],
                let type = rawStatus["type"] as? String
            else {
                return
            }

            switch type {
            case "active":
                let flags = Set(rawStatus["activeFlags"] as? [String] ?? [])
                if flags.contains("waitingOnApproval") {
                    statuses[id] = .waitingOnApproval
                } else if flags.contains("waitingOnUserInput") {
                    statuses[id] = .waitingOnUserInput
                } else {
                    statuses[id] = .active
                }
            case "idle":
                statuses[id] = .idle
            case "systemError":
                statuses[id] = .systemError
            default:
                statuses[id] = .notLoaded
            }
        }
    }
}

struct CodexSharedThreadRuntimeReader: Sendable {
    var timeout: TimeInterval = 4

    func read() throws -> [String: CodexThreadRuntimeStatus] {
        let approvalFallback = Self.approvalFallbackStatuses(
            CodexDesktopApprovalLogReader().pendingApprovals()
        )
        guard FileManager.default.fileExists(atPath: CodexAppServerSender.sharedDaemonSocketURL.path) else {
            return approvalFallback
        }
        guard let executableURL = WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        }) else {
            return approvalFallback
        }

        do {
            return try CodexSharedThreadRuntimeSession(
                executableURL: executableURL,
                timeout: timeout
            ).run()
        } catch {
            return approvalFallback
        }
    }

    static func approvalFallbackStatuses(
        _ approvals: [String: CodexPendingApproval]
    ) -> [String: CodexThreadRuntimeStatus] {
        approvals.reduce(into: [:]) { statuses, entry in
            statuses[entry.key] = .waitingOnApproval
        }
    }
}

private enum CodexSharedThreadRuntimeError: Error {
    case launchFailed
    case timedOut
    case disconnected
    case invalidResponse
}

private final class CodexSharedThreadRuntimeSession {
    private enum RequestID {
        static let initialize = 1
        static let threadList = 2
    }

    private let executableURL: URL
    private let timeout: TimeInterval
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let stateQueue = DispatchQueue(label: "com.silverfire.codexcompanion.thread-runtime")
    private let completion = DispatchSemaphore(value: 0)
    private var webSocketCodec = CodexWebSocketCodec()
    private var statuses: [String: CodexThreadRuntimeStatus]?
    private var failure: Error?
    private var isFinished = false

    init(executableURL: URL, timeout: TimeInterval) {
        self.executableURL = executableURL
        self.timeout = timeout
    }

    func run() throws -> [String: CodexThreadRuntimeStatus] {
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
        process.terminationHandler = { [weak self] _ in
            self?.stateQueue.async { [weak self] in
                self?.finish(error: CodexSharedThreadRuntimeError.disconnected)
            }
        }

        do {
            try process.run()
        } catch {
            cleanup()
            throw CodexSharedThreadRuntimeError.launchFailed
        }

        stateQueue.async { [weak self] in
            guard let self else { return }
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }

        if completion.wait(timeout: .now() + timeout) == .timedOut {
            stateQueue.sync {
                finish(error: CodexSharedThreadRuntimeError.timedOut)
            }
        }

        let outcome = stateQueue.sync { () -> ([String: CodexThreadRuntimeStatus]?, Error?) in
            cleanup()
            return (statuses, failure)
        }
        if let error = outcome.1 {
            throw error
        }
        guard let statuses = outcome.0 else {
            throw CodexSharedThreadRuntimeError.invalidResponse
        }
        return statuses
    }

    private func readStdout(_ data: Data) {
        do {
            for event in try webSocketCodec.receive(data) {
                switch event {
                case .upgraded:
                    sendInitialize()
                case .text(let payload):
                    handle(payload)
                case .ping(let payload):
                    writeRaw(CodexWebSocketCodec.clientFrame(opcode: .pong, payload: payload))
                case .close:
                    finish(error: CodexSharedThreadRuntimeError.disconnected)
                }
            }
        } catch {
            finish(error: error)
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

    private func handle(_ data: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let message = object as? [String: Any]
        else {
            return
        }
        if message["error"] != nil {
            finish(error: CodexSharedThreadRuntimeError.invalidResponse)
            return
        }
        guard let id = numericID(in: message) else { return }

        switch id {
        case RequestID.initialize:
            send(["method": "initialized"])
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
        case RequestID.threadList:
            guard let parsed = CodexSharedThreadStatusParser.statuses(from: message) else {
                finish(error: CodexSharedThreadRuntimeError.invalidResponse)
                return
            }
            statuses = parsed
            finish(error: nil)
        default:
            break
        }
    }

    private func send(_ object: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: object) else {
            finish(error: CodexSharedThreadRuntimeError.invalidResponse)
            return
        }
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: .text, payload: data))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            finish(error: error)
        }
    }

    private func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? NSNumber { return id.intValue }
        return nil
    }

    private func finish(error: Error?) {
        guard !isFinished else { return }
        isFinished = true
        failure = error
        completion.signal()
    }

    private func cleanup() {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
        if process.isRunning {
            process.terminate()
        }
    }
}
