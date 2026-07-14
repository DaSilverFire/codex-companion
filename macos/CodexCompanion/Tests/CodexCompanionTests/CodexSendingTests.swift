import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexSendingTests {
    @Test
    func replyStartsDirectlyWhileSteerTargetsTheActiveTurn() {
        #expect(!CodexAppServerRequestPolicy.requiresTurnDiscovery(before: .reply))
        #expect(CodexAppServerRequestPolicy.requiresTurnDiscovery(before: .steer))
        #expect(CodexAppServerRequestPolicy.turnItemsView == "notLoaded")
        #expect(CodexAppServerRequestPolicy.turnDiscoveryTimeoutSeconds == 30)
    }

    @Test
    @MainActor
    func directCodexSendNeverBlocksTheMainActor() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, _ in
            try? await Task.sleep(for: .milliseconds(150))
            return .sent
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "A nonblocking reply"

        let clock = ContinuousClock()
        let startedAt = clock.now
        model.sendPrompt(mode: RouteMode.codex)
        let callDuration = startedAt.duration(to: clock.now)

        #expect(callDuration < .milliseconds(100))
        #expect(model.isCodexSending)
        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt.isEmpty)
        #expect(model.activeProcessTarget == nil)
        #expect(model.isQuickBarOpen)
        #expect(model.isCodexProcessTrayVisible)
    }

    @Test
    @MainActor
    func unavailableNativeTransportKeepsTheDraftWithoutRequestingARestart() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, _ in
            .sharedDaemonUnavailable
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Keep this draft"

        model.sendPrompt(mode: RouteMode.codex)

        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt == "Keep this draft")
        #expect(model.activeProcessTarget != nil)
        #expect(model.status.contains("local task connection is unavailable"))
        #expect(model.status.contains("Restart ChatGPT") == false)
        #expect(model.codexComposerFeedback?.text.contains("local task connection is unavailable") == true)
        #expect(model.codexComposerFeedback?.isError == true)
    }

    @Test
    @MainActor
    func stalledNativeTransportTimesOutAndReleasesTheComposer() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let gate = StalledSubmitterGate()
        let model = Self.makeModel(
            defaults: isolated.defaults,
            sendTimeout: .milliseconds(40)
        ) { _, _, _, _, _, _, _ in
            await gate.wait()
            return .sent
        }
        await Task.yield()
        model.steer(Self.processItem(activeTurnID: "turn-live"))
        model.prompt = "Keep this draft after the timeout"

        model.sendPrompt(mode: .codex)

        #expect(model.isCodexSending)
        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt == "Keep this draft after the timeout")
        #expect(model.activeProcessTarget != nil)
        #expect(model.status.contains("could not be confirmed"))
        #expect(model.codexComposerFeedback?.isError == true)

        await gate.release()
    }

    @Test
    @MainActor
    func queuedReplyKeepsItsDraftUntilCodexAcceptsIt() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, action, _, _, onQueued in
            #expect(action == .reply)
            onQueued()
            try? await Task.sleep(for: .milliseconds(150))
            return .sent
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Queue this after the running turn"

        model.sendPrompt(mode: RouteMode.codex)

        #expect(await Self.waitUntil { model.status.contains("Reply queued") })
        #expect(model.isCodexSending)
        #expect(model.prompt == "Queue this after the running turn")
        #expect(model.activeProcessTarget != nil)
        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.status == "Sent reply to Current task.")
        #expect(model.codexComposerFeedback == nil)
        #expect(model.prompt.isEmpty)
        #expect(model.activeProcessTarget == nil)
        #expect(model.isQuickBarOpen)
        #expect(model.isCodexProcessTrayVisible)
    }

    @Test
    @MainActor
    func olderCompletionDoesNotEraseANewerComposerTarget() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, _ in
            try? await Task.sleep(for: .milliseconds(150))
            return .sent
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "First draft"
        model.sendPrompt(mode: RouteMode.codex)
        #expect(model.isCodexSending)

        model.reply(to: Self.processItem(id: "thread-new", title: "New task"))
        model.prompt = "Newer draft"

        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt == "Newer draft")
        #expect(model.activeProcessTarget?.threadID == "thread-new")
        #expect(model.status.contains("newer draft is still here"))
    }

    @Test
    @MainActor
    func cancelingAQueuedReplyCancelsTheSendAndClearsTheComposer() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, onQueued in
            onQueued()
            do {
                try await Task.sleep(for: .seconds(10))
                return .sent
            } catch {
                return .failed
            }
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Do not send this after cancel"
        model.sendPrompt(mode: RouteMode.codex)

        #expect(await Self.waitUntil { model.status.contains("Reply queued") })
        model.cancelProcessTarget()

        #expect(!model.isCodexSending)
        #expect(model.activeProcessTarget == nil)
        #expect(model.prompt.isEmpty)
        #expect(model.status == "Canceled pending message to Current task.")
        try? await Task.sleep(for: .milliseconds(50))
        #expect(model.status == "Canceled pending message to Current task.")
    }

    @Test
    @MainActor
    func disappearingProcessDoesNotEraseAnUnresolvedDraft() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, onQueued in
            onQueued()
            try? await Task.sleep(for: .milliseconds(150))
            return .failed
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Keep this even if the process vanishes"
        model.sendPrompt(mode: RouteMode.codex)

        #expect(await Self.waitUntil { model.status.contains("Reply queued") })
        model.reconcileProcessTarget(with: [])
        #expect(model.activeProcessTarget != nil)
        #expect(model.prompt == "Keep this even if the process vanishes")
        #expect(await Self.waitUntil { !model.isCodexSending })
        model.reconcileProcessTarget(with: [])
        #expect(model.activeProcessTarget != nil)
        #expect(model.prompt == "Keep this even if the process vanishes")
    }

    @Test
    @MainActor
    func metadataRefreshDoesNotMakeAnAcknowledgedSendLookUnsent() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, _, _ in
            try? await Task.sleep(for: .milliseconds(150))
            return .sent
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Acknowledge this once"
        model.sendPrompt(mode: RouteMode.codex)

        model.reconcileProcessTarget(with: [
            Self.processItem(title: "Renamed task", status: .completed, cwd: "/tmp/renamed-task"),
        ])

        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt.isEmpty)
        #expect(model.activeProcessTarget == nil)
        #expect(!model.status.contains("newer draft"))
    }

    @Test
    @MainActor
    func retryingAnUnconfirmedSendReusesItsClientMessageID() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let recorder = SendInvocationRecorder()
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, _, _, clientMessageID, _ in
            let invocation = recorder.record(clientMessageID)
            return invocation == 1 ? .timedOut : .sent
        }
        await Task.yield()
        model.reply(to: Self.processItem())
        model.prompt = "Retry this idempotently"

        model.sendPrompt(mode: RouteMode.codex)
        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(model.prompt == "Retry this idempotently")
        model.sendPrompt(mode: RouteMode.codex)
        #expect(await Self.waitUntil { !model.isCodexSending })

        let messageIDs = recorder.messageIDs
        #expect(messageIDs.count == 2)
        #expect(messageIDs[0] == messageIDs[1])
    }

    @Test
    func malformedTurnsListIsNotTreatedAsIdle() {
        let malformed: [String: Any] = [
            "result": ["data": "not-a-turn-array"],
        ]
        let idle: [String: Any] = [
            "result": ["data": [[String: Any]]()],
        ]

        #expect(CodexAppServerResponseParser.turnsListState(from: malformed) == nil)
        #expect(CodexAppServerResponseParser.turnsListState(from: idle) == .idle)
    }

    @Test
    @MainActor
    func steerForwardsTheRolloutTurnIDWithoutHistoryDiscovery() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let recorder = TurnIDRecorder()
        let model = Self.makeModel(defaults: isolated.defaults) { _, _, _, action, expectedTurnID, _, _ in
            recorder.record(action: action, turnID: expectedTurnID)
            return .sent
        }
        await Task.yield()
        model.steer(Self.processItem(activeTurnID: "turn-live"))
        model.prompt = "Use the known turn"

        model.sendPrompt(mode: .codex)

        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(recorder.action == .steer)
        #expect(recorder.turnID == "turn-live")
    }

    @Test
    @MainActor
    func tellCodexDeclinesTheApprovalBeforeSendingGuidanceAsAReply() async throws {
        let isolated = try TestDefaults()
        defer { isolated.reset() }
        let approvalRecorder = ApprovalDecisionRecorder()
        let promptRecorder = PromptInvocationRecorder()
        let model = Self.makeModel(
            defaults: isolated.defaults,
            submitter: { prompt, _, _, action, _, _, _ in
                promptRecorder.record(prompt: prompt, action: action)
                return .sent
            },
            approvalSubmitter: { _, decision in
                approvalRecorder.record(decision)
                return decision == .decline ? .declined : .approved
            }
        )
        await Task.yield()
        let approvalItem = Self.processItem(
            status: .waiting,
            runtimeStatus: .waitingOnApproval
        )

        model.tellCodexSomethingElse(approvalItem)
        model.prompt = "Use the safer command instead"
        model.sendPrompt(mode: .codex)

        #expect(await Self.waitUntil { !model.isCodexSending })
        #expect(approvalRecorder.decisions == [.decline])
        #expect(promptRecorder.prompt == "Use the safer command instead")
        #expect(promptRecorder.action == .reply)
        #expect(model.status == "Sent guidance to Current task.")
        #expect(model.prompt.isEmpty)
        #expect(model.activeProcessTarget == nil)
    }

    @MainActor
    private static func makeModel(
        defaults: UserDefaults,
        sendTimeout: Duration = .seconds(50),
        submitter: @escaping CodexPromptSubmitter,
        approvalSubmitter: @escaping CodexApprovalSubmitter = { _, _ in .requestNotFound }
    ) -> CompanionAppModel {
        CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: CompanionInteractionPreferences(defaults: defaults),
            codexPromptSubmitter: submitter,
            codexApprovalSubmitter: approvalSubmitter,
            codexSendTimeout: sendTimeout,
            startsBackgroundServices: false
        )
    }

    private static func processItem(
        id: String = "thread-current",
        title: String = "Current task",
        status: CodexProcessItem.Status = .running,
        cwd: String = "/tmp/current-task",
        activeTurnID: String? = nil,
        runtimeStatus: CodexThreadRuntimeStatus? = nil
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: id,
            kind: .thread,
            title: title,
            subtitle: "Working now",
            fullMessage: "Latest response",
            updatedAt: Date(),
            startedAt: Date(),
            status: status,
            threadID: id,
            cwd: cwd,
            activeTurnID: activeTurnID,
            goalID: nil,
            goalObjective: nil,
            goalStatus: nil,
            goalElapsedSeconds: nil,
            goalTimerReferenceDate: nil,
            runtimeStatus: runtimeStatus
        )
    }

    @MainActor
    private static func waitUntil(
        timeout: Duration = .seconds(1),
        condition: () -> Bool
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }
}

private actor StalledSubmitterGate {
    private var continuation: CheckedContinuation<Void, Never>?

    func wait() async {
        await withCheckedContinuation { continuation in
            self.continuation = continuation
        }
    }

    func release() {
        continuation?.resume()
        continuation = nil
    }
}

private final class SendInvocationRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storedMessageIDs: [String] = []

    @discardableResult
    func record(_ messageID: String) -> Int {
        lock.withLock {
            storedMessageIDs.append(messageID)
            return storedMessageIDs.count
        }
    }

    var messageIDs: [String] {
        lock.withLock { storedMessageIDs }
    }
}

private final class TurnIDRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storedAction: CodexSendAction?
    private var storedTurnID: String?

    func record(action: CodexSendAction, turnID: String?) {
        lock.withLock {
            storedAction = action
            storedTurnID = turnID
        }
    }

    var action: CodexSendAction? {
        lock.withLock { storedAction }
    }

    var turnID: String? {
        lock.withLock { storedTurnID }
    }
}

private final class ApprovalDecisionRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storedDecisions: [CodexApprovalDecision] = []

    func record(_ decision: CodexApprovalDecision) {
        lock.withLock {
            storedDecisions.append(decision)
        }
    }

    var decisions: [CodexApprovalDecision] {
        lock.withLock { storedDecisions }
    }
}

private final class PromptInvocationRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storedPrompt: String?
    private var storedAction: CodexSendAction?

    func record(prompt: String, action: CodexSendAction) {
        lock.withLock {
            storedPrompt = prompt
            storedAction = action
        }
    }

    var prompt: String? {
        lock.withLock { storedPrompt }
    }

    var action: CodexSendAction? {
        lock.withLock { storedAction }
    }
}

private struct TestDefaults {
    let suiteName: String
    let defaults: UserDefaults

    init() throws {
        suiteName = "CodexSendingTests-\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            throw TestDefaultsError.unavailable
        }
        self.defaults = defaults
    }

    func reset() {
        defaults.removePersistentDomain(forName: suiteName)
    }
}

private enum TestDefaultsError: Error {
    case unavailable
}
