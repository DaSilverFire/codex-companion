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
