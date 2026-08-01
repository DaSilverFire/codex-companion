import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexProcessAccountSwitchTests {
    @Test
    func completedRolloutLifecycleClearsTheActiveTurnImmediately() throws {
        let rolloutURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("rollout-\(UUID().uuidString).jsonl")
        defer { try? FileManager.default.removeItem(at: rolloutURL) }
        let lines = [
            #"{"type":"event_msg","payload":{"type":"task_started","turn_id":"turn-live"}}"#,
            #"{"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Finished now"}],"internal_chat_message_metadata_passthrough":{"turn_id":"turn-live"}}}"#,
            #"{"type":"event_msg","payload":{"type":"task_complete","turn_id":"turn-live"}}"#,
        ]
        try Data((lines.joined(separator: "\n") + "\n").utf8).write(to: rolloutURL)

        let snapshot = CodexProcessStore.latestRolloutSnapshot(
            fromRolloutPath: rolloutURL.path
        )

        #expect(snapshot.assistantMessage == "Finished now")
        #expect(snapshot.lifecycle?.status == .completed)
        #expect(snapshot.activeTurnID == nil)
    }

    @Test
    func onlyStoppedPersistedThreadsCanSwitchAccounts() {
        let rolloutPath = "/tmp/session.jsonl"

        #expect(Self.item(status: .completed, rolloutPath: rolloutPath).canSwitchCodexAccount)
        #expect(Self.item(status: .failed, rolloutPath: rolloutPath).canSwitchCodexAccount)
        #expect(Self.item(
            status: .waiting,
            rolloutPath: rolloutPath,
            runtimeStatus: .idle
        ).canSwitchCodexAccount)

        #expect(!Self.item(status: .running, rolloutPath: rolloutPath).canSwitchCodexAccount)
        #expect(!Self.item(
            status: .waiting,
            rolloutPath: rolloutPath,
            runtimeStatus: .waitingOnApproval
        ).canSwitchCodexAccount)
        #expect(!Self.item(
            status: .waiting,
            rolloutPath: rolloutPath,
            runtimeStatus: .waitingOnUserInput
        ).canSwitchCodexAccount)
        #expect(!Self.item(status: .completed, rolloutPath: nil).canSwitchCodexAccount)
    }

    @Test
    func rolloutURLRejectsBlankAndNonFilePaths() {
        #expect(Self.item(status: .completed, rolloutPath: "  ").rolloutURL == nil)
        #expect(Self.item(status: .completed, rolloutPath: "https://example.com/task").rolloutURL == nil)

        let item = Self.item(status: .completed, rolloutPath: "/tmp/task.jsonl")
        #expect(item.rolloutURL?.standardizedFileURL.path == "/tmp/task.jsonl")
    }

    @Test
    @MainActor
    func modelHandsAStoppedTaskToASelectableProfile() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let recorder = AccountHandoffRecorder()
        let model = Self.model(
            profiles: [profile],
            binding: nil,
            handoff: { threadID, rolloutURL, hasActiveTurn, selectedProfile in
                await recorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: selectedProfile
                )
                return CodexThreadAccountHandoffResult(
                    threadID: "thread-1-fork",
                    rolloutURL: URL(fileURLWithPath: "/tmp/task-fork.jsonl"),
                    profileID: selectedProfile.id
                )
            }
        )
        let item = Self.item(status: .completed, rolloutPath: "/tmp/task.jsonl")

        #expect(model.availableCodexAccountProfiles(for: item) == [profile])
        model.switchCodexAccount(for: item, to: profile)

        #expect(await Self.waitUntil { model.isSwitchingAccountForProcessID == nil })
        let invocation = await recorder.invocation
        #expect(invocation?.threadID == "thread-1")
        #expect(invocation?.rolloutURL.standardizedFileURL.path == "/tmp/task.jsonl")
        #expect(invocation?.hasActiveTurn == false)
        #expect(invocation?.profile == profile)
        #expect(model.accountHandoffError == nil)
        #expect(model.status == "Task continued with Backup as a new task.")
    }

    @Test
    @MainActor
    func modelDoesNotOfferCurrentProfileOrMoveAnActiveTask() async throws {
        let current = CodexAccountProfile(id: UUID(), label: "Current")
        let backup = CodexAccountProfile(id: UUID(), label: "Backup")
        let recorder = AccountHandoffRecorder()
        let model = Self.model(
            profiles: [current, backup],
            binding: current.id,
            handoff: { threadID, rolloutURL, hasActiveTurn, profile in
                await recorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: profile
                )
                return CodexThreadAccountHandoffResult(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    profileID: profile.id
                )
            }
        )
        let completed = Self.item(status: .completed, rolloutPath: "/tmp/task.jsonl")
        let running = Self.item(status: .running, rolloutPath: "/tmp/task.jsonl")

        #expect(model.availableCodexAccountProfiles(for: completed) == [backup])
        #expect(model.availableCodexAccountProfiles(for: running).isEmpty)
        model.switchCodexAccount(for: running, to: backup)

        #expect(await recorder.invocation == nil)
        #expect(model.isSwitchingAccountForProcessID == nil)
        #expect(model.accountHandoffError == "Wait for the current Codex turn to stop before switching accounts.")
    }

    @Test
    @MainActor
    func modelRefusesSignedOutProfileBeforeLaunchingHandoffTransport() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let recorder = AccountHandoffRecorder()
        let model = Self.model(
            profiles: [profile],
            binding: nil,
            authentication: { _ in .signedOut },
            handoff: { threadID, rolloutURL, hasActiveTurn, selectedProfile in
                await recorder.record(
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
        let item = Self.item(status: .completed, rolloutPath: "/tmp/task.jsonl")

        model.switchCodexAccount(for: item, to: profile)

        #expect(await Self.waitUntil { model.isSwitchingAccountForProcessID == nil })
        #expect(await recorder.invocation == nil)
        #expect(
            model.accountHandoffError
                == "Sign in to Backup in Companion Settings before resuming this task."
        )
        #expect(model.accountHandoffFeedback?.processID == item.id)
        #expect(model.accountHandoffFeedback?.isError == true)
        #expect(
            model.accountHandoffFeedback?.message
                == "Sign in to Backup in Companion Settings before resuming this task."
        )
    }

    private static func item(
        status: CodexProcessItem.Status,
        rolloutPath: String?,
        runtimeStatus: CodexThreadRuntimeStatus? = nil
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-1",
            kind: .thread,
            title: "Task",
            subtitle: "Status",
            fullMessage: "Latest response",
            updatedAt: Date(),
            startedAt: nil,
            status: status,
            threadID: "thread-1",
            cwd: nil,
            rolloutPath: rolloutPath,
            runtimeStatus: runtimeStatus
        )
    }

    @MainActor
    private static func model(
        profiles: [CodexAccountProfile],
        binding: UUID?,
        authentication: @escaping CodexAccountProfileAuthenticationChecker = { _ in .signedIn },
        handoff: @escaping CodexAccountHandoffSubmitter
    ) -> CompanionAppModel {
        let suiteName = "CodexProcessAccountSwitchTests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName) ?? .standard
        return CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: CompanionInteractionPreferences(defaults: defaults),
            codexAccountProfilesProvider: { profiles },
            codexThreadProfileIDProvider: { _ in binding },
            codexAccountProfileAuthenticationChecker: authentication,
            codexAccountHandoffSubmitter: handoff,
            startsBackgroundServices: false
        )
    }

    @MainActor
    private static func waitUntil(
        timeout: Duration = .seconds(1),
        condition: () -> Bool
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }
}

private actor AccountHandoffRecorder {
    struct Invocation: Sendable {
        var threadID: String
        var rolloutURL: URL
        var hasActiveTurn: Bool
        var profile: CodexAccountProfile
    }

    private(set) var invocation: Invocation?

    func record(
        threadID: String,
        rolloutURL: URL,
        hasActiveTurn: Bool,
        profile: CodexAccountProfile
    ) {
        invocation = Invocation(
            threadID: threadID,
            rolloutURL: rolloutURL,
            hasActiveTurn: hasActiveTurn,
            profile: profile
        )
    }
}
