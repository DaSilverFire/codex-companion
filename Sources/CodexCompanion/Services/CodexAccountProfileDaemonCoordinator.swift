import Darwin
import Foundation

final class CodexAccountProfileRouteResolver: @unchecked Sendable {
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func selectedProfile() -> CodexAccountProfile? {
        let store = CodexAccountProfileStore(defaults: defaults)
        guard let selectedProfileID = store.selectedProfileID else { return nil }
        return store.profiles.first { $0.id == selectedProfileID }
    }

    func profile(for threadID: String) -> CodexAccountProfile? {
        let bindingStore = CodexThreadAccountProfileBindingStore(defaults: defaults)
        guard let profileID = bindingStore.profileID(for: threadID) else { return nil }
        return CodexAccountProfileStore(defaults: defaults).profiles.first { $0.id == profileID }
    }
}

struct CodexAccountProfileDaemonCommand: Equatable, Sendable {
    var executableURL: URL
    var arguments: [String]
    var environmentOverrides: [String: String]
    var socketURL: URL
}

enum CodexAccountProfileDaemonCommandFactory {
    static func start(
        profile: CodexAccountProfile,
        executableURL: URL,
        profileBaseURL: URL = CodexAccountProfileRuntime.defaultDaemonBaseURL,
        sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    ) -> CodexAccountProfileDaemonCommand {
        let profileHomeURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: profileBaseURL
        )
        return CodexAccountProfileDaemonCommand(
            executableURL: executableURL,
            arguments: ["app-server", "daemon", "start"],
            environmentOverrides: CodexAccountProfileRuntime.daemonTaskEnvironment(
                for: profile,
                daemonBaseURL: profileBaseURL,
                sharedSQLiteHomeURL: sharedSQLiteHomeURL
            ),
            socketURL: profileHomeURL
                .appendingPathComponent("app-server-control", isDirectory: true)
                .appendingPathComponent("app-server-control.sock")
        )
    }
}

enum CodexAccountProfileDaemonError: LocalizedError {
    case missingExecutable
    case launchFailed(String)
    case startFailed(Int32)
    case socketUnavailable

    var errorDescription: String? {
        switch self {
        case .missingExecutable:
            return "The official Codex executable could not be found."
        case .launchFailed(let message):
            return "The Codex account service could not start: \(message)"
        case .startFailed(let status):
            return "The Codex account service ended with status \(status)."
        case .socketUnavailable:
            return "The Codex account service did not become available."
        }
    }
}

actor CodexAccountProfileDaemonCoordinator {
    static let shared = CodexAccountProfileDaemonCoordinator()

    typealias ExecutableProvider = @Sendable () -> URL?
    typealias CommandRunner = @Sendable (CodexAccountProfileDaemonCommand) async throws -> Int32
    typealias SocketProbe = @Sendable (URL) -> Bool

    private let profileBaseURL: URL
    private let credentialBaseURL: URL
    private let sharedSQLiteHomeURL: URL
    private let managedStandaloneURL: URL
    private let executableProvider: ExecutableProvider
    private let commandRunner: CommandRunner
    private let socketProbe: SocketProbe
    private var readyProfileIDs: Set<UUID> = []

    init(
        profileBaseURL: URL = CodexAccountProfileRuntime.defaultDaemonBaseURL,
        credentialBaseURL: URL = CodexAccountProfileRuntime.defaultBaseURL,
        sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL,
        managedStandaloneURL: URL = CodexAccountProfileRuntime.defaultManagedStandaloneURL,
        executableProvider: @escaping ExecutableProvider = {
            WorkspacePaths.codexExecutableURLs.first(where: {
                FileManager.default.isExecutableFile(atPath: $0.path)
            })
        },
        commandRunner: @escaping CommandRunner = { command in
            try await Task.detached(priority: .userInitiated) {
                let process = Process()
                process.executableURL = command.executableURL
                process.arguments = command.arguments
                var environment = ProcessInfo.processInfo.environment
                command.environmentOverrides.forEach { environment[$0.key] = $0.value }
                process.environment = environment
                process.standardOutput = FileHandle.nullDevice
                process.standardError = FileHandle.nullDevice
                do {
                    try process.run()
                } catch {
                    throw CodexAccountProfileDaemonError.launchFailed(error.localizedDescription)
                }
                process.waitUntilExit()
                return process.terminationStatus
            }.value
        },
        socketProbe: @escaping SocketProbe = {
            CodexAccountProfileDaemonCoordinator.socketIsReachable(at: $0)
        }
    ) {
        self.profileBaseURL = profileBaseURL
        self.credentialBaseURL = credentialBaseURL
        self.sharedSQLiteHomeURL = sharedSQLiteHomeURL
        self.managedStandaloneURL = managedStandaloneURL
        self.executableProvider = executableProvider
        self.commandRunner = commandRunner
        self.socketProbe = socketProbe
    }

    func ensureRunning(for profile: CodexAccountProfile) async throws
        -> CodexAccountProfileDaemonCommand
    {
        guard let executableURL = executableProvider() else {
            throw CodexAccountProfileDaemonError.missingExecutable
        }
        try CodexAccountProfileRuntime.prepareDaemonHome(
            for: profile,
            daemonBaseURL: profileBaseURL,
            credentialBaseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )
        let command = CodexAccountProfileDaemonCommandFactory.start(
            profile: profile,
            executableURL: executableURL,
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )

        if socketProbe(command.socketURL) {
            readyProfileIDs.insert(profile.id)
            return command
        }

        let status = try await commandRunner(command)
        guard status == 0 else {
            if socketProbe(command.socketURL) {
                readyProfileIDs.insert(profile.id)
                return command
            }
            throw CodexAccountProfileDaemonError.startFailed(status)
        }

        for _ in 0..<50 {
            if socketProbe(command.socketURL) {
                readyProfileIDs.insert(profile.id)
                return command
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw CodexAccountProfileDaemonError.socketUnavailable
    }

    private static func socketIsReachable(at url: URL) -> Bool {
        let pathBytes = Array(url.path.utf8CString)
        var address = sockaddr_un()
        guard pathBytes.count <= MemoryLayout.size(ofValue: address.sun_path) else {
            return false
        }

        let fileDescriptor = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fileDescriptor >= 0 else { return false }
        defer { Darwin.close(fileDescriptor) }

        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        withUnsafeMutableBytes(of: &address.sun_path) { destination in
            pathBytes.withUnsafeBytes { source in
                destination.copyBytes(from: source)
            }
        }

        let result = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(
                    fileDescriptor,
                    $0,
                    socklen_t(MemoryLayout<sockaddr_un>.size)
                )
            }
        }
        return result == 0
    }
}
