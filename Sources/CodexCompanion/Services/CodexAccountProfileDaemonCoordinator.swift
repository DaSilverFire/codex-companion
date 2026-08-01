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
        command(
            profile: profile,
            executableURL: executableURL,
            arguments: ["app-server", "daemon", "start"],
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )
    }

    static func restart(
        profile: CodexAccountProfile,
        executableURL: URL,
        profileBaseURL: URL = CodexAccountProfileRuntime.defaultDaemonBaseURL,
        sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    ) -> CodexAccountProfileDaemonCommand {
        command(
            profile: profile,
            executableURL: executableURL,
            arguments: ["app-server", "daemon", "restart"],
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )
    }

    private static func command(
        profile: CodexAccountProfile,
        executableURL: URL,
        arguments: [String],
        profileBaseURL: URL,
        sharedSQLiteHomeURL: URL
    ) -> CodexAccountProfileDaemonCommand {
        let profileHomeURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: profileBaseURL
        )
        return CodexAccountProfileDaemonCommand(
            executableURL: executableURL,
            arguments: arguments,
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
    case runtimeUnverifiable

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
        case .runtimeUnverifiable:
            return "The Codex account service could not verify its shared task catalog."
        }
    }
}

enum CodexAccountProfileDaemonRuntimeCompatibility: Equatable, Sendable {
    case compatible
    case incompatible
    case unavailable
}

enum CodexAccountProfileDaemonRuntimeInspector {
    private struct PIDRecord: Decodable {
        var pid: Int32
    }

    static func compatibility(
        for command: CodexAccountProfileDaemonCommand
    ) -> CodexAccountProfileDaemonRuntimeCompatibility {
        guard
            let daemonHomePath = command.environmentOverrides["CODEX_HOME"],
            let sqliteHomePath = command.environmentOverrides["CODEX_SQLITE_HOME"]
        else {
            return .unavailable
        }

        let pidURL = URL(fileURLWithPath: daemonHomePath, isDirectory: true)
            .appendingPathComponent("app-server-daemon", isDirectory: true)
            .appendingPathComponent("app-server.pid")
        guard
            let data = try? Data(contentsOf: pidURL),
            let record = try? JSONDecoder().decode(PIDRecord.self, from: data),
            record.pid > 0,
            let environment = processEnvironment(for: record.pid)
        else {
            return .unavailable
        }

        return compatibility(
            environment: environment,
            expectedDaemonHomePath: daemonHomePath,
            expectedSQLiteHomePath: sqliteHomePath
        )
    }

    static func compatibility(
        environment: [String: String],
        expectedDaemonHomePath: String,
        expectedSQLiteHomePath: String
    ) -> CodexAccountProfileDaemonRuntimeCompatibility {
        guard
            let actualDaemonHomePath = environment["CODEX_HOME"],
            let actualSQLiteHomePath = environment["CODEX_SQLITE_HOME"]
        else {
            return .incompatible
        }
        let expectedDaemonHomeURL = URL(fileURLWithPath: expectedDaemonHomePath)
            .standardizedFileURL
        let expectedSQLiteHomeURL = URL(fileURLWithPath: expectedSQLiteHomePath)
            .standardizedFileURL
        let actualDaemonHomeURL = URL(fileURLWithPath: actualDaemonHomePath)
            .standardizedFileURL
        let actualSQLiteHomeURL = URL(fileURLWithPath: actualSQLiteHomePath)
            .standardizedFileURL
        return actualDaemonHomeURL == expectedDaemonHomeURL
            && actualSQLiteHomeURL == expectedSQLiteHomeURL
            ? .compatible
            : .incompatible
    }

    static func processEnvironment(for pid: Int32) -> [String: String]? {
        var mib = [Int32(CTL_KERN), Int32(KERN_PROCARGS2), pid]
        var size = 0
        let sizeStatus = mib.withUnsafeMutableBufferPointer { pointer in
            Darwin.sysctl(
                pointer.baseAddress,
                u_int(pointer.count),
                nil,
                &size,
                nil,
                0
            )
        }
        guard sizeStatus == 0, size > MemoryLayout<Int32>.size else { return nil }

        var buffer = [UInt8](repeating: 0, count: size)
        let readStatus = mib.withUnsafeMutableBufferPointer { pointer in
            buffer.withUnsafeMutableBytes { bytes in
                Darwin.sysctl(
                    pointer.baseAddress,
                    u_int(pointer.count),
                    bytes.baseAddress,
                    &size,
                    nil,
                    0
                )
            }
        }
        guard readStatus == 0 else { return nil }

        var environment: [String: String] = [:]
        var start = MemoryLayout<Int32>.size
        for index in start..<size where buffer[index] == 0 {
            defer { start = index + 1 }
            guard index > start else { continue }
            let bytes = buffer[start..<index]
            guard
                let entry = String(bytes: bytes, encoding: .utf8),
                let separator = entry.firstIndex(of: "=")
            else {
                continue
            }
            let key = String(entry[..<separator])
            let value = String(entry[entry.index(after: separator)...])
            environment[key] = value
        }
        return environment
    }
}

enum CodexAccountProfileDaemonStalledRestartRecovery {
    private struct PIDRecord: Decodable {
        var pid: Int32
    }

    private struct ProcessSnapshot {
        var parentPID: Int32
        var state: Int8
    }

    static func recover(_ command: CodexAccountProfileDaemonCommand) -> Bool {
        guard let daemonHomePath = command.environmentOverrides["CODEX_HOME"] else {
            return false
        }
        let daemonDirectoryURL = URL(fileURLWithPath: daemonHomePath, isDirectory: true)
            .appendingPathComponent("app-server-daemon", isDirectory: true)
        guard
            let appServer = pidRecord(
                at: daemonDirectoryURL.appendingPathComponent("app-server.pid")
            ),
            let updater = pidRecord(
                at: daemonDirectoryURL.appendingPathComponent("app-server-updater.pid")
            ),
            let appServerSnapshot = processSnapshot(for: appServer.pid),
            appServerSnapshot.state == Int8(SZOMB),
            appServerSnapshot.parentPID == updater.pid,
            let updaterEnvironment = CodexAccountProfileDaemonRuntimeInspector
                .processEnvironment(for: updater.pid),
            let updaterHomePath = updaterEnvironment["CODEX_HOME"],
            URL(fileURLWithPath: updaterHomePath).standardizedFileURL
                == URL(fileURLWithPath: daemonHomePath).standardizedFileURL
        else {
            return false
        }

        guard Darwin.kill(updater.pid, SIGTERM) == 0 else { return false }
        for _ in 0..<30 {
            if processSnapshot(for: updater.pid) == nil {
                return true
            }
            Darwin.usleep(100_000)
        }
        return false
    }

    private static func pidRecord(at url: URL) -> PIDRecord? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(PIDRecord.self, from: data)
    }

    private static func processSnapshot(for pid: Int32) -> ProcessSnapshot? {
        var mib = [Int32(CTL_KERN), Int32(KERN_PROC), Int32(KERN_PROC_PID), pid]
        var process = kinfo_proc()
        var size = MemoryLayout.size(ofValue: process)
        let status = mib.withUnsafeMutableBufferPointer { pointer in
            withUnsafeMutablePointer(to: &process) { processPointer in
                Darwin.sysctl(
                    pointer.baseAddress,
                    u_int(pointer.count),
                    processPointer,
                    &size,
                    nil,
                    0
                )
            }
        }
        guard status == 0, size == MemoryLayout.size(ofValue: process) else {
            return nil
        }
        return ProcessSnapshot(
            parentPID: process.kp_eproc.e_ppid,
            state: process.kp_proc.p_stat
        )
    }
}

actor CodexAccountProfileDaemonCoordinator {
    static let shared = CodexAccountProfileDaemonCoordinator()

    typealias ExecutableProvider = @Sendable () -> URL?
    typealias CommandRunner = @Sendable (CodexAccountProfileDaemonCommand) async throws -> Int32
    typealias SocketProbe = @Sendable (URL) -> Bool
    typealias RuntimeProbe = @Sendable (CodexAccountProfileDaemonCommand) async
        -> CodexAccountProfileDaemonRuntimeCompatibility
    typealias StalledRestartRecovery = @Sendable (CodexAccountProfileDaemonCommand) async
        -> Bool

    private let profileBaseURL: URL
    private let credentialBaseURL: URL
    private let sharedSQLiteHomeURL: URL
    private let managedStandaloneURL: URL
    private let executableProvider: ExecutableProvider
    private let commandRunner: CommandRunner
    private let socketProbe: SocketProbe
    private let runtimeProbe: RuntimeProbe
    private let stalledRestartRecovery: StalledRestartRecovery
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
                let stdoutPipe = Pipe()
                let stderrPipe = Pipe()
                process.executableURL = command.executableURL
                process.arguments = command.arguments
                var environment = ProcessInfo.processInfo.environment
                command.environmentOverrides.forEach { environment[$0.key] = $0.value }
                process.environment = environment
                process.standardOutput = stdoutPipe
                process.standardError = stderrPipe
                do {
                    try process.run()
                } catch {
                    throw CodexAccountProfileDaemonError.launchFailed(error.localizedDescription)
                }
                CodexAccountRuntimeDiagnostics.append(
                    "daemon command pid=\(process.processIdentifier) "
                        + "executable=\(command.executableURL.path) "
                        + "arguments=\(command.arguments.joined(separator: " "))"
                )
                process.waitUntilExit()
                logCodexAccountDaemonOutput(stdoutPipe, label: "daemon stdout")
                logCodexAccountDaemonOutput(stderrPipe, label: "daemon stderr")
                return process.terminationStatus
            }.value
        },
        socketProbe: @escaping SocketProbe = {
            CodexAccountProfileDaemonCoordinator.socketIsReachable(at: $0)
        },
        runtimeProbe: @escaping RuntimeProbe = { command in
            await Task.detached(priority: .utility) {
                CodexAccountProfileDaemonRuntimeInspector.compatibility(for: command)
            }.value
        },
        stalledRestartRecovery: @escaping StalledRestartRecovery = { command in
            await Task.detached(priority: .utility) {
                CodexAccountProfileDaemonStalledRestartRecovery.recover(command)
            }.value
        }
    ) {
        self.profileBaseURL = profileBaseURL
        self.credentialBaseURL = credentialBaseURL
        self.sharedSQLiteHomeURL = sharedSQLiteHomeURL
        self.managedStandaloneURL = managedStandaloneURL
        self.executableProvider = executableProvider
        self.commandRunner = commandRunner
        self.socketProbe = socketProbe
        self.runtimeProbe = runtimeProbe
        self.stalledRestartRecovery = stalledRestartRecovery
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
        CodexAccountRuntimeDiagnostics.append(
            "profile=\(profile.id.uuidString) label=\(profile.label) ensure daemon "
                + "credential_home=\(CodexAccountProfileRuntime.homeURL(for: profile, baseURL: credentialBaseURL).path) "
                + "daemon_home=\(command.environmentOverrides["CODEX_HOME"] ?? "missing") "
                + "sqlite_home=\(command.environmentOverrides["CODEX_SQLITE_HOME"] ?? "missing") "
                + "socket=\(command.socketURL.path) executable=\(command.executableURL.path)"
        )

        if socketProbe(command.socketURL) {
            switch await runtimeProbe(command) {
            case .compatible:
                readyProfileIDs.insert(profile.id)
                CodexAccountRuntimeDiagnostics.append(
                    "profile=\(profile.id.uuidString) reused daemon socket=\(command.socketURL.path)"
                )
                return command
            case .incompatible:
                CodexAccountRuntimeDiagnostics.append(
                    "profile=\(profile.id.uuidString) stale daemon runtime; restarting profile service"
                )
                return try await restartPreparedDaemon(
                    profile: profile,
                    executableURL: executableURL
                )
            case .unavailable:
                CodexAccountRuntimeDiagnostics.append(
                    "profile=\(profile.id.uuidString) daemon runtime environment unavailable"
                )
                throw CodexAccountProfileDaemonError.runtimeUnverifiable
            }
        }

        let status = try await commandRunner(command)
        guard status == 0 else {
            if socketProbe(command.socketURL), await runtimeProbe(command) == .compatible {
                readyProfileIDs.insert(profile.id)
                CodexAccountRuntimeDiagnostics.append(
                    "profile=\(profile.id.uuidString) daemon ready socket=\(command.socketURL.path)"
                )
                return command
            }
            throw CodexAccountProfileDaemonError.startFailed(status)
        }

        for _ in 0..<50 {
            if socketProbe(command.socketURL), await runtimeProbe(command) == .compatible {
                readyProfileIDs.insert(profile.id)
                return command
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw CodexAccountProfileDaemonError.socketUnavailable
    }

    func restart(for profile: CodexAccountProfile) async throws
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
        return try await restartPreparedDaemon(
            profile: profile,
            executableURL: executableURL
        )
    }

    private func restartPreparedDaemon(
        profile: CodexAccountProfile,
        executableURL: URL
    ) async throws -> CodexAccountProfileDaemonCommand {
        let command = CodexAccountProfileDaemonCommandFactory.restart(
            profile: profile,
            executableURL: executableURL,
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )
        readyProfileIDs.remove(profile.id)
        CodexAccountRuntimeDiagnostics.append(
            "profile=\(profile.id.uuidString) label=\(profile.label) restarting daemon "
                + "daemon_home=\(command.environmentOverrides["CODEX_HOME"] ?? "missing") "
                + "sqlite_home=\(command.environmentOverrides["CODEX_SQLITE_HOME"] ?? "missing") "
                + "socket=\(command.socketURL.path)"
        )

        let status = try await commandRunner(command)
        var readyCommand = command
        if status != 0 {
            guard await stalledRestartRecovery(command) else {
                throw CodexAccountProfileDaemonError.startFailed(status)
            }
            CodexAccountRuntimeDiagnostics.append(
                "profile=\(profile.id.uuidString) recovered stalled profile daemon restart"
            )
            readyCommand = CodexAccountProfileDaemonCommandFactory.start(
                profile: profile,
                executableURL: executableURL,
                profileBaseURL: profileBaseURL,
                sharedSQLiteHomeURL: sharedSQLiteHomeURL
            )
            let startStatus = try await commandRunner(readyCommand)
            guard startStatus == 0 else {
                throw CodexAccountProfileDaemonError.startFailed(startStatus)
            }
        }
        for _ in 0..<50 {
            if socketProbe(readyCommand.socketURL),
               await runtimeProbe(readyCommand) == .compatible
            {
                readyProfileIDs.insert(profile.id)
                CodexAccountRuntimeDiagnostics.append(
                    "profile=\(profile.id.uuidString) daemon restarted socket=\(readyCommand.socketURL.path)"
                )
                return readyCommand
            }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw CodexAccountProfileDaemonError.socketUnavailable
    }

    static func socketIsReachable(at url: URL) -> Bool {
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

private func logCodexAccountDaemonOutput(_ pipe: Pipe, label: String) {
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    guard
        let text = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
        !text.isEmpty
    else {
        return
    }
    CodexAccountRuntimeDiagnostics.append("\(label) \(text)")
}
