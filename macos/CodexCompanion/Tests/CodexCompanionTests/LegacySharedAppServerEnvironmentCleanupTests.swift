import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct LegacySharedAppServerEnvironmentCleanupTests {
    @Test
    func removesLegacyLaunchAgentsAndUnsetsForcedDaemonEnvironment() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let launchAgents = root.appendingPathComponent("Library/LaunchAgents", isDirectory: true)
        try FileManager.default.createDirectory(at: launchAgents, withIntermediateDirectories: true)

        let labels = LegacySharedAppServerEnvironmentCleanup.legacyLaunchAgentLabels
        for label in labels {
            try Data("legacy".utf8).write(
                to: launchAgents.appendingPathComponent("\(label).plist")
            )
        }

        let calls = LegacyCleanupCommandRecorder()
        let cleanup = LegacySharedAppServerEnvironmentCleanup(
            homeDirectory: root,
            userID: 501,
            commandRunner: { arguments in
                calls.append(arguments)
                return 0
            }
        )

        cleanup.run()

        #expect(calls.values.contains(["bootout", "gui/501/\(labels[0])"]))
        #expect(calls.values.contains(["bootout", "gui/501/\(labels[1])"]))
        #expect(calls.values.contains([
            "unsetenv",
            LegacySharedAppServerEnvironmentCleanup.environmentKey,
        ]))
        for label in labels {
            #expect(!FileManager.default.fileExists(
                atPath: launchAgents.appendingPathComponent("\(label).plist").path
            ))
        }

        try? FileManager.default.removeItem(at: root)
    }

    @Test
    func cleanupIsIdempotentWhenLegacyFilesAreAbsent() {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let calls = LegacyCleanupCommandRecorder()
        let cleanup = LegacySharedAppServerEnvironmentCleanup(
            homeDirectory: root,
            userID: 501,
            commandRunner: { arguments in
                calls.append(arguments)
                return 113
            }
        )

        cleanup.run()
        cleanup.run()

        #expect(calls.values.filter { $0.first == "unsetenv" }.count == 2)
    }
}

private final class LegacyCleanupCommandRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [[String]] = []

    var values: [[String]] {
        lock.withLock { storage }
    }

    func append(_ value: [String]) {
        lock.withLock {
            storage.append(value)
        }
    }
}
