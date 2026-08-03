import Foundation

struct CodexTaskStreamEndpoint: Equatable, Sendable {
    var profileID: UUID
    var executableURL: URL
    var environmentOverrides: [String: String]
    var socketURL: URL
}

enum CodexAppServerTaskEventParser {
    private static let maximumNotificationBytes = 512 * 1_024
    private static let maximumTextCharacters = 128 * 1_024
    private static let maximumDetailCharacters = 4 * 1_024

    static func event(from data: Data) -> CodexTaskStreamEvent? {
        guard !data.isEmpty, data.count <= maximumNotificationBytes,
              let raw = try? JSONSerialization.jsonObject(with: data),
              let message = raw as? [String: Any],
              let method = message["method"] as? String,
              let params = message["params"] as? [String: Any],
              let threadID = nonempty(params["threadId"])
        else { return nil }

        switch method {
        case "turn/started":
            guard let turn = params["turn"] as? [String: Any],
                  let turnID = nonempty(turn["id"])
            else { return nil }
            return CodexTaskStreamEvent(
                threadID: threadID,
                turnID: turnID,
                kind: .turnStarted,
                taskStatus: .running
            )

        case "item/agentMessage/delta":
            return deltaEvent(
                params: params,
                threadID: threadID,
                kind: .assistantDelta
            )

        case "item/reasoning/summaryTextDelta":
            return deltaEvent(
                params: params,
                threadID: threadID,
                kind: .reasoningSummaryDelta
            )

        case "item/reasoning/textDelta":
            return nil

        case "item/started", "item/completed":
            guard let turnID = nonempty(params["turnId"]),
                  let rawItem = params["item"] as? [String: Any],
                  let itemID = nonempty(rawItem["id"]),
                  let item = projectedItem(
                    from: rawItem,
                    turnID: turnID,
                    defaultStatus: method == "item/started" ? .inProgress : .completed
                  )
            else { return nil }
            return CodexTaskStreamEvent(
                threadID: threadID,
                turnID: turnID,
                itemID: itemID,
                kind: method == "item/started" ? .itemStarted : .itemCompleted,
                item: item
            )

        case "thread/status/changed":
            guard let status = params["status"] as? [String: Any] else { return nil }
            return CodexTaskStreamEvent(
                threadID: threadID,
                kind: .statusChanged,
                taskStatus: taskStatus(from: status)
            )

        case "turn/completed":
            guard let turn = params["turn"] as? [String: Any],
                  let turnID = nonempty(turn["id"]),
                  let status = nonempty(turn["status"])
            else { return nil }
            if status == "completed" {
                return CodexTaskStreamEvent(
                    threadID: threadID,
                    turnID: turnID,
                    kind: .turnCompleted,
                    taskStatus: .completed
                )
            }
            if status == "failed" || status == "interrupted" {
                let error = turn["error"] as? [String: Any]
                return CodexTaskStreamEvent(
                    threadID: threadID,
                    turnID: turnID,
                    kind: .failed,
                    text: bounded(nonempty(error?["message"]), limit: maximumDetailCharacters),
                    taskStatus: .failed,
                    errorCode: status
                )
            }
            return nil

        default:
            return nil
        }
    }

    private static func deltaEvent(
        params: [String: Any],
        threadID: String,
        kind: CompanionBridgeLiveEventKind
    ) -> CodexTaskStreamEvent? {
        guard let turnID = nonempty(params["turnId"]),
              let itemID = nonempty(params["itemId"]),
              let delta = bounded(nonempty(params["delta"]), limit: maximumTextCharacters)
        else { return nil }
        return CodexTaskStreamEvent(
            threadID: threadID,
            turnID: turnID,
            itemID: itemID,
            kind: kind,
            text: delta
        )
    }

    private static func projectedItem(
        from rawItem: [String: Any],
        turnID: String,
        defaultStatus: CompanionBridgeTimelineItemStatus
    ) -> CompanionBridgeTimelineItem? {
        guard let id = nonempty(rawItem["id"]),
              let type = nonempty(rawItem["type"])
        else { return nil }
        let status = timelineStatus(from: nonempty(rawItem["status"])) ?? defaultStatus

        switch type {
        case "agentMessage":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .message,
                status: status,
                role: .assistant,
                text: bounded(nonempty(rawItem["text"]), limit: maximumTextCharacters),
                phase: messagePhase(from: nonempty(rawItem["phase"])),
                turnID: turnID
            )

        case "plan":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .status,
                status: status,
                title: "Updated progress",
                text: bounded(nonempty(rawItem["text"]), limit: maximumTextCharacters),
                turnID: turnID
            )

        case "reasoning":
            let summaries = (rawItem["summary"] as? [Any])?
                .compactMap(nonempty)
                .map { bounded($0, limit: maximumDetailCharacters) }
                .compactMap { $0 }
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .reasoning,
                status: status,
                title: "Thinking",
                text: summaries?.isEmpty == false ? summaries?.joined(separator: "\n") : nil,
                turnID: turnID
            )

        case "commandExecution":
            let input = safeJSONObject([
                "cmd": nonempty(rawItem["command"]) ?? "",
                "workdir": nonempty(rawItem["cwd"]) ?? "",
            ])
            let projection = CodexMobileToolProjection.project(
                name: "exec_command",
                input: input
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "fileChange":
            let paths = (rawItem["changes"] as? [[String: Any]])?
                .compactMap { nonempty($0["path"]) }
                .prefix(50)
                .joined(separator: "\n")
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: status,
                title: "Edited files",
                detail: bounded(paths, limit: maximumDetailCharacters),
                turnID: turnID,
                callID: id
            )

        case "mcpToolCall":
            let projection = CodexMobileToolProjection.project(
                name: nonempty(rawItem["tool"]) ?? "mcp_tool",
                input: safeJSONValue(rawItem["arguments"]),
                server: nonempty(rawItem["server"])
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "dynamicToolCall":
            let projection = CodexMobileToolProjection.project(
                name: nonempty(rawItem["tool"]) ?? "dynamic_tool",
                input: safeJSONValue(rawItem["arguments"]),
                server: nonempty(rawItem["namespace"])
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "collabAgentToolCall":
            let projection = CodexMobileToolProjection.project(
                name: nonempty(rawItem["tool"]) ?? "agent_tool",
                input: nil
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "subAgentActivity":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .status,
                status: status,
                title: "Agent activity",
                detail: bounded(nonempty(rawItem["kind"]), limit: maximumDetailCharacters),
                turnID: turnID
            )

        case "webSearch":
            let projection = CodexMobileToolProjection.project(
                name: "web__run",
                input: safeJSONObject(["q": nonempty(rawItem["query"]) ?? ""])
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "imageView":
            let projection = CodexMobileToolProjection.project(
                name: "view_image",
                input: safeJSONObject(["path": nonempty(rawItem["path"]) ?? ""])
            )
            return toolItem(
                id: id,
                status: status,
                projection: projection,
                turnID: turnID
            )

        case "imageGeneration":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: status,
                title: "Generated an image",
                turnID: turnID,
                callID: id
            )

        case "sleep":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: status,
                title: "Wait",
                turnID: turnID,
                callID: id
            )

        case "enteredReviewMode":
            return statusItem(id: id, title: "Started review", status: status, turnID: turnID)

        case "exitedReviewMode":
            return statusItem(id: id, title: "Finished review", status: status, turnID: turnID)

        case "contextCompaction":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .compaction,
                status: status,
                title: "Compacted context",
                turnID: turnID
            )

        default:
            return nil
        }
    }

    private static func toolItem(
        id: String,
        status: CompanionBridgeTimelineItemStatus,
        projection: CodexMobileToolProjection,
        turnID: String
    ) -> CompanionBridgeTimelineItem {
        CompanionBridgeTimelineItem(
            id: id,
            kind: .tool,
            status: status,
            title: projection.title,
            detail: bounded(projection.detail, limit: maximumDetailCharacters),
            turnID: turnID,
            callID: id
        )
    }

    private static func statusItem(
        id: String,
        title: String,
        status: CompanionBridgeTimelineItemStatus,
        turnID: String
    ) -> CompanionBridgeTimelineItem {
        CompanionBridgeTimelineItem(
            id: id,
            kind: .status,
            status: status,
            title: title,
            turnID: turnID
        )
    }

    private static func taskStatus(from status: [String: Any]) -> CompanionBridgeTaskStatus {
        switch nonempty(status["type"]) {
        case "active":
            let flags = status["activeFlags"] as? [String] ?? []
            return flags.contains("waitingOnApproval") || flags.contains("waitingOnUserInput")
                ? .waiting
                : .running
        case "idle":
            return .completed
        case "notLoaded", "systemError":
            return .failed
        default:
            return .failed
        }
    }

    private static func timelineStatus(
        from status: String?
    ) -> CompanionBridgeTimelineItemStatus? {
        switch status {
        case "inProgress": .inProgress
        case "completed": .completed
        case "failed", "declined": .failed
        default: nil
        }
    }

    private static func messagePhase(
        from phase: String?
    ) -> CompanionBridgeTimelineItemPhase? {
        switch phase {
        case "commentary": .commentary
        case "final_answer": .final
        default: nil
        }
    }

    private static func safeJSONValue(_ value: Any?) -> String? {
        guard let value, JSONSerialization.isValidJSONObject(["value": value]),
              let data = try? JSONSerialization.data(withJSONObject: value),
              data.count <= maximumDetailCharacters
        else { return nil }
        return String(data: data, encoding: .utf8)
    }

    private static func safeJSONObject(_ value: [String: String]) -> String? {
        guard let data = try? JSONSerialization.data(withJSONObject: value),
              data.count <= maximumDetailCharacters
        else { return nil }
        return String(data: data, encoding: .utf8)
    }

    private static func nonempty(_ value: Any?) -> String? {
        guard let value = value as? String else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    private static func bounded(_ value: String?, limit: Int) -> String? {
        guard let value, !value.isEmpty else { return nil }
        guard value.count > limit else { return value }
        return String(value.prefix(limit))
    }
}

enum CodexAppServerTaskEventDiagnostic {
    static func summary(from data: Data, expectedThreadID: String) -> String? {
        guard let raw = try? JSONSerialization.jsonObject(with: data),
              let message = raw as? [String: Any],
              let method = message["method"] as? String,
              let params = message["params"] as? [String: Any]
        else { return nil }

        let safeMethod = schemaToken(method)
        let keys = params.keys.map(schemaToken).sorted().joined(separator: ",")
        let event = CodexAppServerTaskEventParser.event(from: data)
        let parsed = event?.kind.rawValue ?? "none"
        let matches = event.map { $0.threadID == expectedThreadID }
            .map(String.init) ?? "unknown"
        return "method=\(safeMethod) keys=\(keys) parsed=\(parsed) thread-match=\(matches)"
    }

    private static func schemaToken(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "/._-"))
        let scalars = value.unicodeScalars.prefix(80).filter(allowed.contains)
        let token = String(String.UnicodeScalarView(scalars))
        return token.isEmpty ? "unknown" : token
    }
}

struct CodexAppServerTaskEventClient: CodexTaskEventClient, Sendable {
    var endpoint: CodexTaskStreamEndpoint

    func subscribe(
        threadID: String,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) throws -> any CodexTaskEventStream {
        let session = CodexAppServerTaskEventSession(
            endpoint: endpoint,
            threadID: threadID,
            onEvent: onEvent,
            onTermination: onTermination
        )
        try session.start()
        return session
    }
}

private final class CodexAppServerTaskEventSession: CodexTaskEventStream, @unchecked Sendable {
    private enum RequestID {
        static let initialize = 1
        static let threadResume = 2
        static let threadUnsubscribe = 3
    }

    private let endpoint: CodexTaskStreamEndpoint
    private let threadID: String
    private let onEvent: @Sendable (CodexTaskStreamEvent) -> Void
    private let onTermination: @Sendable (String?) -> Void
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let queue = DispatchQueue(label: "com.silverfire.codexcompanion.task-event-stream")
    private var codec = CodexWebSocketCodec()
    private var hasUpgraded = false
    private var didResume = false
    private var isCancelled = false
    private var didTerminate = false
    private var didCleanUp = false
    private var loggedDiagnosticSummaries: Set<String> = []

    init(
        endpoint: CodexTaskStreamEndpoint,
        threadID: String,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) {
        self.endpoint = endpoint
        self.threadID = threadID
        self.onEvent = onEvent
        self.onTermination = onTermination
    }

    func start() throws {
        process.executableURL = endpoint.executableURL
        process.arguments = CodexAppServerProxyCommand.arguments(socketURL: endpoint.socketURL)
        var environment = ProcessInfo.processInfo.environment
        endpoint.environmentOverrides.forEach { environment[$0.key] = $0.value }
        process.environment = environment
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            self?.queue.async { [weak self] in self?.receive(data) }
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            let detail = String(decoding: data, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            self?.queue.async {
                CodexAccountRuntimeDiagnostics.append(
                    "task-stream stderr \(CodexAccountRuntimeDiagnostics.redact(detail))"
                )
            }
        }
        process.terminationHandler = { [weak self] process in
            self?.queue.async { [weak self] in
                guard let self, !isCancelled else { return }
                finish(reason: "The Codex task stream ended (status \(process.terminationStatus)).")
                cleanup()
            }
        }

        do {
            try process.run()
        } catch {
            cleanup()
            throw CodexAppServerControlError.launchFailed(error.localizedDescription)
        }
        queue.async { [weak self] in
            guard let self else { return }
            writeRaw(CodexWebSocketCodec.handshakeRequest(
                key: CodexWebSocketCodec.randomHandshakeKey()
            ))
        }
    }

    func cancel() {
        queue.async { [weak self] in
            guard let self, !isCancelled else { return }
            isCancelled = true
            if hasUpgraded, didResume {
                send([
                    "id": RequestID.threadUnsubscribe,
                    "method": "thread/unsubscribe",
                    "params": ["threadId": threadID],
                ])
            }
            cleanup()
            if process.isRunning { process.terminate() }
        }
    }

    private func receive(_ data: Data) {
        do {
            for event in try codec.receive(data) {
                switch event {
                case .upgraded:
                    hasUpgraded = true
                    sendInitialize()
                case .text(let payload):
                    handle(payload)
                case .ping(let payload):
                    writeRaw(CodexWebSocketCodec.clientFrame(opcode: .pong, payload: payload))
                case .close:
                    finish(reason: "The Codex task stream closed.")
                    terminateProcess()
                }
            }
        } catch {
            finish(reason: "The Codex task stream could not be decoded.")
            terminateProcess()
        }
    }

    private func sendInitialize() {
        send([
            "id": RequestID.initialize,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "codex-companion-mobile",
                    "title": "Codex Companion Mobile",
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
        if let summary = CodexAppServerTaskEventDiagnostic.summary(
            from: data,
            expectedThreadID: threadID
        ), loggedDiagnosticSummaries.insert(summary).inserted {
            CodexAccountRuntimeDiagnostics.append("task-stream notification \(summary)")
        }
        if let event = CodexAppServerTaskEventParser.event(from: data),
           event.threadID == threadID
        {
            onEvent(event)
            return
        }
        guard let raw = try? JSONSerialization.jsonObject(with: data),
              let message = raw as? [String: Any]
        else { return }

        if let error = message["error"] {
            let detail = CodexAccountRuntimeDiagnostics.redact(String(describing: error))
            finish(reason: "The Codex task stream request failed: \(detail)")
            terminateProcess()
            return
        }
        guard let id = numericID(in: message) else { return }
        switch id {
        case RequestID.initialize:
            send(["method": "initialized"])
            send(CodexAppServerResumeRequestFactory.threadResume(
                id: RequestID.threadResume,
                threadID: threadID,
                cwd: nil
            ))
        case RequestID.threadResume:
            didResume = true
            CodexAccountRuntimeDiagnostics.append("task-stream thread/resume acknowledged")
        case RequestID.threadUnsubscribe:
            break
        default:
            break
        }
    }

    private func numericID(in message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private func send(_ object: [String: Any]) {
        guard hasUpgraded,
              JSONSerialization.isValidJSONObject(object),
              let payload = try? JSONSerialization.data(withJSONObject: object)
        else { return }
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: .text, payload: payload))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            finish(reason: "The Codex task stream could not write to its proxy.")
            terminateProcess()
        }
    }

    private func finish(reason: String?) {
        guard !isCancelled, !didTerminate else { return }
        didTerminate = true
        let safeReason = reason.map(CodexAccountRuntimeDiagnostics.redact)
        onTermination(safeReason)
    }

    private func terminateProcess() {
        cleanup()
        if process.isRunning { process.terminate() }
    }

    private func cleanup() {
        guard !didCleanUp else { return }
        didCleanUp = true
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
    }
}
