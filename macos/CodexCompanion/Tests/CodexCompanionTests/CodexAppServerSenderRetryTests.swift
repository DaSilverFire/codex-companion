import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerSenderRetryTests {
    @Test
    func unloadedTaskIsLoadedAndRetriedWithTheSameMessageID() async {
        let attempts = CodexSendAttemptRecorder(outcomes: [.threadNotLoaded, .sent])
        let loads = CodexThreadLoadRecorder(result: true)
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd in
                await attempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            },
            threadLoader: { threadID in
                await loads.load(threadID: threadID)
            },
            retryWaiter: { _ in },
            maximumLoadRetries: 1
        )

        let outcome = await sender.submit(
            prompt: "Continue the current work",
            threadID: "thread-unloaded",
            cwd: "/tmp/project",
            action: .steer,
            expectedTurnID: "turn-live",
            clientMessageID: "message-stable",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await loads.threadIDs == ["thread-unloaded"])
        #expect(await attempts.messageIDs == ["message-stable", "message-stable"])
        #expect(await attempts.actions == [.steer, .steer])
    }

    @Test
    func loadedTaskDoesNotInvokeTheBackgroundLoader() async {
        let attempts = CodexSendAttemptRecorder(outcomes: [.sent])
        let loads = CodexThreadLoadRecorder(result: true)
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd in
                await attempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            },
            threadLoader: { threadID in
                await loads.load(threadID: threadID)
            },
            retryWaiter: { _ in },
            maximumLoadRetries: 3
        )

        let outcome = await sender.submit(
            prompt: "Reply normally",
            threadID: "thread-loaded",
            cwd: nil,
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: "message-once",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await loads.threadIDs.isEmpty)
        #expect(await attempts.messageIDs == ["message-once"])
    }

    @Test
    func failedBackgroundLoadPreservesThreadNotLoadedOutcome() async {
        let attempts = CodexSendAttemptRecorder(outcomes: [.threadNotLoaded])
        let loads = CodexThreadLoadRecorder(result: false)
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd in
                await attempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            },
            threadLoader: { threadID in
                await loads.load(threadID: threadID)
            },
            retryWaiter: { _ in },
            maximumLoadRetries: 3
        )

        let outcome = await sender.submit(
            prompt: "Keep this draft",
            threadID: "thread-missing",
            cwd: nil,
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: "message-unsent",
            onQueued: {}
        )

        #expect(outcome == .threadNotLoaded)
        #expect(await loads.threadIDs == ["thread-missing"])
        #expect(await attempts.messageIDs == ["message-unsent"])
    }

    @Test
    func boundedRetriesStopWhenTheTaskNeverRegistersAnOwner() async {
        let attempts = CodexSendAttemptRecorder(
            outcomes: [.threadNotLoaded, .threadNotLoaded, .threadNotLoaded]
        )
        let loads = CodexThreadLoadRecorder(result: true)
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd in
                await attempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            },
            threadLoader: { threadID in
                await loads.load(threadID: threadID)
            },
            retryWaiter: { _ in },
            maximumLoadRetries: 2
        )

        let outcome = await sender.submit(
            prompt: "Do not duplicate this",
            threadID: "thread-never-owned",
            cwd: nil,
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: "message-bounded",
            onQueued: {}
        )

        #expect(outcome == .threadNotLoaded)
        #expect(await attempts.messageIDs == [
            "message-bounded",
            "message-bounded",
            "message-bounded",
        ])
        #expect(await loads.threadIDs == ["thread-never-owned"])
    }
}

private actor CodexSendAttemptRecorder {
    private var outcomes: [CodexAppServerSendOutcome]
    private(set) var messageIDs: [String] = []
    private(set) var actions: [CodexSendAction] = []

    init(outcomes: [CodexAppServerSendOutcome]) {
        self.outcomes = outcomes
    }

    func submit(
        prompt: String,
        threadID: String,
        action: CodexSendAction,
        clientMessageID: String,
        cwd: String?
    ) -> CodexAppServerSendOutcome {
        _ = prompt
        _ = threadID
        _ = cwd
        messageIDs.append(clientMessageID)
        actions.append(action)
        return outcomes.isEmpty ? .failed : outcomes.removeFirst()
    }
}

private actor CodexThreadLoadRecorder {
    private let result: Bool
    private(set) var threadIDs: [String] = []

    init(result: Bool) {
        self.result = result
    }

    func load(threadID: String) -> Bool {
        threadIDs.append(threadID)
        return result
    }
}
