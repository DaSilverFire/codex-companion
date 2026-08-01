import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionBridgeRequestCoordinatorTests {
    @Test
    func concurrentDuplicateRequestsShareOneOperation() async {
        let coordinator = CompanionBridgeRequestCoordinator(
            cacheLifetime: 60,
            maximumCacheEntryCount: 8
        )
        let counter = RequestExecutionCounter()
        let request = CompanionBridgeRequest(
            operation: .createTask,
            text: "Create this once"
        )

        async let first = coordinator.response(for: request.id) {
            await counter.increment()
            try? await Task.sleep(nanoseconds: 50_000_000)
            return .success(for: request, threadID: "thread-once")
        }
        async let second = coordinator.response(for: request.id) {
            await counter.increment()
            return .success(for: request, threadID: "thread-duplicate")
        }

        let responses = await [first, second]

        #expect(await counter.value == 1)
        #expect(responses[0].threadID == responses[1].threadID)
        #expect(["thread-once", "thread-duplicate"].contains(responses[0].threadID ?? ""))
    }

    @Test
    func completedDuplicateRequestReturnsCachedResponse() async {
        let coordinator = CompanionBridgeRequestCoordinator(
            cacheLifetime: 60,
            maximumCacheEntryCount: 8
        )
        let counter = RequestExecutionCounter()
        let request = CompanionBridgeRequest(
            operation: .createTask,
            text: "Create this once"
        )

        let first = await coordinator.response(for: request.id) {
            await counter.increment()
            return .success(for: request, threadID: "thread-once")
        }
        let second = await coordinator.response(for: request.id) {
            await counter.increment()
            return .success(for: request, threadID: "thread-duplicate")
        }

        #expect(await counter.value == 1)
        #expect(first.threadID == "thread-once")
        #expect(second.threadID == "thread-once")
    }
}

private actor RequestExecutionCounter {
    private(set) var value = 0

    func increment() {
        value += 1
    }
}
