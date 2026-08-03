import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAutomaticGoalContinuationTests {
    @Test
    func preferenceIsOptInAndPersists() throws {
        let defaultsName = "CodexAutomaticGoalContinuationTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)

        #expect(!preferences.automaticallyContinuesGoalsAcrossAccounts)
        preferences.automaticallyContinuesGoalsAcrossAccounts = true
        #expect(
            CompanionInteractionPreferences(defaults: defaults)
                .automaticallyContinuesGoalsAcrossAccounts
        )
    }

    @Test
    func policyRequiresAnIdleQuotaLimitedGoal() {
        #expect(CodexAutomaticGoalContinuationPolicy.isEligible(Self.item()))
        #expect(!CodexAutomaticGoalContinuationPolicy.isEligible(
            Self.item(runtimeStatus: .active)
        ))
        #expect(!CodexAutomaticGoalContinuationPolicy.isEligible(
            Self.item(runtimeStatus: .waitingOnApproval)
        ))
        #expect(!CodexAutomaticGoalContinuationPolicy.isEligible(
            Self.item(goalStatus: .paused)
        ))
        #expect(!CodexAutomaticGoalContinuationPolicy.isEligible(
            Self.item(rolloutPath: nil)
        ))
    }

    @Test
    func policyUsesExplicitQuotaStateAndEveryCodexWindow() throws {
        #expect(CodexAutomaticGoalContinuationPolicy.hasAvailableCodexUsage(
            try Self.usageSnapshot(primaryUsed: 45, weeklyUsed: 60)
        ))
        #expect(!CodexAutomaticGoalContinuationPolicy.hasAvailableCodexUsage(
            try Self.usageSnapshot(
                primaryUsed: 100,
                weeklyUsed: 60,
                reachedType: "primary"
            )
        ))
        #expect(!CodexAutomaticGoalContinuationPolicy.hasAvailableCodexUsage(
            try Self.usageSnapshot(primaryUsed: 20, weeklyUsed: 100)
        ))
    }

    @Test
    func eventKeyTracksTheQuotaEpisodeRatherThanTaskRefreshes() throws {
        let first = Self.item(
            updatedAt: Date(timeIntervalSince1970: 1_700_000_000),
            goalUpdatedAt: Date(timeIntervalSince1970: 1_700_000_100)
        )
        let refreshed = Self.item(
            updatedAt: Date(timeIntervalSince1970: 1_700_000_900),
            goalUpdatedAt: Date(timeIntervalSince1970: 1_700_000_100)
        )
        let nextQuotaEpisode = Self.item(
            updatedAt: Date(timeIntervalSince1970: 1_700_001_000),
            goalUpdatedAt: Date(timeIntervalSince1970: 1_700_001_000)
        )

        let firstKey = try #require(
            CodexAutomaticGoalContinuationPolicy.eventKey(for: first)
        )
        #expect(
            CodexAutomaticGoalContinuationPolicy.eventKey(for: refreshed)
                == firstKey
        )
        #expect(
            CodexAutomaticGoalContinuationPolicy.eventKey(for: nextQuotaEpisode)
                != firstKey
        )
    }

    @Test
    func pendingRecordSurvivesRestartAndCompletionDeduplicatesIt() throws {
        let defaultsName = "CodexAutomaticGoalContinuationStoreTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let record = CodexAutomaticGoalContinuationRecord(
            id: UUID(),
            eventKey: "thread|goal|source|revision",
            threadID: "thread",
            goalID: "goal",
            sourceProfileID: UUID(),
            targetProfileID: UUID(),
            stage: .handedOff
        )

        CodexAutomaticGoalContinuationStore(defaults: defaults).savePending(record)
        let restored = CodexAutomaticGoalContinuationStore(defaults: defaults)
        #expect(restored.pendingRecord == record)

        restored.markCompleted(record)
        #expect(restored.pendingRecord == nil)
        #expect(restored.hasCompleted(eventKey: record.eventKey))
    }

    @Test
    @MainActor
    func modelHandsOffExactRolloutAndSendsOneContinue() async throws {
        let defaultsName = "CodexAutomaticGoalContinuationModelTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)
        preferences.automaticallyContinuesGoalsAcrossAccounts = true

        let source = CodexAccountProfile(id: UUID(), label: "Main")
        let target = CodexAccountProfile(id: UUID(), label: "Backup")
        let third = CodexAccountProfile(id: UUID(), label: "Third")
        let binding = LockedProfileBinding(nil)
        let handoffRecorder = AutomaticHandoffRecorder(binding: binding)
        let sendRecorder = AutomaticContinueRecorder()
        let model = CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: preferences,
            codexPromptSubmitter: {
                prompt, threadID, cwd, action, expectedTurnID, clientMessageID, _ in
                await sendRecorder.record(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID
                )
                return .sent
            },
            codexAccountProfilesProvider: { [source, target, third] },
            codexThreadProfileIDProvider: { _ in binding.value },
            codexThreadSourceProfileIDResolver: { _ in
                binding.set(source.id)
                return source.id
            },
            codexAccountProfileAuthenticationChecker: { _ in .signedIn },
            codexAccountProfileUsageReader: { profile in
                try Self.usageSnapshot(
                    primaryUsed: profile.id == source.id ? 100 : 35,
                    weeklyUsed: 40,
                    reachedType: profile.id == source.id ? "primary" : nil
                )
            },
            codexAccountHandoffSubmitter: {
                threadID, rolloutURL, hasActiveTurn, profile in
                await handoffRecorder.record(
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    hasActiveTurn: hasActiveTurn,
                    profile: profile
                )
                return CodexThreadAccountHandoffResult(
                    threadID: "thread-1",
                    runtimeThreadID: "thread-1-fork",
                    rolloutURL: URL(fileURLWithPath: "/tmp/thread-1-fork.jsonl"),
                    profileID: profile.id
                )
            },
            automaticGoalContinuationStore:
                CodexAutomaticGoalContinuationStore(defaults: defaults),
            startsBackgroundServices: false
        )
        let item = Self.item()

        model.evaluateAutomaticGoalContinuation(for: [item])
        #expect(await Self.waitUntil {
            model.automaticGoalContinuationStatus == "Continued Task with Backup."
        })

        let handoff = await handoffRecorder.invocation
        #expect(handoff?.threadID == "thread-1")
        #expect(handoff?.rolloutURL.path == "/tmp/thread-1.jsonl")
        #expect(handoff?.hasActiveTurn == false)
        #expect(handoff?.profile == target)

        let sends = await sendRecorder.invocations
        #expect(sends.count == 1)
        #expect(sends.first?.prompt == "continue")
        #expect(sends.first?.threadID == "thread-1")
        #expect(sends.first?.cwd == "/tmp/project")
        #expect(sends.first?.action == .reply)
        #expect(sends.first?.expectedTurnID == nil)
        #expect(sends.first?.clientMessageID.hasPrefix("companion-auto-goal-") == true)

        model.evaluateAutomaticGoalContinuation(for: [item])
        try? await Task.sleep(for: .milliseconds(80))
        #expect(await sendRecorder.invocations.count == 1)
    }

    @Test
    @MainActor
    func modelDoesNothingForApprovalPendingWork() async throws {
        let defaultsName = "CodexAutomaticGoalContinuationApprovalTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)
        preferences.automaticallyContinuesGoalsAcrossAccounts = true
        let target = CodexAccountProfile(id: UUID(), label: "Backup")
        let sendRecorder = AutomaticContinueRecorder()
        let model = CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: preferences,
            codexPromptSubmitter: {
                prompt, threadID, cwd, action, expectedTurnID, clientMessageID, _ in
                await sendRecorder.record(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID
                )
                return .sent
            },
            codexAccountProfilesProvider: { [target] },
            codexAccountProfileAuthenticationChecker: { _ in .signedIn },
            codexAccountProfileUsageReader: { _ in
                try Self.usageSnapshot(primaryUsed: 20, weeklyUsed: 30)
            },
            automaticGoalContinuationStore:
                CodexAutomaticGoalContinuationStore(defaults: defaults),
            startsBackgroundServices: false
        )

        model.evaluateAutomaticGoalContinuation(for: [
            Self.item(runtimeStatus: .waitingOnApproval),
        ])
        try? await Task.sleep(for: .milliseconds(80))

        #expect(await sendRecorder.invocations.isEmpty)
        #expect(model.automaticGoalContinuationStatus == nil)
    }

    private static func item(
        goalStatus: CodexGoalStatus = .usageLimited,
        rolloutPath: String? = "/tmp/thread-1.jsonl",
        runtimeStatus: CodexThreadRuntimeStatus = .idle,
        updatedAt: Date = Date(timeIntervalSince1970: 1_700_000_000),
        goalUpdatedAt: Date = Date(timeIntervalSince1970: 1_700_000_100)
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-1",
            kind: .thread,
            title: "Task",
            subtitle: "Goal usage limited",
            fullMessage: "Usage stopped this goal.",
            updatedAt: updatedAt,
            startedAt: Date(timeIntervalSince1970: 1_699_999_000),
            status: .waiting,
            threadID: "thread-1",
            cwd: "/tmp/project",
            rolloutPath: rolloutPath,
            activeTurnID: nil,
            goalID: "goal-1",
            goalObjective: "Finish the task",
            goalStatus: goalStatus,
            goalElapsedSeconds: 1_000,
            goalTimerReferenceDate: nil,
            goalUpdatedAt: goalUpdatedAt,
            runtimeStatus: runtimeStatus
        )
    }

    private static func usageSnapshot(
        primaryUsed: Double,
        weeklyUsed: Double,
        reachedType: String? = nil
    ) throws -> CodexUsageSnapshot {
        let reachedJSON = reachedType.map { "\"\($0)\"" } ?? "null"
        return try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(
                """
                {
                  "rateLimits": {
                    "limitId": "codex",
                    "primary": {
                      "usedPercent": \(primaryUsed),
                      "windowDurationMins": 300
                    },
                    "secondary": {
                      "usedPercent": \(weeklyUsed),
                      "windowDurationMins": 10080
                    },
                    "planType": "pro",
                    "rateLimitReachedType": \(reachedJSON)
                  }
                }
                """.utf8
            )
        )
    }

    @MainActor
    private static func waitUntil(
        timeout: Duration = .seconds(2),
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

private final class LockedProfileBinding: @unchecked Sendable {
    private let lock = NSLock()
    private var profileID: UUID?

    init(_ profileID: UUID?) {
        self.profileID = profileID
    }

    var value: UUID? {
        lock.withLock { profileID }
    }

    func set(_ profileID: UUID?) {
        lock.withLock { self.profileID = profileID }
    }
}

private actor AutomaticHandoffRecorder {
    struct Invocation: Sendable {
        var threadID: String
        var rolloutURL: URL
        var hasActiveTurn: Bool
        var profile: CodexAccountProfile
    }

    private let binding: LockedProfileBinding
    private(set) var invocation: Invocation?

    init(binding: LockedProfileBinding) {
        self.binding = binding
    }

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
        binding.set(profile.id)
    }
}

private actor AutomaticContinueRecorder {
    struct Invocation: Sendable {
        var prompt: String
        var threadID: String
        var cwd: String?
        var action: CodexSendAction
        var expectedTurnID: String?
        var clientMessageID: String
    }

    private(set) var invocations: [Invocation] = []

    func record(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String
    ) {
        invocations.append(
            Invocation(
                prompt: prompt,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                clientMessageID: clientMessageID
            )
        )
    }
}
