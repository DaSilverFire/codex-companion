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

enum CodexAppServerLoadedThreadState: Equatable, Sendable {
    case loaded
    case needsResume
}

enum CodexAppServerRequestPolicy {
    static let turnItemsView = "notLoaded"
    static let turnDiscoveryTimeoutSeconds = 30
    static let threadResumeTimeoutSeconds = 30

    static func requiresTurnDiscovery(before action: CodexSendAction) -> Bool {
        switch action {
        case .reply, .steer:
            true
        }
    }
}

enum CodexAppServerLoadedThreadNextStep: Equatable, Sendable {
    case discoverCurrentTurn
    case steerExistingTurn(String)
    case queueReplyAfterCurrentTurn
    case startTurn
}

enum CodexAppServerLoadedThreadRoutePlanner {
    static func nextStep(
        action: CodexSendAction,
        snapshotTurnID: String?,
        resumedThread: Bool
    ) -> CodexAppServerLoadedThreadNextStep {
        let snapshotTurnID = snapshotTurnID?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if !resumedThread, let snapshotTurnID, !snapshotTurnID.isEmpty {
            switch action {
            case .reply:
                return .queueReplyAfterCurrentTurn
            case .steer:
                return .steerExistingTurn(snapshotTurnID)
            }
        }

        // A turn captured before an unloaded task resumes may no longer be current.
        return CodexAppServerRequestPolicy.requiresTurnDiscovery(before: action)
            ? .discoverCurrentTurn
            : .startTurn
    }
}

enum CodexAppServerSteerRecoveryPolicy {
    static func replacementTurnID(
        errorMessage: String,
        attemptedTurnID: String,
        hasRetried: Bool
    ) -> String? {
        guard !hasRetried else { return nil }
        guard let expectedMarker = errorMessage.range(
            of: "expected active turn id `",
            options: .caseInsensitive
        ) else { return nil }
        let expectedSuffix = errorMessage[expectedMarker.upperBound...]
        guard let expectedEnd = expectedSuffix.firstIndex(of: "`") else { return nil }
        let expected = String(expectedSuffix[..<expectedEnd])

        guard let foundMarker = errorMessage.range(
            of: "but found `",
            options: .caseInsensitive,
            range: expectedEnd..<errorMessage.endIndex
        ) else { return nil }
        let foundSuffix = errorMessage[foundMarker.upperBound...]
        guard let foundEnd = foundSuffix.firstIndex(of: "`") else { return nil }
        let found = String(foundSuffix[..<foundEnd])

        guard expected == attemptedTurnID, !found.isEmpty, found != attemptedTurnID else {
            return nil
        }
        return found
    }
}

struct CodexAppServerResponseParser {
    static func loadedThreadState(
        from message: [String: Any],
        threadID: String
    ) -> CodexAppServerLoadedThreadState? {
        guard
            let result = message["result"] as? [String: Any],
            let threadIDs = result["data"] as? [String]
        else {
            return nil
        }
        return threadIDs.contains(threadID) ? .loaded : .needsResume
    }

    static func resumedThreadID(from message: [String: Any]) -> String? {
        guard
            let result = message["result"] as? [String: Any],
            let thread = result["thread"] as? [String: Any],
            let threadID = thread["id"] as? String
        else {
            return nil
        }
        let trimmed = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

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

    static func threadReadState(
        from message: [String: Any],
        threadID: String
    ) -> CodexAppServerThreadState? {
        guard
            let result = message["result"] as? [String: Any],
            let thread = result["thread"] as? [String: Any],
            let returnedThreadID = thread["id"] as? String
        else {
            return nil
        }
        guard returnedThreadID == threadID else { return .unavailable }
        guard
            let status = thread["status"] as? [String: Any],
            let type = status["type"] as? String
        else {
            return nil
        }
        switch type {
        case "active": return .active
        case "idle": return .idle
        case "notLoaded", "systemError": return .unavailable
        default: return nil
        }
    }
}

enum CodexAppServerResumeRequestFactory {
    static func threadResume(id: Int, threadID: String, cwd: String?) -> [String: Any] {
        var params: [String: Any] = [
            "threadId": threadID,
            "excludeTurns": true,
        ]
        if let cwd = cwd?.trimmingCharacters(in: .whitespacesAndNewlines), !cwd.isEmpty {
            params["cwd"] = cwd
        }
        return [
            "id": id,
            "method": "thread/resume",
            "params": params,
        ]
    }
}

enum CodexAppServerThreadReadRequestFactory {
    static func threadRead(id: Int, threadID: String) -> [String: Any] {
        [
            "id": id,
            "method": "thread/read",
            "params": [
                "threadId": threadID,
                "includeTurns": false,
            ],
        ]
    }
}

typealias CodexFollowerSubmitter = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ action: CodexSendAction,
    _ clientMessageID: String,
    _ cwd: String?,
    _ attachments: [CodexFollowerAttachment]
) async -> CodexAppServerSendOutcome

struct CodexQueuedReplyNotification: Sendable {
    private let callback: @Sendable () -> Void

    init(_ callback: @escaping @Sendable () -> Void) {
        self.callback = callback
    }

    func callAsFunction() {
        callback()
    }
}

typealias CodexQueuedReplySubmitter = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ cwd: String?,
    _ expectedTurnID: String?,
    _ clientMessageID: String,
    _ queuedNotification: CodexQueuedReplyNotification,
    _ attachments: [CodexFollowerAttachment]
) async -> CodexAppServerSendOutcome

typealias CodexResumingSubmitter = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ cwd: String?,
    _ action: CodexSendAction,
    _ expectedTurnID: String?,
    _ clientMessageID: String,
    _ queuedNotification: CodexQueuedReplyNotification,
    _ attachments: [CodexFollowerAttachment]
) async -> CodexAppServerSendOutcome

typealias CodexAccountProfileResolver = @Sendable (
    _ threadID: String
) -> CodexAccountProfile?

typealias CodexAccountProfileBindingResolver = @Sendable (
    _ threadID: String
) -> UUID?

typealias CodexAccountProfileVerifier = @Sendable (
    _ profile: CodexAccountProfile
) async -> Bool

typealias CodexAccountProfileSubmitter = @Sendable (
    _ profile: CodexAccountProfile,
    _ prompt: String,
    _ threadID: String,
    _ cwd: String?,
    _ action: CodexSendAction,
    _ expectedTurnID: String?,
    _ clientMessageID: String,
    _ queuedNotification: CodexQueuedReplyNotification,
    _ attachments: [CodexFollowerAttachment]
) async -> CodexAppServerSendOutcome

final class CodexAppServerSender {
    private let submitter: CodexFollowerSubmitter
    private let profileResolver: CodexAccountProfileResolver
    private let profileBindingResolver: CodexAccountProfileBindingResolver
    private let profileVerifier: CodexAccountProfileVerifier
    private let profileSubmitter: CodexAccountProfileSubmitter
    private let queuedReplySubmitter: CodexQueuedReplySubmitter
    private let resumingSubmitter: CodexResumingSubmitter
    private let runtimeThreadResolver: @Sendable (String) -> String

    init(
        submitter: @escaping CodexFollowerSubmitter = {
            prompt, threadID, action, clientMessageID, cwd, attachments in
            await CodexFollowerIPCTransport().submit(
                prompt: prompt,
                threadID: threadID,
                action: action,
                clientMessageID: clientMessageID,
                cwd: cwd,
                attachments: attachments
            )
        },
        profileResolver: @escaping CodexAccountProfileResolver = { threadID in
            CodexAccountProfileRouteResolver().profile(for: threadID)
        },
        profileBindingResolver: @escaping CodexAccountProfileBindingResolver = { threadID in
            CodexThreadAccountProfileBindingStore().profileID(for: threadID)
        },
        profileVerifier: @escaping CodexAccountProfileVerifier = { profile in
            await CodexAccountProfileRuntimeVerifier().verify(profile)
        },
        profileSubmitter: @escaping CodexAccountProfileSubmitter = {
            profile, prompt, threadID, cwd, action, expectedTurnID,
            clientMessageID, queuedNotification, attachments in
            await CodexAppServerSender.submitThroughProfileProxy(
                profile: profile,
                prompt: prompt,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                clientMessageID: clientMessageID,
                queuedNotification: queuedNotification,
                attachments: attachments
            )
        },
        queuedReplySubmitter: @escaping CodexQueuedReplySubmitter = {
            prompt, threadID, cwd, _, clientMessageID, queuedNotification, attachments in
            let outcome = await CodexFollowerIPCTransport().queueReply(
                prompt: prompt,
                threadID: threadID,
                clientMessageID: clientMessageID,
                cwd: cwd,
                attachments: attachments
            )
            if outcome == .sent {
                queuedNotification()
            }
            return outcome
        },
        resumingSubmitter: @escaping CodexResumingSubmitter = {
            prompt, threadID, cwd, action, expectedTurnID, clientMessageID, queuedNotification, attachments in
            await CodexAppServerSender.submitThroughSharedProxy(
                prompt: prompt,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                clientMessageID: clientMessageID,
                queuedNotification: queuedNotification,
                attachments: attachments
            )
        },
        runtimeThreadResolver: @escaping @Sendable (String) -> String = { threadID in
            CodexThreadLineageStore().activeThreadID(for: threadID)
        }
    ) {
        self.submitter = submitter
        self.profileResolver = profileResolver
        self.profileBindingResolver = profileBindingResolver
        self.profileVerifier = profileVerifier
        self.profileSubmitter = profileSubmitter
        self.queuedReplySubmitter = queuedReplySubmitter
        self.resumingSubmitter = resumingSubmitter
        self.runtimeThreadResolver = runtimeThreadResolver
    }

    func submit(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        onQueued: @escaping @Sendable () -> Void,
        attachments: [CodexFollowerAttachment] = []
    ) async -> CodexAppServerSendOutcome {
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedPrompt.isEmpty, !trimmedThreadID.isEmpty else { return .failed }
        let runtimeThreadID = runtimeThreadResolver(trimmedThreadID)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !runtimeThreadID.isEmpty else { return .failed }

        let queuedNotification = CodexQueuedReplyNotification(onQueued)
        let profile = profileResolver(runtimeThreadID)
        if let missingProfileID = profileBindingResolver(runtimeThreadID) {
            if profile == nil {
                Self.log(
                    "submit refused missing-profile thread=\(runtimeThreadID) profile=\(missingProfileID.uuidString)"
                )
                return .failed
            }
        }

        // A loaded ChatGPT task must be addressed through its follower socket so the
        // visible task owns the input. A profile daemon is only a fallback for tasks
        // that are not currently streamed by ChatGPT (or when that socket is absent).
        let initialOutcome: CodexAppServerSendOutcome
        switch action {
        case .reply:
            Self.log("submit queued-reply thread=\(runtimeThreadID)")
            initialOutcome = await queuedReplySubmitter(
                trimmedPrompt,
                runtimeThreadID,
                cwd,
                expectedTurnID,
                clientMessageID,
                queuedNotification,
                attachments
            )
            Self.log(
                "queued-reply finished thread=\(runtimeThreadID) outcome=\(String(describing: initialOutcome))"
            )
        case .steer:
            Self.log(
                "submit native-ipc action=\(action.logName) thread=\(runtimeThreadID) socket=\(CodexFollowerIPCProtocol.socketURL.path)"
            )
            initialOutcome = await submitter(
                trimmedPrompt,
                runtimeThreadID,
                action,
                clientMessageID,
                cwd,
                attachments
            )
            Self.log(
                "native-ipc finished action=\(action.logName) thread=\(runtimeThreadID) outcome=\(String(describing: initialOutcome))"
            )
        }

        if let profile,
           initialOutcome == .threadNotLoaded || initialOutcome == .sharedDaemonUnavailable
        {
            Self.log(
                "submit profile-proxy fallback action=\(action.logName) canonical_thread=\(trimmedThreadID) "
                    + "runtime_thread=\(runtimeThreadID) profile=\(profile.id.uuidString) "
                    + "native_outcome=\(String(describing: initialOutcome))"
            )
            guard await profileVerifier(profile) else {
                Self.log(
                    "submit refused unverified-profile thread=\(runtimeThreadID) profile=\(profile.id.uuidString)"
                )
                return .failed
            }
            return await profileSubmitter(
                profile,
                trimmedPrompt,
                runtimeThreadID,
                cwd,
                action,
                expectedTurnID,
                clientMessageID,
                queuedNotification,
                attachments
            )
        }

        guard initialOutcome == .threadNotLoaded else { return initialOutcome }
        Self.log("target thread unloaded; resuming through shared proxy thread=\(runtimeThreadID)")
        return await resumingSubmitter(
            trimmedPrompt,
            runtimeThreadID,
            cwd,
            action,
            expectedTurnID,
            clientMessageID,
            queuedNotification,
            attachments
        )
    }

    private static func submitThroughSharedProxy(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        queuedNotification: CodexQueuedReplyNotification,
        attachments: [CodexFollowerAttachment]
    ) async -> CodexAppServerSendOutcome {
        guard let executableURL = runningCodexExecutableURL() else {
            log("resume fallback failed: no running ChatGPT Codex executable")
            return .sharedDaemonUnavailable
        }

        let handle = CodexAppServerSessionHandle()
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                let session = CodexAppServerSession(
                    executableURL: executableURL,
                    environmentOverrides: [:],
                    socketURL: nil,
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    attachments: attachments,
                    runtimeLogContext: "shared",
                    onQueued: { queuedNotification() },
                    completion: { outcome in
                        continuation.resume(returning: outcome)
                    }
                )
                guard handle.install(session) else {
                    continuation.resume(returning: .failed)
                    return
                }
                session.start()
            }
        } onCancel: {
            handle.cancel()
        }
    }

    private static func submitThroughProfileProxy(
        profile: CodexAccountProfile,
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        queuedNotification: CodexQueuedReplyNotification,
        attachments: [CodexFollowerAttachment]
    ) async -> CodexAppServerSendOutcome {
        let command: CodexAccountProfileDaemonCommand
        do {
            command = try await CodexAccountProfileDaemonCoordinator.shared.ensureRunning(
                for: profile
            )
        } catch {
            log(
                "profile daemon unavailable profile=\(profile.id.uuidString) error=\(error.localizedDescription)"
            )
            return .sharedDaemonUnavailable
        }
        CodexAccountRuntimeDiagnostics.append(
            "profile=\(profile.id.uuidString) label=\(profile.label) send route "
                + "thread=\(threadID) socket=\(command.socketURL.path) "
                + "daemon_home=\(command.environmentOverrides["CODEX_HOME"] ?? "missing") "
                + "sqlite_home=\(command.environmentOverrides["CODEX_SQLITE_HOME"] ?? "missing")"
        )

        let handle = CodexAppServerSessionHandle()
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                let session = CodexAppServerSession(
                    executableURL: command.executableURL,
                    environmentOverrides: command.environmentOverrides,
                    socketURL: command.socketURL,
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    attachments: attachments,
                    runtimeLogContext: "profile=\(profile.id.uuidString) socket=\(command.socketURL.path)",
                    onQueued: { queuedNotification() },
                    completion: { outcome in
                        continuation.resume(returning: outcome)
                    }
                )
                guard handle.install(session) else {
                    continuation.resume(returning: .failed)
                    return
                }
                session.start()
            }
        } onCancel: {
            handle.cancel()
        }
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
        static let resumeThread = 6
    }

    private let executableURL: URL
    private let environmentOverrides: [String: String]
    private let socketURL: URL?
    private let prompt: String
    private let threadID: String
    private let cwd: String?
    private let action: CodexSendAction
    private let expectedTurnID: String?
    private let clientMessageID: String
    private let attachments: [CodexFollowerAttachment]
    private let runtimeLogContext: String
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
    private var lastSteerTurnID: String?
    private var didRetryStaleSteer = false

    init(
        executableURL: URL,
        environmentOverrides: [String: String] = [:],
        socketURL: URL? = nil,
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        attachments: [CodexFollowerAttachment],
        runtimeLogContext: String,
        onQueued: @escaping @Sendable () -> Void,
        completion: @escaping @Sendable (CodexAppServerSendOutcome) -> Void
    ) {
        self.executableURL = executableURL
        self.environmentOverrides = environmentOverrides
        self.socketURL = socketURL
        self.prompt = prompt
        self.threadID = threadID
        self.cwd = cwd
        self.action = action
        self.expectedTurnID = expectedTurnID
        self.clientMessageID = clientMessageID
        self.attachments = attachments
        self.runtimeLogContext = runtimeLogContext
        self.onQueued = onQueued
        self.completion = completion
    }

    func start() {
        process.executableURL = executableURL
        process.arguments = CodexAppServerProxyCommand.arguments(socketURL: socketURL)
        if !environmentOverrides.isEmpty {
            var environment = ProcessInfo.processInfo.environment
            environmentOverrides.forEach { environment[$0.key] = $0.value }
            process.environment = environment
        }
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
            guard let loadedThreadState = CodexAppServerResponseParser.loadedThreadState(
                from: message,
                threadID: threadID
            ) else {
                appendLog("loaded-thread discovery response was malformed")
                finish(outcome: .failed)
                terminate()
                return
            }
            if loadedThreadState == .loaded {
                routeLoadedThread(resumedThread: false)
            } else {
                requestThreadResume()
            }
        case RequestID.resumeThread:
            guard CodexAppServerResponseParser.resumedThreadID(from: message) == threadID else {
                appendLog("thread/resume returned a different or malformed thread")
                finish(outcome: .threadNotLoaded)
                terminate()
                return
            }
            appendLog("target thread resumed")
            routeLoadedThread(resumedThread: true)
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
            guard let state = CodexAppServerResponseParser.threadReadState(
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

    private func routeLoadedThread(resumedThread: Bool) {
        switch CodexAppServerLoadedThreadRoutePlanner.nextStep(
            action: action,
            snapshotTurnID: expectedTurnID,
            resumedThread: resumedThread
        ) {
        case .discoverCurrentTurn:
            schedulePhaseTimeout(
                message: "turn discovery timed out",
                after: .seconds(CodexAppServerRequestPolicy.turnDiscoveryTimeoutSeconds)
            )
            requestRecentTurns(id: RequestID.recentTurns)
        case .steerExistingTurn(let turnID):
            steerTurn(expectedTurnID: turnID)
        case .queueReplyAfterCurrentTurn:
            queueReply()
        case .startTurn:
            startTurn()
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
        if requestID == RequestID.resumeThread {
            let missingThreadPhrases = ["not found", "no rollout", "does not exist", "unknown thread"]
            let outcome: CodexAppServerSendOutcome = missingThreadPhrases.contains(where: {
                message.localizedCaseInsensitiveContains($0)
            }) ? .threadNotLoaded : .failed
            finish(outcome: outcome)
            terminate()
            return
        }
        if requestID == RequestID.sendAction,
           pendingTurnStart == nil,
           message.localizedCaseInsensitiveContains("active turn") {
            if action == .reply {
                queueReply()
            } else if let lastSteerTurnID,
                      let replacementTurnID = CodexAppServerSteerRecoveryPolicy.replacementTurnID(
                          errorMessage: message,
                          attemptedTurnID: lastSteerTurnID,
                          hasRetried: didRetryStaleSteer
                      ) {
                didRetryStaleSteer = true
                appendLog(
                    "steer active turn advanced; retrying expectedTurnID=\(replacementTurnID)"
                )
                steerTurn(expectedTurnID: replacementTurnID)
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

    private func requestThreadResume() {
        appendLog("thread/resume thread=\(threadID)")
        schedulePhaseTimeout(
            message: "thread/resume timed out",
            after: .seconds(CodexAppServerRequestPolicy.threadResumeTimeoutSeconds)
        )
        send(CodexAppServerResumeRequestFactory.threadResume(
            id: RequestID.resumeThread,
            threadID: threadID,
            cwd: cwd
        ))
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
        appendLog("thread/read id=\(id)")
        send(CodexAppServerThreadReadRequestFactory.threadRead(
            id: id,
            threadID: threadID
        ))
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
        lastSteerTurnID = expectedTurnID
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
        var input: [[String: Any]] = [
            [
                "type": "text",
                "text": prompt,
                "text_elements": [],
            ],
        ]
        input.append(contentsOf: attachments.map(\.appServerInputItem))
        return input
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
        CodexSendLog.append(
            "app-server-session \(runtimeLogContext) thread=\(threadID) "
                + CodexAccountRuntimeDiagnostics.redact(message)
        )
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
