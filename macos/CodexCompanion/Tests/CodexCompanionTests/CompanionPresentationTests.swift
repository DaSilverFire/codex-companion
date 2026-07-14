import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionPresentationTests {
    @Test
    func codexComposerRequiresReplyOrSteerTarget() {
        #expect(!CompanionPresentationPolicy.showsPromptField(
            routeMode: .codex,
            isCodexProcessTrayVisible: true,
            hasProcessTarget: false,
            showsChatGPTAppHandoff: false
        ))
        #expect(CompanionPresentationPolicy.showsPromptField(
            routeMode: .codex,
            isCodexProcessTrayVisible: true,
            hasProcessTarget: true,
            showsChatGPTAppHandoff: false
        ))
        #expect(CompanionPresentationPolicy.showsPromptField(
            routeMode: .chatGPT,
            isCodexProcessTrayVisible: false,
            hasProcessTarget: false,
            showsChatGPTAppHandoff: false
        ))
        #expect(!CompanionPresentationPolicy.showsPromptField(
            routeMode: .chatGPT,
            isCodexProcessTrayVisible: false,
            hasProcessTarget: false,
            showsChatGPTAppHandoff: true
        ))
    }

    @Test
    func processTrayNeverCreatesASeparateComposerSurface() {
        #expect(!CompanionPresentationPolicy.showsComposerSurface(
            isCodexProcessTrayVisible: true,
            hasProcessTarget: false,
            showsAccessibilityNotice: false
        ))
        #expect(!CompanionPresentationPolicy.showsComposerSurface(
            isCodexProcessTrayVisible: true,
            hasProcessTarget: true,
            showsAccessibilityNotice: false
        ))
        #expect(!CompanionPresentationPolicy.showsComposerSurface(
            isCodexProcessTrayVisible: true,
            hasProcessTarget: false,
            showsAccessibilityNotice: true
        ))
        #expect(CompanionPresentationPolicy.showsComposerSurface(
            isCodexProcessTrayVisible: false,
            hasProcessTarget: false,
            showsAccessibilityNotice: false
        ))
    }

    @Test
    func processActionsAppearOnlyForTheHoveredTargetableCard() {
        #expect(!CompanionPresentationPolicy.showsProcessActions(
            isHovered: false,
            canTargetCodexThread: true
        ))
        #expect(CompanionPresentationPolicy.showsProcessActions(
            isHovered: true,
            canTargetCodexThread: true
        ))
        #expect(!CompanionPresentationPolicy.showsProcessActions(
            isHovered: true,
            canTargetCodexThread: false
        ))
    }

    @Test
    func failedProcessCardOffersReplyWithoutSteer() {
        #expect(CompanionPresentationPolicy.processActions(
            status: .failed,
            isHovered: true,
            canTargetCodexThread: true
        ) == [.reply])
        #expect(CompanionPresentationPolicy.processActions(
            status: .failed,
            isHovered: false,
            canTargetCodexThread: true
        ).isEmpty)
        #expect(CompanionPresentationPolicy.processActions(
            status: .failed,
            isHovered: true,
            canTargetCodexThread: false
        ).isEmpty)
    }

    @Test
    func nonfailedProcessCardActionsRemainUnchanged() {
        #expect(CompanionPresentationPolicy.processActions(
            status: .running,
            isHovered: true,
            canTargetCodexThread: true
        ) == [.reply, .steer])
        #expect(CompanionPresentationPolicy.processActions(
            status: .completed,
            isHovered: true,
            canTargetCodexThread: true
        ) == [.reply, .steer])
        #expect(CompanionPresentationPolicy.processActions(
            status: .waiting,
            isHovered: true,
            canTargetCodexThread: true
        ).isEmpty)
    }

    @Test
    func autonomousRoamingPausesForTrayHiddenPetOrAttentionMessage() {
        #expect(!CompanionPresentationPolicy.pausesRoaming(
            isQuickBarOpen: false,
            isPetVisible: true,
            hasAttentionMessage: false
        ))
        #expect(CompanionPresentationPolicy.pausesRoaming(
            isQuickBarOpen: true,
            isPetVisible: true,
            hasAttentionMessage: false
        ))
        #expect(CompanionPresentationPolicy.pausesRoaming(
            isQuickBarOpen: false,
            isPetVisible: false,
            hasAttentionMessage: false
        ))
        #expect(CompanionPresentationPolicy.pausesRoaming(
            isQuickBarOpen: false,
            isPetVisible: true,
            hasAttentionMessage: true
        ))
    }

    @Test
    func autonomousMovementRequiresAnUnownedRunningAnimation() {
        #expect(CompanionPresentationPolicy.acceptsRoamingMotion(
            isPetHovered: false,
            isPetDragging: false,
            hasInteractionAnimation: false
        ))
        #expect(!CompanionPresentationPolicy.acceptsRoamingMotion(
            isPetHovered: true,
            isPetDragging: false,
            hasInteractionAnimation: false
        ))
        #expect(!CompanionPresentationPolicy.acceptsRoamingMotion(
            isPetHovered: false,
            isPetDragging: true,
            hasInteractionAnimation: false
        ))
        #expect(!CompanionPresentationPolicy.acceptsRoamingMotion(
            isPetHovered: false,
            isPetDragging: false,
            hasInteractionAnimation: true
        ))
    }

    @Test
    func runningAnimationIsPrimedBeforeTheWindowCanMove() {
        var primer = PetRoamingMotionPrimer()

        let firstAcceptedTick = primer.shouldMove(animationAccepted: true)
        #expect(!firstAcceptedTick)
        #expect(primer.isPrimed)
        let secondAcceptedTick = primer.shouldMove(animationAccepted: true)
        #expect(secondAcceptedTick)

        let rejectedTick = primer.shouldMove(animationAccepted: false)
        #expect(!rejectedTick)
        #expect(!primer.isPrimed)
        let acceptedAfterReset = primer.shouldMove(animationAccepted: true)
        #expect(!acceptedAfterReset)
        let secondAcceptedAfterReset = primer.shouldMove(animationAccepted: true)
        #expect(secondAcceptedAfterReset)
    }

    @Test
    func processOnlyTrayIsShorterThanTargetComposerTray() {
        let item = CodexProcessItem(
            id: "thread-1",
            kind: .thread,
            title: "Task",
            subtitle: "Working now",
            fullMessage: "Latest response",
            updatedAt: Date(),
            startedAt: nil,
            status: .running,
            threadID: "thread-1",
            cwd: nil,
            goalID: nil,
            goalObjective: nil,
            goalStatus: nil,
            goalElapsedSeconds: nil,
            goalTimerReferenceDate: nil
        )
        let processOnly = PetWindowMetrics.traySize(
            showProcesses: true,
            processItems: [item],
            hasProcessTarget: false
        )
        let withComposer = PetWindowMetrics.traySize(
            showProcesses: true,
            processItems: [item],
            prompt: "A reply",
            hasProcessTarget: true,
            targetProcessID: item.id
        )

        #expect(withComposer.height > processOnly.height + 60)
    }

    @Test
    func openingAttentionTargetsTheVisibleCodexProcessTray() {
        let destination = CompanionPresentationPolicy.attentionDestination

        #expect(destination.routeMode == .codex)
        #expect(destination.isQuickBarOpen)
        #expect(destination.isCodexProcessTrayVisible)
    }

    @Test
    func petSpeechPanelExpandsForTheFullWrappedMessage() {
        let short = Self.attentionMessage(
            title: "Done.",
            detail: "Task finished."
        )
        let long = Self.attentionMessage(
            title: String(repeating: "I checked the result carefully. ", count: 8),
            detail: "The full process update is not displayed in the pet bubble."
        )
        let visibleFrame = NSRect(x: 0, y: 0, width: 1440, height: 900)

        let shortSize = PetAttentionLayout.panelSize(for: short, visibleFrame: visibleFrame)
        let longSize = PetAttentionLayout.panelSize(for: long, visibleFrame: visibleFrame)

        #expect(shortSize.width >= PetAttentionLayout.minimumSize.width)
        #expect(shortSize.height >= PetAttentionLayout.minimumSize.height)
        #expect(longSize.width <= PetAttentionLayout.maximumWidth)
        #expect(longSize.height <= PetAttentionLayout.maximumHeight)
        #expect(longSize.height > shortSize.height)
    }

    @Test
    func shortPetSpeechStaysCompactWhenTheProcessNameIsLong() {
        let message = Self.attentionMessage(
            title: "Done checking that.",
            detail: "Task finished."
        )
        var longProcessMessage = message
        longProcessMessage.processTitle = String(repeating: "Very long process name ", count: 8)

        let size = PetAttentionLayout.panelSize(for: longProcessMessage)

        #expect(size.width < PetAttentionLayout.maximumWidth)
        #expect(size.height == PetAttentionLayout.minimumSize.height)
    }

    @Test
    func inlinePromptFieldStaysCompactWhileGrowingWithWrappedText() {
        let empty = PetWindowMetrics.promptFieldHeight(for: "")
        let twoLines = PetWindowMetrics.promptFieldHeight(
            for: String(repeating: "a", count: 50)
        )
        let manyLines = PetWindowMetrics.promptFieldHeight(
            for: String(repeating: "a", count: 400)
        )

        #expect(empty == 38)
        #expect(twoLines > empty)
        #expect(manyLines == 68)
    }

    @Test
    func allFiveLoadedProcessesRemainPartOfTheScrollableLayout() {
        let items = (1...5).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let firstFour = Array(items.prefix(4))

        #expect(
            PetWindowMetrics.naturalProcessListHeight(for: items)
                > PetWindowMetrics.naturalProcessListHeight(for: firstFour)
        )
    }

    @Test
    func everyRetainedProcessRemainsInTheScrollableList() {
        let items = (1...6).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let storedItems = CodexProcessStore.retainedProcessItems(items)
        let visible = PetWindowMetrics.visibleProcessItems(
            from: storedItems,
            targetProcessID: items[5].id
        )

        #expect(storedItems.count == 6)
        #expect(visible.count == 6)
        #expect(visible.contains(where: { $0.id == items[5].id }))
        #expect(visible.contains(where: { $0.id == items[4].id }))
    }

    @Test
    func sharedRuntimeApprovalOverridesTimestampHeuristics() {
        var item = Self.processItem(index: 1, goalStatus: nil)
        item.status = .completed
        item.subtitle = "Updated earlier"
        item.fullMessage = "Old model response"
        let threadID = item.threadID!

        let merged = CodexProcessStore.applyingRuntimeStatuses(
            [threadID: .waitingOnApproval],
            to: [item]
        )

        #expect(merged.first?.status == .waiting)
        #expect(merged.first?.subtitle == "Needs your approval")
        #expect(merged.first?.fullMessage == "This task is waiting for your approval.")
    }

    @Test
    func goalRefreshDoesNotEraseSharedApprovalState() throws {
        var item = Self.processItem(index: 1, goalStatus: nil)
        let threadID = try #require(item.threadID)
        item = try #require(CodexProcessStore.applyingRuntimeStatuses(
            [threadID: .waitingOnApproval],
            to: [item]
        ).first)
        let goal = CodexGoalSnapshot(
            threadID: threadID,
            objective: "Finish the current task",
            status: .active,
            tokenBudget: nil,
            tokensUsed: 0,
            timeUsedSeconds: 60,
            createdAt: 1_000,
            updatedAt: 1_060
        )

        let refreshed = CodexProcessStore.applying(goal: goal, to: item)

        #expect(refreshed.status == .waiting)
        #expect(refreshed.subtitle == "Needs your approval")
        #expect(refreshed.runtimeStatus == .waitingOnApproval)
    }

    @Test
    func ordinaryCompletedThreadDisappearsFiveMinutesAfterItStopsRunning() {
        let now = Date(timeIntervalSince1970: 10_000)
        var visible = Self.processItem(index: 1, goalStatus: nil)
        visible.status = .completed
        visible.updatedAt = now.addingTimeInterval(-(3 * 60 + 5 * 60 - 1))
        var expired = visible
        expired.id = "thread-expired"
        expired.threadID = "thread-expired"
        expired.updatedAt = now.addingTimeInterval(-(3 * 60 + 5 * 60 + 1))

        let retained = CodexProcessStore.retainedProcessItems(
            [visible, expired],
            now: now
        )

        #expect(retained.map(\.id) == [visible.id])
    }

    @Test
    func completedJobDisappearsFiveMinutesAfterItsCompletionUpdate() {
        let now = Date(timeIntervalSince1970: 10_000)
        var visible = Self.processItem(index: 1, goalStatus: nil)
        visible.kind = .job
        visible.status = .completed
        visible.updatedAt = now.addingTimeInterval(-(5 * 60 - 1))
        var expired = visible
        expired.id = "job-expired"
        expired.updatedAt = now.addingTimeInterval(-(5 * 60 + 1))

        let retained = CodexProcessStore.retainedProcessItems(
            [visible, expired],
            now: now
        )

        #expect(retained.map(\.id) == [visible.id])
    }

    @Test
    func failedJobDoesNotExpireUntilItIsHandled() {
        let oldFailureDate = Date(timeIntervalSince1970: 1)
        var failed = Self.processItem(index: 1, goalStatus: nil)
        failed.kind = .job
        failed.status = .failed
        failed.updatedAt = oldFailureDate

        #expect(CodexProcessStore.isCurrentJob(
            status: failed.status,
            updatedAt: failed.updatedAt
        ))
        #expect(CodexProcessStore.retainedProcessItems(
            [failed],
            now: Date(timeIntervalSince1970: 1_000_000)
        ).map(\.id) == [failed.id])
    }

    @Test
    func unresolvedFailureSurvivesRefreshUntilNewThreadActivityAppears() {
        let failureDate = Date(timeIntervalSince1970: 100)
        var failed = Self.processItem(index: 1, goalStatus: nil)
        failed.status = .failed
        failed.updatedAt = failureDate

        var unchanged = failed
        unchanged.status = .completed
        let stillFailed = CodexProcessStore.reconcileFailures(
            cached: [failed.id: failed],
            refreshed: [unchanged]
        )
        #expect(stillFailed.items.first?.status == .failed)
        #expect(stillFailed.unresolvedFailures[failed.id] != nil)

        var resumed = unchanged
        resumed.status = .running
        resumed.runtimeStatus = .active
        resumed.updatedAt = failureDate.addingTimeInterval(1)
        let resolved = CodexProcessStore.reconcileFailures(
            cached: stillFailed.unresolvedFailures,
            refreshed: [resumed]
        )
        #expect(resolved.items.first?.status == .running)
        #expect(resolved.unresolvedFailures[failed.id] == nil)
    }

    @Test
    func rediscoveredFailureUsesOneCanonicalPersistedIdentity() {
        var cached = Self.processItem(index: 1, goalStatus: nil)
        cached.id = "job-old"
        cached.kind = .job
        cached.status = .failed

        var refreshed = cached
        refreshed.id = "thread-1"
        refreshed.kind = .thread

        let reconciled = CodexProcessStore.reconcileFailures(
            cached: [cached.id: cached],
            refreshed: [refreshed]
        )

        #expect(reconciled.items.map(\.id) == [refreshed.id])
        #expect(reconciled.unresolvedFailures.count == 1)
        #expect(reconciled.unresolvedFailures[refreshed.id] == refreshed)
    }

    @Test
    func handledFailureStaysDismissedUntilANewerFailureOccurs() {
        let originalFailureDate = Date(timeIntervalSince1970: 100)
        var failed = Self.processItem(index: 1, goalStatus: nil)
        failed.status = .failed
        failed.updatedAt = originalFailureDate
        let handled = CodexProcessStore.HandledFailure(
            processID: failed.id,
            threadID: failed.threadID,
            failureUpdatedAt: originalFailureDate
        )

        let suppressed = CodexProcessStore.reconcileFailures(
            cached: [:],
            handled: [handled.processID: handled],
            refreshed: [failed]
        )
        #expect(suppressed.items.isEmpty)
        #expect(suppressed.unresolvedFailures.isEmpty)
        #expect(suppressed.handledFailures[handled.processID] == handled)

        failed.updatedAt = originalFailureDate.addingTimeInterval(1)
        let resurfaced = CodexProcessStore.reconcileFailures(
            cached: suppressed.unresolvedFailures,
            handled: suppressed.handledFailures,
            refreshed: [failed]
        )
        #expect(resurfaced.items.map(\.id) == [failed.id])
        #expect(resurfaced.unresolvedFailures[failed.id] == failed)
        #expect(resurfaced.handledFailures.isEmpty)
    }

    @Test
    func failedProcessCardKeepsItsRedAccent() {
        #expect(CompanionPresentationPolicy.processAccent(
            status: .failed,
            runtimeStatus: nil,
            attentionAccent: .blue
        ) == .red)
        #expect(CompanionPresentationPolicy.processAccent(
            status: .waiting,
            runtimeStatus: .waitingOnApproval,
            attentionAccent: .blue
        ) == .yellow)
    }

    @Test
    func completedGoalKeepsItsSeparateThirtyMinutePresentationRule() {
        let now = Date(timeIntervalSince1970: 10_000)
        var goal = Self.processItem(index: 1, goalStatus: .complete)
        goal.status = .completed
        goal.updatedAt = now.addingTimeInterval(-20 * 60)

        let retained = CodexProcessStore.retainedProcessItems([goal], now: now)

        #expect(retained.map(\.id) == [goal.id])
    }

    @Test
    func processViewportStopsGrowingAfterThreeGoalCards() {
        let threeItems = (1...3).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let sixItems = (1...6).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }

        let threeHeight = PetWindowMetrics.processListHeight(
            for: threeItems,
            isLoading: false
        )
        let sixHeight = PetWindowMetrics.processListHeight(
            for: sixItems,
            isLoading: false
        )

        #expect(PetWindowMetrics.maximumProcessCardsWithoutScrolling == 3)
        #expect(threeHeight == PetWindowMetrics.maxProcessListHeight)
        #expect(sixHeight == threeHeight)
        #expect(PetWindowMetrics.processListNeedsScrolling(
            for: sixItems,
            isLoading: false
        ))
    }

    @Test
    func topEdgeTrayFallsBackBesideThePetWithoutOverlap() {
        let visibleFrame = NSRect(x: 0, y: 0, width: 1_440, height: 900)
        let anchorFrame = NSRect(x: 1_280, y: 720, width: 124, height: 124)
        let traySize = CGSize(width: 292, height: 416)

        let origin = PetWindowMetrics.positionedTrayOrigin(
            anchorFrame: anchorFrame,
            traySize: traySize,
            visibleFrame: visibleFrame
        )
        let trayFrame = NSRect(origin: origin, size: traySize)

        #expect(trayFrame.maxX < anchorFrame.minX)
        #expect(!trayFrame.intersects(anchorFrame))
        #expect(visibleFrame.contains(trayFrame))
    }

    @Test
    func trayCentersOverThePetWhenThereIsRoomAbove() {
        let visibleFrame = NSRect(x: 0, y: 0, width: 1_440, height: 900)
        let anchorFrame = NSRect(x: 640, y: 120, width: 124, height: 124)
        let traySize = CGSize(width: 292, height: 240)

        let origin = PetWindowMetrics.positionedTrayOrigin(
            anchorFrame: anchorFrame,
            traySize: traySize,
            visibleFrame: visibleFrame
        )
        let trayFrame = NSRect(origin: origin, size: traySize)

        #expect(abs(trayFrame.midX - anchorFrame.midX) < 0.001)
        #expect(trayFrame.minY == anchorFrame.maxY + PetWindowMetrics.trayGap)
    }

    @Test
    func directionalLookFramesMatchNativeClockwiseAngleOrder() throws {
        let petFrame = CGRect(x: 100, y: 100, width: 124, height: 124)
        let center = CGPoint(x: petFrame.midX, y: petFrame.midY)

        let up = try #require(PetDirectionalLookFrame.resolve(
            pointer: CGPoint(x: center.x, y: center.y + 100),
            petFrame: petFrame,
            startRow: 9
        ))
        let right = try #require(PetDirectionalLookFrame.resolve(
            pointer: CGPoint(x: center.x + 100, y: center.y),
            petFrame: petFrame,
            startRow: 9
        ))
        let down = try #require(PetDirectionalLookFrame.resolve(
            pointer: CGPoint(x: center.x, y: center.y - 100),
            petFrame: petFrame,
            startRow: 9
        ))
        let left = try #require(PetDirectionalLookFrame.resolve(
            pointer: CGPoint(x: center.x - 100, y: center.y),
            petFrame: petFrame,
            startRow: 9
        ))

        #expect(up == PetDirectionalLookFrame(row: 9, column: 0))
        #expect(right == PetDirectionalLookFrame(row: 9, column: 4))
        #expect(down == PetDirectionalLookFrame(row: 10, column: 0))
        #expect(left == PetDirectionalLookFrame(row: 10, column: 4))
    }

    @Test
    func inlineSendFeedbackExpandsOnlyTheSelectedProcessComposer() {
        let withoutFeedback = PetWindowMetrics.inlineProcessComposerHeight(
            prompt: "Keep this draft",
            showsAccessibilityNotice: false
        )
        let withFeedback = PetWindowMetrics.inlineProcessComposerHeight(
            prompt: "Keep this draft",
            showsAccessibilityNotice: false,
            showsSendFeedback: true
        )

        #expect(withFeedback == withoutFeedback + 36)
    }

    @Test
    @MainActor
    func disappearingProcessPreservesDraftUntilTheComposerIsEmpty() throws {
        let suiteName = "CompanionPresentationTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let model = CompanionAppModel(
            petReactionCoordinator: PetReactionCoordinator(
                generator: UnavailablePetReactionGenerator(),
                defaults: defaults
            ),
            petVisibilityPreference: PetVisibilityPreference(defaults: defaults),
            interactionPreferences: CompanionInteractionPreferences(defaults: defaults),
            startsBackgroundServices: false
        )
        let items = (1...6).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let storedItems = CodexProcessStore.retainedProcessItems(items)
        let target = CodexProcessTarget(
            item: items[5],
            action: .reply
        )
        #expect(target != nil)
        model.activeProcessTarget = target
        model.prompt = "Keep this draft"

        model.reconcileProcessTarget(with: storedItems)
        #expect(model.activeProcessTarget?.processID == items[5].id)
        #expect(model.prompt == "Keep this draft")

        let failedRefreshItems = CodexProcessStore.itemsAfterRefreshFailure(
            current: storedItems,
            error: NSError(domain: "CompanionPresentationTests", code: 1)
        )
        #expect(failedRefreshItems == storedItems)
        model.reconcileProcessTarget(with: failedRefreshItems)
        #expect(model.activeProcessTarget?.processID == items[5].id)
        #expect(model.prompt == "Keep this draft")

        let initialFailureItems = CodexProcessStore.itemsAfterRefreshFailure(
            current: [],
            error: NSError(domain: "CompanionPresentationTests", code: 2)
        )
        #expect(initialFailureItems.count == 1)
        #expect(initialFailureItems[0].id == "codex-process-error")

        model.reconcileProcessTarget(with: CodexProcessStore.retainedProcessItems(
            Array(items.prefix(5))
        ))
        #expect(model.activeProcessTarget?.processID == items[5].id)
        #expect(model.prompt == "Keep this draft")

        model.prompt = ""
        model.reconcileProcessTarget(with: CodexProcessStore.retainedProcessItems(
            Array(items.prefix(5))
        ))
        #expect(model.activeProcessTarget == nil)
        #expect(model.prompt.isEmpty)
    }

    @Test
    func targetableProcessCardExpandsOnlyForItsActionRow() {
        let item = Self.processItem(index: 1, goalStatus: .active)
        let collapsed = PetWindowMetrics.processRowHeight(
            for: item,
            showsActions: false
        )
        let expanded = PetWindowMetrics.processRowHeight(
            for: item,
            showsActions: true
        )

        #expect(collapsed == 72)
        #expect(expanded == collapsed + 29)
    }

    @Test
    func selectedProcessCardOwnsTheInlineComposerHeight() {
        let item = Self.processItem(index: 1, goalStatus: .active)
        let collapsed = PetWindowMetrics.processRowHeight(
            for: item,
            showsActions: false
        )
        let composerHeight = PetWindowMetrics.inlineProcessComposerHeight(
            prompt: "A reply",
            showsAccessibilityNotice: false
        )
        let targeted = PetWindowMetrics.processRowHeight(
            for: item,
            showsActions: false,
            inlineComposerHeight: composerHeight
        )

        #expect(targeted == collapsed + 5 + composerHeight)
    }

    @Test
    func hoveringOneProcessExpandsTrayByOnlyThatActionRow() {
        let items = (1...2).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let collapsed = PetWindowMetrics.traySize(
            showProcesses: true,
            processItems: items
        )
        let expanded = PetWindowMetrics.traySize(
            showProcesses: true,
            processItems: items,
            expandedProcessID: items[0].id
        )

        #expect(expanded.height == collapsed.height + 29)
    }

    @Test
    func embeddedComposerScrollsInsideTheBoundedProcessViewport() {
        let items = (1...5).map { index in
            Self.processItem(index: index, goalStatus: .active)
        }
        let prompt = String(repeating: "Long reply text ", count: 28)
        let listHeight = PetWindowMetrics.processListHeight(
            for: items,
            isLoading: false,
            prompt: prompt,
            hasProcessTarget: true,
            showsAccessibilityNotice: true,
            targetProcessID: items[0].id
        )
        let tray = PetWindowMetrics.traySize(
            showProcesses: true,
            processItems: items,
            prompt: prompt,
            hasProcessTarget: true,
            showsAccessibilityNotice: true,
            targetProcessID: items[0].id
        )

        #expect(listHeight == PetWindowMetrics.maxProcessListHeight)
        #expect(tray.height <= PetWindowMetrics.maxProcessTrayHeight)
        #expect(PetWindowMetrics.processListNeedsScrolling(
            for: items,
            isLoading: false,
            prompt: prompt,
            hasProcessTarget: true,
            showsAccessibilityNotice: true,
            targetProcessID: items[0].id
        ))
    }

    private static func processItem(
        index: Int,
        goalStatus: CodexGoalStatus?
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "thread-\(index)",
            kind: .thread,
            title: "Task \(index)",
            subtitle: "Working now",
            fullMessage: "Latest response",
            updatedAt: Date(),
            startedAt: nil,
            status: .running,
            threadID: "thread-\(index)",
            cwd: nil,
            goalID: goalStatus == nil ? nil : "goal-\(index)",
            goalObjective: goalStatus == nil ? nil : "Finish task \(index)",
            goalStatus: goalStatus,
            goalElapsedSeconds: goalStatus == nil ? nil : 10,
            goalTimerReferenceDate: nil
        )
    }

    private static func attentionMessage(title: String, detail: String) -> PetAttentionMessage {
        PetAttentionMessage(
            kind: .response,
            title: title,
            detail: detail,
            processTitle: "Codex Companion",
            processID: "thread-1",
            threadID: "thread-1",
            reactionContext: PetReactionContext(
                event: .response,
                processID: "thread-1",
                processTitle: "Codex Companion",
                detail: detail,
                goalObjective: nil
            )
        )
    }
}
