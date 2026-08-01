import Foundation

enum CodexAppServerProxyCommand {
    static func arguments(socketURL: URL?) -> [String] {
        guard let socketURL else {
            return ["app-server", "proxy"]
        }
        return ["app-server", "proxy", "--sock", socketURL.path]
    }

    static func initializeRequestID(excluding requestIDs: [Int]) -> Int {
        let reservedIDs = Set(requestIDs)
        var candidate = 0
        while reservedIDs.contains(candidate) {
            candidate -= 1
        }
        return candidate
    }
}

struct CodexAppServerProxyRPCClient: CodexAppServerRPCPerforming, Sendable {
    var executableURL: URL
    var environmentOverrides: [String: String]
    var socketURL: URL? = nil
    var timeout: TimeInterval = 30
    var logContext: String

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        try CodexAppServerProxyRPCSession(
            executableURL: executableURL,
            environmentOverrides: environmentOverrides,
            socketURL: socketURL,
            requests: requests,
            timeout: timeout,
            logContext: logContext
        ).run()
    }
}

enum CodexAccountRuntimeDiagnostics {
    static func append(_ message: String) {
        let safeMessage = redact(message)
        NSLog("CodexAccountRuntime: %@", safeMessage)
        CodexSendLog.append("account-runtime \(safeMessage)")
    }

    static func redact(_ text: String) -> String {
        var result = text
        let patterns = [
            #"(?i)(\"?(?:access_token|refresh_token|id_token|api_key|token)\"?\s*[:=]\s*\"?)[^\"\s,}]+"#,
            #"\bsk-[A-Za-z0-9_-]+\b"#,
        ]
        for pattern in patterns {
            guard let expression = try? NSRegularExpression(pattern: pattern) else { continue }
            let range = NSRange(result.startIndex..<result.endIndex, in: result)
            result = expression.stringByReplacingMatches(
                in: result,
                range: range,
                withTemplate: "$1<redacted>"
            )
        }
        return result
    }
}

private final class CodexAppServerProxyRPCSession {
    private let executableURL: URL
    private let environmentOverrides: [String: String]
    private let socketURL: URL?
    private let requests: [CodexRPCRequest]
    private let initializeRequestID: Int
    private let timeout: TimeInterval
    private let logContext: String
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let stateQueue = DispatchQueue(
        label: "com.silverfire.codexcompanion.profile-proxy-rpc"
    )
    private let completion = DispatchSemaphore(value: 0)
    private var webSocketCodec = CodexWebSocketCodec()
    private var responses: [Int: CodexRPCResponse] = [:]
    private var failure: Error?
    private var isFinished = false

    init(
        executableURL: URL,
        environmentOverrides: [String: String],
        socketURL: URL?,
        requests: [CodexRPCRequest],
        timeout: TimeInterval,
        logContext: String
    ) {
        self.executableURL = executableURL
        self.environmentOverrides = environmentOverrides
        self.socketURL = socketURL
        self.requests = requests
        initializeRequestID = CodexAppServerProxyCommand.initializeRequestID(
            excluding: requests.map(\.id)
        )
        self.timeout = timeout
        self.logContext = logContext
    }

    func run() throws -> [Int: CodexRPCResponse] {
        process.executableURL = executableURL
        process.arguments = CodexAppServerProxyCommand.arguments(socketURL: socketURL)
        var environment = ProcessInfo.processInfo.environment
        environmentOverrides.forEach { environment[$0.key] = $0.value }
        process.environment = environment
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
            guard let text = String(data: data, encoding: .utf8), !text.isEmpty else { return }
            self?.stateQueue.async { [weak self] in
                self?.log("proxy stderr \(text.trimmingCharacters(in: .whitespacesAndNewlines))")
            }
        }
        process.terminationHandler = { [weak self] process in
            self?.stateQueue.async { [weak self] in
                self?.finish(
                    error: CodexAppServerControlError.processExited(process.terminationStatus)
                )
            }
        }

        do {
            try process.run()
        } catch {
            cleanup()
            throw CodexAppServerControlError.launchFailed(error.localizedDescription)
        }
        log("proxy launched pid=\(process.processIdentifier) executable=\(executableURL.path)")

        stateQueue.async { [weak self] in
            guard let self else { return }
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }

        if completion.wait(timeout: .now() + timeout) == .timedOut {
            stateQueue.sync {
                finish(error: CodexAppServerControlError.timedOut)
            }
        }

        let outcome = stateQueue.sync { () -> ([Int: CodexRPCResponse], Error?) in
            cleanup()
            return (responses, failure)
        }
        if let error = outcome.1 {
            throw error
        }
        return outcome.0
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
                    finish(error: CodexAppServerControlError.processExited(-1))
                }
            }
        } catch {
            finish(error: error)
        }
    }

    private func sendInitialize() {
        send([
            "id": initializeRequestID,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "codex-companion-profile",
                    "title": "Codex Companion Profile",
                    "version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
                        ?? "0",
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
            let message = object as? [String: Any],
            let id = numericID(in: message)
        else {
            return
        }

        if id == initializeRequestID {
            if message["error"] != nil {
                finish(error: CodexAppServerControlError.server(errorText(from: message)))
                return
            }
            send(["method": "initialized"])
            requests.forEach { send($0.jsonObject) }
            if requests.isEmpty {
                finish(error: nil)
            }
            return
        }

        guard requests.contains(where: { $0.id == id }) else { return }
        let response: CodexRPCResponse
        if message["error"] != nil {
            response = CodexRPCResponse(result: nil, error: errorText(from: message))
        } else {
            response = CodexRPCResponse(
                result: message["result"] as? [String: Any],
                error: nil
            )
        }
        responses[id] = response
        if responses.count == requests.count {
            finish(error: nil)
        }
    }

    private func send(_ object: [String: Any]) {
        guard
            JSONSerialization.isValidJSONObject(object),
            let data = try? JSONSerialization.data(withJSONObject: object)
        else {
            finish(error: CodexAppServerControlError.invalidResponse)
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
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private func errorText(from message: [String: Any]) -> String {
        guard let error = message["error"] else {
            return "Codex app-server request failed."
        }
        if JSONSerialization.isValidJSONObject(error),
           let data = try? JSONSerialization.data(withJSONObject: error),
           let text = String(data: data, encoding: .utf8) {
            return CodexAccountRuntimeDiagnostics.redact(text)
        }
        return CodexAccountRuntimeDiagnostics.redact(String(describing: error))
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

    private func log(_ message: String) {
        CodexAccountRuntimeDiagnostics.append("\(logContext) \(message)")
    }
}
