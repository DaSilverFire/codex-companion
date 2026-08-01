import Foundation

enum CodexAppServerTaskCreationOutcome: Equatable, Sendable {
    case created(threadID: String)
    case sharedDaemonUnavailable
    case timedOut
    case failed
}

struct CodexAppServerTaskCreationRequest: Sendable {
    var prompt: String
    var cwd: String?
    var model: String?
    var reasoningEffort: String?
    var skillName: String?
    var skillPath: String?
    var attachments: [CodexFollowerAttachment]
    var clientMessageID: String
}

typealias CodexSelectedAccountProfileProvider = @Sendable () -> CodexAccountProfile?
typealias CodexAccountProfileProvider = @Sendable (_ profileID: UUID) -> CodexAccountProfile?
typealias CodexSharedTaskCreator = @Sendable (
    _ request: CodexAppServerTaskCreationRequest
) async -> CodexAppServerTaskCreationOutcome
typealias CodexProfileTaskCreator = @Sendable (
    _ profile: CodexAccountProfile,
    _ request: CodexAppServerTaskCreationRequest
) async -> CodexAppServerTaskCreationOutcome
typealias CodexAccountProfileBinder = @Sendable (
    _ threadID: String,
    _ profileID: UUID
) -> Void

enum CodexAppServerTaskRequestFactory {
    static func threadStart(id: Int, cwd: String?, model: String? = nil) -> [String: Any] {
        var params: [String: Any] = [
            "ephemeral": false,
            "serviceName": "codex-companion-mobile",
        ]
        if let cwd = cwd?.trimmingCharacters(in: .whitespacesAndNewlines), !cwd.isEmpty {
            params["cwd"] = cwd
        }
        if let model = model?.trimmingCharacters(in: .whitespacesAndNewlines), !model.isEmpty {
            params["model"] = model
        }
        return [
            "id": id,
            "method": "thread/start",
            "params": params,
        ]
    }

    static func turnStart(
        id: Int,
        threadID: String,
        prompt: String,
        clientMessageID: String,
        model: String? = nil,
        reasoningEffort: String? = nil,
        skillName: String? = nil,
        skillPath: String? = nil,
        attachments: [CodexFollowerAttachment] = []
    ) -> [String: Any] {
        var input: [[String: Any]] = [[
            "type": "text",
            "text": prompt,
            "text_elements": [],
        ]]
        let resolvedSkillName = skillName?.trimmingCharacters(in: .whitespacesAndNewlines)
        let resolvedSkillPath = skillPath?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let resolvedSkillName, !resolvedSkillName.isEmpty,
           let resolvedSkillPath, !resolvedSkillPath.isEmpty {
            input.append([
                "type": "skill",
                "name": resolvedSkillName,
                "path": resolvedSkillPath,
            ])
        }
        input.append(contentsOf: attachments.map(\.appServerInputItem))

        var params: [String: Any] = [
            "threadId": threadID,
            "input": input,
            "clientUserMessageId": clientMessageID,
        ]
        if let model = model?.trimmingCharacters(in: .whitespacesAndNewlines), !model.isEmpty {
            params["model"] = model
        }
        if let reasoningEffort = reasoningEffort?.trimmingCharacters(in: .whitespacesAndNewlines),
           !reasoningEffort.isEmpty {
            params["effort"] = reasoningEffort
        }

        return [
            "id": id,
            "method": "turn/start",
            "params": params,
        ]
    }
}

enum CodexAppServerTaskResponseParser {
    static func threadID(from message: [String: Any]) -> String? {
        guard
            let result = message["result"] as? [String: Any],
            let thread = result["thread"] as? [String: Any],
            let threadID = thread["id"] as? String
        else { return nil }
        let trimmed = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

final class CodexAppServerTaskCreator {
    private let selectedProfileProvider: CodexSelectedAccountProfileProvider
    private let profileProvider: CodexAccountProfileProvider
    private let sharedTaskCreator: CodexSharedTaskCreator
    private let profileTaskCreator: CodexProfileTaskCreator
    private let profileBinder: CodexAccountProfileBinder

    init(
        selectedProfileProvider: @escaping CodexSelectedAccountProfileProvider = {
            CodexAccountProfileRouteResolver().selectedProfile()
        },
        profileProvider: @escaping CodexAccountProfileProvider = { profileID in
            CodexAccountProfileStore().profiles.first { $0.id == profileID }
        },
        sharedTaskCreator: @escaping CodexSharedTaskCreator = { request in
            await CodexAppServerTaskCreator.createThroughSharedProxy(request)
        },
        profileTaskCreator: @escaping CodexProfileTaskCreator = { profile, request in
            await CodexAppServerTaskCreator.createThroughProfileProxy(
                profile: profile,
                request: request
            )
        },
        profileBinder: @escaping CodexAccountProfileBinder = { threadID, profileID in
            CodexThreadAccountProfileBindingStore().bind(threadID: threadID, to: profileID)
        }
    ) {
        self.selectedProfileProvider = selectedProfileProvider
        self.profileProvider = profileProvider
        self.sharedTaskCreator = sharedTaskCreator
        self.profileTaskCreator = profileTaskCreator
        self.profileBinder = profileBinder
    }

    func create(
        prompt: String,
        cwd: String?,
        model: String? = nil,
        reasoningEffort: String? = nil,
        skillName: String? = nil,
        skillPath: String? = nil,
        accountProfileID: UUID? = nil,
        attachments: [CodexFollowerAttachment] = [],
        clientMessageID: String = UUID().uuidString
    ) async -> CodexAppServerTaskCreationOutcome {
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedPrompt.isEmpty else { return .failed }
        let request = CodexAppServerTaskCreationRequest(
            prompt: trimmedPrompt,
            cwd: cwd?.trimmingCharacters(in: .whitespacesAndNewlines),
            model: model,
            reasoningEffort: reasoningEffort,
            skillName: skillName,
            skillPath: skillPath,
            attachments: attachments,
            clientMessageID: clientMessageID
        )

        let profile: CodexAccountProfile?
        if let accountProfileID {
            guard let requestedProfile = profileProvider(accountProfileID) else {
                CodexSendLog.append("task-creator requested account profile is unavailable")
                return .failed
            }
            profile = requestedProfile
        } else {
            profile = selectedProfileProvider()
        }

        if let profile {
            let outcome = await profileTaskCreator(profile, request)
            if case .created(let threadID) = outcome {
                profileBinder(threadID, profile.id)
            }
            return outcome
        }
        return await sharedTaskCreator(request)
    }

    private static func createThroughSharedProxy(
        _ request: CodexAppServerTaskCreationRequest
    ) async -> CodexAppServerTaskCreationOutcome {
        guard FileManager.default.fileExists(atPath: CodexAppServerSender.sharedDaemonSocketURL.path) else {
            CodexSendLog.append("task-creator shared app-server socket is unavailable")
            return .sharedDaemonUnavailable
        }
        guard let executableURL = CodexAppServerSender.runningCodexExecutableURL()
            ?? WorkspacePaths.codexExecutableURLs.first(where: {
                FileManager.default.isExecutableFile(atPath: $0.path)
            })
        else { return .failed }

        return await createThroughProxy(
            request,
            executableURL: executableURL,
            environmentOverrides: [:]
        )
    }

    private static func createThroughProfileProxy(
        profile: CodexAccountProfile,
        request: CodexAppServerTaskCreationRequest
    ) async -> CodexAppServerTaskCreationOutcome {
        let command: CodexAccountProfileDaemonCommand
        do {
            command = try await CodexAccountProfileDaemonCoordinator.shared.ensureRunning(
                for: profile
            )
        } catch {
            CodexSendLog.append(
                "task-creator profile daemon unavailable profile=\(profile.id.uuidString) error=\(error.localizedDescription)"
            )
            return .sharedDaemonUnavailable
        }

        return await createThroughProxy(
            request,
            executableURL: command.executableURL,
            environmentOverrides: command.environmentOverrides
        )
    }

    private static func createThroughProxy(
        _ request: CodexAppServerTaskCreationRequest,
        executableURL: URL,
        environmentOverrides: [String: String]
    ) async -> CodexAppServerTaskCreationOutcome {
        let handle = CodexAppServerTaskCreationSessionHandle()
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                let session = CodexAppServerTaskCreationSession(
                    executableURL: executableURL,
                    environmentOverrides: environmentOverrides,
                    prompt: request.prompt,
                    cwd: request.cwd,
                    model: request.model,
                    reasoningEffort: request.reasoningEffort,
                    skillName: request.skillName,
                    skillPath: request.skillPath,
                    attachments: request.attachments,
                    clientMessageID: request.clientMessageID,
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
}

private final class CodexAppServerTaskCreationSession {
    private enum RequestID {
        static let initialize = 1
        static let threadStart = 2
        static let turnStart = 3
    }

    private let executableURL: URL
    private let environmentOverrides: [String: String]
    private let prompt: String
    private let cwd: String?
    private let model: String?
    private let reasoningEffort: String?
    private let skillName: String?
    private let skillPath: String?
    private let attachments: [CodexFollowerAttachment]
    private let clientMessageID: String
    private let completion: @Sendable (CodexAppServerTaskCreationOutcome) -> Void
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let queue = DispatchQueue(label: "com.silverfire.codexcompanion.task-creator")
    private var codec = CodexWebSocketCodec()
    private var hasUpgraded = false
    private var threadID: String?
    private var timeoutWorkItem: DispatchWorkItem?
    private var isFinished = false
    private var didCleanUp = false

    init(
        executableURL: URL,
        environmentOverrides: [String: String] = [:],
        prompt: String,
        cwd: String?,
        model: String?,
        reasoningEffort: String?,
        skillName: String?,
        skillPath: String?,
        attachments: [CodexFollowerAttachment],
        clientMessageID: String,
        completion: @escaping @Sendable (CodexAppServerTaskCreationOutcome) -> Void
    ) {
        self.executableURL = executableURL
        self.environmentOverrides = environmentOverrides
        self.prompt = prompt
        self.cwd = cwd
        self.model = model
        self.reasoningEffort = reasoningEffort
        self.skillName = skillName
        self.skillPath = skillPath
        self.attachments = attachments
        self.clientMessageID = clientMessageID
        self.completion = completion
    }

    func start() {
        process.executableURL = executableURL
        process.arguments = ["app-server", "proxy"]
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
            self?.queue.async { [weak self] in self?.receive(data) }
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            let detail = String(decoding: data, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            self?.queue.async { CodexSendLog.append("task-creator stderr \(detail)") }
        }
        process.terminationHandler = { [weak self] process in
            self?.queue.async { [weak self] in
                guard let self else { return }
                CodexSendLog.append("task-creator proxy terminated status=\(process.terminationStatus)")
                finish(.sharedDaemonUnavailable)
                cleanup()
            }
        }

        do {
            try process.run()
        } catch {
            queue.sync {
                CodexSendLog.append("task-creator launch failed \(error.localizedDescription)")
                finish(.sharedDaemonUnavailable)
                cleanup()
            }
            return
        }
        queue.async { [weak self] in
            guard let self else { return }
            scheduleTimeout()
            writeRaw(CodexWebSocketCodec.handshakeRequest(key: CodexWebSocketCodec.randomHandshakeKey()))
        }
    }

    func cancel() {
        queue.async { [weak self] in
            guard let self else { return }
            finish(.failed)
            terminate()
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
                    writeFrame(opcode: .pong, payload: payload)
                case .close:
                    finish(.sharedDaemonUnavailable)
                    terminate()
                }
            }
        } catch {
            CodexSendLog.append("task-creator decode failed \(error)")
            finish(.sharedDaemonUnavailable)
            terminate()
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
        guard
            let raw = try? JSONSerialization.jsonObject(with: data),
            let message = raw as? [String: Any]
        else { return }
        if let error = message["error"] {
            CodexSendLog.append("task-creator server error \(String(describing: error))")
            finish(.failed)
            terminate()
            return
        }
        guard let id = numericID(in: message) else { return }
        switch id {
        case RequestID.initialize:
            send(["method": "initialized"])
            send(CodexAppServerTaskRequestFactory.threadStart(
                id: RequestID.threadStart,
                cwd: cwd,
                model: model
            ))
        case RequestID.threadStart:
            guard let createdThreadID = CodexAppServerTaskResponseParser.threadID(from: message) else {
                finish(.failed)
                terminate()
                return
            }
            threadID = createdThreadID
            send(CodexAppServerTaskRequestFactory.turnStart(
                id: RequestID.turnStart,
                threadID: createdThreadID,
                prompt: prompt,
                clientMessageID: clientMessageID,
                model: model,
                reasoningEffort: reasoningEffort,
                skillName: skillName,
                skillPath: skillPath,
                attachments: attachments
            ))
        case RequestID.turnStart:
            guard let threadID else {
                finish(.failed)
                terminate()
                return
            }
            CodexSendLog.append("task-creator started thread=\(threadID)")
            finish(.created(threadID: threadID))
            terminate()
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
        guard
            hasUpgraded,
            JSONSerialization.isValidJSONObject(object),
            let data = try? JSONSerialization.data(withJSONObject: object)
        else { return }
        writeFrame(opcode: .text, payload: data)
    }

    private func writeFrame(opcode: CodexWebSocketCodec.Opcode, payload: Data) {
        writeRaw(CodexWebSocketCodec.clientFrame(opcode: opcode, payload: payload))
    }

    private func writeRaw(_ data: Data) {
        do {
            try stdinPipe.fileHandleForWriting.write(contentsOf: data)
        } catch {
            finish(.sharedDaemonUnavailable)
            terminate()
        }
    }

    private func scheduleTimeout() {
        timeoutWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            guard let self else { return }
            CodexSendLog.append("task-creator timed out")
            finish(.timedOut)
            terminate()
        }
        timeoutWorkItem = workItem
        queue.asyncAfter(deadline: .now() + .seconds(12), execute: workItem)
    }

    private func finish(_ outcome: CodexAppServerTaskCreationOutcome) {
        guard !isFinished else { return }
        isFinished = true
        timeoutWorkItem?.cancel()
        timeoutWorkItem = nil
        completion(outcome)
    }

    private func terminate() {
        cleanup()
        if process.isRunning { process.terminate() }
    }

    private func cleanup() {
        guard !didCleanUp else { return }
        didCleanUp = true
        timeoutWorkItem?.cancel()
        timeoutWorkItem = nil
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
    }
}

private final class CodexAppServerTaskCreationSessionHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var session: CodexAppServerTaskCreationSession?
    private var isCanceled = false

    func install(_ session: CodexAppServerTaskCreationSession) -> Bool {
        lock.withLock {
            guard !isCanceled else { return false }
            self.session = session
            return true
        }
    }

    func cancel() {
        let session = lock.withLock { () -> CodexAppServerTaskCreationSession? in
            isCanceled = true
            return self.session
        }
        session?.cancel()
    }
}
