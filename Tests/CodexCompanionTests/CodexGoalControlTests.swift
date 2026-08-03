import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexGoalControlTests {
    @Test
    func pausedAndBlockedGoalsCanResumeButLimitedGoalsCannot() {
        #expect(CodexGoalStatus.paused.canResume)
        #expect(CodexGoalStatus.blocked.canResume)
        #expect(!CodexGoalStatus.active.canResume)
        #expect(!CodexGoalStatus.usageLimited.canResume)
        #expect(!CodexGoalStatus.budgetLimited.canResume)
        #expect(!CodexGoalStatus.complete.canResume)
    }

    @Test
    func goalControlStateCopiesEditableGoalMetadata() throws {
        let item = Self.item(
            goalStatus: .blocked,
            objective: "Fix the stage",
            tokenBudget: 24_000
        )
        let state = try #require(CodexGoalControlState(item: item))

        #expect(state.threadID == "thread-1")
        #expect(state.draftObjective == "Fix the stage")
        #expect(state.status == .blocked)
        #expect(state.tokenBudget == 24_000)
        #expect(state.canResume)
        #expect(state.canEdit)
    }

    @Test
    func applyingAppServerGoalPreservesElapsedAccountingAndLimitedStatus() {
        let item = Self.item(goalStatus: nil, objective: nil, tokenBudget: nil)
        let goal = CodexGoalSnapshot(
            threadID: "thread-1",
            objective: "Verify the app",
            status: .usageLimited,
            tokenBudget: 50_000,
            tokensUsed: 20_000,
            timeUsedSeconds: 3661,
            createdAt: 100,
            updatedAt: 200
        )

        let updated = CodexProcessStore.applying(goal: goal, to: item)

        #expect(updated.goalStatus == .usageLimited)
        #expect(updated.goalObjective == "Verify the app")
        #expect(updated.goalTokenBudget == 50_000)
        #expect(updated.goalElapsedSeconds == 3661)
        #expect(updated.status == .waiting)
        #expect(updated.subtitle == "Goal usage limited at 1h 1m")
    }

    @Test
    func ordinaryProcessRefreshPreservesNewerAppServerGoalState() {
        let cached = Self.item(
            goalStatus: .blocked,
            objective: "Keep the live goal",
            tokenBudget: 10_000
        )
        let refreshed = Self.item(goalStatus: nil, objective: nil, tokenBudget: nil)

        let merged = CodexProcessStore.preservingCachedGoal(
            from: cached,
            in: refreshed
        )

        #expect(merged.goalStatus == .blocked)
        #expect(merged.goalObjective == "Keep the live goal")
        #expect(merged.goalTokenBudget == 10_000)
        #expect(merged.goalElapsedSeconds == 90)
        #expect(merged.status == .waiting)
    }

    @Test
    func staleGoalRefreshCannotOverwriteANewerMutation() {
        #expect(CodexProcessStore.shouldApplyGoalRefresh(
            startedAtRevision: 4,
            currentRevision: 4
        ))
        #expect(!CodexProcessStore.shouldApplyGoalRefresh(
            startedAtRevision: 4,
            currentRevision: 5
        ))
    }

    @Test
    @MainActor
    func canonicalDesktopGoalMutationTargetsTheActivePhysicalFork() async throws {
        let defaultsName = "CodexGoalControlLineageTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let lineages = CodexThreadLineageStore(defaults: defaults)
        #expect(lineages.registerFork(
            sourceThreadID: "canonical-thread",
            destinationThreadID: "physical-fork"
        ) == "canonical-thread")
        let recorder = GoalWriteRecorder()
        let store = CodexProcessStore(
            writeGoal: { threadID, objective, status, tokenBudget in
                recorder.write(
                    threadID: threadID,
                    objective: objective,
                    status: status,
                    tokenBudget: tokenBudget
                )
            },
            defaults: defaults,
            lineageStore: lineages
        )

        let goal = try await store.setGoal(
            threadID: "canonical-thread",
            objective: nil,
            status: .active,
            tokenBudget: nil
        )

        #expect(recorder.threadIDs == ["physical-fork"])
        #expect(goal.threadID == "canonical-thread")
    }

    private static func item(
        goalStatus: CodexGoalStatus?,
        objective: String?,
        tokenBudget: Int?
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-thread-1",
            kind: .thread,
            title: "Example task",
            subtitle: "Working now",
            fullMessage: "Latest response",
            updatedAt: Date(timeIntervalSince1970: 200),
            startedAt: nil,
            status: .running,
            threadID: "thread-1",
            cwd: "/tmp/project",
            goalID: goalStatus == nil ? nil : "100",
            goalObjective: objective,
            goalStatus: goalStatus,
            goalTokenBudget: tokenBudget,
            goalElapsedSeconds: goalStatus == nil ? nil : 90,
            goalTimerReferenceDate: nil
        )
    }
}

private final class GoalWriteRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recordedThreadIDs: [String] = []

    var threadIDs: [String] {
        lock.withLock { recordedThreadIDs }
    }

    func write(
        threadID: String,
        objective: String?,
        status: CodexGoalStatus?,
        tokenBudget: Int?
    ) -> CodexGoalSnapshot {
        _ = objective
        _ = tokenBudget
        lock.withLock { recordedThreadIDs.append(threadID) }
        return CodexGoalSnapshot(
            threadID: threadID,
            objective: "Preserve the goal",
            status: status ?? .blocked,
            tokenBudget: nil,
            tokensUsed: 0,
            timeUsedSeconds: 10,
            createdAt: 100,
            updatedAt: 110
        )
    }
}
