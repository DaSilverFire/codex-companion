import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionBridgeRequestTests {
    @Test
    func retryingARequestKeepsTheSameNativeClientMessageID() {
        let id = UUID()
        let first = CompanionBridgeRequest(
            id: id,
            operation: .sendMessage,
            threadID: "thread-1",
            text: "send exactly once",
            sendAction: .reply
        )
        let retry = CompanionBridgeRequest(
            id: id,
            operation: .sendMessage,
            threadID: "thread-1",
            text: "send exactly once",
            sendAction: .reply
        )

        #expect(first.clientMessageID == id.uuidString)
        #expect(retry.clientMessageID == first.clientMessageID)
    }

    @Test
    func goalControlPayloadsRoundTripAcrossTheSharedBridgeContract() throws {
        let request = CompanionBridgeRequest(
            operation: .createGoal,
            threadID: "thread-goal",
            goalObjective: "Finish end-to-end goal controls",
            goalTokenBudget: 120_000
        )
        let decodedRequest = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decodedRequest.operation == .createGoal)
        #expect(decodedRequest.goalObjective == "Finish end-to-end goal controls")
        #expect(decodedRequest.goalTokenBudget == 120_000)

        let goal = CompanionBridgeGoal(
            threadID: "thread-goal",
            objective: "Finish end-to-end goal controls",
            status: .blocked,
            tokenBudget: 120_000,
            tokensUsed: 4_200,
            elapsedSeconds: 90,
            createdAt: 100,
            updatedAt: 200
        )
        let response = CompanionBridgeResponse.success(for: request, goal: goal)
        let decodedResponse = try JSONDecoder().decode(
            CompanionBridgeResponse.self,
            from: JSONEncoder().encode(response)
        )

        #expect(decodedResponse.goal == goal)
        #expect(decodedResponse.operation == .createGoal)
    }

    @Test(arguments: [
        CompanionBridgeOperation.createGoal,
        .resumeGoal,
        .updateGoal,
    ])
    func goalControlOperationsRemainCodable(_ operation: CompanionBridgeOperation) throws {
        let request = CompanionBridgeRequest(operation: operation, threadID: "thread-goal")
        let decoded = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decoded.operation == operation)
    }
}
