import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexFollowerIPCProtocolTests {
    @Test
    func frameUsesLittleEndianLengthPrefixAndRoundTripsJSON() throws {
        let message: [String: Any] = [
            "type": "request",
            "method": "initialize",
        ]

        let frame = try CodexFollowerIPCProtocol.frame(for: message)

        #expect(frame.count > 4)
        let byte0 = Int(frame[0])
        let byte1 = Int(frame[1]) << 8
        let byte2 = Int(frame[2]) << 16
        let byte3 = Int(frame[3]) << 24
        let payloadLength = byte0 | byte1 | byte2 | byte3
        #expect(payloadLength == frame.count - 4)
        let decoded = try #require(
            JSONSerialization.jsonObject(with: frame.dropFirst(4)) as? [String: Any]
        )
        #expect(decoded["type"] as? String == "request")
        #expect(decoded["method"] as? String == "initialize")
    }

    @Test
    func initializeRequestRegistersACompanionIPCClient() {
        let request = CodexFollowerIPCProtocol.initializeRequest(requestID: "init-id")

        #expect(request["type"] as? String == "request")
        #expect(request["requestId"] as? String == "init-id")
        #expect(request["sourceClientId"] as? String == "initializing-client")
        #expect(request["version"] as? Int == 0)
        #expect(request["method"] as? String == "initialize")
        let params = request["params"] as? [String: Any]
        #expect(params?["clientType"] as? String == "codex-companion")
    }

    @Test
    func steerRequestUsesTheNativeThreadFollowerPayload() {
        let request = CodexFollowerIPCProtocol.actionRequest(
            requestID: "steer-id",
            clientID: "client-id",
            threadID: "thread-id",
            prompt: "Keep working",
            action: .steer,
            clientMessageID: "message-id",
            cwd: "/tmp/project"
        )

        #expect(request["method"] as? String == "thread-follower-steer-turn")
        #expect(request["version"] as? Int == 1)
        #expect(request["sourceClientId"] as? String == "client-id")
        #expect(request["timeoutMs"] as? Int == CodexFollowerIPCProtocol.routerTimeoutMilliseconds)
        let params = request["params"] as? [String: Any]
        #expect(params?["conversationId"] as? String == "thread-id")
        #expect(params?["clientUserMessageId"] as? String == "message-id")
        #expect((params?["attachments"] as? [Any])?.isEmpty == true)
        let restoreMessage = params?["restoreMessage"] as? [String: Any]
        #expect(restoreMessage?["id"] as? String == "message-id")
        #expect(restoreMessage?["text"] as? String == "Keep working")
        #expect(restoreMessage?["cwd"] as? String == "/tmp/project")
        #expect(restoreMessage?["createdAt"] is Int64)
        let context = restoreMessage?["context"] as? [String: Any]
        #expect(context?["prompt"] as? String == "Keep working")
        #expect(context?["workspaceRoots"] as? [String] == ["/tmp/project"])
        #expect((context?["addedFiles"] as? [Any])?.isEmpty == true)
        #expect((context?["fileAttachments"] as? [Any])?.isEmpty == true)
        #expect((context?["imageAttachments"] as? [Any])?.isEmpty == true)
        let input = params?["input"] as? [[String: Any]]
        #expect(input?.first?["type"] as? String == "text")
        #expect(input?.first?["text"] as? String == "Keep working")
        #expect((input?.first?["text_elements"] as? [Any])?.isEmpty == true)
    }

    @Test
    func replyRequestUsesNativeFollowerStartTurnWithoutASecondAppServer() {
        let request = CodexFollowerIPCProtocol.actionRequest(
            requestID: "reply-id",
            clientID: "client-id",
            threadID: "thread-id",
            prompt: "Follow up",
            action: .reply,
            clientMessageID: "message-id"
        )

        #expect(request["method"] as? String == "thread-follower-start-turn")
        let params = request["params"] as? [String: Any]
        #expect(params?["conversationId"] as? String == "thread-id")
        let turnStartParams = params?["turnStartParams"] as? [String: Any]
        #expect(turnStartParams?["clientUserMessageId"] as? String == "message-id")
        let input = turnStartParams?["input"] as? [[String: Any]]
        #expect(input?.first?["text"] as? String == "Follow up")
        #expect(turnStartParams?["threadId"] == nil)
    }

    @Test
    func queuedReplyRequestPreservesEveryExistingThreadAndAppendsOnlyOnce() throws {
        let existingState: [String: Any] = [
            "other-thread": [[
                "id": "existing-message",
                "text": "Do not replace me",
                "createdAt": Int64(1),
            ]],
            "thread-id": [[
                "id": "older-message",
                "text": "Already queued",
                "createdAt": Int64(2),
            ]],
        ]

        let request = try CodexFollowerIPCProtocol.queuedReplyRequest(
            requestID: "queue-id",
            clientID: "client-id",
            threadID: "thread-id",
            prompt: "Follow up later",
            clientMessageID: "new-message",
            cwd: "/tmp/project",
            existingState: existingState,
            createdAtMilliseconds: 3
        )

        #expect(request["method"] as? String == "thread-follower-set-queued-follow-ups-state")
        #expect(request["version"] as? Int == 1)
        let params = try #require(request["params"] as? [String: Any])
        #expect(params["conversationId"] as? String == "thread-id")
        let state = try #require(params["state"] as? [String: Any])
        let otherMessages = try #require(state["other-thread"] as? [[String: Any]])
        #expect(otherMessages.count == 1)
        #expect(otherMessages.first?["id"] as? String == "existing-message")

        let messages = try #require(state["thread-id"] as? [[String: Any]])
        #expect(messages.count == 2)
        #expect(messages.last?["id"] as? String == "new-message")
        #expect(messages.last?["text"] as? String == "Follow up later")
        #expect(messages.last?["cwd"] as? String == "/tmp/project")
        #expect(messages.last?["createdAt"] as? Int64 == 3)
        let context = try #require(messages.last?["context"] as? [String: Any])
        #expect(context["prompt"] as? String == "Follow up later")
        #expect(context["workspaceRoots"] as? [String] == ["/tmp/project"])

        let duplicate = try CodexFollowerIPCProtocol.queuedReplyRequest(
            requestID: "queue-id-2",
            clientID: "client-id",
            threadID: "thread-id",
            prompt: "Follow up later",
            clientMessageID: "new-message",
            cwd: "/tmp/project",
            existingState: state,
            createdAtMilliseconds: 4
        )
        let duplicateParams = try #require(duplicate["params"] as? [String: Any])
        let duplicateState = try #require(duplicateParams["state"] as? [String: Any])
        let duplicateMessages = try #require(duplicateState["thread-id"] as? [[String: Any]])
        #expect(duplicateMessages.count == 2)
    }

    @Test
    func queuedReplyRequestRejectsMalformedExistingThreadState() {
        let malformedState: [String: Any] = ["thread-id": "not-an-array"]

        #expect(throws: CodexFollowerIPCError.invalidQueuedFollowUpState) {
            _ = try CodexFollowerIPCProtocol.queuedReplyRequest(
                requestID: "queue-id",
                clientID: "client-id",
                threadID: "thread-id",
                prompt: "Follow up later",
                clientMessageID: "new-message",
                cwd: nil,
                existingState: malformedState,
                createdAtMilliseconds: 3
            )
        }
    }

    @Test
    func queuedFollowUpStateStoreReturnsEmptyWhenTheGlobalStateFileDoesNotExist() throws {
        let missingURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathComponent("missing-global-state.json")

        let state = try CodexQueuedFollowUpStateStore.load(from: missingURL)

        #expect(state.isEmpty)
    }

    @Test
    func queuedFollowUpStateStoreLoadsOnlyTheCompletePersistedQueue() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("codex-companion-queue-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }
        let root: [String: Any] = [
            "unrelated-setting": true,
            "queued-follow-ups": [
                "thread-a": [[
                    "id": "message-a",
                    "text": "Keep me",
                    "createdAt": Int64(1),
                ]],
            ],
        ]
        try JSONSerialization.data(withJSONObject: root).write(to: url, options: .atomic)

        let state = try CodexQueuedFollowUpStateStore.load(from: url)

        #expect(state.count == 1)
        let messages = try #require(state["thread-a"] as? [[String: Any]])
        #expect(messages.first?["id"] as? String == "message-a")
        #expect(state["unrelated-setting"] == nil)
    }

    @Test
    func queuedFollowUpStateStoreRejectsMalformedPersistedQueues() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("codex-companion-queue-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }
        let root: [String: Any] = ["queued-follow-ups": ["thread-a": "not-an-array"]]
        try JSONSerialization.data(withJSONObject: root).write(to: url, options: .atomic)

        #expect(throws: CodexFollowerIPCError.invalidQueuedFollowUpState) {
            _ = try CodexQueuedFollowUpStateStore.load(from: url)
        }
    }

    @Test
    func commandApprovalRequestUsesTheNativeThreadFollowerPayload() {
        let pending = CodexPendingApproval(
            threadID: "thread-id",
            requestID: 42,
            method: .commandExecution,
            proposedExecpolicyAmendment: ["git", "status"]
        )

        let request = CodexFollowerIPCProtocol.approvalRequest(
            requestID: "approval-id",
            clientID: "client-id",
            request: pending,
            decision: .approveSimilarCommands
        )

        #expect(request["method"] as? String == "thread-follower-command-approval-decision")
        #expect(request["version"] as? Int == 1)
        #expect(request["sourceClientId"] as? String == "client-id")
        let params = request["params"] as? [String: Any]
        #expect(params?["conversationId"] as? String == "thread-id")
        #expect(params?["requestId"] as? Int == 42)
        let decision = params?["decision"] as? [String: Any]
        let accepted = decision?["acceptWithExecpolicyAmendment"] as? [String: Any]
        #expect(accepted?["execpolicy_amendment"] as? [String] == ["git", "status"])
    }

    @Test
    func fileApprovalRequestUsesTheNativeThreadFollowerPayload() {
        let pending = CodexPendingApproval(
            threadID: "thread-id",
            requestID: 17,
            method: .fileChange
        )

        let request = CodexFollowerIPCProtocol.approvalRequest(
            requestID: "approval-id",
            clientID: "client-id",
            request: pending,
            decision: .approveOnce
        )

        #expect(request["method"] as? String == "thread-follower-file-approval-decision")
        let params = request["params"] as? [String: Any]
        #expect(params?["requestId"] as? Int == 17)
        #expect(params?["decision"] as? String == "accept")
    }

    @Test(arguments: [
        ("no-client-found", CodexAppServerSendOutcome.threadNotLoaded),
        ("client-disconnected", CodexAppServerSendOutcome.threadNotLoaded),
        ("request-timeout", CodexAppServerSendOutcome.timedOut),
        ("Conversation is not being streamed", CodexAppServerSendOutcome.threadNotLoaded),
        ("Cannot steer without an active turn", CodexAppServerSendOutcome.noActiveTurn),
    ])
    func nativeIPCErrorMapping(error: String, outcome: CodexAppServerSendOutcome) {
        #expect(CodexFollowerIPCProtocol.outcome(forError: error) == outcome)
    }

    @Test
    func liveQueuedReplyRunsOnlyWhenExplicitlyEnabled() async {
        let environment = ProcessInfo.processInfo.environment
        guard
            let threadID = environment["CODEX_COMPANION_LIVE_REPLY_THREAD"],
            let prompt = environment["CODEX_COMPANION_LIVE_REPLY_PROMPT"]
        else {
            return
        }

        let outcome = await CodexAppServerSender().submit(
            prompt: prompt,
            threadID: threadID,
            cwd: environment["CODEX_COMPANION_LIVE_REPLY_CWD"],
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: UUID().uuidString,
            onQueued: {}
        )
        #expect(outcome == .sent)
    }

    @Test
    func liveSteerRunsOnlyWhenExplicitlyEnabled() async {
        let environment = ProcessInfo.processInfo.environment
        guard
            let threadID = environment["CODEX_COMPANION_LIVE_STEER_THREAD"],
            let prompt = environment["CODEX_COMPANION_LIVE_STEER_PROMPT"]
        else {
            return
        }

        let outcome = await CodexAppServerSender().submit(
            prompt: prompt,
            threadID: threadID,
            cwd: nil,
            action: .steer,
            expectedTurnID: nil,
            clientMessageID: UUID().uuidString,
            onQueued: {}
        )
        #expect(outcome == .sent)
    }

    @Test
    func liveApprovalRunsOnlyWhenExplicitlyEnabled() async {
        let environment = ProcessInfo.processInfo.environment
        guard
            let threadID = environment["CODEX_COMPANION_LIVE_APPROVAL_THREAD"],
            let rawRequestID = environment["CODEX_COMPANION_LIVE_APPROVAL_REQUEST"],
            let requestID = Int(rawRequestID),
            let rawMethod = environment["CODEX_COMPANION_LIVE_APPROVAL_METHOD"],
            let method = CodexPendingApproval.Method(rawValue: rawMethod)
        else {
            return
        }

        let outcome = await CodexFollowerIPCTransport().respond(
            to: CodexPendingApproval(
                threadID: threadID,
                requestID: requestID,
                method: method
            ),
            decision: .approveOnce
        )
        #expect(outcome == .approved)
    }
}
