import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerSenderRetryTests {
    @Test
    func unloadedTaskStopsAfterOneNativeAttemptWithoutChangingSelection() async {
        let attempts = CodexSendAttemptRecorder(outcomes: [.threadNotLoaded, .sent])
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd, attachments in
                _ = attachments
                return await attempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            }
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

        #expect(outcome == .threadNotLoaded)
        #expect(await attempts.messageIDs == ["message-stable"])
        #expect(await attempts.actions == [.steer])
    }

    @Test
    func replyUsesTheQueuedFollowUpTransportExactlyOnce() async {
        let nativeAttempts = CodexSendAttemptRecorder(outcomes: [.failed])
        let queuedAttempts = CodexQueuedReplyAttemptRecorder(outcomes: [.sent])
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd, attachments in
                _ = attachments
                return await nativeAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd
                )
            },
            queuedReplySubmitter: { prompt, threadID, cwd, expectedTurnID, clientMessageID, queuedNotification, attachments in
                _ = attachments
                return await queuedAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    queuedNotification: queuedNotification
                )
            }
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
        #expect(await nativeAttempts.messageIDs.isEmpty)
        #expect(await queuedAttempts.messageIDs == ["message-once"])
        #expect(await queuedAttempts.threadIDs == ["thread-loaded"])
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

private actor CodexQueuedReplyAttemptRecorder {
    private var outcomes: [CodexAppServerSendOutcome]
    private(set) var messageIDs: [String] = []
    private(set) var threadIDs: [String] = []

    init(outcomes: [CodexAppServerSendOutcome]) {
        self.outcomes = outcomes
    }

    func submit(
        prompt: String,
        threadID: String,
        cwd: String?,
        expectedTurnID: String?,
        clientMessageID: String,
        queuedNotification: CodexQueuedReplyNotification
    ) -> CodexAppServerSendOutcome {
        _ = prompt
        _ = cwd
        _ = expectedTurnID
        _ = queuedNotification
        messageIDs.append(clientMessageID)
        threadIDs.append(threadID)
        return outcomes.isEmpty ? .failed : outcomes.removeFirst()
    }
}
