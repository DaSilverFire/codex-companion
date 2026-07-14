import Combine
import CoreGraphics
import Foundation

typealias CodexPromptSubmitter = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ cwd: String?,
    _ action: CodexSendAction,
    _ expectedTurnID: String?,
    _ clientMessageID: String,
    _ onQueued: @escaping @Sendable () -> Void
) async -> CodexAppServerSendOutcome

typealias CodexApprovalSubmitter = @Sendable (
    _ threadID: String,
    _ decision: CodexApprovalDecision
) async -> CodexAppServerApprovalOutcome

@MainActor
final class CompanionAppModel: ObservableObject {
    static let codexOnlyEnvironmentKey = "CODEX_COMPANION_CODEX_ONLY"
    private static let codexOnlyModeEnabled = {
        guard let value = ProcessInfo.processInfo.environment[codexOnlyEnvironmentKey]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
        else { return false }
        return ["1", "true", "yes", "codex"].contains(value)
    }()

    let isCodexOnlyMode = CompanionAppModel.codexOnlyModeEnabled
    let petStore = PetStore()
    let historyStore = RouteHistoryStore()
    let rateLimitStore = CodexRateLimitStore()
    let processStore = CodexProcessStore()
    let chatGPTQuickBar = ChatGPTQuickBarHandoff()

    private let petReactionCoordinator: PetReactionCoordinator
    private let petVisibilityPreference: PetVisibilityPreference
    private let interactionPreferences: CompanionInteractionPreferences
    private let codexPromptSubmitter: CodexPromptSubmitter
    private let codexApprovalSubmitter: CodexApprovalSubmitter
    private let codexSendTimeout: Duration
    private let onDeviceChatService: any OnDeviceChatServing

    @Published var selectedState: PetAnimationState = .idle
    @Published var routeMode: RouteMode = CompanionAppModel.codexOnlyModeEnabled ? .codex : .chatGPT
    @Published var prompt = ""
    @Published var status = "Ready"
    @Published var isQuickBarOpen = false
    @Published var isCodexProcessTrayVisible = false
    @Published var isChatGPTModelPickerExpanded = false
    @Published var chatGPTMenuResponse: ChatGPTMenuResponse?
    @Published var isChatGPTResponding = false
    @Published var isCodexSending = false
    @Published private(set) var approvingThreadID: String?
    @Published private(set) var codexComposerFeedback: CodexComposerFeedback?
    @Published var openAIAPIKeyInput = ""
    @Published var openAIAPIKeyStatus: String
    @Published var hasOpenAIAPIKey: Bool
    @Published var roamingState: PetAnimationState = .running
    @Published private(set) var petInteractionState: PetAnimationState?
    @Published private(set) var directionalLookFrame: PetDirectionalLookFrame?
    @Published var activeProcessTarget: CodexProcessTarget?
    @Published var activeGoalControl: CodexGoalControlState?
    @Published private(set) var isUpdatingGoal = false
    @Published private(set) var goalControlError: String?
    @Published private(set) var isCodexAccessibilityTrusted = CodexVisibleReplySender.isAccessibilityTrusted
    @Published private(set) var goalConfettiTrigger = 0
    @Published private(set) var lastReachedGoalTitle: String?
    @Published private(set) var isPetVisible: Bool {
        didSet {
            petVisibilityPreference.isVisible = isPetVisible
        }
    }
    @Published var hidesMenuButtonUntilHover: Bool {
        didSet {
            interactionPreferences.hidesMenuButtonUntilHover = hidesMenuButtonUntilHover
        }
    }
    @Published private(set) var isPetPointerHovered = false
    @Published private(set) var hoveredProcessID: String?
    @Published private(set) var attentionMessage: PetAttentionMessage?
    @Published private(set) var latestAttentionHighlight: PetAttentionHighlight?
    @Published var animationSpeedScale: Double {
        didSet {
            UserDefaults.standard.set(animationSpeedScale, forKey: Self.animationSpeedKey)
        }
    }
    @Published var useRiggedShadowRenderer: Bool {
        didSet {
            UserDefaults.standard.set(useRiggedShadowRenderer, forKey: Self.riggedShadowRendererKey)
        }
    }
    @Published var selectedChatGPTModel: ChatGPTModel {
        didSet {
            UserDefaults.standard.set(selectedChatGPTModel.rawValue, forKey: Self.chatGPTModelKey)
        }
    }
    @Published var selectedChatGPTDeliveryMode: ChatGPTDeliveryMode {
        didSet {
            UserDefaults.standard.set(selectedChatGPTDeliveryMode.rawValue, forKey: Self.chatGPTDeliveryModeKey)
            if selectedChatGPTDeliveryMode == .appHandoff, routeMode == .chatGPT, isQuickBarOpen {
                chatGPTQuickBar.open(model: selectedChatGPTModel)
            } else {
                chatGPTQuickBar.close()
            }
        }
    }

    private let router = PromptRouter()
    private let openAIAPIKeyStore = OpenAIAPIKeyStore()
    private let openAIChatService = OpenAIChatService()
    private var cancellables: Set<AnyCancellable> = []
    private var isPetHovered = false
    private var isPetDragging = false
    private var lastManualRunState: PetAnimationState = .runningRight
    private var lastRoamingRunState: PetAnimationState = .runningRight
    private var processRefreshTimer: Timer?
    private var passiveProcessRefreshTimer: Timer?
    private var processSnapshots: [String: PetProcessSnapshot] = [:]
    private var hasSeededProcessSnapshots = false
    private var goalAnimationTask: Task<Void, Never>?
    private var attentionDismissTask: Task<Void, Never>?
    private var attentionReactionTask: Task<Void, Never>?
    private var attentionReactionRequestToken = UUID()
    private var pendingPresentedReaction: (messageID: UUID, title: String)?
    private var seenGoalCompletionKeys: Set<String> = []
    private var activeCodexSendRequestID: UUID?
    private var activeCodexSendTask: Task<CodexAppServerSendOutcome, Never>?
    private var activeCodexSendTimeoutTask: Task<Void, Never>?
    private var activeApprovalFeedbackTask: Task<Void, Never>?
    private var pendingCodexSendIdentity: CodexSendIdentity?
    private var pendingCodexClientMessageID: String?

    init(
        petReactionCoordinator: PetReactionCoordinator = PetReactionCoordinator(),
        petVisibilityPreference: PetVisibilityPreference = PetVisibilityPreference(),
        interactionPreferences: CompanionInteractionPreferences = CompanionInteractionPreferences(),
        onDeviceChatService: any OnDeviceChatServing = OnDeviceChatServiceFactory.make(),
        codexPromptSubmitter: @escaping CodexPromptSubmitter = { prompt, threadID, cwd, action, expectedTurnID, clientMessageID, onQueued in
            await CodexAppServerSender().submit(
                prompt: prompt,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                clientMessageID: clientMessageID,
                onQueued: onQueued
            )
        },
        codexApprovalSubmitter: @escaping CodexApprovalSubmitter = { threadID, decision in
            await CodexAppServerApprovalSender().respond(
                threadID: threadID,
                decision: decision
            )
        },
        codexSendTimeout: Duration = .seconds(50),
        startsBackgroundServices: Bool = true
    ) {
        self.petReactionCoordinator = petReactionCoordinator
        self.petVisibilityPreference = petVisibilityPreference
        self.interactionPreferences = interactionPreferences
        self.onDeviceChatService = onDeviceChatService
        self.codexPromptSubmitter = codexPromptSubmitter
        self.codexApprovalSubmitter = codexApprovalSubmitter
        self.codexSendTimeout = codexSendTimeout
        isPetVisible = petVisibilityPreference.isVisible
        hidesMenuButtonUntilHover = interactionPreferences.hidesMenuButtonUntilHover
        let speedTimingVersion = UserDefaults.standard.integer(forKey: Self.animationSpeedTimingVersionKey)
        let savedSpeed = UserDefaults.standard.double(forKey: Self.animationSpeedKey)
        if speedTimingVersion < Self.currentAnimationSpeedTimingVersion {
            let migratedSpeed = 1.15
            animationSpeedScale = migratedSpeed
            UserDefaults.standard.set(migratedSpeed, forKey: Self.animationSpeedKey)
            UserDefaults.standard.set(Self.currentAnimationSpeedTimingVersion, forKey: Self.animationSpeedTimingVersionKey)
        } else {
            animationSpeedScale = savedSpeed > 0 ? savedSpeed : 1.15
        }
        useRiggedShadowRenderer = false
        UserDefaults.standard.set(false, forKey: Self.riggedShadowRendererKey)
        let savedChatGPTModel = UserDefaults.standard.string(forKey: Self.chatGPTModelKey)
        selectedChatGPTModel = savedChatGPTModel.flatMap(ChatGPTModel.init(rawValue:)) ?? .gpt55
        let savedChatGPTDeliveryMode = UserDefaults.standard.string(forKey: Self.chatGPTDeliveryModeKey)
        let restoredDeliveryMode = savedChatGPTDeliveryMode.flatMap(ChatGPTDeliveryMode.init(rawValue:)) ?? .onDevice
        selectedChatGPTDeliveryMode = isCodexOnlyMode && restoredDeliveryMode == .appHandoff
            ? .onDevice
            : restoredDeliveryMode
        let savedAPIKeyExists = openAIAPIKeyStore.hasKey
        hasOpenAIAPIKey = savedAPIKeyExists
        openAIAPIKeyStatus = savedAPIKeyExists ? "OpenAI API key saved for this Mac." : "No OpenAI API key saved."
        seenGoalCompletionKeys = Set(UserDefaults.standard.stringArray(forKey: Self.seenGoalCompletionKeysKey) ?? [])
        if isCodexOnlyMode {
            routeMode = .codex
            chatGPTQuickBar.close()
            status = "Codex-only Companion ready."
        }

        petStore.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        historyStore.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        rateLimitStore.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        processStore.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        processStore.$items
            .sink { [weak self] items in
                Task { @MainActor in
                    self?.reconcileProcessTarget(with: items)
                    self?.handleProcessTransitions(items)
                    self?.syncAnimationWithProcesses(items)
                }
            }
            .store(in: &cancellables)

        if startsBackgroundServices {
            CodexPendingApprovalBroker.shared.start()
            rateLimitStore.refresh()
            processStore.refresh()
            startPassiveProcessRefreshTimer()
            refreshCodexAccessibilityStatus()
            Task(priority: .utility) {
                await petReactionCoordinator.prewarm()
                await onDeviceChatService.prewarm()
            }
        }
    }

    var promptPlaceholder: String {
        guard let activeProcessTarget else {
            if routeMode == .codex {
                return "Message"
            }
            return selectedChatGPTDeliveryMode == .onDevice ? "Ask on device" : "Ask ChatGPT"
        }
        return activeProcessTarget.promptTitle
    }

    var selectableRouteModes: [RouteMode] {
        RouteMode.selectableCases
    }

    var selectableChatGPTDeliveryModes: [ChatGPTDeliveryMode] {
        isCodexOnlyMode ? [.onDevice, .openAIAPI] : ChatGPTDeliveryMode.allCases
    }

    var processTargetSummary: String? {
        activeProcessTarget?.promptTitle
    }

    var processTargetSystemName: String? {
        activeProcessTarget?.action.systemName
    }

    var renderedPetState: PetAnimationState {
        if let petInteractionState {
            return petInteractionState
        }
        return isQuickBarOpen ? selectedState : roamingState
    }

    var activeCodexProcessCount: Int {
        let activeItems = processStore.items.filter { item in
            item.kind != .notice && (item.status == .running || item.status == .waiting)
        }
        return activeItems.count
    }

    var shouldShowPetMenuButton: Bool {
        isQuickBarOpen || !hidesMenuButtonUntilHover || isPetPointerHovered
    }

    var shouldShowCodexUsageInfo: Bool {
        routeMode == .codex || isCodexProcessTrayVisible
    }

    var shouldShowCodexAccessibilityNotice: Bool {
        // Reply, Steer, and approvals use ChatGPT's native follower IPC.
        return false
    }

    var shouldShowChatGPTModelPicker: Bool {
        false
    }

    var shouldExpandQuickBarForChatGPTModelPicker: Bool {
        shouldShowChatGPTModelPicker && isChatGPTModelPickerExpanded
    }

    var shouldShowChatGPTMenuResponse: Bool {
        routeMode == .chatGPT
            && selectedChatGPTDeliveryMode != .appHandoff
            && !isCodexProcessTrayVisible
            && (chatGPTMenuResponse != nil || isChatGPTResponding)
    }

    var shouldShowChatGPTAppHandoff: Bool {
        !isCodexOnlyMode && routeMode == .chatGPT && selectedChatGPTDeliveryMode == .appHandoff && !isCodexProcessTrayVisible
    }

    var shouldShowCompanionPromptField: Bool {
        CompanionPresentationPolicy.showsPromptField(
            routeMode: routeMode,
            isCodexProcessTrayVisible: isCodexProcessTrayVisible,
            hasProcessTarget: activeProcessTarget != nil,
            showsChatGPTAppHandoff: shouldShowChatGPTAppHandoff
        )
    }

    var shouldShowComposerSurface: Bool {
        CompanionPresentationPolicy.showsComposerSurface(
            isCodexProcessTrayVisible: isCodexProcessTrayVisible,
            hasProcessTarget: activeProcessTarget != nil,
            showsAccessibilityNotice: shouldShowCodexAccessibilityNotice
        )
    }

    var shouldShowCompanionSendButton: Bool {
        shouldShowCompanionPromptField
    }

    func sendPrompt(mode: RouteMode? = nil) {
        let selectedMode = mode ?? routeMode
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let target = selectedMode == .codex ? activeProcessTarget : nil
        let result: RouteResult
        CodexSendLog.append("companion send invoked mode=\(selectedMode.rawValue) hasTarget=\(target != nil) promptChars=\(trimmedPrompt.count)")

        if target == nil && selectedMode == .chatGPT {
            sendChatGPTMenuPrompt(trimmedPrompt)
            return
        }

        if target == nil && selectedMode == .codex {
            CodexSendLog.append("companion send blocked missing codex target")
            refreshCodexAccessibilityStatus()
            routeMode = .codex
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            selectedState = .waiting
            status = "Pick Reply or Steer on a Codex process first."
            processStore.refreshIfStale()
            startProcessRefreshTimer()
            return
        }

        if let target {
            let threadID = target.threadID

            guard !trimmedPrompt.isEmpty else {
                CodexSendLog.append("companion send opened target thread=\(target.threadID) action=\(target.action.logName)")
                router.openCodexThread(threadID)
                status = "Opened \(target.title)."
                selectedState = target.animationState
                isQuickBarOpen = true
                isCodexProcessTrayVisible = true
                processStore.refreshIfStale()
                startProcessRefreshTimer()
                return
            }

            sendCodexPromptAsync(trimmedPrompt, target: target)
            return
        } else {
            result = router.route(
                prompt: prompt,
                mode: selectedMode,
                history: historyStore,
                chatGPTModel: selectedChatGPTModel
            )
        }

        if let target, !trimmedPrompt.isEmpty, result.destination == .codex, result.succeeded {
            let action: String
            action = target.sentActionTitle
            status = "\(action) to \(target.title)."
        } else {
            status = result.message
        }
        selectedState = result.destination == .codex ? (result.succeeded ? .running : .failed) : .waving
        if result.destination == .codex {
            refreshCodexAccessibilityStatus()
            chatGPTQuickBar.close()
            if !trimmedPrompt.isEmpty, result.succeeded {
                isQuickBarOpen = false
                isCodexProcessTrayVisible = false
                stopProcessRefreshTimer()
            } else {
                isQuickBarOpen = true
                isCodexProcessTrayVisible = true
                processStore.refreshIfStale()
                startProcessRefreshTimer()
            }
            chatGPTMenuResponse = nil
        } else {
            isQuickBarOpen = true
            isCodexProcessTrayVisible = false
            stopProcessRefreshTimer()
            isChatGPTModelPickerExpanded = false
            clearProcessTarget()
        }
        if !trimmedPrompt.isEmpty, result.succeeded {
            clearProcessTarget()
            prompt = ""
        }
    }

    func continueCodex() {
        refreshCodexAccessibilityStatus()
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmedPrompt.isEmpty, let activeProcessTarget {
            sendCodexPromptAsync(trimmedPrompt, target: activeProcessTarget)
            return
        }

        let result = router.continueCodex(
            prompt: prompt,
            history: historyStore,
            threadID: activeProcessTarget?.threadID,
            cwd: activeProcessTarget?.cwd,
            action: activeProcessTarget?.action.codexSendAction ?? .reply
        )
        status = result.message
        selectedState = result.succeeded ? .running : .failed
        refreshCodexAccessibilityStatus()
        if !trimmedPrompt.isEmpty, result.succeeded {
            isQuickBarOpen = false
            isCodexProcessTrayVisible = false
            stopProcessRefreshTimer()
        } else {
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            processStore.refreshIfStale()
            startProcessRefreshTimer()
        }
        chatGPTQuickBar.close()
        if !trimmedPrompt.isEmpty, result.succeeded {
            clearProcessTarget()
            prompt = ""
        }
    }

    private func sendCodexPromptAsync(_ trimmedPrompt: String, target: CodexProcessTarget) {
        guard !isCodexSending else {
            CodexSendLog.append("companion send ignored already sending thread=\(target.threadID) action=\(target.action.logName)")
            status = "Still sending to \(target.title)..."
            return
        }

        let threadID = target.threadID
        let cwd = target.cwd
        let action = target.action.codexSendAction
        let expectedTurnID = target.activeTurnID
        let targetTitle = target.title
        let sentActionTitle = target.sentActionTitle
        let promptToSend = trimmedPrompt
        let requestID = UUID()
        let sendIdentity = CodexSendIdentity(
            threadID: threadID,
            action: action,
            prompt: promptToSend
        )
        let clientMessageID: String
        if pendingCodexSendIdentity == sendIdentity,
           let existingMessageID = pendingCodexClientMessageID
        {
            clientMessageID = existingMessageID
        } else {
            clientMessageID = UUID().uuidString
            pendingCodexSendIdentity = sendIdentity
            pendingCodexClientMessageID = clientMessageID
        }

        activeCodexSendRequestID = requestID
        isCodexSending = true
        routeMode = .codex
        isQuickBarOpen = true
        isCodexProcessTrayVisible = true
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.close()
        selectedState = .running
        switch target.action {
        case .reply:
            status = "Sending reply to \(targetTitle)..."
        case .steer:
            status = "Steering \(targetTitle)..."
        case .approvalFeedback:
            status = "Telling Codex what to do instead..."
        }
        codexComposerFeedback = CodexComposerFeedback(text: status, isError: false)
        CodexSendLog.append("companion send started thread=\(threadID) action=\(target.action.logName) title=\(targetTitle) promptChars=\(promptToSend.count)")

        if target.action == .approvalFeedback {
            startApprovalFeedbackSend(
                promptToSend: promptToSend,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                target: target,
                targetTitle: targetTitle,
                sentActionTitle: sentActionTitle,
                clientMessageID: clientMessageID,
                requestID: requestID
            )
        } else {
            startDirectCodexSend(
                promptToSend: promptToSend,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                target: target,
                targetTitle: targetTitle,
                sentActionTitle: sentActionTitle,
                clientMessageID: clientMessageID,
                requestID: requestID
            )
        }
    }

    private func startApprovalFeedbackSend(
        promptToSend: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        target: CodexProcessTarget,
        targetTitle: String,
        sentActionTitle: String,
        clientMessageID: String,
        requestID: UUID
    ) {
        let approvalSubmitter = codexApprovalSubmitter
        activeApprovalFeedbackTask = Task { [weak self] in
            let approvalOutcome = await Task.detached(priority: .userInitiated) {
                await approvalSubmitter(threadID, .decline)
            }.value
            guard
                let self,
                !Task.isCancelled,
                activeCodexSendRequestID == requestID
            else { return }

            activeApprovalFeedbackTask = nil
            guard approvalOutcome == .declined else {
                finishApprovalFeedbackFailure(
                    approvalOutcome,
                    targetTitle: targetTitle,
                    requestID: requestID
                )
                return
            }

            processStore.refresh()
            status = "Sending guidance to \(targetTitle)..."
            codexComposerFeedback = CodexComposerFeedback(text: status, isError: false)
            startDirectCodexSend(
                promptToSend: promptToSend,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                target: target,
                targetTitle: targetTitle,
                sentActionTitle: sentActionTitle,
                clientMessageID: clientMessageID,
                requestID: requestID
            )
        }
    }

    private func startDirectCodexSend(
        promptToSend: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        target: CodexProcessTarget,
        targetTitle: String,
        sentActionTitle: String,
        clientMessageID: String,
        requestID: UUID
    ) {
        let submitter = codexPromptSubmitter
        let onQueued: @Sendable () -> Void = { [weak self] in
            Task { @MainActor [weak self] in
                guard let self, self.activeCodexSendRequestID == requestID else { return }
                self.status = "Reply queued for \(targetTitle). It will send after the current response."
                self.codexComposerFeedback = CodexComposerFeedback(text: self.status, isError: false)
                self.selectedState = .waiting
            }
        }
        // Start on the main actor so executor starvation elsewhere in Companion cannot
        // prevent the native transport from being entered at all. The transport moves
        // its blocking socket work onto a dedicated dispatch queue.
        let sendTask = Task(priority: .userInitiated) {
            await submitter(
                promptToSend,
                threadID,
                cwd,
                action,
                expectedTurnID,
                clientMessageID,
                onQueued
            )
        }
        activeCodexSendTask = sendTask
        let sendTimeout = codexSendTimeout
        activeCodexSendTimeoutTask?.cancel()
        activeCodexSendTimeoutTask = Task { [weak self] in
            do {
                try await Task.sleep(for: sendTimeout)
            } catch {
                return
            }
            guard
                let self,
                self.activeCodexSendRequestID == requestID,
                self.isCodexSending
            else { return }

            CodexSendLog.append(
                "companion send deadline exceeded thread=\(threadID) action=\(action.logName)"
            )
            sendTask.cancel()
            self.finishCodexSend(
                outcome: .timedOut,
                prompt: promptToSend,
                target: target,
                targetTitle: targetTitle,
                sentActionTitle: sentActionTitle,
                action: action,
                requestID: requestID
            )
        }
        Task { [weak self] in
            let outcome = await sendTask.value
            guard let self else { return }
            CodexSendLog.append(
                "companion app-server \(action.logName) outcome=\(String(describing: outcome)) thread=\(threadID)"
            )
            finishCodexSend(
                outcome: outcome,
                prompt: promptToSend,
                target: target,
                targetTitle: targetTitle,
                sentActionTitle: sentActionTitle,
                action: action,
                requestID: requestID
            )
        }
    }

    private func finishApprovalFeedbackFailure(
        _ outcome: CodexAppServerApprovalOutcome,
        targetTitle: String,
        requestID: UUID
    ) {
        guard activeCodexSendRequestID == requestID else { return }
        activeCodexSendRequestID = nil
        activeApprovalFeedbackTask = nil
        isCodexSending = false
        switch outcome {
        case .requestNotFound:
            status = "The approval request is no longer available. Your guidance is still here."
        case .sharedDaemonUnavailable:
            status = "ChatGPT's native task connection is unavailable. Your guidance is still here."
        case .timedOut:
            status = "Codex did not confirm the decline. Check \(targetTitle) before retrying."
        case .failed, .approved:
            status = "Codex did not accept the approval response. Your guidance is still here."
        case .declined:
            return
        }
        selectedState = .failed
        codexComposerFeedback = CodexComposerFeedback(text: status, isError: true)
        isQuickBarOpen = true
        isCodexProcessTrayVisible = true
        startProcessRefreshTimer()
    }

    private func finishCodexSend(
        outcome: CodexAppServerSendOutcome,
        prompt sentPrompt: String,
        target: CodexProcessTarget,
        targetTitle: String,
        sentActionTitle: String,
        action: CodexSendAction,
        requestID: UUID
    ) {
        guard activeCodexSendRequestID == requestID else {
            CodexSendLog.append("companion ignored stale send completion request=\(requestID)")
            return
        }
        activeCodexSendRequestID = nil
        activeCodexSendTask = nil
        activeCodexSendTimeoutTask?.cancel()
        activeCodexSendTimeoutTask = nil
        isCodexSending = false
        refreshCodexAccessibilityStatus()
        CodexSendLog.append(
            "companion send finished outcome=\(String(describing: outcome)) target=\(targetTitle) action=\(sentActionTitle) promptChars=\(sentPrompt.count)"
        )

        guard outcome.succeeded else {
            switch outcome {
            case .sharedDaemonUnavailable:
                status = "ChatGPT's local task connection is unavailable. Your draft is still here."
            case .threadNotLoaded:
                status = "ChatGPT could not load \(targetTitle) in the background. Your draft is still here."
            case .noActiveTurn:
                status = "\(targetTitle) is not running. Use Reply instead of Steer."
            case .timedOut:
                status = "Delivery could not be confirmed. Check the task before retrying; your draft is still here."
            case .failed:
                status = "Native Codex \(action.logName) failed. Your draft is still here."
            case .sent:
                break
            }
            selectedState = .failed
            codexComposerFeedback = CodexComposerFeedback(text: status, isError: true)
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            startProcessRefreshTimer()
            return
        }

        if pendingCodexSendIdentity == CodexSendIdentity(
            threadID: target.threadID,
            action: action,
            prompt: sentPrompt
        ) {
            pendingCodexSendIdentity = nil
            pendingCodexClientMessageID = nil
        }

        let composerStillMatches = activeProcessTarget?.processID == target.processID
            && activeProcessTarget?.threadID == target.threadID
            && activeProcessTarget?.action == target.action
            && prompt.trimmingCharacters(in: .whitespacesAndNewlines) == sentPrompt
        status = "\(sentActionTitle) to \(targetTitle)."
        codexComposerFeedback = nil
        selectedState = .running
        historyStore.add(prompt: sentPrompt, destination: .codex)
        processStore.markFailureHandled(
            processID: target.processID,
            threadID: target.threadID
        )
        if composerStillMatches {
            clearProcessTarget()
            prompt = ""
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            processStore.refresh()
            startProcessRefreshTimer()
        } else {
            status += " Your newer draft is still here."
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            startProcessRefreshTimer()
        }
    }

    func setAnimation(_ state: PetAnimationState) {
        clearDirectionalLook()
        petInteractionState = nil
        selectedState = state
    }

    @discardableResult
    func setRoamingMotion(dx: CGFloat, dy: CGFloat) -> Bool {
        guard CompanionPresentationPolicy.acceptsRoamingMotion(
            isPetHovered: isPetHovered,
            isPetDragging: isPetDragging,
            hasInteractionAnimation: petInteractionState != nil
        ) else {
            return false
        }
        clearDirectionalLook()
        let state = horizontalRunState(dx: dx, fallback: lastRoamingRunState)
        lastRoamingRunState = state
        setRoamingState(state)
        return true
    }

    func setPetHovering(_ isHovering: Bool) {
        clearDirectionalLook()
        isPetHovered = isHovering
        isPetPointerHovered = isHovering
        guard !isPetDragging else { return }
        if isHovering {
            setPetInteractionState(.jumping)
            setRoamingState(.jumping)
        } else {
            if let attentionMessage {
                setPetInteractionState(Self.animationState(for: attentionMessage.kind))
            } else {
                clearPetInteractionState()
                setRoamingState(.running)
            }
        }
    }

    func setProcessHovering(_ processID: String, isHovering: Bool) {
        if isHovering {
            hoveredProcessID = processID
        } else if hoveredProcessID == processID {
            hoveredProcessID = nil
        }
    }

    func beginPetDrag() {
        clearDirectionalLook()
        isPetDragging = true
        setPetInteractionState(lastManualRunState)
    }

    func updatePetDrag(dx: CGFloat, dy: CGFloat) {
        clearDirectionalLook()
        isPetDragging = true
        let state = horizontalRunState(dx: dx, fallback: lastManualRunState)
        lastManualRunState = state
        setPetInteractionState(state)
        setRoamingState(state)
    }

    func endPetDrag() {
        isPetDragging = false
        if isPetHovered {
            setPetInteractionState(.jumping)
            setRoamingState(.jumping)
        } else if let attentionMessage {
            setPetInteractionState(Self.animationState(for: attentionMessage.kind))
        } else {
            clearPetInteractionState()
            setRoamingState(.running)
        }
    }

    func setRoamingIdle() {
        guard !isPetHovered, !isPetDragging else { return }
        setRoamingState(.idle)
    }

    func updateDirectionalLook(pointer: CGPoint, petFrame: CGRect) {
        guard !isQuickBarOpen,
              !isPetHovered,
              !isPetDragging,
              petInteractionState == nil,
              roamingState == .idle,
              let lookFrames = petStore.selectedPet?.directionalLookFrames
        else {
            clearDirectionalLook()
            return
        }

        let nextFrame = PetDirectionalLookFrame.resolve(
            pointer: pointer,
            petFrame: petFrame,
            startRow: lookFrames.startRow
        )
        guard directionalLookFrame != nextFrame else { return }
        directionalLookFrame = nextFrame
    }

    func clearDirectionalLook() {
        guard directionalLookFrame != nil else { return }
        directionalLookFrame = nil
    }

    func showCodexProcesses() {
        routeMode = .codex
        refreshCodexAccessibilityStatus()
        isCodexProcessTrayVisible = true
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.close()
        syncAnimationWithProcesses(processStore.items)
        processStore.refreshGoalsIfStale()
        refreshCodexProcessesAfterTraySettles()
        startProcessRefreshTimer()
    }

    func showChatGPT() {
        if isCodexOnlyMode, selectedChatGPTDeliveryMode == .appHandoff {
            selectedChatGPTDeliveryMode = .onDevice
        }
        routeMode = .chatGPT
        isCodexProcessTrayVisible = false
        isChatGPTModelPickerExpanded = false
        stopProcessRefreshTimer()
        clearProcessTarget()
        isQuickBarOpen = true
        if selectedChatGPTDeliveryMode == .appHandoff {
            chatGPTQuickBar.open(model: selectedChatGPTModel)
        }
    }

    func useChatGPTAPI() {
        selectedChatGPTDeliveryMode = .openAIAPI
        routeMode = .chatGPT
        isQuickBarOpen = true
        isCodexProcessTrayVisible = false
        stopProcessRefreshTimer()
        isChatGPTModelPickerExpanded = false
        chatGPTQuickBar.close()
        status = "Using ChatGPT API mode."
    }

    func useOnDeviceChat() {
        selectedChatGPTDeliveryMode = .onDevice
        routeMode = .chatGPT
        isQuickBarOpen = true
        isCodexProcessTrayVisible = false
        stopProcessRefreshTimer()
        isChatGPTModelPickerExpanded = false
        chatGPTQuickBar.close()
        status = "Using the on-device Apple model."
    }

    func useChatGPTAppHandoff() {
        guard !isCodexOnlyMode else {
            showCodexProcesses()
            return
        }
        selectedChatGPTDeliveryMode = .appHandoff
        routeMode = .chatGPT
        isQuickBarOpen = true
        isCodexProcessTrayVisible = false
        stopProcessRefreshTimer()
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.open(model: selectedChatGPTModel)
        status = "Using ChatGPT app quick bar."
    }

    func hideCodexProcesses() {
        isCodexProcessTrayVisible = false
        hoveredProcessID = nil
        stopProcessRefreshTimer()
        isChatGPTModelPickerExpanded = false
        clearProcessTarget()
    }

    func openGoalControls(for item: CodexProcessItem) {
        guard let control = CodexGoalControlState(item: item) else {
            activeGoalControl = nil
            goalControlError = "This process does not have a controllable goal."
            return
        }
        activeGoalControl = control
        goalControlError = nil
    }

    func dismissGoalControls() {
        activeGoalControl = nil
        goalControlError = nil
    }

    func beginGoalEditing() {
        guard var control = activeGoalControl, control.canEdit else { return }
        control.isEditing = true
        activeGoalControl = control
        goalControlError = nil
    }

    func cancelGoalEditing() {
        guard var control = activeGoalControl else { return }
        control.draftObjective = control.originalObjective
        control.isEditing = false
        activeGoalControl = control
        goalControlError = nil
    }

    func updateGoalDraft(_ objective: String) {
        guard var control = activeGoalControl else { return }
        control.draftObjective = objective
        activeGoalControl = control
    }

    func saveGoalEdit() {
        guard let control = activeGoalControl, control.canEdit, !isUpdatingGoal else { return }
        let objective = control.draftObjective.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !objective.isEmpty else {
            goalControlError = "Enter a goal objective before saving."
            return
        }

        isUpdatingGoal = true
        goalControlError = nil
        let mutation = CodexGoalMutation.editObjective(objective)
        Task { @MainActor in
            do {
                let goal = try await processStore.setGoal(
                    threadID: control.threadID,
                    objective: mutation.objective,
                    status: mutation.status,
                    tokenBudget: mutation.tokenBudget
                )
                guard activeGoalControl?.threadID == goal.threadID else {
                    isUpdatingGoal = false
                    return
                }
                var updated = control
                updated.originalObjective = goal.objective
                updated.draftObjective = goal.objective
                updated.status = goal.status
                updated.tokenBudget = goal.tokenBudget
                updated.isEditing = false
                activeGoalControl = updated
                status = "Updated goal for \(control.taskTitle)."
            } catch {
                goalControlError = error.localizedDescription
            }
            isUpdatingGoal = false
        }
    }

    func resumeGoal() {
        guard let control = activeGoalControl, control.canResume, !isUpdatingGoal else { return }
        isUpdatingGoal = true
        goalControlError = nil
        Task { @MainActor in
            do {
                let goal = try await processStore.setGoal(
                    threadID: control.threadID,
                    objective: nil,
                    status: .active,
                    tokenBudget: nil
                )
                guard activeGoalControl?.threadID == goal.threadID else {
                    isUpdatingGoal = false
                    return
                }
                var updated = control
                updated.originalObjective = goal.objective
                updated.draftObjective = goal.objective
                updated.status = goal.status
                updated.tokenBudget = goal.tokenBudget
                updated.isEditing = false
                activeGoalControl = updated
                status = "Resumed goal for \(control.taskTitle)."
            } catch {
                goalControlError = error.localizedDescription
            }
            isUpdatingGoal = false
        }
    }

    func refreshCodexAccessibilityStatus() {
        isCodexAccessibilityTrusted = CodexVisibleReplySender.isAccessibilityTrusted
    }

    func requestCodexAccessibilityPermission() {
        CodexVisibleReplySender.requestAccessibilityTrustNow()
        CodexVisibleReplySender.openAccessibilitySettings()
        refreshCodexAccessibilityStatus()
        status = isCodexAccessibilityTrusted
            ? "Codex Reply fallback is allowed."
            : "Enable Codex Companion in Accessibility, then send again."
    }

    func dismissChatGPTMenuResponse() {
        chatGPTMenuResponse = nil
    }

    func saveOpenAIAPIKey() {
        do {
            try openAIAPIKeyStore.save(openAIAPIKeyInput)
            openAIAPIKeyInput = ""
            hasOpenAIAPIKey = openAIAPIKeyStore.hasKey
            openAIAPIKeyStatus = hasOpenAIAPIKey ? "OpenAI API key saved for this Mac." : "No OpenAI API key saved."
        } catch {
            openAIAPIKeyStatus = error.localizedDescription
        }
    }

    func clearOpenAIAPIKey() {
        openAIAPIKeyStore.clear()
        openAIAPIKeyInput = ""
        hasOpenAIAPIKey = false
        openAIAPIKeyStatus = "OpenAI API key removed."
    }

    func reply(to item: CodexProcessItem) {
        guard let target = CodexProcessTarget(item: item, action: .reply) else {
            CodexSendLog.append("companion target failed action=reply title=\(item.title) thread=\(item.threadID ?? "nil")")
            clearProcessTarget()
            prompt = ""
            status = "No reply target for \(item.title)."
            routeMode = .codex
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            selectedState = .waiting
            return
        }

        routeMode = .codex
        isQuickBarOpen = true
        isCodexProcessTrayVisible = true
        selectedState = Self.animationState(for: item.status)
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.close()
        activeProcessTarget = target
        prompt = ""
        codexComposerFeedback = nil
        status = target.promptTitle
        CodexSendLog.append("companion target selected action=reply thread=\(target.threadID) title=\(target.title)")

        processStore.refreshIfStale()
        startProcessRefreshTimer()
    }

    func steer(_ item: CodexProcessItem) {
        guard let target = CodexProcessTarget(item: item, action: .steer) else {
            CodexSendLog.append("companion target failed action=steer title=\(item.title) thread=\(item.threadID ?? "nil")")
            clearProcessTarget()
            prompt = ""
            status = "No steer target for \(item.title)."
            routeMode = .codex
            isQuickBarOpen = true
            isCodexProcessTrayVisible = true
            selectedState = .waiting
            return
        }

        routeMode = .codex
        isQuickBarOpen = true
        isCodexProcessTrayVisible = true
        selectedState = Self.animationState(for: item.status)
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.close()
        activeProcessTarget = target
        prompt = ""
        codexComposerFeedback = nil
        status = "Steering \(item.title)"
        CodexSendLog.append("companion target selected action=steer thread=\(target.threadID) title=\(target.title)")

        processStore.refreshIfStale()
        startProcessRefreshTimer()
    }

    func approveOnce(_ item: CodexProcessItem) {
        submitApproval(item, decision: .approveOnce)
    }

    func approveSimilarCommands(_ item: CodexProcessItem) {
        submitApproval(item, decision: .approveSimilarCommands)
    }

    func tellCodexSomethingElse(_ item: CodexProcessItem) {
        guard
            item.runtimeStatus == .waitingOnApproval,
            let target = CodexProcessTarget(item: item, action: .approvalFeedback)
        else { return }

        routeMode = .codex
        isQuickBarOpen = true
        isCodexProcessTrayVisible = true
        selectedState = .waiting
        isChatGPTModelPickerExpanded = false
        chatGPTMenuResponse = nil
        chatGPTQuickBar.close()
        activeProcessTarget = target
        prompt = ""
        codexComposerFeedback = nil
        status = target.promptTitle
        CodexSendLog.append(
            "companion target selected action=approval_feedback thread=\(target.threadID) title=\(target.title)"
        )
        processStore.refreshIfStale()
        startProcessRefreshTimer()
    }

    func review(_ item: CodexProcessItem) {
        approveOnce(item)
    }

    private func submitApproval(
        _ item: CodexProcessItem,
        decision: CodexApprovalDecision
    ) {
        guard
            item.runtimeStatus == .waitingOnApproval,
            let threadID = item.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
            !threadID.isEmpty,
            approvingThreadID == nil
        else { return }

        approvingThreadID = threadID
        status = decision == .approveSimilarCommands
            ? "Approving similar commands for \(item.title)..."
            : "Approving request for \(item.title)..."
        selectedState = .waiting
        CodexSendLog.append(
            "companion approval tapped thread=\(threadID) title=\(item.title) decision=\(String(describing: decision))"
        )
        let submitter = codexApprovalSubmitter
        Task { [weak self] in
            let outcome = await Task.detached(priority: .userInitiated) {
                await submitter(threadID, decision)
            }.value
            guard let self else { return }
            self.approvingThreadID = nil
            switch outcome {
            case .approved:
                self.status = decision == .approveSimilarCommands
                    ? "Approved similar commands for \(item.title)."
                    : "Approved request for \(item.title)."
                self.selectedState = .running
                self.processStore.refresh()
            case .declined:
                self.status = "Codex treated the approval as declined."
                self.selectedState = .failed
            case .requestNotFound:
                self.status = "The approval details are not available yet. Keep Companion open for the next request."
                self.selectedState = .failed
            case .sharedDaemonUnavailable:
                self.status = "ChatGPT's native approval connection is unavailable. Refresh the process, then retry."
                self.selectedState = .failed
            case .timedOut:
                self.status = "Approval could not be confirmed. Refresh the process before retrying."
                self.selectedState = .failed
            case .failed:
                self.status = "Codex did not accept the approval."
                self.selectedState = .failed
            }
            self.startProcessRefreshTimer()
        }
    }

    func clearProcessTarget() {
        activeProcessTarget = nil
        prompt = ""
        codexComposerFeedback = nil
    }

    func cancelProcessTarget() {
        let canceledTitle = activeProcessTarget?.title
        activeCodexSendRequestID = nil
        activeApprovalFeedbackTask?.cancel()
        activeApprovalFeedbackTask = nil
        activeCodexSendTask?.cancel()
        activeCodexSendTask = nil
        activeCodexSendTimeoutTask?.cancel()
        activeCodexSendTimeoutTask = nil
        pendingCodexSendIdentity = nil
        pendingCodexClientMessageID = nil
        isCodexSending = false
        clearProcessTarget()
        if let canceledTitle {
            status = "Canceled pending message to \(canceledTitle)."
        }
    }

    func reconcileProcessTarget(with items: [CodexProcessItem]) {
        guard let target = activeProcessTarget else { return }
        guard let item = items.first(where: { $0.id == target.processID }),
              let refreshedTarget = CodexProcessTarget(item: item, action: target.action)
        else {
            if !isCodexSending,
               prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            {
                clearProcessTarget()
            }
            return
        }
        if refreshedTarget != target {
            activeProcessTarget = refreshedTarget
        }
    }

    private func setRoamingState(_ state: PetAnimationState) {
        guard roamingState != state else { return }
        roamingState = state
    }

    private func setPetInteractionState(_ state: PetAnimationState) {
        guard petInteractionState != state else { return }
        petInteractionState = state
    }

    private func clearPetInteractionState() {
        guard petInteractionState != nil else { return }
        petInteractionState = nil
    }

    private func sendChatGPTMenuPrompt(_ trimmedPrompt: String) {
        isQuickBarOpen = true
        isCodexProcessTrayVisible = false
        isChatGPTModelPickerExpanded = false
        clearProcessTarget()

        if selectedChatGPTDeliveryMode == .appHandoff {
            chatGPTQuickBar.open(prompt: trimmedPrompt, model: selectedChatGPTModel, submit: !trimmedPrompt.isEmpty)
            if trimmedPrompt.isEmpty {
                status = "Opened ChatGPT app quick bar."
            } else {
                status = "Sent prompt to ChatGPT app quick bar."
                historyStore.add(prompt: trimmedPrompt, destination: .chatGPT)
                prompt = ""
            }
            selectedState = .waving
            chatGPTMenuResponse = nil
            isChatGPTResponding = false
            return
        }

        if selectedChatGPTDeliveryMode == .onDevice {
            sendOnDeviceChatPrompt(trimmedPrompt)
            return
        }

        guard !trimmedPrompt.isEmpty else {
            status = "Ask inside the Companion menu with \(selectedChatGPTModel.title)."
            chatGPTMenuResponse = nil
            return
        }

        guard let apiKey = openAIAPIKeyStore.load() else {
            status = "Add an OpenAI API key in Settings to answer inside Companion."
            selectedState = .waiting
            chatGPTMenuResponse = ChatGPTMenuResponse(
                model: selectedChatGPTModel,
                prompt: trimmedPrompt,
                message: "I did not open ChatGPT. To answer here, paste your OpenAI API key into Codex Companion Settings. ChatGPT Pro and API billing are separate.",
                usageSummary: nil
            )
            return
        }

        let model = selectedChatGPTModel
        prompt = ""
        status = "Asking \(model.title)..."
        selectedState = .waving
        isChatGPTResponding = true
        chatGPTMenuResponse = ChatGPTMenuResponse(
            model: model,
            prompt: trimmedPrompt,
            message: "Thinking...",
            usageSummary: nil
        )
        historyStore.add(prompt: trimmedPrompt, destination: .chatGPT)

        Task {
            do {
                let result = try await openAIChatService.send(prompt: trimmedPrompt, model: model, apiKey: apiKey)
                isChatGPTResponding = false
                selectedState = .review
                status = "Answered with \(model.title)."
                chatGPTMenuResponse = ChatGPTMenuResponse(
                    model: model,
                    prompt: trimmedPrompt,
                    message: result.text,
                    usageSummary: usageSummary(for: result)
                )
            } catch {
                isChatGPTResponding = false
                selectedState = .failed
                status = "OpenAI request failed."
                chatGPTMenuResponse = ChatGPTMenuResponse(
                    model: model,
                    prompt: trimmedPrompt,
                    message: error.localizedDescription,
                    usageSummary: nil
                )
            }
        }
    }

    private func sendOnDeviceChatPrompt(_ trimmedPrompt: String) {
        guard !trimmedPrompt.isEmpty else {
            status = "Ask inside the Companion menu with the on-device model."
            chatGPTMenuResponse = nil
            return
        }

        let model = selectedChatGPTModel
        let chatService = onDeviceChatService
        prompt = ""
        status = "Thinking on device..."
        selectedState = .waiting
        isChatGPTResponding = true
        chatGPTMenuResponse = ChatGPTMenuResponse(
            model: model,
            sourceTitle: "On-device",
            prompt: trimmedPrompt,
            message: "Thinking...",
            usageSummary: nil
        )
        historyStore.add(prompt: trimmedPrompt, destination: .chatGPT)

        Task { [weak self] in
            do {
                let message = try await chatService.send(prompt: trimmedPrompt)
                guard let self else { return }
                self.isChatGPTResponding = false
                self.selectedState = .review
                self.status = "Answered on device."
                self.chatGPTMenuResponse = ChatGPTMenuResponse(
                    model: model,
                    sourceTitle: "On-device",
                    prompt: trimmedPrompt,
                    message: message,
                    usageSummary: "Private on-device response"
                )
            } catch {
                guard let self else { return }
                self.isChatGPTResponding = false
                self.selectedState = .failed
                self.status = "On-device response failed."
                self.chatGPTMenuResponse = ChatGPTMenuResponse(
                    model: model,
                    sourceTitle: "On-device",
                    prompt: trimmedPrompt,
                    message: error.localizedDescription,
                    usageSummary: nil
                )
            }
        }
    }

    private func usageSummary(for result: OpenAIChatResult) -> String? {
        guard let inputTokens = result.inputTokens, let outputTokens = result.outputTokens else {
            return nil
        }
        return "\(inputTokens) in · \(outputTokens) out"
    }

    private func horizontalRunState(dx: CGFloat, fallback: PetAnimationState) -> PetAnimationState {
        let threshold: CGFloat = 0.12
        if dx < -threshold {
            return .runningLeft
        }
        if dx > threshold {
            return .runningRight
        }
        return fallback == .runningLeft ? .runningLeft : .runningRight
    }

    func toggleQuickBar() {
        isQuickBarOpen.toggle()
        if !isQuickBarOpen {
            stopProcessRefreshTimer()
            chatGPTQuickBar.close()
        } else {
            showPet()
            if routeMode == .chatGPT {
                showChatGPT()
            } else if isCodexProcessTrayVisible {
                startProcessRefreshTimer()
            } else {
                showCodexProcesses()
            }
        }
    }

    func showQuickBar() {
        showPet()
        isQuickBarOpen = true
        if routeMode == .chatGPT {
            showChatGPT()
        } else {
            showCodexProcesses()
        }
    }

    func hideQuickBar() {
        isQuickBarOpen = false
        hoveredProcessID = nil
        stopProcessRefreshTimer()
        chatGPTQuickBar.close()
    }

    func showPet() {
        isPetVisible = true
    }

    func hidePet() {
        cancelPendingAttentionReaction()
        hideQuickBar()
        isPetHovered = false
        isPetPointerHovered = false
        PetWindowRoamer.shared.setInteractionHold(false)
        clearPetInteractionState()
        setRoamingState(.idle)
        isPetVisible = false
    }

    func openAttentionMessage() {
        guard attentionMessage != nil else { return }
        let destination = CompanionPresentationPolicy.attentionDestination
        routeMode = destination.routeMode
        isQuickBarOpen = destination.isQuickBarOpen
        isCodexProcessTrayVisible = destination.isCodexProcessTrayVisible
        showCodexProcesses()
        dismissAttentionMessage()
    }

    func dismissAttentionMessage() {
        cancelPendingAttentionReaction()
        attentionDismissTask?.cancel()
        attentionMessage = nil
        if petInteractionState != .goalComplete, !isPetHovered, !isPetDragging {
            clearPetInteractionState()
        }
    }

    func presentAttentionMessage(_ message: PetAttentionMessage) {
        latestAttentionHighlight = PetAttentionHighlight(message: message)
        guard isPetVisible, !isQuickBarOpen else { return }
        attentionReactionTask?.cancel()
        let requestToken = UUID()
        attentionReactionRequestToken = requestToken
        let petReactionCoordinator = petReactionCoordinator

        attentionReactionTask = Task { @MainActor [weak self] in
            let title = await petReactionCoordinator.reaction(for: message.reactionContext)
            guard !Task.isCancelled,
                  let self,
                  self.attentionReactionRequestToken == requestToken,
                  self.isPetVisible,
                  !self.isQuickBarOpen
            else {
                return
            }

            self.displayAttentionMessage(
                message.replacingTitle(title),
                pendingPresentedTitle: title
            )

            if self.attentionReactionRequestToken == requestToken {
                self.attentionReactionTask = nil
            }
        }
    }

    func attentionMessageDidBecomeVisible(_ messageID: UUID) {
        guard attentionMessage?.id == messageID,
              let pending = pendingPresentedReaction,
              pending.messageID == messageID
        else {
            return
        }
        pendingPresentedReaction = nil
        let petReactionCoordinator = petReactionCoordinator
        Task {
            await petReactionCoordinator.recordPresented(pending.title)
        }
    }

    private func displayAttentionMessage(
        _ message: PetAttentionMessage,
        pendingPresentedTitle: String? = nil
    ) {
        attentionDismissTask?.cancel()
        pendingPresentedReaction = pendingPresentedTitle.map {
            (messageID: message.id, title: $0)
        }
        attentionMessage = message
        if petInteractionState != .goalComplete, !isPetDragging {
            setPetInteractionState(Self.animationState(for: message.kind))
        }

        let messageID = message.id
        attentionDismissTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(9))
            guard !Task.isCancelled, self.attentionMessage?.id == messageID else { return }
            if self.pendingPresentedReaction?.messageID == messageID {
                self.pendingPresentedReaction = nil
            }
            self.attentionMessage = nil
            if self.petInteractionState != .goalComplete, !self.isPetHovered, !self.isPetDragging {
                self.clearPetInteractionState()
            }
        }
    }

    private func cancelPendingAttentionReaction() {
        attentionReactionTask?.cancel()
        attentionReactionTask = nil
        attentionReactionRequestToken = UUID()
        pendingPresentedReaction = nil
    }

    private func handleProcessTransitions(_ items: [CodexProcessItem]) {
        let nextSnapshots = Dictionary(uniqueKeysWithValues: items.map {
            ($0.id, PetProcessSnapshot(item: $0))
        })

        guard hasSeededProcessSnapshots else {
            hasSeededProcessSnapshots = true
            processSnapshots = nextSnapshots
            if let completedGoal = firstUnseenCompletedGoal(in: items) {
                triggerGoalConfetti(for: completedGoal)
            }
            return
        }

        for item in items where item.hasReachedGoal {
            guard item.kind != .notice else { continue }
            let completionKey = goalCompletionKey(for: item)
            guard seenGoalCompletionKeys.contains(completionKey) == false else { continue }
            let previous = processSnapshots[item.id]
            let goalWasIncomplete = previous?.goalStatus != .complete || previous?.goalID != item.goalID
            let processJustCompleted = previous?.status == .running && item.status == .completed
            if goalWasIncomplete || processJustCompleted {
                triggerGoalConfetti(for: item)
                break
            }
        }

        let attention = items
            .compactMap { item -> PetAttentionMessage? in
                if let previous = processSnapshots[item.id] {
                    return PetAttentionMessage.transition(previous: previous, current: item)
                }
                return PetAttentionMessage.appearance(current: item)
            }
            .max { $0.kind.rawValue < $1.kind.rawValue }
        if let attention {
            presentAttentionMessage(attention)
        }

        processSnapshots = nextSnapshots
    }

    private func firstUnseenCompletedGoal(in items: [CodexProcessItem]) -> CodexProcessItem? {
        items.first { item in
            guard item.kind != .notice, item.hasReachedGoal else { return false }
            return seenGoalCompletionKeys.contains(goalCompletionKey(for: item)) == false
        }
    }

    private func triggerGoalConfetti(for item: CodexProcessItem) {
        markGoalCompletionSeen(for: item)
        lastReachedGoalTitle = item.title
        goalConfettiTrigger += 1
        let trigger = goalConfettiTrigger
        goalAnimationTask?.cancel()
        petInteractionState = .goalComplete
        selectedState = .goalComplete
        status = "Goal reached: \(item.title)"

        goalAnimationTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 6_200_000_000)
            guard !Task.isCancelled, self.goalConfettiTrigger == trigger else { return }
            if self.petInteractionState == .goalComplete {
                self.petInteractionState = nil
            }
            if self.selectedState == .goalComplete {
                self.selectedState = .review
            }
        }
    }

    private func markGoalCompletionSeen(for item: CodexProcessItem) {
        seenGoalCompletionKeys.insert(goalCompletionKey(for: item))
        let recentKeys = Array(seenGoalCompletionKeys).sorted().suffix(80)
        seenGoalCompletionKeys = Set(recentKeys)
        UserDefaults.standard.set(Array(recentKeys), forKey: Self.seenGoalCompletionKeysKey)
    }

    private func goalCompletionKey(for item: CodexProcessItem) -> String {
        let goalID = item.goalID?.trimmingCharacters(in: .whitespacesAndNewlines)
        return [
            Self.goalCelebrationVersion,
            item.id,
            goalID?.isEmpty == false ? goalID! : "goal",
        ].joined(separator: "|")
    }

    private func startProcessRefreshTimer() {
        guard processRefreshTimer == nil else { return }
        let timer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard self?.isCodexProcessTrayVisible == true else {
                    self?.stopProcessRefreshTimer()
                    return
                }
                self?.processStore.refresh()
                self?.processStore.refreshGoalsIfStale()
            }
        }
        timer.tolerance = 8.0
        RunLoop.main.add(timer, forMode: .common)
        processRefreshTimer = timer
    }

    private func refreshCodexProcessesAfterTraySettles() {
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(140))
            guard isCodexProcessTrayVisible else { return }
            processStore.refreshIfStale()
            processStore.refreshGoalsIfStale()
        }
    }

    private func stopProcessRefreshTimer() {
        processRefreshTimer?.invalidate()
        processRefreshTimer = nil
    }

    private func startPassiveProcessRefreshTimer() {
        guard passiveProcessRefreshTimer == nil else { return }
        let timer = Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, !self.isCodexProcessTrayVisible else { return }
                self.processStore.refresh()
            }
        }
        timer.tolerance = 5
        RunLoop.main.add(timer, forMode: .common)
        passiveProcessRefreshTimer = timer
    }

    private func syncAnimationWithProcesses(_ items: [CodexProcessItem]) {
        guard isCodexProcessTrayVisible else { return }
        if items.contains(where: { $0.status == .failed }) {
            selectedState = Self.animationState(for: .failed)
        } else if items.contains(where: { $0.status == .running }) {
            selectedState = Self.animationState(for: .running)
        } else if items.contains(where: { $0.status == .completed }) {
            selectedState = Self.animationState(for: .completed)
        } else {
            selectedState = Self.animationState(for: .waiting)
        }
    }

    nonisolated static func animationState(
        for status: CodexProcessItem.Status
    ) -> PetAnimationState {
        switch status {
        case .running:
            return .thinking
        case .completed:
            return .review
        case .failed:
            return .failed
        case .waiting:
            return .waiting
        }
    }

    nonisolated static func animationState(
        for kind: PetAttentionMessage.Kind
    ) -> PetAnimationState {
        switch kind {
        case .response:
            return .talking
        case .attention:
            return .waiting
        case .goal:
            return .running
        case .completion:
            return .goalComplete
        case .failure:
            return .failed
        }
    }

    private static let animationSpeedKey = "animationSpeedScale"
    private static let animationSpeedTimingVersionKey = "animationSpeedTimingVersion"
    private static let currentAnimationSpeedTimingVersion = 2
    private static let riggedShadowRendererKey = "useRiggedShadowRenderer"
    private static let chatGPTModelKey = "selectedChatGPTModel"
    private static let chatGPTDeliveryModeKey = "selectedChatGPTDeliveryMode"
    private static let seenGoalCompletionKeysKey = "seenGoalCompletionKeys"
    private static let goalCelebrationVersion = "goal-celebration-v2"
}

struct CodexProcessTarget: Equatable, Sendable {
    enum Action: Equatable, Sendable {
        case reply
        case steer
        case approvalFeedback
    }

    var processID: String
    var threadID: String
    var cwd: String?
    var activeTurnID: String?
    var title: String
    var action: Action
    var animationState: PetAnimationState

    init?(item: CodexProcessItem, action: Action) {
        guard item.kind != .notice else { return nil }
        let threadID = item.threadID?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard !threadID.isEmpty else { return nil }

        processID = item.id
        self.threadID = threadID
        cwd = item.cwd
        activeTurnID = item.activeTurnID
        title = item.title
        self.action = action

        animationState = CompanionAppModel.animationState(for: item.status)
    }

    var promptTitle: String {
        switch action {
        case .reply:
            return "Reply to \(title)"
        case .steer:
            return "Steer \(title)"
        case .approvalFeedback:
            return "Tell Codex what to do instead"
        }
    }

    var sentActionTitle: String {
        switch action {
        case .reply:
            return "Sent reply"
        case .steer:
            return "Sent steer"
        case .approvalFeedback:
            return "Sent guidance"
        }
    }
}

private struct CodexSendIdentity: Equatable, Sendable {
    var threadID: String
    var action: CodexSendAction
    var prompt: String
}

struct CodexComposerFeedback: Equatable, Sendable {
    var text: String
    var isError: Bool
}

private extension CodexProcessTarget.Action {
    var logName: String {
        switch self {
        case .reply:
            return "reply"
        case .steer:
            return "steer"
        case .approvalFeedback:
            return "approval_feedback"
        }
    }

    var codexSendAction: CodexSendAction {
        switch self {
        case .reply:
            return .reply
        case .steer:
            return .steer
        case .approvalFeedback:
            return .reply
        }
    }

    var systemName: String {
        switch self {
        case .reply:
            return "arrowshape.turn.up.left"
        case .steer:
            return "arrow.turn.down.right"
        case .approvalFeedback:
            return "text.bubble"
        }
    }
}
