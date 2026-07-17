import Combine
import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionAttentionTests {
    @Test
    func hoverOnlyArrowPreferenceDefaultsOffAndPersists() throws {
        let suiteName = "CompanionAttentionTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let preferences = CompanionInteractionPreferences(defaults: defaults)
        #expect(!preferences.hidesMenuButtonUntilHover)

        preferences.hidesMenuButtonUntilHover = true
        #expect(CompanionInteractionPreferences(defaults: defaults).hidesMenuButtonUntilHover)
    }

    @Test
    func autonomousPetMovementDefaultsOnAndPersists() throws {
        let suiteName = "CompanionAttentionTests-Movement-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let preferences = CompanionInteractionPreferences(defaults: defaults)
        #expect(preferences.allowsAutonomousPetMovement)

        preferences.allowsAutonomousPetMovement = false
        #expect(!CompanionInteractionPreferences(defaults: defaults).allowsAutonomousPetMovement)
    }

    @Test
    @MainActor
    func autonomousPetMovementChangesReachTheLiveRoamer() throws {
        let suiteName = "CompanionAttentionTests-LiveMovement-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }
        var receivedValues: [Bool] = []
        let model = CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: CompanionInteractionPreferences(defaults: defaults),
            autonomousPetMovementHandler: { receivedValues.append($0) },
            startsBackgroundServices: false
        )

        model.allowsAutonomousPetMovement = false
        model.allowsAutonomousPetMovement = true

        #expect(receivedValues == [false, true])
    }

    @Test
    func transitionSelectorCoversAttentionGoalResponseCompletionAndFailure() throws {
        let running = Self.item(status: .running, message: "Working", goalStatus: nil, goalID: nil)
        let runningSnapshot = PetProcessSnapshot(item: running)

        let waiting = Self.item(status: .waiting, message: "Need approval", goalStatus: nil, goalID: nil)
        #expect(PetAttentionMessage.transition(previous: runningSnapshot, current: waiting)?.kind == .attention)

        let goal = Self.item(status: .running, message: "Starting", goalStatus: .active, goalID: "goal-2")
        #expect(PetAttentionMessage.transition(previous: runningSnapshot, current: goal)?.kind == .goal)

        let response = Self.item(status: .running, message: "New model response", goalStatus: nil, goalID: nil)
        #expect(PetAttentionMessage.transition(previous: runningSnapshot, current: response)?.kind == .response)

        let complete = Self.item(status: .completed, message: "Done", goalStatus: nil, goalID: nil)
        #expect(PetAttentionMessage.transition(previous: runningSnapshot, current: complete)?.kind == .completion)

        let failed = Self.item(status: .failed, message: "Disconnected", goalStatus: nil, goalID: nil)
        #expect(PetAttentionMessage.transition(previous: runningSnapshot, current: failed)?.kind == .failure)
    }

    @Test
    func failureWinsWhenMessageAlsoChanges() throws {
        let previous = PetProcessSnapshot(
            item: Self.item(status: .running, message: "Working", goalStatus: nil, goalID: nil)
        )
        let failed = Self.item(
            status: .failed,
            message: "Connection lost",
            goalStatus: .blocked,
            goalID: "goal-3"
        )

        let message = try #require(PetAttentionMessage.transition(previous: previous, current: failed))
        #expect(message.kind == .failure)
        #expect(message.detail == "Connection lost")
    }

    @Test
    func newlyDiscoveredActionableProcessCreatesAttention() throws {
        let waiting = Self.item(
            status: .waiting,
            message: "Approve the pending request",
            goalStatus: nil,
            goalID: nil
        )
        let failed = Self.item(
            status: .failed,
            message: "Connection lost",
            goalStatus: nil,
            goalID: nil
        )
        let activeGoal = Self.item(
            status: .running,
            message: "Working",
            goalStatus: .active,
            goalID: "goal-4"
        )

        #expect(PetAttentionMessage.appearance(current: waiting)?.kind == .attention)
        #expect(PetAttentionMessage.appearance(current: failed)?.kind == .failure)
        #expect(PetAttentionMessage.appearance(current: activeGoal)?.kind == .goal)
    }

    @Test
    func completedGoalUsesGoalCompletionReactionContext() throws {
        let running = Self.item(
            status: .running,
            message: "Working",
            goalStatus: .active,
            goalID: "goal-1"
        )
        let completed = Self.item(
            status: .completed,
            message: "Done",
            goalStatus: .complete,
            goalID: "goal-1"
        )

        let transition = try #require(PetAttentionMessage.transition(
            previous: PetProcessSnapshot(item: running),
            current: completed
        ))
        let appearance = try #require(PetAttentionMessage.appearance(current: completed))

        #expect(transition.reactionContext.event == .goalCompletion)
        #expect(appearance.reactionContext.event == .goalCompletion)
    }

    @Test
    func ordinaryCompletionUsesCompletionReactionContext() throws {
        let running = Self.item(
            status: .running,
            message: "Working",
            goalStatus: nil,
            goalID: nil
        )
        let completed = Self.item(
            status: .completed,
            message: "Done",
            goalStatus: nil,
            goalID: nil
        )

        let transition = try #require(PetAttentionMessage.transition(
            previous: PetProcessSnapshot(item: running),
            current: completed
        ))
        let appearance = try #require(PetAttentionMessage.appearance(current: completed))

        #expect(transition.reactionContext.event == .completion)
        #expect(appearance.reactionContext.event == .completion)
    }

    @Test
    func waitingUsesApprovalReactionContext() throws {
        let running = Self.item(
            status: .running,
            message: "Working",
            goalStatus: nil,
            goalID: nil
        )
        let waiting = Self.item(
            status: .waiting,
            message: "Need approval",
            goalStatus: nil,
            goalID: nil
        )

        let transition = try #require(PetAttentionMessage.transition(
            previous: PetProcessSnapshot(item: running),
            current: waiting
        ))
        let appearance = try #require(PetAttentionMessage.appearance(current: waiting))

        #expect(transition.reactionContext.event == .approval)
        #expect(appearance.reactionContext.event == .approval)
    }

    @Test
    func responseFailureAndGoalStartUseMatchingReactionContexts() throws {
        let running = Self.item(
            status: .running,
            message: "Working",
            goalStatus: nil,
            goalID: nil
        )
        let previous = PetProcessSnapshot(item: running)
        let response = Self.item(
            status: .running,
            message: "New model response",
            goalStatus: nil,
            goalID: nil
        )
        let failed = Self.item(
            status: .failed,
            message: "Connection lost",
            goalStatus: nil,
            goalID: nil
        )
        let activeGoal = Self.item(
            status: .running,
            message: "Starting",
            goalStatus: .active,
            goalID: "goal-2"
        )

        #expect(try #require(PetAttentionMessage.transition(
            previous: previous,
            current: response
        )).reactionContext.event == .response)
        #expect(try #require(PetAttentionMessage.transition(
            previous: previous,
            current: failed
        )).reactionContext.event == .failure)
        #expect(try #require(PetAttentionMessage.appearance(current: failed))
            .reactionContext.event == .failure)
        #expect(try #require(PetAttentionMessage.transition(
            previous: previous,
            current: activeGoal
        )).reactionContext.event == .goalStarted)
        #expect(try #require(PetAttentionMessage.appearance(current: activeGoal))
            .reactionContext.event == .goalStarted)
    }

    @Test
    func initialHeadlineUsesPetFallbackInsteadOfMechanicalStatusCopy() throws {
        let items = [
            Self.item(status: .waiting, message: "Need approval", goalStatus: nil, goalID: nil),
            Self.item(status: .completed, message: "Done", goalStatus: nil, goalID: nil),
            Self.item(status: .failed, message: "Connection lost", goalStatus: nil, goalID: nil),
        ]

        for item in items {
            let message = try #require(PetAttentionMessage.appearance(current: item))
            let mechanicalTitles = [
                "\(message.processTitle) finished",
                "\(message.processTitle) failed",
                "\(message.processTitle) needs attention",
            ]

            #expect(message.title == PetReactionCopy.fallback(
                for: message.reactionContext,
                excluding: []
            ))
            #expect(!mechanicalTitles.contains(message.title))
        }
    }

    @Test
    func supportingTextUsesOnlyTheTaskNameInsteadOfRepeatingTheFullUpdate() throws {
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Connection lost",
            goalStatus: nil,
            goalID: nil
        )))

        #expect(message.title == "Uh-oh, I hit a snag.")
        #expect(message.supportingText == "Example task")
        #expect(message.title != message.supportingText)

        let duplicate = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "  Example task  ",
            goalStatus: nil,
            goalID: nil
        )))
        let empty = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "   ",
            goalStatus: nil,
            goalID: nil
        )))
        let punctuationDuplicate = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Example task.",
            goalStatus: nil,
            goalID: nil
        )))

        #expect(duplicate.supportingText == "Example task")
        #expect(empty.supportingText == "Example task")
        #expect(punctuationDuplicate.supportingText == "Example task")
    }

    @Test
    func attentionKindsMapToSemanticTrayAccents() {
        let processID = "process-1"
        let accents: [(PetAttentionMessage.Kind, PetAttentionAccent)] = [
            (.response, .blue),
            (.attention, .yellow),
            (.completion, .green),
            (.goal, .indigo),
            (.failure, .red),
        ]

        for (kind, accent) in accents {
            let highlight = PetAttentionHighlight(message: PetAttentionMessage(
                kind: kind,
                title: "Update",
                detail: "Full process update",
                processTitle: "Example task",
                processID: processID,
                threadID: processID,
                reactionContext: PetReactionContext(
                    event: .response,
                    processID: processID,
                    processTitle: "Example task",
                    detail: "Full process update",
                    goalObjective: nil
                )
            ))

            #expect(highlight.processID == processID)
            #expect(highlight.accent == accent)
        }
    }

    @Test
    func replacingTitlePreservesIdentityAndMetadata() throws {
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Connection lost",
            goalStatus: .blocked,
            goalID: "goal-3"
        )))
        var expected = message
        expected.title = "I found a different line!"

        #expect(message.replacingTitle("I found a different line!") == expected)
        #expect(message.replacingTitle("I found a different line!").id == message.id)
    }

    @Test
    @MainActor
    func appModelAcceptsOnlyLatestReactionThenRecordsPresentedCopy() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let staleTitle = "This stale update should stay hidden."
        let latestTitle = "The newest update is ready."
        let generator = AttentionReactionGenerator(responses: [
            .ignoresCancellation(staleTitle, .milliseconds(80)),
            .immediate(latestTitle),
        ])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let first = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .completed,
            message: "First result",
            goalStatus: nil,
            goalID: nil
        )))
        let latest = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Latest result",
            goalStatus: nil,
            goalID: nil
        )))

        model.presentAttentionMessage(first)
        #expect(await Self.waitForGenerationCalls(1, generator: generator))
        model.presentAttentionMessage(latest)
        #expect(await Self.waitForGenerationCalls(2, generator: generator))
        #expect(await Self.waitForAttentionTitle(latestTitle, model: model))
        let messageID = try #require(model.attentionMessage?.id)
        model.attentionMessageDidBecomeVisible(messageID)
        try await Task.sleep(for: .milliseconds(120))

        #expect(model.attentionMessage?.title == latestTitle)
        #expect(
            isolated.defaults.stringArray(
                forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
            ) == [latestTitle]
        )
        model.dismissAttentionMessage()
    }

    @Test
    @MainActor
    func appModelPublishesBeforeRecordingPresentedCopy() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let title = "I found a fresh update."
        let generator = AttentionReactionGenerator(responses: [.immediate(title)])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .completed,
            message: "Done",
            goalStatus: nil,
            goalID: nil
        )))
        var historyAtPublication: [String]?
        let observation = model.$attentionMessage.sink { attention in
            guard attention?.title == title else { return }
            historyAtPublication = isolated.defaults.stringArray(
                forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
            ) ?? []
        }

        model.presentAttentionMessage(message)
        #expect(await Self.waitForAttentionTitle(title, model: model))
        #expect(historyAtPublication == [])
        #expect(isolated.defaults.stringArray(
            forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
        ) == nil)
        let messageID = try #require(model.attentionMessage?.id)
        model.attentionMessageDidBecomeVisible(messageID)
        #expect(await Self.waitForRecordedTitle(title, defaults: isolated.defaults))
        withExtendedLifetime(observation) {}
        model.dismissAttentionMessage()
    }

    @Test
    @MainActor
    func dismissCancelsTheCurrentReactionAfterAStaleRequestFinishes() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let generator = AttentionReactionGenerator(responses: [
            .ignoresCancellation("The old request came back late.", .milliseconds(30)),
            .ignoresCancellation("This should stay hidden too.", .milliseconds(140)),
        ])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let first = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .completed,
            message: "First result",
            goalStatus: nil,
            goalID: nil
        )))
        let second = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Second result",
            goalStatus: nil,
            goalID: nil
        )))

        model.presentAttentionMessage(first)
        #expect(await Self.waitForGenerationCalls(1, generator: generator))
        model.presentAttentionMessage(second)
        #expect(await Self.waitForGenerationCalls(2, generator: generator))
        try await Task.sleep(for: .milliseconds(50))
        model.dismissAttentionMessage()
        try await Task.sleep(for: .milliseconds(150))

        #expect(model.attentionMessage == nil)
        #expect(isolated.defaults.stringArray(
            forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
        ) == nil)
    }

    @Test
    @MainActor
    func hiddenPetAndOpenTrayDoNotStartUnseenReactions() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let generator = AttentionReactionGenerator(responses: [
            .immediate("This must not be generated."),
            .immediate("Neither should this one."),
        ])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .completed,
            message: "Done",
            goalStatus: nil,
            goalID: nil
        )))

        model.hidePet()
        model.presentAttentionMessage(message)
        model.showPet()
        model.isQuickBarOpen = true
        model.presentAttentionMessage(message)
        try await Task.sleep(for: .milliseconds(40))

        #expect(await generator.generationCount() == 0)
        #expect(model.attentionMessage == nil)
        #expect(model.latestAttentionHighlight == PetAttentionHighlight(message: message))
        #expect(isolated.defaults.stringArray(
            forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
        ) == nil)
    }

    @Test
    @MainActor
    func dragReleaseRestoresTheActiveAttentionAnimation() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let title = "I hit a small snag."
        let generator = AttentionReactionGenerator(responses: [.immediate(title)])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Failure",
            goalStatus: nil,
            goalID: nil
        )))

        model.presentAttentionMessage(message)
        #expect(await Self.waitForAttentionTitle(title, model: model))
        model.beginPetDrag()
        model.endPetDrag()

        #expect(model.renderedPetState == .failed)
        model.dismissAttentionMessage()
    }

    @Test
    @MainActor
    func isolatedModelDoesNotPrewarmFoundationModels() async {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let generator = AttentionReactionGenerator(responses: [])
        _ = Self.makeModel(generator: generator, defaults: isolated.defaults)
        try? await Task.sleep(for: .milliseconds(30))

        #expect(await generator.prewarmCount() == 0)
    }

    @Test
    @MainActor
    func hidingPetCancelsPendingReactionWithoutRecordingIt() async throws {
        let isolated = AttentionTestDefaults()
        defer { isolated.reset() }
        let generator = AttentionReactionGenerator(responses: [
            .ignoresCancellation("This hidden line must not appear.", .milliseconds(80)),
        ])
        let model = Self.makeModel(generator: generator, defaults: isolated.defaults)
        let message = try #require(PetAttentionMessage.appearance(current: Self.item(
            status: .failed,
            message: "Failure",
            goalStatus: nil,
            goalID: nil
        )))

        model.presentAttentionMessage(message)
        #expect(await Self.waitForGenerationCalls(1, generator: generator))
        model.hidePet()
        try await Task.sleep(for: .milliseconds(100))

        #expect(model.attentionMessage == nil)
        #expect(!model.isPetVisible)
        #expect(isolated.defaults.stringArray(
            forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
        ) == nil)
    }

    @MainActor
    private static func makeModel(
        generator: AttentionReactionGenerator,
        defaults: UserDefaults
    ) -> CompanionAppModel {
        CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: generator,
                defaults: defaults,
                timeout: .seconds(1)
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: CompanionInteractionPreferences(defaults: defaults),
            startsBackgroundServices: false
        )
    }

    @MainActor
    private static func waitForAttentionTitle(
        _ title: String,
        model: CompanionAppModel,
        timeout: Duration = .seconds(1)
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            if model.attentionMessage?.title == title {
                return true
            }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return model.attentionMessage?.title == title
    }

    private static func waitForGenerationCalls(
        _ count: Int,
        generator: AttentionReactionGenerator,
        timeout: Duration = .seconds(1)
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            if await generator.generationCount() >= count {
                return true
            }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return await generator.generationCount() >= count
    }

    private static func waitForRecordedTitle(
        _ title: String,
        defaults: UserDefaults,
        timeout: Duration = .seconds(1)
    ) async -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            if defaults.stringArray(
                forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
            ) == [title] {
                return true
            }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return defaults.stringArray(
            forKey: PetReactionCoordinator.recentHeadlinesDefaultsKey
        ) == [title]
    }

    private static func item(
        status: CodexProcessItem.Status,
        message: String,
        goalStatus: CodexGoalStatus?,
        goalID: String?
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-1",
            kind: .thread,
            title: "Example task",
            subtitle: "Status",
            fullMessage: message,
            updatedAt: Date(),
            startedAt: nil,
            status: status,
            threadID: "thread-1",
            cwd: nil,
            goalID: goalID,
            goalObjective: goalStatus == nil ? nil : "Finish it",
            goalStatus: goalStatus,
            goalElapsedSeconds: goalStatus == nil ? nil : 10,
            goalTimerReferenceDate: nil
        )
    }
}

private actor AttentionReactionGenerator: PetReactionGenerating {
    enum Response: Sendable {
        case immediate(String?)
        case ignoresCancellation(String?, Duration)
    }

    private var responses: [Response]
    private var calls = 0
    private var prewarms = 0

    init(responses: [Response]) {
        self.responses = responses
    }

    func prewarm() async {
        prewarms += 1
    }

    func generate(
        for context: PetReactionContext,
        excluding recent: [String]
    ) async -> String? {
        calls += 1
        let response = responses.isEmpty ? Response.immediate(nil) : responses.removeFirst()
        switch response {
        case .immediate(let line):
            return line
        case .ignoresCancellation(let line, let delay):
            return await Task.detached {
                try? await Task.sleep(for: delay)
                return line
            }.value
        }
    }

    func generationCount() -> Int {
        calls
    }

    func prewarmCount() -> Int {
        prewarms
    }
}

private struct AttentionTestDefaults {
    let suiteName = "CodexCompanionTests.Attention.\(UUID().uuidString)"
    let defaults: UserDefaults

    init() {
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            fatalError("Could not create isolated UserDefaults")
        }
        self.defaults = defaults
        defaults.removePersistentDomain(forName: suiteName)
    }

    func reset() {
        defaults.removePersistentDomain(forName: suiteName)
    }
}
