import Foundation

struct LegacySharedAppServerEnvironmentCleanup: Sendable {
    static let environmentKey = "CODEX_APP_SERVER_USE_LOCAL_DAEMON"
    static let legacyLaunchAgentLabels = [
        "com.harlin.codex-companion.shared-app-server-environment",
        "com.silverfire.codexcompanion.shared-app-server-environment",
    ]

    typealias CommandRunner = @Sendable (_ arguments: [String]) -> Int32

    private let homeDirectory: URL
    private let userID: uid_t
    private let commandRunner: CommandRunner

    init(
        homeDirectory: URL = FileManager.default.homeDirectoryForCurrentUser,
        userID: uid_t = getuid(),
        commandRunner: @escaping CommandRunner = Self.runLaunchctl
    ) {
        self.homeDirectory = homeDirectory
        self.userID = userID
        self.commandRunner = commandRunner
    }

    func run() {
        let launchAgents = homeDirectory
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("LaunchAgents", isDirectory: true)

        for label in Self.legacyLaunchAgentLabels {
            _ = commandRunner(["bootout", "gui/\(userID)/\(label)"])
            try? FileManager.default.removeItem(
                at: launchAgents.appendingPathComponent("\(label).plist")
            )
        }
        _ = commandRunner(["unsetenv", Self.environmentKey])
    }

    private static func runLaunchctl(arguments: [String]) -> Int32 {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus
        } catch {
            return -1
        }
    }
}
