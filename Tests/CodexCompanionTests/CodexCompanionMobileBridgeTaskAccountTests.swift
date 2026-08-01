import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeTaskAccountTests {
    @Test
    func taskListIncludesTheAccountThatOwnsEachThread() async throws {
        let fixture = try TaskAccountBridgeFixture()
        defer { fixture.remove() }

        let profile = CodexAccountProfile(id: UUID(), label: "Account 3")
        let archive = CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            sqliteExecutableURL: fixture.sqliteURL,
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            accountProfileProvider: { profileID in
                profileID == profile.id ? profile : nil
            },
            threadAccountProfileIDProvider: { threadID in
                threadID == fixture.threadID ? profile.id : nil
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(operation: .listTasks)
        )

        #expect(response.succeeded)
        #expect(response.tasks?.count == 1)
        #expect(response.tasks?.first?.accountProfileID == profile.id)
        #expect(response.tasks?.first?.accountProfileLabel == "Account 3")
    }

    @Test
    func stoppedTaskCanBeHandedToASelectedAccount() async throws {
        let fixture = try TaskAccountBridgeFixture()
        defer { fixture.remove() }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let recorder = TaskAccountHandoffRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            sqliteExecutableURL: fixture.sqliteURL,
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            accountProfileProvider: { profileID in
                profileID == profile.id ? profile : nil
            },
            threadAccountHandoffSubmitter: { threadID, rolloutURL, hasActiveTurn, selectedProfile in
                recorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: selectedProfile
                )
                return CodexThreadAccountHandoffResult(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    profileID: selectedProfile.id
                )
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .switchTaskAccount,
                threadID: fixture.threadID,
                accountProfileID: profile.id
            )
        )

        #expect(response.succeeded)
        #expect(response.threadID == fixture.threadID)
        #expect(response.message == "Task will resume with Main.")
        let invocation = try #require(recorder.invocation)
        #expect(invocation.threadID == fixture.threadID)
        #expect(invocation.rolloutURL == fixture.rolloutURL.standardizedFileURL)
        #expect(invocation.hasActiveTurn == false)
        #expect(invocation.profile == profile)
    }

    @Test
    func activeTaskCannotBeHandedToAnotherAccount() async throws {
        let fixture = try TaskAccountBridgeFixture(lifecycleType: "turn_started")
        defer { fixture.remove() }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let recorder = TaskAccountHandoffRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            sqliteExecutableURL: fixture.sqliteURL,
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            accountProfileProvider: { _ in profile },
            threadAccountHandoffSubmitter: { threadID, rolloutURL, hasActiveTurn, selectedProfile in
                recorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: selectedProfile
                )
                throw CodexThreadAccountHandoffError.activeTurn
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .switchTaskAccount,
                threadID: fixture.threadID,
                accountProfileID: profile.id
            )
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "account_handoff_active")
        #expect(recorder.invocation?.hasActiveTurn == true)
    }

    @Test
    func unknownDestinationAccountIsRejectedBeforeHandoff() async throws {
        let fixture = try TaskAccountBridgeFixture()
        defer { fixture.remove() }

        let recorder = TaskAccountHandoffRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            sqliteExecutableURL: fixture.sqliteURL,
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            accountProfileProvider: { _ in nil },
            threadAccountHandoffSubmitter: { threadID, rolloutURL, hasActiveTurn, selectedProfile in
                recorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: selectedProfile
                )
                return CodexThreadAccountHandoffResult(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    profileID: selectedProfile.id
                )
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .switchTaskAccount,
                threadID: fixture.threadID,
                accountProfileID: UUID()
            )
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "unknown_account_profile")
        #expect(recorder.invocation == nil)
    }
}

private struct TaskAccountBridgeFixture {
    let threadID = "thread-account-3"
    let root: URL
    let sqliteURL: URL
    let rolloutURL: URL

    init(lifecycleType: String = "task_complete") throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("CompanionTaskAccountBridgeTests-\(UUID().uuidString)", isDirectory: true)
        let codexDirectory = root.appendingPathComponent(".codex", isDirectory: true)
        let databaseURL = codexDirectory.appendingPathComponent("state_5.sqlite")
        sqliteURL = root.appendingPathComponent("fixture-sqlite")
        rolloutURL = codexDirectory.appendingPathComponent("rollout-\(threadID).jsonl")

        try FileManager.default.createDirectory(at: codexDirectory, withIntermediateDirectories: true)
        _ = FileManager.default.createFile(atPath: databaseURL.path, contents: Data())
        let lifecycleLine = #"{"type":"event_msg","payload":{"type":"\#(lifecycleType)","turn_id":"turn-account"}}"#
        try Data((lifecycleLine + "\n").utf8).write(to: rolloutURL, options: .atomic)

        let columns = [
            threadID,
            "Owned task",
            root.path,
            "1785211200000",
            "Create an owned task",
            rolloutURL.path,
            "Account-bound preview",
            "gpt-5.6-sol",
            "high",
            "1785211200000",
        ]
        let row = columns.joined(separator: "\u{1f}") + "\u{1e}"
        let script = """
        #!/bin/sh
        case "$*" in
          *"select rollout_path"*) printf '%s' \(Self.shellQuoted(rolloutURL.path)) ;;
          *) printf '%s' \(Self.shellQuoted(row)) ;;
        esac
        """
        try Data(script.utf8).write(to: sqliteURL, options: .atomic)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755],
            ofItemAtPath: sqliteURL.path
        )
    }

    func remove() {
        try? FileManager.default.removeItem(at: root)
    }

    private static func shellQuoted(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }
}

private struct TaskAccountHandoffInvocation: Equatable {
    var threadID: String
    var rolloutURL: URL
    var hasActiveTurn: Bool
    var profile: CodexAccountProfile
}

private final class TaskAccountHandoffRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recordedInvocation: TaskAccountHandoffInvocation?

    var invocation: TaskAccountHandoffInvocation? {
        lock.withLock { recordedInvocation }
    }

    func record(
        threadID: String,
        rolloutURL: URL,
        hasActiveTurn: Bool,
        profile: CodexAccountProfile
    ) {
        lock.withLock {
            recordedInvocation = TaskAccountHandoffInvocation(
                threadID: threadID,
                rolloutURL: rolloutURL,
                hasActiveTurn: hasActiveTurn,
                profile: profile
            )
        }
    }
}
