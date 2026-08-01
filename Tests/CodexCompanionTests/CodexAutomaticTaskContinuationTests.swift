import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAutomaticTaskContinuationTests {
    @Test
    func preferenceIsOptInAndPersistsSeparatelyFromGoals() throws {
        let defaultsName = "CodexAutomaticTaskContinuationTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)

        #expect(!preferences.automaticallyContinuesQuotaInterruptedTasksAcrossAccounts)
        #expect(!preferences.automaticallyContinuesGoalsAcrossAccounts)

        preferences.automaticallyContinuesQuotaInterruptedTasksAcrossAccounts = true

        let restored = CompanionInteractionPreferences(defaults: defaults)
        #expect(restored.automaticallyContinuesQuotaInterruptedTasksAcrossAccounts)
        #expect(!restored.automaticallyContinuesGoalsAcrossAccounts)
    }

    @Test
    func parserAcceptsOnlyExplicitUsageLimitFailures() throws {
        let interruption = try #require(
            try CodexQuotaInterruptionParser.interruption(
                threadID: "thread-1",
                response: Self.turnResponse(
                    status: "failed",
                    codexErrorInfo: "usageLimitExceeded"
                )
            )
        )
        #expect(interruption.threadID == "thread-1")
        #expect(interruption.turnID == "turn-1")
        #expect(interruption.completedAt == Date(timeIntervalSince1970: 1_700_000_123))
        #expect(interruption.eventKey == "thread-1|turn-1")

        #expect(
            try CodexQuotaInterruptionParser.interruption(
                threadID: "thread-1",
                response: Self.turnResponse(
                    status: "failed",
                    codexErrorInfo: "internalServerError"
                )
            ) == nil
        )
        #expect(
            try CodexQuotaInterruptionParser.interruption(
                threadID: "thread-1",
                response: Self.turnResponse(
                    status: "completed",
                    codexErrorInfo: "usageLimitExceeded"
                )
            ) == nil
        )
        #expect(
            try CodexQuotaInterruptionParser.interruption(
                threadID: "thread-1",
                response: Self.turnResponse(
                    status: "failed",
                    codexErrorInfo: [
                        "responseStreamDisconnected": [
                            "httpStatusCode": 503,
                        ],
                    ]
                )
            ) == nil
        )
    }

    @Test
    func inspectionRequestReadsOnlyLatestUnloadedTurn() {
        let request = CodexQuotaInterruptionRequestFactory.latestTurn(
            id: 42,
            threadID: "thread-1"
        )

        #expect(request.id == 42)
        #expect(request.method == "thread/turns/list")
        #expect(request.params["threadId"] as? String == "thread-1")
        #expect(request.params["limit"] as? Int == 1)
        #expect(request.params["sortDirection"] as? String == "desc")
        #expect(request.params["itemsView"] as? String == "notLoaded")
    }

    @Test
    func policyRequiresStoppedNonGoalWork() {
        #expect(CodexAutomaticTaskContinuationPolicy.isCandidate(Self.item()))
        #expect(CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(runtimeStatus: .systemError)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(runtimeStatus: .active)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(runtimeStatus: .waitingOnApproval)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(runtimeStatus: .waitingOnUserInput)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(goalStatus: .usageLimited)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.isCandidate(
            Self.item(rolloutPath: nil)
        ))
    }

    @Test
    func policyRequiresConfirmedSourceExhaustion() throws {
        #expect(CodexAutomaticTaskContinuationPolicy.hasConfirmedCodexExhaustion(
            try Self.usageSnapshot(
                primaryUsed: 100,
                weeklyUsed: 65,
                reachedType: "primary"
            )
        ))
        #expect(CodexAutomaticTaskContinuationPolicy.hasConfirmedCodexExhaustion(
            try Self.usageSnapshot(primaryUsed: 35, weeklyUsed: 100)
        ))
        #expect(!CodexAutomaticTaskContinuationPolicy.hasConfirmedCodexExhaustion(
            try Self.usageSnapshot(primaryUsed: 35, weeklyUsed: 65)
        ))
    }

    @Test
    func pendingAndCompletedRecordsPreserveOriginAndDeduplicateEpisode() throws {
        let defaultsName = "CodexAutomaticTaskContinuationStoreTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let record = CodexAutomaticTaskContinuationRecord(
            id: UUID(),
            eventKey: "thread-1|turn-1",
            threadID: "thread-1",
            sourceProfileID: UUID(),
            targetProfileID: UUID(),
            stage: .handedOff
        )

        CodexAutomaticTaskContinuationStore(defaults: defaults).savePending(record)
        let restored = CodexAutomaticTaskContinuationStore(defaults: defaults)
        #expect(restored.pendingRecord == record)

        restored.markCompleted(record)
        #expect(restored.pendingRecord == nil)
        #expect(restored.hasCompleted(eventKey: record.eventKey))
        #expect(restored.completedRecord(eventKey: record.eventKey) == record)
    }

    @Test
    @MainActor
    func modelHandsOffExactTaskAndSendsOneGuardedContinue() async throws {
        let defaultsName = "CodexAutomaticTaskContinuationModelTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)
        preferences.automaticallyContinuesQuotaInterruptedTasksAcrossAccounts = true

        let source = CodexAccountProfile(id: UUID(), label: "Main")
        let target = CodexAccountProfile(id: UUID(), label: "Backup")
        let third = CodexAccountProfile(id: UUID(), label: "Third")
        let binding = TaskContinuationProfileBinding(source.id)
        let handoffRecorder = TaskContinuationHandoffRecorder(binding: binding)
        let sendRecorder = TaskContinuationSendRecorder()
        let inspectionRecorder = TaskContinuationInspectionRecorder(
            result: CodexQuotaInterruption(
                threadID: "thread-1",
                turnID: "turn-1",
                completedAt: Date(timeIntervalSince1970: 1_700_000_123)
            )
        )
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
                    threadID: threadID,
                    rolloutURL: rolloutURL,
                    profileID: profile.id
                )
            },
            codexQuotaInterruptionReader: { threadID, profile in
                try inspectionRecorder.read(threadID: threadID, profile: profile)
            },
            automaticTaskContinuationStore:
                CodexAutomaticTaskContinuationStore(defaults: defaults),
            startsBackgroundServices: false
        )
        let item = Self.item()

        model.evaluateAutomaticTaskContinuation(for: [item])
        #expect(await Self.waitUntil {
            model.automaticTaskContinuationStatus == "Continued Task with Backup."
        })

        #expect(inspectionRecorder.invocations == [
            .init(threadID: "thread-1", profileID: source.id),
        ])
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
        #expect(sends.first?.clientMessageID.hasPrefix("companion-auto-task-") == true)

        model.evaluateAutomaticTaskContinuation(for: [item])
        try? await Task.sleep(for: .milliseconds(80))
        #expect(await sendRecorder.invocations.count == 1)
    }

    @Test
    @MainActor
    func modelIgnoresNetworkFailuresAndApprovalPendingWork() async throws {
        let defaultsName = "CodexAutomaticTaskContinuationFailureTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let preferences = CompanionInteractionPreferences(defaults: defaults)
        preferences.automaticallyContinuesQuotaInterruptedTasksAcrossAccounts = true
        let source = CodexAccountProfile(id: UUID(), label: "Main")
        let target = CodexAccountProfile(id: UUID(), label: "Backup")
        let sendRecorder = TaskContinuationSendRecorder()
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
            codexAccountProfilesProvider: { [source, target] },
            codexThreadProfileIDProvider: { _ in source.id },
            codexAccountProfileAuthenticationChecker: { _ in .signedIn },
            codexAccountProfileUsageReader: { profile in
                try Self.usageSnapshot(
                    primaryUsed: profile.id == source.id ? 100 : 20,
                    weeklyUsed: 30,
                    reachedType: profile.id == source.id ? "primary" : nil
                )
            },
            codexQuotaInterruptionReader: { _, _ in
                throw TaskContinuationTestError.network
            },
            automaticTaskContinuationStore:
                CodexAutomaticTaskContinuationStore(defaults: defaults),
            startsBackgroundServices: false
        )

        model.evaluateAutomaticTaskContinuation(for: [
            Self.item(runtimeStatus: .systemError),
        ])
        #expect(await Self.waitUntil {
            model.automaticTaskContinuationStatus?.contains("could not verify") == true
        })
        model.evaluateAutomaticTaskContinuation(for: [
            Self.item(runtimeStatus: .waitingOnApproval),
        ])
        try? await Task.sleep(for: .milliseconds(80))

        #expect(await sendRecorder.invocations.isEmpty)
    }

    private static func item(
        runtimeStatus: CodexThreadRuntimeStatus = .idle,
        goalStatus: CodexGoalStatus? = nil,
        rolloutPath: String? = "/tmp/thread-1.jsonl"
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-1",
            kind: .thread,
            title: "Task",
            subtitle: "Stopped",
            fullMessage: "This task stopped.",
            updatedAt: Date(timeIntervalSince1970: 1_700_000_123),
            startedAt: Date(timeIntervalSince1970: 1_700_000_000),
            status: .failed,
            threadID: "thread-1",
            cwd: "/tmp/project",
            rolloutPath: rolloutPath,
            activeTurnID: "stale-turn-id",
            goalID: goalStatus == nil ? nil : "goal-1",
            goalObjective: goalStatus == nil ? nil : "Finish the task",
            goalStatus: goalStatus,
            goalElapsedSeconds: nil,
            goalTimerReferenceDate: nil,
            goalUpdatedAt: nil,
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

    private static func turnResponse(
        status: String,
        codexErrorInfo: Any
    ) -> CodexRPCResponse {
        CodexRPCResponse(
            result: [
                "data": [
                    [
                        "id": "turn-1",
                        "status": status,
                        "completedAt": 1_700_000_123,
                        "items": [],
                        "error": [
                            "message": "Turn stopped.",
                            "codexErrorInfo": codexErrorInfo,
                        ],
                    ],
                ],
            ],
            error: nil
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

private enum TaskContinuationTestError: Error {
    case network
}

private final class TaskContinuationProfileBinding: @unchecked Sendable {
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

private actor TaskContinuationHandoffRecorder {
    struct Invocation: Sendable {
        var threadID: String
        var rolloutURL: URL
        var hasActiveTurn: Bool
        var profile: CodexAccountProfile
    }

    private let binding: TaskContinuationProfileBinding
    private(set) var invocation: Invocation?

    init(binding: TaskContinuationProfileBinding) {
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

private actor TaskContinuationSendRecorder {
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

private final class TaskContinuationInspectionRecorder: @unchecked Sendable {
    struct Invocation: Equatable {
        var threadID: String
        var profileID: UUID
    }

    private let lock = NSLock()
    private let result: CodexQuotaInterruption?
    private var storedInvocations: [Invocation] = []

    init(result: CodexQuotaInterruption?) {
        self.result = result
    }

    var invocations: [Invocation] {
        lock.withLock { storedInvocations }
    }

    func read(
        threadID: String,
        profile: CodexAccountProfile
    ) throws -> CodexQuotaInterruption? {
        lock.withLock {
            storedInvocations.append(
                Invocation(threadID: threadID, profileID: profile.id)
            )
        }
        return result
    }
}
