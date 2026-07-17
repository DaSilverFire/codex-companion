import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerControlServiceTests {
    @Test
    func createGoalUsesNativeGoalSetAndReturnsTheServerSnapshot() throws {
        let rpc = RecordingGoalRPCClient()
        let service = CodexAppServerControlService(client: rpc)

        let goal = try service.createGoal(
            threadID: " thread-create ",
            objective: " Build mobile goal controls ",
            tokenBudget: 90_000
        )

        let request = try #require(rpc.recordedRequests.first)
        #expect(rpc.recordedRequests.count == 1)
        #expect(request.method == "thread/goal/set")
        #expect(request.params["threadId"] as? String == "thread-create")
        #expect(request.params["objective"] as? String == "Build mobile goal controls")
        #expect(request.params["tokenBudget"] as? Int == 90_000)
        #expect(request.params["status"] == nil)
        #expect(goal.threadID == "thread-create")
        #expect(goal.status == .active)
        #expect(goal.objective == "Build mobile goal controls")
    }

    @Test
    func resumeGoalOnlySetsTheActiveStatus() throws {
        let rpc = RecordingGoalRPCClient()
        let service = CodexAppServerControlService(client: rpc)

        _ = try service.resumeGoal(threadID: " thread-resume ")

        let request = try #require(rpc.recordedRequests.first)
        #expect(request.method == "thread/goal/set")
        #expect(request.params["threadId"] as? String == "thread-resume")
        #expect(request.params["status"] as? String == "active")
        #expect(request.params["objective"] == nil)
        #expect(request.params["tokenBudget"] == nil)
    }

    @Test
    func updateGoalOnlySetsTheTrimmedObjective() throws {
        let rpc = RecordingGoalRPCClient()
        let service = CodexAppServerControlService(client: rpc)

        _ = try service.updateGoal(
            threadID: " thread-edit ",
            objective: " Refine the mobile bridge contract "
        )

        let request = try #require(rpc.recordedRequests.first)
        #expect(request.method == "thread/goal/set")
        #expect(request.params["threadId"] as? String == "thread-edit")
        #expect(request.params["objective"] as? String == "Refine the mobile bridge contract")
        #expect(request.params["status"] == nil)
        #expect(request.params["tokenBudget"] == nil)
    }

    @Test
    func createGoalRejectsAnEmptyObjectiveBeforeSending() {
        let rpc = RecordingGoalRPCClient()
        let service = CodexAppServerControlService(client: rpc)

        #expect(throws: CodexAppServerControlError.self) {
            try service.createGoal(
                threadID: "thread-create",
                objective: " \n ",
                tokenBudget: nil
            )
        }
        #expect(rpc.recordedRequests.isEmpty)
    }

    @Test(arguments: [0, -1])
    func createGoalRejectsNonpositiveTokenBudgets(_ tokenBudget: Int) {
        let rpc = RecordingGoalRPCClient()
        let service = CodexAppServerControlService(client: rpc)

        #expect(throws: CodexAppServerControlError.self) {
            try service.createGoal(
                threadID: "thread-create",
                objective: "Build mobile goal controls",
                tokenBudget: tokenBudget
            )
        }
        #expect(rpc.recordedRequests.isEmpty)
    }
}

private final class RecordingGoalRPCClient: CodexAppServerRPCPerforming, @unchecked Sendable {
    private let lock = NSLock()
    private var requests: [CodexRPCRequest] = []

    var recordedRequests: [CodexRPCRequest] {
        lock.lock()
        defer { lock.unlock() }
        return requests
    }

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        lock.lock()
        self.requests.append(contentsOf: requests)
        lock.unlock()

        return [
            2: CodexRPCResponse(
                result: [
                    "goal": [
                        "threadId": "thread-create",
                        "objective": "Build mobile goal controls",
                        "status": "active",
                        "tokenBudget": 90_000,
                        "tokensUsed": 0,
                        "timeUsedSeconds": 0,
                        "createdAt": 100,
                        "updatedAt": 100,
                    ],
                ],
                error: nil
            ),
        ]
    }
}
