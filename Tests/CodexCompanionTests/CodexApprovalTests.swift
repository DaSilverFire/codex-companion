import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexApprovalTests {
    @Test
    func findsTheNewestUnansweredCommandApproval() {
        let lines = [
            "show approval conversationId=thread-a kind=fileChange requestId=4",
            "[desktop-notifications] show approval conversationId=thread-a kind=commandExecution requestId=8",
        ]

        let request = CodexDesktopApprovalLogParser.pendingApproval(
            for: "thread-a",
            lines: lines
        )

        #expect(request == CodexPendingApproval(
            threadID: "thread-a",
            requestID: 8,
            method: .commandExecution
        ))
    }

    @Test
    func ignoresARequestThatChatGPTHasAlreadyAnswered() {
        let lines = [
            "[desktop-notifications] show approval conversationId=thread-a kind=commandExecution requestId=36",
            "Sending server response id=36 method=item/commandExecution/requestApproval response={decision:accept}",
        ]

        #expect(CodexDesktopApprovalLogParser.pendingApproval(
            for: "thread-a",
            lines: lines
        ) == nil)
    }

    @Test
    func resolvesAnApprovalAcrossRotatedLogsOutsideTheTailWindow() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("approval-logs-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let prefix = "codex-desktop-session-123-t0-i1-000001"
        let first = root.appendingPathComponent("\(prefix)-0.log")
        let second = root.appendingPathComponent("\(prefix)-1.log")
        try "2026-07-13T12:00:00.000Z info [desktop-notifications] show approval conversationId=thread-a kind=commandExecution requestId=36\n"
            .write(to: first, atomically: true, encoding: .utf8)
        try ("2026-07-13T12:01:00.000Z info Sending server response id=36 method=item/commandExecution/requestApproval response={decision:accept}\n"
            + String(repeating: "later log output\n", count: 100))
            .write(to: second, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: 100)], ofItemAtPath: first.path)
        try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: 101)], ofItemAtPath: second.path)

        var reader = CodexDesktopApprovalLogReader(
            maximumFileAge: .greatestFiniteMagnitude,
            maximumTailBytes: 64,
            logsRootURL: root
        )
        #expect(reader.pendingApproval(for: "thread-a") == nil)

        let handle = try FileHandle(forWritingTo: second)
        try handle.seekToEnd()
        handle.write(Data("2026-07-13T12:02:00.000Z info [desktop-notifications] show approval conversationId=thread-a kind=fileChange requestId=41\n".utf8))
        try handle.close()

        #expect(reader.pendingApproval(for: "thread-a")?.requestID == 41)
    }

    @Test
    func freshProcessSnapshotDoesNotInheritAResolvedApprovalState() {
        let cached = processItem(
            status: .waiting,
            subtitle: "Needs your approval",
            goalStatus: .active,
            runtimeStatus: .waitingOnApproval
        )
        let refreshed = processItem(
            status: .completed,
            subtitle: "Updated recently",
            goalStatus: nil,
            runtimeStatus: nil
        )

        let merged = CodexProcessStore.preservingCachedGoal(
            from: cached,
            in: refreshed
        )

        #expect(merged.goalStatus == .active)
        #expect(merged.runtimeStatus == nil)
        #expect(merged.subtitle.hasPrefix("Goal running"))
    }

    @Test
    func doesNotMixApprovalsFromDifferentThreads() {
        let lines = [
            "[desktop-notifications] show approval conversationId=thread-b kind=fileChange requestId=9",
        ]

        #expect(CodexDesktopApprovalLogParser.pendingApproval(
            for: "thread-a",
            lines: lines
        ) == nil)
    }

    @Test
    func findsEveryUnansweredApprovalForTheProcessList() {
        let lines = [
            "[desktop-notifications] show approval conversationId=thread-a kind=commandExecution requestId=8",
            "[desktop-notifications] show approval conversationId=thread-b kind=fileChange requestId=9",
        ]

        let requests = CodexDesktopApprovalLogParser.pendingApprovals(lines: lines)

        #expect(requests["thread-a"]?.method == .commandExecution)
        #expect(requests["thread-b"]?.method == .fileChange)
    }

    @Test
    func unrelatedServerResponsesDoNotClearAnApprovalWithTheSameID() {
        let lines = [
            "[desktop-notifications] show approval conversationId=thread-a kind=fileChange requestId=4",
            "Sending server response id=4 method=thread/list response={}",
        ]

        #expect(CodexDesktopApprovalLogParser.pendingApproval(
            for: "thread-a",
            lines: lines
        )?.requestID == 4)
    }

    @Test
    func findsTheNewestVisibleLocalThreadFromDesktopLogs() {
        let lines = [
            "IAB_LIFECYCLE received browser sidebar owner sync ownerRoutePath=/local/thread-a windowId=1",
            "IAB_LIFECYCLE received browser sidebar owner sync ownerRoutePath=/projects windowId=1",
            "IAB_LIFECYCLE received browser sidebar owner sync ownerRoutePath=/local/thread-b windowId=1",
        ]

        #expect(CodexDesktopApprovalLogParser.currentVisibleThreadID(
            lines: lines
        ) == "thread-b")
    }

    @Test
    func stripsLocalRouteSuffixesFromVisibleThreadID() {
        let lines = [
            "browser sidebar owner sync ownerRoutePath=/local/thread-a?view=compact windowId=1",
        ]

        #expect(CodexDesktopApprovalLogParser.currentVisibleThreadID(
            lines: lines
        ) == "thread-a")
    }

    @Test
    func approvalFallbackMarksEveryPendingThreadAsWaiting() {
        let statuses = CodexSharedThreadRuntimeReader.approvalFallbackStatuses([
            "thread-a": CodexPendingApproval(
                threadID: "thread-a",
                requestID: 8,
                method: .commandExecution
            ),
            "thread-b": CodexPendingApproval(
                threadID: "thread-b",
                requestID: 9,
                method: .fileChange
            ),
        ])

        #expect(statuses == [
            "thread-a": .waitingOnApproval,
            "thread-b": .waitingOnApproval,
        ])
    }

    @Test
    func capturesTheProposedCommandPolicyFromALiveRequest() {
        let request = CodexApprovalRequestParser.pendingApproval(from: [
            "id": 91,
            "method": "item/commandExecution/requestApproval",
            "params": [
                "threadId": "thread-a",
                "proposedExecpolicyAmendment": ["swift", "test"],
            ],
        ])

        #expect(request == CodexPendingApproval(
            threadID: "thread-a",
            requestID: 91,
            method: .commandExecution,
            proposedExecpolicyAmendment: ["swift", "test"]
        ))
    }

    @Test
    func approveOnceUsesTheSingleRequestDecision() {
        let result = CodexApprovalResponseFactory.result(
            for: .approveOnce,
            request: commandRequest()
        )

        #expect(result["decision"] as? String == "accept")
    }

    @Test
    func approveSimilarUsesTheExactProposedCommandPolicy() {
        let result = CodexApprovalResponseFactory.result(
            for: .approveSimilarCommands,
            request: commandRequest(amendment: ["swift", "test"])
        )
        let taggedDecision = result["decision"] as? [String: Any]
        let payload = taggedDecision?["acceptWithExecpolicyAmendment"] as? [String: Any]

        #expect(payload?["execpolicy_amendment"] as? [String] == ["swift", "test"])
    }

    @Test
    func approveSimilarFallsBackToTheSessionDecisionWithoutAProposal() {
        let result = CodexApprovalResponseFactory.result(
            for: .approveSimilarCommands,
            request: commandRequest()
        )

        #expect(result["decision"] as? String == "acceptForSession")
    }

    @Test
    func tellCodexFirstDeclinesTheApproval() {
        let result = CodexApprovalResponseFactory.result(
            for: .decline,
            request: commandRequest()
        )

        #expect(result["decision"] as? String == "decline")
    }

    private func commandRequest(amendment: [String]? = nil) -> CodexPendingApproval {
        CodexPendingApproval(
            threadID: "thread-a",
            requestID: 8,
            method: .commandExecution,
            proposedExecpolicyAmendment: amendment
        )
    }

    private func processItem(
        status: CodexProcessItem.Status,
        subtitle: String,
        goalStatus: CodexGoalStatus?,
        runtimeStatus: CodexThreadRuntimeStatus?
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-thread-a",
            kind: .thread,
            title: "Port Apple runtime to Windows",
            subtitle: subtitle,
            fullMessage: subtitle,
            updatedAt: Date(timeIntervalSince1970: 1_700_000_000),
            startedAt: nil,
            status: status,
            threadID: "thread-a",
            cwd: "/tmp/apple-runtime-port",
            activeTurnID: nil,
            goalID: goalStatus == nil ? nil : "goal-id",
            goalObjective: goalStatus == nil ? nil : "Port Apple runtime to Windows",
            goalStatus: goalStatus,
            goalTokenBudget: nil,
            goalElapsedSeconds: goalStatus == nil ? nil : 0,
            goalTimerReferenceDate: nil,
            runtimeStatus: runtimeStatus
        )
    }
}
