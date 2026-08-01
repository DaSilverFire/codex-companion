import Foundation

protocol CodexAccountProfileRPCClientProviding: Sendable {
    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming
}

struct CodexThreadAccountHandoffResult: Equatable, Sendable {
    var threadID: String
    var rolloutURL: URL
    var profileID: UUID
}

enum CodexThreadAccountHandoffError: Error, Equatable {
    case activeTurn
    case invalidThreadID
    case invalidRolloutPath
    case missingResponse
    case server(String)
    case invalidResponse
    case resumeMismatch
}

extension CodexThreadAccountHandoffError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .activeTurn:
            return "Wait for the current Codex turn to stop before switching accounts."
        case .invalidThreadID:
            return "The Codex task does not have a valid thread identifier."
        case .invalidRolloutPath:
            return "The Codex task does not have a valid persisted rollout path."
        case .missingResponse:
            return "Codex did not return a response for the account handoff."
        case .server(let message):
            return message
        case .invalidResponse:
            return "Codex returned an unreadable account handoff response."
        case .resumeMismatch:
            return "Codex resumed a different task or rollout, so the account was not changed."
        }
    }
}

enum CodexAccountProfileRuntime {
    static var defaultBaseURL: URL {
        FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        )[0]
        .appendingPathComponent("Codex Companion", isDirectory: true)
        .appendingPathComponent("Codex Profiles", isDirectory: true)
    }

    static var defaultSharedSQLiteHomeURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
    }

    static var defaultManagedStandaloneURL: URL {
        defaultSharedSQLiteHomeURL
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
    }

    static var defaultDaemonBaseURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".ccp", isDirectory: true)
    }

    static func homeURL(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL
    ) -> URL {
        baseURL.standardizedFileURL.appendingPathComponent(
            profile.id.uuidString.lowercased(),
            isDirectory: true
        )
    }

    static func daemonHomeURL(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultDaemonBaseURL
    ) -> URL {
        let compactID = profile.id.uuidString
            .replacingOccurrences(of: "-", with: "")
            .lowercased()
            .prefix(16)
        return baseURL.standardizedFileURL.appendingPathComponent(
            String(compactID),
            isDirectory: true
        )
    }

    static func taskEnvironment(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL,
        sharedSQLiteHomeURL: URL = defaultSharedSQLiteHomeURL
    ) -> [String: String] {
        [
            "CODEX_HOME": homeURL(for: profile, baseURL: baseURL).path,
            "CODEX_SQLITE_HOME": sharedSQLiteHomeURL.standardizedFileURL.path,
        ]
    }

    static func daemonTaskEnvironment(
        for profile: CodexAccountProfile,
        daemonBaseURL: URL = defaultDaemonBaseURL,
        sharedSQLiteHomeURL: URL = defaultSharedSQLiteHomeURL
    ) -> [String: String] {
        [
            "CODEX_HOME": daemonHomeURL(for: profile, baseURL: daemonBaseURL).path,
            "CODEX_SQLITE_HOME": sharedSQLiteHomeURL.standardizedFileURL.path,
        ]
    }

    static func prepareHome(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL,
        managedStandaloneURL: URL? = nil
    ) throws {
        let profileHomeURL = homeURL(for: profile, baseURL: baseURL)
        try FileManager.default.createDirectory(
            at: profileHomeURL,
            withIntermediateDirectories: true
        )
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: profileHomeURL.path
        )

        if let managedStandaloneURL {
            try linkManagedStandaloneRuntime(
                into: profileHomeURL,
                managedStandaloneURL: managedStandaloneURL
            )
        }
    }

    static func prepareDaemonHome(
        for profile: CodexAccountProfile,
        daemonBaseURL: URL = defaultDaemonBaseURL,
        credentialBaseURL: URL = defaultBaseURL,
        managedStandaloneURL: URL = defaultManagedStandaloneURL
    ) throws {
        try prepareHome(
            for: profile,
            baseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )

        let fileManager = FileManager.default
        try fileManager.createDirectory(
            at: daemonBaseURL,
            withIntermediateDirectories: true
        )
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: daemonBaseURL.path
        )

        let credentialHomeURL = homeURL(for: profile, baseURL: credentialBaseURL)
        let daemonHomeURL = daemonHomeURL(for: profile, baseURL: daemonBaseURL)
        if let attributes = try? fileManager.attributesOfItem(atPath: daemonHomeURL.path) {
            switch attributes[.type] as? FileAttributeType {
            case .typeDirectory:
                break
            case .typeSymbolicLink:
                let destination = try fileManager.destinationOfSymbolicLink(
                    atPath: daemonHomeURL.path
                )
                let destinationURL = URL(
                    fileURLWithPath: destination,
                    relativeTo: daemonHomeURL.deletingLastPathComponent()
                ).standardizedFileURL
                guard destinationURL == credentialHomeURL.standardizedFileURL else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        daemonHomeURL.path
                    )
                }
                try fileManager.removeItem(at: daemonHomeURL)
                try fileManager.createDirectory(
                    at: daemonHomeURL,
                    withIntermediateDirectories: true
                )
            default:
                throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                    daemonHomeURL.path
                )
            }
        } else {
            try fileManager.createDirectory(
                at: daemonHomeURL,
                withIntermediateDirectories: true
            )
        }
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: daemonHomeURL.path
        )

        let daemonOwnedNames: Set<String> = [
            "app-server-control",
            "app-server-daemon",
            "log",
            "tmp",
        ]
        let credentialArtifacts = try fileManager.contentsOfDirectory(
            at: credentialHomeURL,
            includingPropertiesForKeys: nil
        )
        for sourceURL in credentialArtifacts
            where !daemonOwnedNames.contains(sourceURL.lastPathComponent)
        {
            let destinationURL = daemonHomeURL.appendingPathComponent(
                sourceURL.lastPathComponent,
                isDirectory: sourceURL.hasDirectoryPath
            )
            if let attributes = try? fileManager.attributesOfItem(
                atPath: destinationURL.path
            ) {
                guard attributes[.type] as? FileAttributeType == .typeSymbolicLink else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        destinationURL.path
                    )
                }
                let existingDestination = try fileManager.destinationOfSymbolicLink(
                    atPath: destinationURL.path
                )
                let existingDestinationURL = URL(
                    fileURLWithPath: existingDestination,
                    relativeTo: destinationURL.deletingLastPathComponent()
                ).standardizedFileURL
                guard existingDestinationURL == sourceURL.standardizedFileURL else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        destinationURL.path
                    )
                }
                continue
            }

            try fileManager.createSymbolicLink(
                at: destinationURL,
                withDestinationURL: sourceURL.standardizedFileURL
            )
        }
    }

    private static func linkManagedStandaloneRuntime(
        into profileHomeURL: URL,
        managedStandaloneURL: URL
    ) throws {
        let fileManager = FileManager.default
        let managedCodexURL = managedStandaloneURL
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")
        guard fileManager.isExecutableFile(atPath: managedCodexURL.path) else {
            throw CodexAccountProfileRuntimeError.managedStandaloneUnavailable(
                managedCodexURL.path
            )
        }

        let packagesURL = profileHomeURL.appendingPathComponent(
            "packages",
            isDirectory: true
        )
        try fileManager.createDirectory(
            at: packagesURL,
            withIntermediateDirectories: true
        )
        let profileStandaloneURL = packagesURL.appendingPathComponent(
            "standalone",
            isDirectory: true
        )

        if let attributes = try? fileManager.attributesOfItem(
            atPath: profileStandaloneURL.path
        ) {
            if attributes[.type] as? FileAttributeType == .typeSymbolicLink {
                let destination = try fileManager.destinationOfSymbolicLink(
                    atPath: profileStandaloneURL.path
                )
                let destinationURL = URL(
                    fileURLWithPath: destination,
                    relativeTo: profileStandaloneURL.deletingLastPathComponent()
                ).standardizedFileURL
                if destinationURL.path == managedStandaloneURL.standardizedFileURL.path {
                    return
                }
                try fileManager.removeItem(at: profileStandaloneURL)
            } else {
                let existingCodexURL = profileStandaloneURL
                    .appendingPathComponent("current", isDirectory: true)
                    .appendingPathComponent("codex")
                guard fileManager.isExecutableFile(atPath: existingCodexURL.path) else {
                    throw CodexAccountProfileRuntimeError.managedStandaloneConflict(
                        profileStandaloneURL.path
                    )
                }
                return
            }
        }

        try fileManager.createSymbolicLink(
            at: profileStandaloneURL,
            withDestinationURL: managedStandaloneURL.standardizedFileURL
        )
    }
}

enum CodexAccountProfileRuntimeError: LocalizedError {
    case managedStandaloneUnavailable(String)
    case managedStandaloneConflict(String)
    case daemonAliasConflict(String)

    var errorDescription: String? {
        switch self {
        case .managedStandaloneUnavailable(let path):
            return "The managed Codex runtime is unavailable at \(path)."
        case .managedStandaloneConflict(let path):
            return "The Codex profile runtime at \(path) is incomplete."
        case .daemonAliasConflict(let path):
            return "The Codex profile daemon path at \(path) is already in use."
        }
    }
}

final class CodexThreadAccountProfileBindingStore: @unchecked Sendable {
    static let bindingsKey = "com.silverfire.codexcompanion.codex-thread-account-profile-bindings"

    private let defaults: UserDefaults
    private let lock = NSLock()

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func profileID(for threadID: String) -> UUID? {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return nil }
        return lock.withLock {
            let bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            return bindings[normalizedThreadID].flatMap(UUID.init(uuidString:))
        }
    }

    func bind(threadID: String, to profileID: UUID) {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return }
        lock.withLock {
            var bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            bindings[normalizedThreadID] = profileID.uuidString
            defaults.set(bindings, forKey: Self.bindingsKey)
        }
    }

    func hasBindings(to profileID: UUID) -> Bool {
        lock.withLock {
            let bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            return bindings.values.contains { UUID(uuidString: $0) == profileID }
        }
    }

    func removeBinding(for threadID: String) {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return }
        lock.withLock {
            var bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            bindings.removeValue(forKey: normalizedThreadID)
            defaults.set(bindings, forKey: Self.bindingsKey)
        }
    }
}

enum CodexThreadAccountHandoffRequestFactory {
    static func resume(
        id: Int,
        threadID: String,
        rolloutURL: URL
    ) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "thread/resume",
            params: [
                "threadId": threadID,
                "path": rolloutURL.standardizedFileURL.path,
            ]
        )
    }
}

final class CodexThreadAccountHandoffService: @unchecked Sendable {
    private let clientProvider: any CodexAccountProfileRPCClientProviding
    private let bindingStore: CodexThreadAccountProfileBindingStore

    init(
        clientProvider: any CodexAccountProfileRPCClientProviding =
            CodexAccountProfileRPCClientProvider(),
        bindingStore: CodexThreadAccountProfileBindingStore =
            CodexThreadAccountProfileBindingStore()
    ) {
        self.clientProvider = clientProvider
        self.bindingStore = bindingStore
    }

    func handoff(
        threadID: String,
        rolloutURL: URL,
        hasActiveTurn: Bool,
        to profile: CodexAccountProfile
    ) throws -> CodexThreadAccountHandoffResult {
        guard !hasActiveTurn else {
            throw CodexThreadAccountHandoffError.activeTurn
        }

        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else {
            throw CodexThreadAccountHandoffError.invalidThreadID
        }

        let normalizedRolloutURL = rolloutURL.standardizedFileURL
        guard normalizedRolloutURL.isFileURL, !normalizedRolloutURL.path.isEmpty else {
            throw CodexThreadAccountHandoffError.invalidRolloutPath
        }

        let request = CodexThreadAccountHandoffRequestFactory.resume(
            id: 2,
            threadID: normalizedThreadID,
            rolloutURL: normalizedRolloutURL
        )
        let client = try clientProvider.client(for: profile)
        let responses = try client.perform([request])
        guard let response = responses[request.id] else {
            throw CodexThreadAccountHandoffError.missingResponse
        }
        if let error = response.error {
            throw CodexThreadAccountHandoffError.server(error)
        }
        guard
            let result = response.result,
            let thread = result["thread"] as? [String: Any],
            let resumedThreadID = thread["id"] as? String,
            let resumedPath = thread["path"] as? String
        else {
            throw CodexThreadAccountHandoffError.invalidResponse
        }

        let normalizedResumedThreadID = resumedThreadID.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        let normalizedResumedURL = URL(fileURLWithPath: resumedPath).standardizedFileURL
        guard
            normalizedResumedThreadID == normalizedThreadID,
            normalizedResumedURL == normalizedRolloutURL
        else {
            throw CodexThreadAccountHandoffError.resumeMismatch
        }

        bindingStore.bind(threadID: normalizedThreadID, to: profile.id)
        return CodexThreadAccountHandoffResult(
            threadID: normalizedThreadID,
            rolloutURL: normalizedRolloutURL,
            profileID: profile.id
        )
    }
}

struct CodexAccountProfileRPCClientProvider: CodexAccountProfileRPCClientProviding {
    var baseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    var sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    var executableURLProvider: @Sendable () -> URL? = {
        WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        })
    }
    var timeout: TimeInterval = 20

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        guard let executableURL = executableURLProvider() else {
            throw CodexAppServerControlError.missingExecutable
        }

        let profileHomeURL = CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: baseURL
        )
        try CodexAccountProfileRuntime.prepareHome(for: profile, baseURL: baseURL)
        return CodexAccountProfileRPCClient(
            executableURL: executableURL,
            profileHomeURL: profileHomeURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            timeout: timeout
        )
    }
}

private struct CodexAccountProfileRPCClient: CodexAppServerRPCPerforming, Sendable {
    var executableURL: URL
    var profileHomeURL: URL
    var sharedSQLiteHomeURL: URL
    var timeout: TimeInterval

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        try CodexAccountProfileRPCSession(
            executableURL: executableURL,
            profileHomeURL: profileHomeURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            requests: requests,
            timeout: timeout
        ).run()
    }
}

private final class CodexAccountProfileRPCSession {
    private let executableURL: URL
    private let profileHomeURL: URL
    private let sharedSQLiteHomeURL: URL
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

    init(
        executableURL: URL,
        profileHomeURL: URL,
        sharedSQLiteHomeURL: URL,
        requests: [CodexRPCRequest],
        timeout: TimeInterval
    ) {
        self.executableURL = executableURL
        self.profileHomeURL = profileHomeURL
        self.sharedSQLiteHomeURL = sharedSQLiteHomeURL
        self.requests = requests
        self.timeout = timeout
    }

    func run() throws -> [Int: CodexRPCResponse] {
        process.executableURL = executableURL
        process.arguments = ["app-server", "--listen", "stdio://"]
        var environment = ProcessInfo.processInfo.environment
        environment["CODEX_HOME"] = profileHomeURL.path
        environment["CODEX_SQLITE_HOME"] = sharedSQLiteHomeURL.standardizedFileURL.path
        process.environment = environment
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.read(handle.availableData)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { handle in
            _ = handle.availableData
        }
        process.terminationHandler = { [weak self] process in
            self?.finishIfNeeded(
                error: CodexAppServerControlError.processExited(process.terminationStatus)
            )
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

        if completion.wait(timeout: .now() + timeout) == .timedOut {
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
        let lines: [Data] = lock.withLock {
            stdoutBuffer.append(data)
            let newline = Data([0x0A])
            var lines: [Data] = []
            while let range = stdoutBuffer.firstRange(of: newline) {
                lines.append(
                    stdoutBuffer.subdata(in: stdoutBuffer.startIndex..<range.lowerBound)
                )
                stdoutBuffer.removeSubrange(stdoutBuffer.startIndex..<range.upperBound)
            }
            return lines
        }

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
                finishIfNeeded(
                    error: CodexAppServerControlError.server(Self.errorText(from: message))
                )
                return
            }
            send(["method": "initialized"])
            let shouldSend = lock.withLock {
                guard !didInitialize else { return false }
                didInitialize = true
                return true
            }
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
            response = CodexRPCResponse(
                result: nil,
                error: Self.errorText(from: message)
            )
        } else {
            response = CodexRPCResponse(
                result: message["result"] as? [String: Any],
                error: nil
            )
        }

        let complete = lock.withLock {
            responses[id] = response
            return responses.count == requests.count
        }
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
        lock.withLock {
            guard !didFinish else { return }
            didFinish = true
            failure = error
            completion.signal()
        }
    }

    private func cleanup() {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
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
