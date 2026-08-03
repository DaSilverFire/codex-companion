import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerSenderRetryTests {
    @Test
    func loadedTaskUsesItsLiveSnapshotWithoutHydratingTurnHistory() {
        #expect(CodexAppServerLoadedThreadRoutePlanner.nextStep(
            action: .steer,
            snapshotTurnID: "turn-live",
            resumedThread: false
        ) == .steerExistingTurn("turn-live"))
        #expect(CodexAppServerLoadedThreadRoutePlanner.nextStep(
            action: .reply,
            snapshotTurnID: "turn-live",
            resumedThread: false
        ) == .queueReplyAfterCurrentTurn)
    }

    @Test
    func resumedOrSnapshotlessTaskRediscoversItsCurrentTurn() {
        #expect(CodexAppServerLoadedThreadRoutePlanner.nextStep(
            action: .steer,
            snapshotTurnID: "turn-before-resume",
            resumedThread: true
        ) == .discoverCurrentTurn)
        #expect(CodexAppServerLoadedThreadRoutePlanner.nextStep(
            action: .reply,
            snapshotTurnID: nil,
            resumedThread: false
        ) == .discoverCurrentTurn)
    }

    @Test
    func unloadedTaskResumesThenRetriesWithTheSameMessageIdentity() async {
        let nativeAttempts = CodexSendAttemptRecorder(outcomes: [.threadNotLoaded])
        let resumedAttempts = CodexResumingAttemptRecorder(outcomes: [.sent])
        let attachment = CodexFollowerAttachment(
            id: UUID(),
            kind: .image,
            label: "evidence.png",
            path: "/tmp/evidence.png",
            fsPath: "/tmp/evidence.png",
            mimeType: "image/png"
        )
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
            resumingSubmitter: {
                prompt, threadID, cwd, action, expectedTurnID, clientMessageID, queuedNotification, attachments in
                _ = queuedNotification
                return await resumedAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    attachments: attachments
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
            onQueued: {},
            attachments: [attachment]
        )

        #expect(outcome == .sent)
        #expect(await nativeAttempts.messageIDs == ["message-stable"])
        #expect(await nativeAttempts.actions == [.steer])
        #expect(await resumedAttempts.threadIDs == ["thread-unloaded"])
        #expect(await resumedAttempts.cwds == ["/tmp/project"])
        #expect(await resumedAttempts.actions == [.steer])
        #expect(await resumedAttempts.expectedTurnIDs == ["turn-live"])
        #expect(await resumedAttempts.messageIDs == ["message-stable"])
        #expect(await resumedAttempts.attachments == [[attachment]])
    }

    @Test
    func unloadedReplyKeepsQueuedTransportSemanticsAcrossResume() async {
        let nativeAttempts = CodexSendAttemptRecorder(outcomes: [.failed])
        let queuedAttempts = CodexQueuedReplyAttemptRecorder(outcomes: [.threadNotLoaded])
        let resumedAttempts = CodexResumingAttemptRecorder(outcomes: [.sent])
        let queuedNotifications = CodexQueueNotificationRecorder()
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
            queuedReplySubmitter: {
                prompt, threadID, cwd, expectedTurnID, clientMessageID, queuedNotification, attachments in
                _ = attachments
                return await queuedAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    queuedNotification: queuedNotification
                )
            },
            resumingSubmitter: {
                prompt, threadID, cwd, action, expectedTurnID, clientMessageID, queuedNotification, attachments in
                queuedNotification()
                return await resumedAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    attachments: attachments
                )
            }
        )

        let outcome = await sender.submit(
            prompt: "Reply after the current turn",
            threadID: "thread-unloaded-reply",
            cwd: "/tmp/reply-project",
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: "reply-message-stable",
            onQueued: { queuedNotifications.record() }
        )

        #expect(outcome == .sent)
        #expect(await nativeAttempts.messageIDs.isEmpty)
        #expect(await queuedAttempts.messageIDs == ["reply-message-stable"])
        #expect(await resumedAttempts.actions == [.reply])
        #expect(await resumedAttempts.messageIDs == ["reply-message-stable"])
        #expect(queuedNotifications.count == 1)
    }

    @Test
    func resumeFallbackRunsAtMostOnce() async {
        let nativeAttempts = CodexSendAttemptRecorder(outcomes: [.threadNotLoaded])
        let resumedAttempts = CodexResumingAttemptRecorder(outcomes: [.threadNotLoaded, .sent])
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
            resumingSubmitter: {
                prompt, threadID, cwd, action, expectedTurnID, clientMessageID, queuedNotification, attachments in
                _ = queuedNotification
                return await resumedAttempts.submit(
                    prompt: prompt,
                    threadID: threadID,
                    cwd: cwd,
                    action: action,
                    expectedTurnID: expectedTurnID,
                    clientMessageID: clientMessageID,
                    attachments: attachments
                )
            }
        )

        let outcome = await sender.submit(
            prompt: "Do not duplicate this",
            threadID: "thread-still-unavailable",
            cwd: nil,
            action: .steer,
            expectedTurnID: "turn-live",
            clientMessageID: "message-once-only",
            onQueued: {}
        )

        #expect(outcome == .threadNotLoaded)
        #expect(await nativeAttempts.messageIDs == ["message-once-only"])
        #expect(await resumedAttempts.messageIDs == ["message-once-only"])
    }

    @Test
    func resumeRequestTargetsExactTaskWithoutHydratingTurnHistory() throws {
        let request = CodexAppServerResumeRequestFactory.threadResume(
            id: 6,
            threadID: "thread-exact",
            cwd: "  /tmp/exact-project  "
        )
        let params = try #require(request["params"] as? [String: Any])

        #expect(request["id"] as? Int == 6)
        #expect(request["method"] as? String == "thread/resume")
        #expect(params["threadId"] as? String == "thread-exact")
        #expect(params["cwd"] as? String == "/tmp/exact-project")
        #expect(params["excludeTurns"] as? Bool == true)
    }

    @Test
    func queuedReplyPollReadsTheExactTaskWithoutHydratingTurns() throws {
        let request = CodexAppServerThreadReadRequestFactory.threadRead(
            id: 5,
            threadID: "thread-exact"
        )
        let params = try #require(request["params"] as? [String: Any])

        #expect(request["id"] as? Int == 5)
        #expect(request["method"] as? String == "thread/read")
        #expect(params["threadId"] as? String == "thread-exact")
        #expect(params["includeTurns"] as? Bool == false)
    }

    @Test
    func exactThreadReadParsesIdleActiveAndUnavailableStates() {
        let idle: [String: Any] = [
            "result": [
                "thread": [
                    "id": "thread-exact",
                    "status": ["type": "idle"],
                ],
            ],
        ]
        let active: [String: Any] = [
            "result": [
                "thread": [
                    "id": "thread-exact",
                    "status": ["type": "active", "activeFlags": []],
                ],
            ],
        ]
        let other: [String: Any] = [
            "result": [
                "thread": [
                    "id": "thread-other",
                    "status": ["type": "idle"],
                ],
            ],
        ]

        #expect(CodexAppServerResponseParser.threadReadState(
            from: idle,
            threadID: "thread-exact"
        ) == .idle)
        #expect(CodexAppServerResponseParser.threadReadState(
            from: active,
            threadID: "thread-exact"
        ) == .active)
        #expect(CodexAppServerResponseParser.threadReadState(
            from: other,
            threadID: "thread-exact"
        ) == .unavailable)
    }

    @Test
    func staleSteerRetriesOnceWithTheAuthoritativeActiveTurn() {
        let message = "expected active turn id `turn-stale` but found `turn-current`"

        #expect(CodexAppServerSteerRecoveryPolicy.replacementTurnID(
            errorMessage: message,
            attemptedTurnID: "turn-stale",
            hasRetried: false
        ) == "turn-current")
        #expect(CodexAppServerSteerRecoveryPolicy.replacementTurnID(
            errorMessage: message,
            attemptedTurnID: "turn-stale",
            hasRetried: true
        ) == nil)
        #expect(CodexAppServerSteerRecoveryPolicy.replacementTurnID(
            errorMessage: message,
            attemptedTurnID: "turn-other",
            hasRetried: false
        ) == nil)
    }

    @Test
    func malformedActiveTurnErrorsNeverTriggerSteerRecovery() {
        #expect(CodexAppServerSteerRecoveryPolicy.replacementTurnID(
            errorMessage: "active turn changed",
            attemptedTurnID: "turn-stale",
            hasRetried: false
        ) == nil)
        #expect(CodexAppServerSteerRecoveryPolicy.replacementTurnID(
            errorMessage: "expected active turn id `turn-stale` but found `turn-stale`",
            attemptedTurnID: "turn-stale",
            hasRetried: false
        ) == nil)
    }

    @Test
    func missingLoadedTaskRequiresResumeWhileMalformedDataFailsParsing() {
        let loaded: [String: Any] = [
            "result": ["data": ["thread-other", "thread-exact"]],
        ]
        let unloaded: [String: Any] = [
            "result": ["data": ["thread-other"]],
        ]
        let noLoadedTasks: [String: Any] = [
            "result": ["data": [String]()],
        ]
        let malformed: [String: Any] = [
            "result": ["data": "thread-exact"],
        ]

        #expect(CodexAppServerResponseParser.loadedThreadState(
            from: loaded,
            threadID: "thread-exact"
        ) == .loaded)
        #expect(CodexAppServerResponseParser.loadedThreadState(
            from: unloaded,
            threadID: "thread-exact"
        ) == .needsResume)
        #expect(CodexAppServerResponseParser.loadedThreadState(
            from: noLoadedTasks,
            threadID: "thread-exact"
        ) == .needsResume)
        #expect(CodexAppServerResponseParser.loadedThreadState(
            from: malformed,
            threadID: "thread-exact"
        ) == nil)
    }

    @Test
    func resumeResponseMustReturnTheRequestedThreadIdentity() {
        let response: [String: Any] = [
            "result": [
                "thread": ["id": "thread-exact"],
            ],
        ]

        #expect(CodexAppServerResponseParser.resumedThreadID(from: response) == "thread-exact")
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

private actor CodexResumingAttemptRecorder {
    private var outcomes: [CodexAppServerSendOutcome]
    private(set) var threadIDs: [String] = []
    private(set) var cwds: [String?] = []
    private(set) var actions: [CodexSendAction] = []
    private(set) var expectedTurnIDs: [String?] = []
    private(set) var messageIDs: [String] = []
    private(set) var attachments: [[CodexFollowerAttachment]] = []

    init(outcomes: [CodexAppServerSendOutcome]) {
        self.outcomes = outcomes
    }

    func submit(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        attachments: [CodexFollowerAttachment]
    ) -> CodexAppServerSendOutcome {
        _ = prompt
        threadIDs.append(threadID)
        cwds.append(cwd)
        actions.append(action)
        expectedTurnIDs.append(expectedTurnID)
        messageIDs.append(clientMessageID)
        self.attachments.append(attachments)
        return outcomes.isEmpty ? .failed : outcomes.removeFirst()
    }
}

private final class CodexQueueNotificationRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recordedCount = 0

    var count: Int {
        lock.withLock { recordedCount }
    }

    func record() {
        lock.withLock {
            recordedCount += 1
        }
    }
}
