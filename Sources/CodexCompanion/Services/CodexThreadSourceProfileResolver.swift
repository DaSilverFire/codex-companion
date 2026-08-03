import Foundation

enum CodexThreadSourceProfileResolutionError: Error, Equatable {
    case invalidThreadID
    case sharedRuntimeUnavailable
    case missingResponse
    case server(String)
    case unverifiableAccountIdentity
    case noMatchingProfile
    case ambiguousProfiles
    case bindingChanged
    case missingExecutable
    case profileRuntimeUnavailable
}

extension CodexThreadSourceProfileResolutionError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .invalidThreadID:
            return "The Codex task does not have a valid thread identifier."
        case .sharedRuntimeUnavailable:
            return "The shared Codex account service is unavailable."
        case .missingResponse:
            return "The shared Codex account service did not identify its account."
        case .server(let message):
            return message
        case .unverifiableAccountIdentity:
            return "The shared Codex account could not be verified."
        case .noMatchingProfile:
            return "No Companion account profile matches the shared Codex account."
        case .ambiguousProfiles:
            return "More than one Companion profile represents the shared Codex account."
        case .bindingChanged:
            return "The task's account changed while Companion was identifying it."
        case .missingExecutable:
            return "The official Codex executable could not be found."
        case .profileRuntimeUnavailable:
            return "The selected Codex account service is not currently available."
        }
    }
}

struct CodexSharedAccountRuntimeIdentityReader: Sendable {
    private let client: any CodexAppServerRPCPerforming

    init(client: any CodexAppServerRPCPerforming = Self.liveClient()) {
        self.client = client
    }

    func identity() throws -> CodexAccountProfileIdentity {
        // Temporary stdio app-server sessions reserve request 1 for initialize.
        let request = CodexThreadAccountHandoffRequestFactory.accountRead(id: 2)
        let responses = try client.perform([request])
        guard let response = responses[request.id] else {
            throw CodexThreadSourceProfileResolutionError.missingResponse
        }
        if let error = response.error {
            throw CodexThreadSourceProfileResolutionError.server(error)
        }
        guard
            let result = response.result,
            let identity = CodexAccountProfileIdentityParser.identity(from: result)
        else {
            throw CodexThreadSourceProfileResolutionError.unverifiableAccountIdentity
        }
        return identity
    }

    private static func liveClient() -> any CodexAppServerRPCPerforming {
        let socketURL = CodexAppServerSender.sharedDaemonSocketURL
        guard let executableURL = WorkspacePaths.codexExecutableURLs.first(where: {
                FileManager.default.isExecutableFile(atPath: $0.path)
            })
        else {
            return UnavailableSharedAccountRuntimeRPCClient()
        }

        let daemonClient = CodexAppServerProxyRPCClient(
            executableURL: executableURL,
            environmentOverrides: [:],
            socketURL: socketURL,
            timeout: 15,
            logContext: "shared source-account socket=\(socketURL.path)"
        )
        let temporaryClient = CodexAppServerRPCClient(
            executableURLProvider: { executableURL },
            timeout: 15
        )
        let socketIsReachable = CodexAccountProfileDaemonCoordinator.socketIsReachable(
            at: socketURL
        )
        CodexAccountRuntimeDiagnostics.append(
            socketIsReachable
                ? "source-account runtime=shared-daemon socket=\(socketURL.path)"
                : "source-account runtime=temporary-shared-stdio reason=shared-socket-unreachable"
        )
        return CodexSharedAccountRuntimeClientFactory.make(
            socketURL: socketURL,
            socketProbe: { _ in socketIsReachable },
            daemonClient: daemonClient,
            temporaryClient: temporaryClient
        )
    }
}

enum CodexSharedAccountRuntimeClientFactory {
    static func make(
        socketURL: URL,
        socketProbe: @Sendable (URL) -> Bool,
        daemonClient: any CodexAppServerRPCPerforming,
        temporaryClient: any CodexAppServerRPCPerforming
    ) -> any CodexAppServerRPCPerforming {
        if socketProbe(socketURL) {
            return daemonClient
        }
        return temporaryClient
    }
}

private struct UnavailableSharedAccountRuntimeRPCClient: CodexAppServerRPCPerforming {
    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        throw CodexThreadSourceProfileResolutionError.sharedRuntimeUnavailable
    }
}

final class CodexThreadSourceProfileResolver: @unchecked Sendable {
    typealias ProfilesProvider = @Sendable () -> [CodexAccountProfile]
    typealias SharedIdentityReader = @Sendable () throws -> CodexAccountProfileIdentity
    typealias ExecutableURLsProvider = @Sendable () -> [URL]
    typealias SocketProbe = @Sendable (URL) -> Bool

    private let profilesProvider: ProfilesProvider
    private let bindingStore: CodexThreadAccountProfileBindingStore
    private let identityStore: CodexAccountProfileIdentityStore
    private let sharedIdentityReader: SharedIdentityReader
    private let executableURLsProvider: ExecutableURLsProvider
    private let socketProbe: SocketProbe
    private let daemonBaseURL: URL
    private let sharedSQLiteHomeURL: URL

    init(
        profilesProvider: @escaping ProfilesProvider = {
            CodexAccountProfileStore().profiles
        },
        bindingStore: CodexThreadAccountProfileBindingStore =
            CodexThreadAccountProfileBindingStore(),
        identityStore: CodexAccountProfileIdentityStore =
            CodexAccountProfileIdentityStore(),
        sharedIdentityReader: @escaping SharedIdentityReader = {
            try CodexSharedAccountRuntimeIdentityReader().identity()
        },
        executableURLsProvider: @escaping ExecutableURLsProvider = {
            WorkspacePaths.codexExecutableURLs
        },
        socketProbe: @escaping SocketProbe = {
            CodexAccountProfileDaemonCoordinator.socketIsReachable(at: $0)
        },
        daemonBaseURL: URL = CodexAccountProfileRuntime.defaultDaemonBaseURL,
        sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    ) {
        self.profilesProvider = profilesProvider
        self.bindingStore = bindingStore
        self.identityStore = identityStore
        self.sharedIdentityReader = sharedIdentityReader
        self.executableURLsProvider = executableURLsProvider
        self.socketProbe = socketProbe
        self.daemonBaseURL = daemonBaseURL
        self.sharedSQLiteHomeURL = sharedSQLiteHomeURL
    }

    func resolveProfileID(for threadID: String) throws -> UUID {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else {
            throw CodexThreadSourceProfileResolutionError.invalidThreadID
        }
        if let existingProfileID = bindingStore.profileID(for: normalizedThreadID) {
            return existingProfileID
        }

        let sharedIdentity = try sharedIdentityReader()
        let matches = profilesProvider().filter { profile in
            identityStore.identity(for: profile.id)?.matchesAccount(sharedIdentity) == true
        }
        guard !matches.isEmpty else {
            CodexAccountRuntimeDiagnostics.append(
                "source-account unresolved thread=\(normalizedThreadID) reason=no-profile-match"
            )
            throw CodexThreadSourceProfileResolutionError.noMatchingProfile
        }
        guard matches.count == 1, let profile = matches.first else {
            CodexAccountRuntimeDiagnostics.append(
                "source-account unresolved thread=\(normalizedThreadID) reason=ambiguous-profile-match count=\(matches.count)"
            )
            throw CodexThreadSourceProfileResolutionError.ambiguousProfiles
        }

        let committedProfileID = bindingStore.bindIfUnbound(
            threadID: normalizedThreadID,
            to: profile.id
        )
        guard committedProfileID == profile.id else {
            CodexAccountRuntimeDiagnostics.append(
                "source-account unresolved thread=\(normalizedThreadID) reason=binding-changed"
            )
            throw CodexThreadSourceProfileResolutionError.bindingChanged
        }
        CodexAccountRuntimeDiagnostics.append(
            "source-account bound thread=\(normalizedThreadID) profile=\(profile.id.uuidString) label=\(profile.label) account_type=\(sharedIdentity.accountType)"
        )
        return profile.id
    }

    func resolveTaskStreamEndpoint(for threadID: String) throws -> CodexTaskStreamEndpoint {
        let profileID = try resolveProfileID(for: threadID)
        guard let profile = profilesProvider().first(where: { $0.id == profileID }) else {
            throw CodexThreadSourceProfileResolutionError.noMatchingProfile
        }
        guard let executableURL = executableURLsProvider().first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        }) else {
            throw CodexThreadSourceProfileResolutionError.missingExecutable
        }

        let daemonHomeURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: daemonBaseURL
        )
        let socketURL = daemonHomeURL
            .appendingPathComponent("app-server-control", isDirectory: true)
            .appendingPathComponent("app-server-control.sock")
        guard socketProbe(socketURL) else {
            CodexAccountRuntimeDiagnostics.append(
                "task-stream unavailable thread=\(threadID) profile=\(profile.id.uuidString) reason=socket-unreachable"
            )
            throw CodexThreadSourceProfileResolutionError.profileRuntimeUnavailable
        }

        return CodexTaskStreamEndpoint(
            profileID: profile.id,
            executableURL: executableURL,
            environmentOverrides: CodexAccountProfileRuntime.daemonTaskEnvironment(
                for: profile,
                daemonBaseURL: daemonBaseURL,
                sharedSQLiteHomeURL: sharedSQLiteHomeURL
            ),
            socketURL: socketURL
        )
    }
}
