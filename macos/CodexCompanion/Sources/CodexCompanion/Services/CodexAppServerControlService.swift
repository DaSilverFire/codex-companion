import Foundation

final class CodexAppServerControlService: Sendable {
    static let shared = CodexAppServerControlService()

    private let client: CodexAppServerRPCClient

    init(client: CodexAppServerRPCClient = CodexAppServerRPCClient()) {
        self.client = client
    }

    func readGoals(threadIDs: [String]) throws -> [String: CodexGoalSnapshot?] {
        let uniqueThreadIDs = Array(Set(threadIDs.map {
            $0.trimmingCharacters(in: .whitespacesAndNewlines)
        })).filter { !$0.isEmpty }.sorted()
        guard !uniqueThreadIDs.isEmpty else { return [:] }

        let requests = uniqueThreadIDs.enumerated().map { offset, threadID in
            CodexControlRequestFactory.goalGet(id: offset + 2, threadID: threadID)
        }
        let responses = try client.perform(requests)
        var goals: [String: CodexGoalSnapshot?] = [:]

        for (offset, threadID) in uniqueThreadIDs.enumerated() {
            let response = try responseData(for: offset + 2, in: responses)
            let envelope = try JSONDecoder().decode(GoalGetEnvelope.self, from: response)
            goals.updateValue(envelope.goal, forKey: threadID)
        }
        return goals
    }

    func setGoal(
        threadID: String,
        objective: String?,
        status: CodexGoalStatus?,
        tokenBudget: Int?
    ) throws -> CodexGoalSnapshot {
        let request = CodexControlRequestFactory.goalSet(
            id: 2,
            threadID: threadID,
            objective: objective,
            status: status,
            tokenBudget: tokenBudget
        )
        let responses = try client.perform([request])
        let data = try responseData(for: 2, in: responses)
        return try JSONDecoder().decode(GoalSetEnvelope.self, from: data).goal
    }

    func readRateLimits<T: Decodable>(as type: T.Type) throws -> T {
        let responses = try client.perform([CodexControlRequestFactory.rateLimitsRead(id: 2)])
        let data = try responseData(for: 2, in: responses)
        return try JSONDecoder().decode(type, from: data)
    }

    func consumeResetCredit(
        creditID: String,
        idempotencyKey: UUID
    ) throws -> CodexResetConsumeOutcome {
        let trimmedCreditID = creditID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedCreditID.isEmpty else {
            throw CodexAppServerControlError.invalidCreditID
        }
        let request = CodexControlRequestFactory.consumeReset(
            id: 2,
            creditID: trimmedCreditID,
            idempotencyKey: idempotencyKey.uuidString
        )
        let responses = try client.perform([request])
        let data = try responseData(for: 2, in: responses)
        return try JSONDecoder().decode(ResetConsumeEnvelope.self, from: data).outcome
    }

    private func responseData(
        for id: Int,
        in responses: [Int: CodexRPCResponse]
    ) throws -> Data {
        guard let response = responses[id] else {
            throw CodexAppServerControlError.missingResponse(id)
        }
        if let error = response.error {
            throw CodexAppServerControlError.server(error)
        }
        guard let result = response.result, JSONSerialization.isValidJSONObject(result) else {
            throw CodexAppServerControlError.invalidResponse
        }
        return try JSONSerialization.data(withJSONObject: result)
    }
}

struct CodexAppServerRPCClient: Sendable {
    var executableURLProvider: @Sendable () -> URL? = {
        WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        })
    }
    var timeout: TimeInterval = 20

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        guard let executableURL = executableURLProvider() else {
            throw CodexAppServerControlError.missingExecutable
        }
        let session = CodexAppServerRPCSession(
            executableURL: executableURL,
            requests: requests,
            timeout: timeout
        )
        return try session.run()
    }
}

struct CodexRPCResponse {
    var result: [String: Any]?
    var error: String?
}

enum CodexAppServerControlError: LocalizedError {
    case missingExecutable
    case launchFailed(String)
    case timedOut
    case processExited(Int32)
    case invalidResponse
    case missingResponse(Int)
    case server(String)
    case invalidCreditID

    var errorDescription: String? {
        switch self {
        case .missingExecutable:
            return "The ChatGPT Codex executable could not be found."
        case .launchFailed(let message):
            return "Codex app-server could not start: \(message)"
        case .timedOut:
            return "Codex app-server did not respond in time."
        case .processExited(let status):
            return "Codex app-server exited with status \(status)."
        case .invalidResponse:
            return "Codex app-server returned an unreadable response."
        case .missingResponse(let id):
            return "Codex app-server omitted response \(id)."
        case .server(let message):
            return message
        case .invalidCreditID:
            return "Choose an available Codex reset first."
        }
    }
}

private final class CodexAppServerRPCSession {
    private let executableURL: URL
    private let requests: [CodexRPCRequest]
    private let timeout: TimeInterval
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let lock = NSLock()
    private let completion = DispatchSemaphore(value: 0)
    private var stdoutBuffer = Data()
    private var responses: [Int: CodexRPCResponse] = [:]
    private var failure: Error?
    private var didInitialize = false
    private var didFinish = false

    init(executableURL: URL, requests: [CodexRPCRequest], timeout: TimeInterval) {
        self.executableURL = executableURL
        self.requests = requests
        self.timeout = timeout
    }

    func run() throws -> [Int: CodexRPCResponse] {
        process.executableURL = executableURL
        process.arguments = ["app-server", "--listen", "stdio://"]
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.read(handle.availableData)
        }
        process.terminationHandler = { [weak self] process in
            self?.finishIfNeeded(error: CodexAppServerControlError.processExited(process.terminationStatus))
        }

        do {
            try process.run()
        } catch {
            cleanup()
            throw CodexAppServerControlError.launchFailed(error.localizedDescription)
        }

        send([
            "id": 1,
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

        let waitResult = completion.wait(timeout: .now() + timeout)
        if waitResult == .timedOut {
            finishIfNeeded(error: CodexAppServerControlError.timedOut)
        }
        cleanup()
        if process.isRunning {
            process.terminate()
        }

        if let failure {
            throw failure
        }
        return responses
    }

    private func read(_ data: Data) {
        guard !data.isEmpty else { return }
        lock.lock()
        stdoutBuffer.append(data)
        let newline = Data([0x0A])
        var lines: [Data] = []
        while let range = stdoutBuffer.firstRange(of: newline) {
            lines.append(stdoutBuffer.subdata(in: stdoutBuffer.startIndex..<range.lowerBound))
            stdoutBuffer.removeSubrange(stdoutBuffer.startIndex..<range.upperBound)
        }
        lock.unlock()

        for line in lines where !line.isEmpty {
            handle(line)
        }
    }

    private func handle(_ data: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let message = object as? [String: Any],
            let id = Self.responseID(from: message)
        else { return }

        if id == 1 {
            guard message["error"] == nil else {
                finishIfNeeded(error: CodexAppServerControlError.server(Self.errorText(from: message)))
                return
            }
            send(["method": "initialized"])
            lock.lock()
            let shouldSend = !didInitialize
            didInitialize = true
            lock.unlock()
            if shouldSend {
                requests.forEach { send($0.jsonObject) }
                if requests.isEmpty {
                    finishIfNeeded(error: nil)
                }
            }
            return
        }

        guard requests.contains(where: { $0.id == id }) else { return }
        let response: CodexRPCResponse
        if message["error"] != nil {
            response = CodexRPCResponse(result: nil, error: Self.errorText(from: message))
        } else {
            response = CodexRPCResponse(result: message["result"] as? [String: Any], error: nil)
        }

        lock.lock()
        responses[id] = response
        let complete = responses.count == requests.count
        lock.unlock()
        if complete {
            finishIfNeeded(error: nil)
        }
    }

    private func send(_ object: [String: Any]) {
        guard
            JSONSerialization.isValidJSONObject(object),
            let data = try? JSONSerialization.data(withJSONObject: object)
        else {
            finishIfNeeded(error: CodexAppServerControlError.invalidResponse)
            return
        }
        stdinPipe.fileHandleForWriting.write(data)
        stdinPipe.fileHandleForWriting.write(Data([0x0A]))
    }

    private func finishIfNeeded(error: Error?) {
        lock.lock()
        defer { lock.unlock() }
        guard !didFinish else { return }
        didFinish = true
        failure = error
        completion.signal()
    }

    private func cleanup() {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
    }

    private static func responseID(from message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private static func errorText(from message: [String: Any]) -> String {
        guard let error = message["error"] as? [String: Any] else {
            return "Codex app-server request failed."
        }
        return error["message"] as? String ?? String(describing: error)
    }
}

private struct GoalGetEnvelope: Decodable {
    var goal: CodexGoalSnapshot?
}

private struct GoalSetEnvelope: Decodable {
    var goal: CodexGoalSnapshot
}

private struct ResetConsumeEnvelope: Decodable {
    var outcome: CodexResetConsumeOutcome
}
