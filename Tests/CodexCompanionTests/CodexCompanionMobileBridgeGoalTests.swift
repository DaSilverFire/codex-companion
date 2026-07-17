import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeGoalTests {
    @Test
    func createGoalRoutesObjectiveAndBudgetToNativeControl() async throws {
        let controller = RecordingGoalController(
            goal: Self.goal(objective: "Create mobile controls", status: .active)
        )
        let server = CodexCompanionMobileBridgeServer(goalControlService: controller)
        let request = CompanionBridgeRequest(
            operation: .createGoal,
            threadID: "thread-create",
            goalObjective: "Create mobile controls",
            goalTokenBudget: 80_000
        )

        let response = await server.handle(request)

        #expect(response.succeeded)
        #expect(response.goal?.objective == "Create mobile controls")
        #expect(response.goal?.status == .active)
        #expect(controller.recordedCalls == [
            .create(
                threadID: "thread-create",
                objective: "Create mobile controls",
                tokenBudget: 80_000
            ),
        ])
    }

    @Test
    func resumeGoalRoutesWithoutReplacingGoalMetadata() async {
        let controller = RecordingGoalController(
            goal: Self.goal(objective: "Resume mobile controls", status: .active)
        )
        let server = CodexCompanionMobileBridgeServer(goalControlService: controller)

        let response = await server.handle(
            CompanionBridgeRequest(operation: .resumeGoal, threadID: "thread-resume")
        )

        #expect(response.succeeded)
        #expect(response.goal?.objective == "Resume mobile controls")
        #expect(controller.recordedCalls == [.resume(threadID: "thread-resume")])
    }

    @Test
    func updateGoalRoutesTheReplacementObjective() async {
        let controller = RecordingGoalController(
            goal: Self.goal(objective: "Changed objective", status: .blocked)
        )
        let server = CodexCompanionMobileBridgeServer(goalControlService: controller)
        let request = CompanionBridgeRequest(
            operation: .updateGoal,
            threadID: "thread-edit",
            goalObjective: "Changed objective"
        )

        let response = await server.handle(request)

        #expect(response.succeeded)
        #expect(response.goal?.status == .blocked)
        #expect(controller.recordedCalls == [
            .update(threadID: "thread-edit", objective: "Changed objective"),
        ])
    }

    @Test
    func createGoalRejectsMissingObjectiveWithoutCallingNativeControl() async {
        let controller = RecordingGoalController(goal: Self.goal())
        let server = CodexCompanionMobileBridgeServer(goalControlService: controller)

        let response = await server.handle(
            CompanionBridgeRequest(operation: .createGoal, threadID: "thread-create")
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "invalid_goal")
        #expect(controller.recordedCalls.isEmpty)
    }

    @Test
    func taskGoalHydrationUsesTheSharedBridgeGoalShape() throws {
        let task = CompanionBridgeTask(
            id: "thread-goal",
            title: "Goal task",
            preview: "Working",
            updatedAt: Date(timeIntervalSince1970: 200),
            cwd: "/workspace",
            status: .running,
            needsApproval: false,
            activeTurnID: nil,
            model: "gpt-current",
            reasoningEffort: "high"
        )

        let hydrated = CodexCompanionMobileBridgeServer.attachingGoals(
            ["thread-goal": Self.goal(objective: "Hydrated goal", status: .paused)],
            to: [task]
        )
        let goal = try #require(hydrated.first?.goal)

        #expect(goal.objective == "Hydrated goal")
        #expect(goal.status == .paused)
        #expect(goal.tokenBudget == 80_000)
        #expect(goal.elapsedSeconds == 45)
    }

    private static func goal(
        objective: String = "Goal objective",
        status: CodexGoalStatus = .active
    ) -> CodexGoalSnapshot {
        CodexGoalSnapshot(
            threadID: "thread-goal",
            objective: objective,
            status: status,
            tokenBudget: 80_000,
            tokensUsed: 1_200,
            timeUsedSeconds: 45,
            createdAt: 100,
            updatedAt: 200
        )
    }
}

private final class RecordingGoalController: CodexGoalControlling, @unchecked Sendable {
    enum Call: Equatable {
        case create(threadID: String, objective: String, tokenBudget: Int?)
        case resume(threadID: String)
        case update(threadID: String, objective: String)
    }

    private let lock = NSLock()
    private let goal: CodexGoalSnapshot
    private var calls: [Call] = []

    init(goal: CodexGoalSnapshot) {
        self.goal = goal
    }

    var recordedCalls: [Call] {
        lock.lock()
        defer { lock.unlock() }
        return calls
    }

    func readGoals(threadIDs: [String]) throws -> [String: CodexGoalSnapshot?] {
        Dictionary(uniqueKeysWithValues: threadIDs.map { ($0, goal) })
    }

    func createGoal(
        threadID: String,
        objective: String,
        tokenBudget: Int?
    ) throws -> CodexGoalSnapshot {
        record(.create(threadID: threadID, objective: objective, tokenBudget: tokenBudget))
        return goal
    }

    func resumeGoal(threadID: String) throws -> CodexGoalSnapshot {
        record(.resume(threadID: threadID))
        return goal
    }

    func updateGoal(threadID: String, objective: String) throws -> CodexGoalSnapshot {
        record(.update(threadID: threadID, objective: objective))
        return goal
    }

    private func record(_ call: Call) {
        lock.lock()
        calls.append(call)
        lock.unlock()
    }
}
