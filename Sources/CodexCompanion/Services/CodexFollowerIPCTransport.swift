import Darwin
import Foundation

enum CodexFollowerIPCProtocol {
    static let routerTimeoutMilliseconds = 30_000
    static let responseTimeoutSeconds: TimeInterval = 45
    static let maximumParsedFrameBytes = 4 * 1_024 * 1_024
    static let maximumWireFrameBytes = 256 * 1_024 * 1_024

    static var socketURLs: [URL] {
        socketURLs(
            homeDirectory: FileManager.default.homeDirectoryForCurrentUser,
            temporaryDirectory: FileManager.default.temporaryDirectory
        )
    }

    static var socketURL: URL {
        socketURLs.first(where: { FileManager.default.fileExists(atPath: $0.path) })
            ?? socketURLs[0]
    }

    static func socketURLs(homeDirectory: URL, temporaryDirectory: URL) -> [URL] {
        let current = homeDirectory
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("ipc", isDirectory: true)
            .appendingPathComponent("ipc.sock")
        let legacy = temporaryDirectory
            .appendingPathComponent("codex-ipc", isDirectory: true)
            .appendingPathComponent("ipc-\(getuid()).sock")
        return [current, legacy]
    }

    static func frame(for message: [String: Any]) throws -> Data {
        let payload = try JSONSerialization.data(withJSONObject: message)
        guard payload.count <= Int(UInt32.max) else {
            throw CodexFollowerIPCError.invalidFrame
        }

        var length = UInt32(payload.count).littleEndian
        var result = Data(bytes: &length, count: MemoryLayout<UInt32>.size)
        result.append(payload)
        return result
    }

    static func initializeRequest(requestID: String) -> [String: Any] {
        [
            "type": "request",
            "requestId": requestID,
            "sourceClientId": "initializing-client",
            "version": 0,
            "method": "initialize",
            "params": ["clientType": "codex-companion"],
        ]
    }

    static func actionRequest(
        requestID: String,
        clientID: String,
        threadID: String,
        prompt: String,
        action: CodexSendAction,
        clientMessageID: String,
        cwd: String? = nil,
        attachments: [CodexFollowerAttachment] = []
    ) -> [String: Any] {
        var input: [[String: Any]] = [[
            "type": "text",
            "text": prompt,
            "text_elements": [Any](),
        ]]
        input.append(contentsOf: attachments.compactMap(\.inputItem))
        let nativeAttachments = attachments.map(\.nativeAttachment)
        let fileAttachments = attachments
            .filter { $0.kind == .file }
            .map(\.nativeAttachment)
        let imageAttachments = attachments.compactMap(\.queuedImageAttachment)

        let method: String
        let params: [String: Any]
        switch action {
        case .steer:
            method = "thread-follower-steer-turn"
            let trimmedCWD = cwd?.trimmingCharacters(in: .whitespacesAndNewlines)
            let workspaceRoots = trimmedCWD.flatMap { $0.isEmpty ? nil : $0 }.map { [$0] } ?? []
            var restoreMessage: [String: Any] = [
                "id": clientMessageID,
                "text": prompt,
                "context": [
                    "prompt": prompt,
                    "addedFiles": [Any](),
                    "fileAttachments": fileAttachments,
                    "ideContext": NSNull(),
                    "imageAttachments": imageAttachments,
                    "workspaceRoots": workspaceRoots,
                ],
                "createdAt": Int64(Date().timeIntervalSince1970 * 1_000),
            ]
            if let trimmedCWD, !trimmedCWD.isEmpty {
                restoreMessage["cwd"] = trimmedCWD
            }
            params = [
                "conversationId": threadID,
                "clientUserMessageId": clientMessageID,
                "input": input,
                "serviceTier": NSNull(),
                "attachments": nativeAttachments,
                "restoreMessage": restoreMessage,
            ]
        case .reply:
            method = "thread-follower-start-turn"
            params = [
                "conversationId": threadID,
                "turnStartParams": [
                    "input": input,
                    "clientUserMessageId": clientMessageID,
                    "attachments": nativeAttachments,
                ],
            ]
        }

        return [
            "type": "request",
            "requestId": requestID,
            "sourceClientId": clientID,
            "version": 1,
            "method": method,
            "params": params,
            "timeoutMs": routerTimeoutMilliseconds,
        ]
    }

    static func queuedReplyRequest(
        requestID: String,
        clientID: String,
        threadID: String,
        prompt: String,
        clientMessageID: String,
        cwd: String?,
        existingState: [String: Any],
        attachments: [CodexFollowerAttachment] = [],
        createdAtMilliseconds: Int64 = Int64(Date().timeIntervalSince1970 * 1_000)
    ) throws -> [String: Any] {
        for value in existingState.values where !(value is [[String: Any]]) {
            throw CodexFollowerIPCError.invalidQueuedFollowUpState
        }

        let trimmedCWD = cwd?.trimmingCharacters(in: .whitespacesAndNewlines)
        let workspaceRoots = trimmedCWD.flatMap { $0.isEmpty ? nil : $0 }.map { [$0] } ?? []
        let fileAttachments = attachments
            .filter { $0.kind == .file }
            .map(\.nativeAttachment)
        let imageAttachments = attachments.compactMap(\.queuedImageAttachment)
        var message: [String: Any] = [
            "id": clientMessageID,
            "text": prompt,
            "context": [
                "prompt": prompt,
                "addedFiles": [Any](),
                "fileAttachments": fileAttachments,
                "ideContext": NSNull(),
                "imageAttachments": imageAttachments,
                "workspaceRoots": workspaceRoots,
            ],
            "createdAt": createdAtMilliseconds,
        ]
        if let trimmedCWD, !trimmedCWD.isEmpty {
            message["cwd"] = trimmedCWD
        }

        var state = existingState
        var messages = state[threadID] as? [[String: Any]] ?? []
        if !messages.contains(where: { $0["id"] as? String == clientMessageID }) {
            messages.append(message)
        }
        state[threadID] = messages

        return [
            "type": "request",
            "requestId": requestID,
            "sourceClientId": clientID,
            "version": 1,
            "method": "thread-follower-set-queued-follow-ups-state",
            "params": [
                "conversationId": threadID,
                "state": state,
            ],
            "timeoutMs": routerTimeoutMilliseconds,
        ]
    }

    static func threadSettingsRequest(
        requestID: String,
        clientID: String,
        threadID: String,
        model: String?,
        reasoningEffort: String?
    ) -> [String: Any] {
        var threadSettings: [String: Any] = [:]
        if let model = model?.trimmingCharacters(in: .whitespacesAndNewlines),
           !model.isEmpty {
            threadSettings["model"] = model
        }
        if let reasoningEffort = reasoningEffort?.trimmingCharacters(in: .whitespacesAndNewlines),
           !reasoningEffort.isEmpty {
            threadSettings["effort"] = reasoningEffort
        }

        return [
            "type": "request",
            "requestId": requestID,
            "sourceClientId": clientID,
            "version": 1,
            "method": "thread-follower-update-thread-settings",
            "params": [
                "conversationId": threadID,
                "threadSettings": threadSettings,
            ],
            "timeoutMs": routerTimeoutMilliseconds,
        ]
    }

    static func approvalRequest(
        requestID: String,
        clientID: String,
        request: CodexPendingApproval,
        decision: CodexApprovalDecision
    ) -> [String: Any] {
        let method: String
        switch request.method {
        case .commandExecution:
            method = "thread-follower-command-approval-decision"
        case .fileChange:
            method = "thread-follower-file-approval-decision"
        }
        let response = CodexApprovalResponseFactory.result(for: decision, request: request)

        return [
            "type": "request",
            "requestId": requestID,
            "sourceClientId": clientID,
            "version": 1,
            "method": method,
            "params": [
                "conversationId": request.threadID,
                "requestId": request.requestID,
                "decision": response["decision"] ?? "decline",
            ],
            "timeoutMs": routerTimeoutMilliseconds,
        ]
    }

    static func outcome(forError error: String) -> CodexAppServerSendOutcome {
        let normalized = error.lowercased()
        if normalized.contains("no-client-found")
            || normalized.contains("client-disconnected")
            || normalized.contains("not being streamed")
            || normalized.contains("no client found")
        {
            return .threadNotLoaded
        }
        if normalized.contains("timeout") || normalized.contains("timed out") {
            return .timedOut
        }
        if normalized.contains("active turn")
            || normalized.contains("without an active")
            || normalized.contains("no active turn")
        {
            return .noActiveTurn
        }
        return .failed
    }
}

struct CodexFollowerIPCTransport {
    func updateThreadSettings(
        threadID: String,
        model: String?,
        reasoningEffort: String?
    ) async -> CodexAppServerSendOutcome {
        let result = await perform(
            .threadSettings(
                threadID: threadID,
                model: model,
                reasoningEffort: reasoningEffort
            )
        )

        switch result {
        case .success:
            return .sent
        case .connectionUnavailable:
            return .sharedDaemonUnavailable
        case .timedOut:
            return .timedOut
        case .error(let message):
            return CodexFollowerIPCProtocol.outcome(forError: message)
        case .failed:
            return .failed
        }
    }

    func submit(
        prompt: String,
        threadID: String,
        action: CodexSendAction,
        clientMessageID: String,
        cwd: String? = nil,
        attachments: [CodexFollowerAttachment] = []
    ) async -> CodexAppServerSendOutcome {
        let result = await perform(
            .send(
                prompt: prompt,
                threadID: threadID,
                action: action,
                clientMessageID: clientMessageID,
                cwd: cwd,
                attachments: attachments
            )
        )

        switch result {
        case .success:
            return .sent
        case .connectionUnavailable:
            return .sharedDaemonUnavailable
        case .timedOut:
            return .timedOut
        case .error(let message):
            return CodexFollowerIPCProtocol.outcome(forError: message)
        case .failed:
            return .failed
        }
    }

    func respond(
        to request: CodexPendingApproval,
        decision: CodexApprovalDecision
    ) async -> CodexAppServerApprovalOutcome {
        let result = await perform(.approval(request: request, decision: decision))
        switch result {
        case .success:
            return decision == .decline ? .declined : .approved
        case .connectionUnavailable:
            return .sharedDaemonUnavailable
        case .timedOut:
            return .timedOut
        case .error(let message):
            let normalized = message.lowercased()
            if normalized.contains("request not found")
                || normalized.contains("no pending approval")
                || normalized.contains("approval request") && normalized.contains("not found")
            {
                return .requestNotFound
            }
            return .failed
        case .failed:
            return .failed
        }
    }

    func queueReply(
        prompt: String,
        threadID: String,
        clientMessageID: String,
        cwd: String? = nil,
        attachments: [CodexFollowerAttachment] = [],
        stateURL: URL = CodexQueuedFollowUpStateStore.defaultURL
    ) async -> CodexAppServerSendOutcome {
        let existingState: [String: Any]
        do {
            existingState = try CodexQueuedFollowUpStateStore.load(from: stateURL)
        } catch {
            CodexSendLog.append("native-ipc queued reply refused malformed global state")
            return .failed
        }

        let result = await perform(
            .queuedReply(
                prompt: prompt,
                threadID: threadID,
                clientMessageID: clientMessageID,
                cwd: cwd,
                existingState: existingState,
                attachments: attachments
            )
        )
        switch result {
        case .success:
            return .sent
        case .connectionUnavailable:
            return .sharedDaemonUnavailable
        case .timedOut:
            return .timedOut
        case .error(let message):
            return CodexFollowerIPCProtocol.outcome(forError: message)
        case .failed:
            return .failed
        }
    }

    private func perform(
        _ operation: CodexFollowerIPCOperation
    ) async -> CodexFollowerIPCResult {
        let session = CodexFollowerIPCSession(operation: operation)

        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                DispatchQueue.global(qos: .userInitiated).async {
                    continuation.resume(returning: session.run())
                }
            }
        } onCancel: {
            session.cancel()
        }
    }
}

private enum CodexFollowerIPCOperation {
    case threadSettings(
        threadID: String,
        model: String?,
        reasoningEffort: String?
    )
    case send(
        prompt: String,
        threadID: String,
        action: CodexSendAction,
        clientMessageID: String,
        cwd: String?,
        attachments: [CodexFollowerAttachment]
    )
    case approval(request: CodexPendingApproval, decision: CodexApprovalDecision)

    case queuedReply(
        prompt: String,
        threadID: String,
        clientMessageID: String,
        cwd: String?,
        existingState: [String: Any],
        attachments: [CodexFollowerAttachment]
    )

    func request(requestID: String, clientID: String) throws -> [String: Any] {
        switch self {
        case let .threadSettings(threadID, model, reasoningEffort):
            return CodexFollowerIPCProtocol.threadSettingsRequest(
                requestID: requestID,
                clientID: clientID,
                threadID: threadID,
                model: model,
                reasoningEffort: reasoningEffort
            )
        case let .send(prompt, threadID, action, clientMessageID, cwd, attachments):
            return CodexFollowerIPCProtocol.actionRequest(
                requestID: requestID,
                clientID: clientID,
                threadID: threadID,
                prompt: prompt,
                action: action,
                clientMessageID: clientMessageID,
                cwd: cwd,
                attachments: attachments
            )
        case let .approval(request, decision):
            return CodexFollowerIPCProtocol.approvalRequest(
                requestID: requestID,
                clientID: clientID,
                request: request,
                decision: decision
            )
        case let .queuedReply(prompt, threadID, clientMessageID, cwd, existingState, attachments):
            return try CodexFollowerIPCProtocol.queuedReplyRequest(
                requestID: requestID,
                clientID: clientID,
                threadID: threadID,
                prompt: prompt,
                clientMessageID: clientMessageID,
                cwd: cwd,
                existingState: existingState,
                attachments: attachments
            )
        }
    }
}

private enum CodexFollowerIPCResult: Sendable {
    case success
    case connectionUnavailable
    case timedOut
    case error(String)
    case failed
}

enum CodexFollowerIPCError: Error, Equatable {
    case cancelled
    case connectionUnavailable
    case invalidFrame
    case invalidQueuedFollowUpState
    case invalidResponse
    case timedOut
    case transportFailure
}

private final class CodexFollowerIPCSession: @unchecked Sendable {
    private let operation: CodexFollowerIPCOperation
    private let lock = NSLock()
    private var socketFD: Int32 = -1
    private var isCancelled = false

    init(operation: CodexFollowerIPCOperation) {
        self.operation = operation
    }

    func run() -> CodexFollowerIPCResult {
        do {
            let fd = try connectSocket()
            guard install(fd: fd) else {
                Darwin.close(fd)
                return .failed
            }
            defer { closeIfInstalled(fd: fd) }

            let deadline = Date().addingTimeInterval(CodexFollowerIPCProtocol.responseTimeoutSeconds)
            let initializeID = UUID().uuidString
            try send(
                CodexFollowerIPCProtocol.initializeRequest(requestID: initializeID),
                to: fd
            )
            let initializeResponse = try waitForResponse(
                requestID: initializeID,
                from: fd,
                deadline: deadline
            )
            guard responseSucceeded(initializeResponse) else {
                return responseResult(initializeResponse)
            }
            guard
                let result = initializeResponse["result"] as? [String: Any],
                let clientID = result["clientId"] as? String,
                !clientID.isEmpty
            else {
                return .failed
            }

            let actionID = UUID().uuidString
            try send(
                try operation.request(requestID: actionID, clientID: clientID),
                to: fd
            )
            let actionResponse = try waitForResponse(
                requestID: actionID,
                from: fd,
                deadline: deadline
            )
            return responseSucceeded(actionResponse) ? .success : responseResult(actionResponse)
        } catch CodexFollowerIPCError.connectionUnavailable {
            return .connectionUnavailable
        } catch CodexFollowerIPCError.timedOut {
            return .timedOut
        } catch CodexFollowerIPCError.cancelled {
            return .failed
        } catch {
            return .failed
        }
    }

    func cancel() {
        lock.lock()
        isCancelled = true
        let fd = socketFD
        socketFD = -1
        lock.unlock()

        if fd >= 0 {
            Darwin.shutdown(fd, SHUT_RDWR)
            Darwin.close(fd)
        }
    }

    private func connectSocket() throws -> Int32 {
        for socketURL in CodexFollowerIPCProtocol.socketURLs {
            try checkCancellation()
            do {
                return try connectSocket(at: socketURL)
            } catch CodexFollowerIPCError.connectionUnavailable {
                continue
            }
        }
        throw CodexFollowerIPCError.connectionUnavailable
    }

    private func connectSocket(at socketURL: URL) throws -> Int32 {
        let path = socketURL.path
        let pathBytes = Array(path.utf8CString)
        var address = sockaddr_un()
        guard pathBytes.count <= MemoryLayout.size(ofValue: address.sun_path) else {
            throw CodexFollowerIPCError.connectionUnavailable
        }

        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw CodexFollowerIPCError.connectionUnavailable }

        var noSignal: Int32 = 1
        _ = withUnsafePointer(to: &noSignal) {
            Darwin.setsockopt(
                fd,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                $0,
                socklen_t(MemoryLayout<Int32>.size)
            )
        }

        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        withUnsafeMutableBytes(of: &address.sun_path) { destination in
            pathBytes.withUnsafeBytes { source in
                destination.copyBytes(from: source)
            }
        }

        let connectResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connectResult == 0 else {
            Darwin.close(fd)
            throw CodexFollowerIPCError.connectionUnavailable
        }
        return fd
    }

    private func send(_ message: [String: Any], to fd: Int32) throws {
        let frame = try CodexFollowerIPCProtocol.frame(for: message)
        try frame.withUnsafeBytes { rawBuffer in
            guard let baseAddress = rawBuffer.baseAddress else {
                throw CodexFollowerIPCError.invalidFrame
            }
            var offset = 0
            while offset < rawBuffer.count {
                try checkCancellation()
                let written = Darwin.send(
                    fd,
                    baseAddress.advanced(by: offset),
                    rawBuffer.count - offset,
                    0
                )
                if written > 0 {
                    offset += written
                } else if written < 0 && errno == EINTR {
                    continue
                } else {
                    throw CodexFollowerIPCError.transportFailure
                }
            }
        }
    }

    private func waitForResponse(
        requestID: String,
        from fd: Int32,
        deadline: Date
    ) throws -> [String: Any] {
        while true {
            try checkCancellation()
            let frameLength = try readFrameLength(from: fd, deadline: deadline)
            guard frameLength <= CodexFollowerIPCProtocol.maximumWireFrameBytes else {
                throw CodexFollowerIPCError.invalidFrame
            }

            if frameLength > CodexFollowerIPCProtocol.maximumParsedFrameBytes {
                try drain(byteCount: frameLength, from: fd, deadline: deadline)
                continue
            }

            let payload = try readExactly(byteCount: frameLength, from: fd, deadline: deadline)
            guard
                let message = try JSONSerialization.jsonObject(with: payload) as? [String: Any]
            else {
                continue
            }
            guard message["type"] as? String == "response" else { continue }
            guard message["requestId"] as? String == requestID else { continue }
            return message
        }
    }

    private func readFrameLength(from fd: Int32, deadline: Date) throws -> Int {
        let header = try readExactly(
            byteCount: MemoryLayout<UInt32>.size,
            from: fd,
            deadline: deadline
        )
        return Int(header[0])
            | (Int(header[1]) << 8)
            | (Int(header[2]) << 16)
            | (Int(header[3]) << 24)
    }

    private func readExactly(byteCount: Int, from fd: Int32, deadline: Date) throws -> Data {
        var result = Data(count: byteCount)
        var offset = 0
        try result.withUnsafeMutableBytes { rawBuffer in
            guard let baseAddress = rawBuffer.baseAddress else { return }
            while offset < byteCount {
                try waitUntilReadable(fd: fd, deadline: deadline)
                let count = Darwin.read(
                    fd,
                    baseAddress.advanced(by: offset),
                    byteCount - offset
                )
                if count > 0 {
                    offset += count
                } else if count < 0 && errno == EINTR {
                    continue
                } else {
                    throw CodexFollowerIPCError.transportFailure
                }
            }
        }
        return result
    }

    private func drain(byteCount: Int, from fd: Int32, deadline: Date) throws {
        var remaining = byteCount
        var buffer = [UInt8](repeating: 0, count: 64 * 1_024)
        while remaining > 0 {
            try waitUntilReadable(fd: fd, deadline: deadline)
            let chunkSize = min(remaining, buffer.count)
            let count = buffer.withUnsafeMutableBytes {
                Darwin.read(fd, $0.baseAddress, chunkSize)
            }
            if count > 0 {
                remaining -= count
            } else if count < 0 && errno == EINTR {
                continue
            } else {
                throw CodexFollowerIPCError.transportFailure
            }
        }
    }

    private func waitUntilReadable(fd: Int32, deadline: Date) throws {
        while true {
            try checkCancellation()
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { throw CodexFollowerIPCError.timedOut }
            let timeout = Int32(min(remaining * 1_000, Double(Int32.max)).rounded(.up))
            var descriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let result = Darwin.poll(&descriptor, 1, timeout)
            if result > 0 {
                if descriptor.revents & Int16(POLLIN) != 0 { return }
                throw CodexFollowerIPCError.transportFailure
            }
            if result == 0 { throw CodexFollowerIPCError.timedOut }
            if errno != EINTR { throw CodexFollowerIPCError.transportFailure }
        }
    }

    private func responseSucceeded(_ response: [String: Any]) -> Bool {
        response["resultType"] as? String == "success"
    }

    private func responseResult(_ response: [String: Any]) -> CodexFollowerIPCResult {
        if let error = response["error"] as? String {
            return .error(error)
        }
        if let error = response["error"] as? [String: Any] {
            let message = error["message"] as? String
                ?? error["code"] as? String
                ?? String(describing: error)
            return .error(message)
        }
        return .failed
    }

    private func install(fd: Int32) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard !isCancelled else { return false }
        socketFD = fd
        return true
    }

    private func closeIfInstalled(fd: Int32) {
        lock.lock()
        let shouldClose = socketFD == fd
        if shouldClose { socketFD = -1 }
        lock.unlock()

        if shouldClose { Darwin.close(fd) }
    }

    private func checkCancellation() throws {
        lock.lock()
        let cancelled = isCancelled
        lock.unlock()
        if cancelled || Task.isCancelled {
            throw CodexFollowerIPCError.cancelled
        }
    }
}

enum CodexQueuedFollowUpStateStore {
    static var defaultURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent(".codex-global-state.json")
    }

    static func load(from url: URL) throws -> [String: Any] {
        guard FileManager.default.fileExists(atPath: url.path) else { return [:] }
        let data = try Data(contentsOf: url)
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw CodexFollowerIPCError.invalidQueuedFollowUpState
        }
        guard let rawState = root["queued-follow-ups"] else { return [:] }
        guard let state = rawState as? [String: Any] else {
            throw CodexFollowerIPCError.invalidQueuedFollowUpState
        }
        for value in state.values where !(value is [[String: Any]]) {
            throw CodexFollowerIPCError.invalidQueuedFollowUpState
        }
        return state
    }
}
