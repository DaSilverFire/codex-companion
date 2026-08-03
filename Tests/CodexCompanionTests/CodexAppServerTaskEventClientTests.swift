import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerTaskEventClientTests {
    @Test
    func parsesCurrentAppServerV2Notifications() throws {
        let fixtures: [(String, CodexTaskStreamEvent)] = [
            (
                #"{"method":"turn/started","params":{"threadId":"thread-1","turn":{"id":"turn-1","items":[],"status":"inProgress"}}}"#,
                CodexTaskStreamEvent(
                    threadID: "thread-1",
                    turnID: "turn-1",
                    kind: .turnStarted,
                    taskStatus: .running
                )
            ),
            (
                #"{"method":"item/agentMessage/delta","params":{"threadId":"thread-1","turnId":"turn-1","itemId":"message-1","delta":"Hello"}}"#,
                CodexTaskStreamEvent(
                    threadID: "thread-1",
                    turnID: "turn-1",
                    itemID: "message-1",
                    kind: .assistantDelta,
                    text: "Hello"
                )
            ),
            (
                #"{"method":"item/reasoning/summaryTextDelta","params":{"threadId":"thread-1","turnId":"turn-1","itemId":"reasoning-1","summaryIndex":0,"delta":"Checking files"}}"#,
                CodexTaskStreamEvent(
                    threadID: "thread-1",
                    turnID: "turn-1",
                    itemID: "reasoning-1",
                    kind: .reasoningSummaryDelta,
                    text: "Checking files"
                )
            ),
            (
                #"{"method":"thread/status/changed","params":{"threadId":"thread-1","status":{"type":"active","activeFlags":["waitingOnApproval"]}}}"#,
                CodexTaskStreamEvent(
                    threadID: "thread-1",
                    kind: .statusChanged,
                    taskStatus: .waiting
                )
            ),
            (
                #"{"method":"turn/completed","params":{"threadId":"thread-1","turn":{"id":"turn-1","items":[],"status":"completed"}}}"#,
                CodexTaskStreamEvent(
                    threadID: "thread-1",
                    turnID: "turn-1",
                    kind: .turnCompleted,
                    taskStatus: .completed
                )
            ),
        ]

        for (json, expected) in fixtures {
            #expect(CodexAppServerTaskEventParser.event(from: Data(json.utf8)) == expected)
        }
    }

    @Test
    func projectsToolLifecycleWithoutForwardingCommandOutput() throws {
        let started = Data(
            #"{"method":"item/started","params":{"threadId":"thread-1","turnId":"turn-1","startedAtMs":1,"item":{"type":"commandExecution","id":"tool-1","command":"cat /tmp/example.txt","commandActions":[],"cwd":"/tmp","status":"inProgress","aggregatedOutput":null}}}"#.utf8
        )
        let completed = Data(
            #"{"method":"item/completed","params":{"threadId":"thread-1","turnId":"turn-1","completedAtMs":2,"item":{"type":"commandExecution","id":"tool-1","command":"cat /tmp/example.txt","commandActions":[],"cwd":"/tmp","status":"completed","exitCode":0,"aggregatedOutput":"PRIVATE RAW OUTPUT"}}}"#.utf8
        )

        let startedEvent = try #require(CodexAppServerTaskEventParser.event(from: started))
        let completedEvent = try #require(CodexAppServerTaskEventParser.event(from: completed))

        #expect(startedEvent.kind == .itemStarted)
        #expect(startedEvent.item?.id == "tool-1")
        #expect(startedEvent.item?.title == "Read files")
        #expect(startedEvent.item?.status == .inProgress)
        #expect(completedEvent.kind == .itemCompleted)
        #expect(completedEvent.item?.status == .completed)
        #expect(completedEvent.item?.detail?.contains("PRIVATE RAW OUTPUT") != true)
    }

    @Test
    func failedAndInterruptedTurnsBecomeSafeFailureEvents() throws {
        let failed = Data(
            #"{"method":"turn/completed","params":{"threadId":"thread-1","turn":{"id":"turn-failed","items":[],"status":"failed","error":{"message":"The tool disconnected","additionalDetails":"SECRET INTERNAL DETAIL"}}}}"#.utf8
        )
        let interrupted = Data(
            #"{"method":"turn/completed","params":{"threadId":"thread-1","turn":{"id":"turn-interrupted","items":[],"status":"interrupted","error":null}}}"#.utf8
        )

        let failedEvent = try #require(CodexAppServerTaskEventParser.event(from: failed))
        let interruptedEvent = try #require(CodexAppServerTaskEventParser.event(from: interrupted))

        #expect(failedEvent.kind == .failed)
        #expect(failedEvent.taskStatus == .failed)
        #expect(failedEvent.text == "The tool disconnected")
        #expect(failedEvent.text?.contains("SECRET") != true)
        #expect(interruptedEvent.kind == .failed)
        #expect(interruptedEvent.errorCode == "interrupted")
    }

    @Test
    func rawReasoningTextIsNeverProjectedToMobile() {
        let rawReasoning = Data(
            #"{"method":"item/reasoning/textDelta","params":{"threadId":"thread-1","turnId":"turn-1","itemId":"reasoning-1","contentIndex":0,"delta":"private chain of thought"}}"#.utf8
        )

        #expect(CodexAppServerTaskEventParser.event(from: rawReasoning) == nil)
    }

    @Test
    func diagnosticSummaryReportsOnlyProtocolShape() throws {
        let privateText = "PRIVATE RESPONSE CONTENT"
        let notification = Data(
            #"{"method":"item/agentMessage/delta","params":{"threadId":"thread-1","turnId":"turn-1","itemId":"message-1","delta":"PRIVATE RESPONSE CONTENT"}}"#.utf8
        )

        let summary = try #require(
            CodexAppServerTaskEventDiagnostic.summary(
                from: notification,
                expectedThreadID: "thread-1"
            )
        )

        #expect(summary.contains("method=item/agentMessage/delta"))
        #expect(summary.contains("keys=delta,itemId,threadId,turnId"))
        #expect(summary.contains("parsed=assistantDelta"))
        #expect(summary.contains("thread-match=true"))
        #expect(!summary.contains(privateText))
        #expect(!summary.contains("thread-1"))
    }
}
