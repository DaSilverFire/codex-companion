import AppKit
import SwiftUI

struct QuickBarTrayView: View {
    @ObservedObject var model: CompanionAppModel
    @FocusState private var promptFocused: Bool
    @Namespace private var trayGlassNamespace
    @State private var isTrayMaterialized = false
    @State private var isChatModelPickerPresented = false

    var body: some View {
        Group {
            if isTrayMaterialized {
                trayContent
                    .transition(.opacity.combined(with: .scale(scale: 0.965, anchor: .bottom)))
            }
        }
            .foregroundStyle(TrayColors.textPrimary)
            .padding(.horizontal, 4)
            .padding(.top, model.shouldShowChatGPTMenuResponse ? 22 : 14)
            .padding(.bottom, 14)
            .frame(maxWidth: .infinity)
            .overlay {
                GoalConfettiView(trigger: model.goalConfettiTrigger)
                    .allowsHitTesting(false)
            }
            .onAppear {
                DispatchQueue.main.async {
                    withAnimation(TrayAnimation.materialize) {
                        isTrayMaterialized = true
                    }
                }
                DispatchQueue.main.async {
                    promptFocused = model.shouldShowCompanionPromptField
                }
                model.refreshCodexAccessibilityStatus()
                model.rateLimitStore.refresh()
                model.processStore.refresh()
            }
            .onChange(of: model.isQuickBarOpen) { _, isOpen in
                withAnimation(TrayAnimation.materialize) {
                    isTrayMaterialized = isOpen
                }
            }
    }

    @ViewBuilder
    private var trayContent: some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: 6) {
                trayStack
            }
        } else {
            trayStack
        }
    }

    private var trayStack: some View {
        VStack(alignment: .leading, spacing: trayStackSpacing) {
            if model.isCodexProcessTrayVisible {
                codexProcesses
            }

            if model.shouldShowComposerSurface {
                composer
            }
        }
        .frame(maxHeight: .infinity, alignment: .bottom)
    }

    private var trayStackSpacing: CGFloat {
        8
    }

    private var codexProcesses: some View {
        VStack(alignment: .leading, spacing: 6) {
            if model.processStore.isLoading && model.processStore.items.isEmpty {
                ProcessLoadingCard()
            }

            ScrollViewReader { proxy in
                ScrollView {
                    processListContent
                }
                .scrollDisabled(!processListNeedsScrolling)
                .scrollIndicators(processListNeedsScrolling ? .automatic : .never)
                .onAppear {
                    scrollToRelevantProcess(using: proxy, animated: false)
                }
                .onChange(of: model.activeProcessTarget?.processID) { _, _ in
                    scrollToRelevantProcess(using: proxy, animated: true)
                }
                .onChange(of: model.latestAttentionHighlight?.processID) { _, _ in
                    scrollToRelevantProcess(using: proxy, animated: true)
                }
            }
            .frame(height: processListHeight)

            if model.shouldShowCodexAccessibilityNotice, model.activeProcessTarget == nil {
                CodexAccessibilityNotice {
                    model.requestCodexAccessibilityPermission()
                }
                .frame(height: 38)
            }
        }
        .padding(8)
        .traySurface(cornerRadius: 28)
        .shadow(color: .black.opacity(0.25), radius: 14, y: 8)
        .transition(.asymmetric(
            insertion: .opacity.combined(with: .scale(scale: 0.985, anchor: .bottom)),
            removal: .opacity.combined(with: .scale(scale: 0.99, anchor: .bottom))
        ))
    }

    private var processListHeight: CGFloat {
        PetWindowMetrics.processListHeight(
            for: model.processStore.items,
            isLoading: model.processStore.isLoading,
            prompt: model.prompt,
            hasProcessTarget: model.activeProcessTarget != nil,
            showsAccessibilityNotice: model.shouldShowCodexAccessibilityNotice,
            targetProcessID: model.activeProcessTarget?.processID,
            expandedProcessID: model.hoveredProcessID,
            showsCodexSendFeedback: model.codexComposerFeedback != nil
        )
    }

    private var processListNeedsScrolling: Bool {
        PetWindowMetrics.processListNeedsScrolling(
            for: model.processStore.items,
            isLoading: model.processStore.isLoading,
            prompt: model.prompt,
            hasProcessTarget: model.activeProcessTarget != nil,
            showsAccessibilityNotice: model.shouldShowCodexAccessibilityNotice,
            targetProcessID: model.activeProcessTarget?.processID,
            expandedProcessID: model.hoveredProcessID,
            showsCodexSendFeedback: model.codexComposerFeedback != nil
        )
    }

    private var processListContent: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(PetWindowMetrics.visibleProcessItems(
                from: model.processStore.items,
                targetProcessID: model.activeProcessTarget?.processID
            )) { item in
                let isTargeted = model.activeProcessTarget?.processID == item.id
                let inlineComposerHeight = isTargeted
                    ? PetWindowMetrics.inlineProcessComposerHeight(
                        prompt: model.prompt,
                        showsAccessibilityNotice: model.shouldShowCodexAccessibilityNotice,
                        showsSendFeedback: model.codexComposerFeedback != nil
                    )
                    : 0
                ProcessCard(
                    item: item,
                    isHovering: model.hoveredProcessID == item.id,
                    attentionAccent: model.latestAttentionHighlight?.processID == item.id
                        ? model.latestAttentionHighlight?.accent
                        : nil,
                    hoverChanged: { isHovering in
                        model.setProcessHovering(item.id, isHovering: isHovering)
                    },
                    inlineComposer: isTargeted
                        ? AnyView(processInlineComposer(for: item))
                        : nil,
                    inlineComposerHeight: inlineComposerHeight,
                    isApproving: model.approvingThreadID == item.threadID,
                    goalControl: model.activeGoalControl,
                    isUpdatingGoal: model.isUpdatingGoal,
                    goalControlError: model.goalControlError,
                    openGoal: { model.openGoalControls(for: item) },
                    dismissGoal: model.dismissGoalControls,
                    beginGoalEditing: model.beginGoalEditing,
                    cancelGoalEditing: model.cancelGoalEditing,
                    updateGoalDraft: model.updateGoalDraft,
                    saveGoal: model.saveGoalEdit,
                    resumeGoal: model.resumeGoal
                ) {
                    model.reply(to: item)
                    promptFocused = true
                } steer: {
                    model.steer(item)
                    promptFocused = true
                } approveOnce: {
                    model.approveOnce(item)
                } approveSimilar: {
                    model.approveSimilarCommands(item)
                } tellCodex: {
                    model.tellCodexSomethingElse(item)
                    promptFocused = true
                }
                .id(item.id)
            }
        }
        .padding(.vertical, 1)
    }

    private func scrollToRelevantProcess(using proxy: ScrollViewProxy, animated: Bool) {
        guard let processID = model.activeProcessTarget?.processID
            ?? model.processStore.items.first(where: { $0.status == .waiting && $0.kind != .notice })?.id
            ?? model.latestAttentionHighlight?.processID
        else { return }
        let scroll = {
            proxy.scrollTo(processID, anchor: .center)
        }
        if animated {
            withAnimation(TrayAnimation.panel) {
                scroll()
            }
        } else {
            scroll()
        }
    }

    private func processInlineComposer(for item: CodexProcessItem) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: model.processTargetSystemName ?? "arrowshape.turn.up.left")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)

                Text(processTargetActionTitle)
                    .font(.system(size: 10, weight: .semibold))

                Spacer(minLength: 0)

                Button {
                    model.cancelProcessTarget()
                    promptFocused = false
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 8, weight: .bold))
                        .frame(width: 20, height: 20)
                        .trayCircleControl(isInteractive: true)
                }
                .buttonStyle(.plain)
                .help("Cancel \(processTargetActionTitle)")
            }
            .frame(height: 22)

            if model.shouldShowCodexAccessibilityNotice {
                CodexAccessibilityNotice {
                    model.requestCodexAccessibilityPermission()
                    promptFocused = true
                }
                .frame(height: 38)
            }

            promptField

            if let feedback = model.codexComposerFeedback {
                HStack(alignment: .top, spacing: 6) {
                    Image(systemName: feedback.isError ? "exclamationmark.triangle.fill" : "clock.arrow.circlepath")
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(feedback.isError ? Color.orange : TrayColors.textSecondary)

                    Text(feedback.text)
                        .font(.system(size: 9, weight: .medium))
                        .foregroundStyle(feedback.isError ? Color.orange : TrayColors.textSecondary)
                        .lineLimit(2)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, minHeight: 30, alignment: .leading)
                .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .animation(TrayAnimation.panel, value: model.codexComposerFeedback)
        .onAppear {
            DispatchQueue.main.async {
                promptFocused = true
            }
        }
    }

    private var processTargetActionTitle: String {
        switch model.activeProcessTarget?.action {
        case .steer:
            return "Steer"
        case .approvalFeedback:
            return "Tell Codex"
        case .reply, nil:
            return "Reply"
        }
    }

    private var composer: some View {
        Group {
            if model.shouldShowChatGPTAppHandoff {
                chatGPTHandoffComposer
            } else {
                standardComposer
            }
        }
    }

    private var standardComposer: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let targetSummary = model.processTargetSummary {
                ProcessTargetChip(
                    systemName: model.processTargetSystemName ?? "arrowshape.turn.up.left",
                    summary: targetSummary
                ) {
                    model.clearProcessTarget()
                    promptFocused = true
                }
            }

            if model.shouldShowCodexAccessibilityNotice {
                CodexAccessibilityNotice {
                    model.requestCodexAccessibilityPermission()
                    promptFocused = true
                }
            }

            if model.shouldShowChatGPTMenuResponse, let response = model.chatGPTMenuResponse {
                ChatGPTMenuResponseCard(response: response) {
                    model.dismissChatGPTMenuResponse()
                    promptFocused = true
                }
                .transition(.opacity.combined(with: .scale(scale: 0.985, anchor: .bottom)))
            }

            if model.shouldShowCompanionPromptField {
                promptInputRow
                .zIndex(18)
            }

        }
        .padding(10)
        .traySurface(cornerRadius: 26)
        .shadow(color: .black.opacity(0.30), radius: 18, y: 10)
        .transition(.opacity.combined(with: .scale(scale: 0.99, anchor: .bottom)))
    }

    private var shouldShowInlineChatGPTSendButton: Bool {
        model.routeMode == .chatGPT
            && model.selectedChatGPTDeliveryMode != .appHandoff
            && !model.isCodexProcessTrayVisible
    }

    private var shouldShowInlineSendButton: Bool {
        model.shouldShowCompanionSendButton
    }

    private var isInlineSendInProgress: Bool {
        model.routeMode == .chatGPT && !model.isCodexProcessTrayVisible
            ? model.isChatGPTResponding
            : model.isCodexSending
    }

    private var inlineSendHelp: String {
        if shouldShowInlineChatGPTSendButton {
            return model.isChatGPTResponding ? "ChatGPT is responding" : "Send to ChatGPT"
        }
        return model.isCodexSending ? "Sending to Codex" : "Send using selected route"
    }

    private var shouldShowRouteSendButton: Bool {
        model.shouldShowCompanionSendButton && !shouldShowInlineSendButton
    }

    private var promptInputRow: some View {
        promptField
    }

    private var promptField: some View {
        ZStack(alignment: .bottomTrailing) {
            TextField(
                "",
                text: $model.prompt,
                prompt: Text(model.promptPlaceholder)
                    .foregroundStyle(TrayColors.textSecondary),
                axis: .vertical
            )
            .textFieldStyle(.plain)
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(TrayColors.textPrimary)
            .padding(.leading, 12)
            .padding(.trailing, shouldShowInlineSendButton ? 48 : 12)
            .padding(.vertical, 8)
            .lineLimit(1...3)
            .frame(height: promptFieldHeight, alignment: .topLeading)
            .focused($promptFocused)
            .submitLabel(.send)
            .accessibilityLabel("Codex Companion prompt")
            .onSubmit {
                CodexSendLog.append("companion prompt submitted from text field")
                model.sendPrompt()
                promptFocused = true
            }

            if shouldShowInlineSendButton {
                InlineSendButton(
                    isSending: isInlineSendInProgress,
                    help: inlineSendHelp,
                    glassNamespace: trayGlassNamespace,
                    longPressAction: shouldShowInlineChatGPTSendButton
                        ? { isChatModelPickerPresented = true }
                        : nil
                ) {
                    guard !isInlineSendInProgress else { return }
                    CodexSendLog.append("companion inline send button tapped")
                    model.sendPrompt()
                    promptFocused = true
                }
                .popover(isPresented: $isChatModelPickerPresented, arrowEdge: .bottom) {
                    ChatDeliveryPicker(model: model) {
                        isChatModelPickerPresented = false
                    }
                }
                .padding(.trailing, 6)
                .padding(.bottom, 6)
            }
        }
        .frame(height: promptFieldHeight, alignment: .topLeading)
        .animation(TrayAnimation.panel, value: promptFieldHeight)
        .trayRoundedControl(
            cornerRadius: 18,
            isSelected: promptFocused
        )
    }

    private var promptFieldHeight: CGFloat {
        PetWindowMetrics.promptFieldHeight(for: model.prompt)
    }

    private var chatGPTHandoffComposer: some View {
        VStack(alignment: .leading, spacing: 8) {
            ChatGPTHandoffHeader(model: model)

            Color.clear
                .frame(height: PetWindowMetrics.chatGPTQuickBarHeight)
                .contentShape(Rectangle())
                .allowsHitTesting(false)

            routeControls
                .padding(10)
                .traySurface(cornerRadius: 22)
        }
        .frame(maxWidth: .infinity)
        .shadow(color: .black.opacity(0.26), radius: 16, y: 8)
        .transition(.opacity.combined(with: .scale(scale: 0.99, anchor: .bottom)))
    }

    private var routeControls: some View {
        Group {
            if #available(macOS 26.0, *) {
                GlassEffectContainer(spacing: 5) {
                    routeControlStack
                }
            } else {
                routeControlStack
            }
        }
    }

    private var routeControlStack: some View {
        VStack(alignment: .leading, spacing: 6) {
            routeControlRow
        }
    }

    private var routeControlRow: some View {
        HStack(alignment: .bottom, spacing: 5) {
            if !model.isCodexOnlyMode {
                RoutePillButton(
                    title: RouteMode.chatGPT.title,
                    isSelected: model.routeMode == .chatGPT,
                    usesInteractiveGlass: true,
                    glassID: "route-chatgpt",
                    glassNamespace: trayGlassNamespace
                ) {
                    withAnimation(TrayAnimation.routeMorph) {
                        model.showChatGPT()
                    }
                    promptFocused = true
                }
            }

            if !model.isCodexOnlyMode {
                RoutePillButton(
                    title: RouteMode.codex.title,
                    isSelected: model.routeMode == .codex || model.isCodexProcessTrayVisible,
                    usesInteractiveGlass: true,
                    glassID: "route-codex",
                    glassNamespace: trayGlassNamespace
                ) {
                    withAnimation(TrayAnimation.routeMorph) {
                        model.showCodexProcesses()
                    }
                    promptFocused = true
                }
            }

            Spacer(minLength: 3)

            if model.shouldShowChatGPTModelPicker {
                ChatGPTModelMenu(
                    selection: $model.selectedChatGPTModel,
                    deliveryMode: model.selectedChatGPTDeliveryMode,
                    isExpanded: $model.isChatGPTModelPickerExpanded,
                    opensUpward: true,
                    isCompact: true
                )
                .zIndex(35)
            }

            if model.routeMode == .chatGPT && !model.isCodexProcessTrayVisible {
                if model.selectedChatGPTDeliveryMode == .appHandoff {
                    RoutePillButton(
                        title: "API",
                        isSelected: false,
                        usesInteractiveGlass: true,
                        glassID: "chatgpt-delivery-toggle",
                        glassNamespace: trayGlassNamespace
                    ) {
                        withAnimation(TrayAnimation.routeMorph) {
                            model.useChatGPTAPI()
                        }
                        promptFocused = true
                    }
                } else {
                    RoutePillButton(
                        title: "App",
                        isSelected: false,
                        usesInteractiveGlass: true,
                        glassID: "chatgpt-delivery-toggle",
                        glassNamespace: trayGlassNamespace
                    ) {
                        withAnimation(TrayAnimation.routeMorph) {
                            model.useChatGPTAppHandoff()
                        }
                        promptFocused = true
                    }
                }
            }

            if shouldShowRouteSendButton {
                TrayIconButton(
                    systemName: model.isCodexSending ? "hourglass" : "arrow.up",
                    help: model.isCodexSending ? "Sending to Codex" : "Send using selected route",
                    isPrimary: true,
                    glassID: "quickbar-send",
                    glassNamespace: trayGlassNamespace
                ) {
                    if !model.isCodexSending {
                        CodexSendLog.append("companion send button tapped")
                        model.sendPrompt()
                    }
                }
                .keyboardShortcut(.return, modifiers: [])
            }
        }
    }
}

private struct ChatGPTMenuResponseCard: View {
    var response: ChatGPTMenuResponse
    var dismiss: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 7) {
                Image(systemName: "bubble.left.and.text.bubble.right")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)

                Text(response.displayTitle)
                    .font(.system(size: 11, weight: .semibold))

                Spacer(minLength: 0)

                Button(action: dismiss) {
                    Image(systemName: "xmark")
                        .font(.system(size: 9, weight: .bold))
                        .frame(width: 20, height: 20)
                        .trayCircleControl(isInteractive: true)
                }
                .buttonStyle(.plain)
                .help("Dismiss")
            }

            Text(response.prompt)
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(TrayColors.textPrimary.opacity(0.82))
                .lineLimit(1)

            ScrollView {
                ChatGPTResponseMessageText(message: response.message)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.trailing, 4)
            }
            .scrollIndicators(.automatic)
            .frame(minHeight: response.message == "Thinking..." ? 18 : 54, maxHeight: 90)

            if let usageSummary = response.usageSummary {
                Text(usageSummary)
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary.opacity(0.72))
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .trayRoundedControl(cornerRadius: 17)
    }
}

private struct ChatGPTResponseMessageText: View {
    var message: String

    var body: some View {
        Group {
            if let attributedText {
                Text(attributedText)
            } else {
                Text(displayMessage)
            }
        }
        .font(.system(size: 10.5, weight: .medium))
        .foregroundStyle(TrayColors.textSecondary)
        .lineSpacing(2)
        .textSelection(.enabled)
    }

    private var displayMessage: String {
        MathTextFormatter.displayString(from: message)
    }

    private var attributedText: AttributedString? {
        try? AttributedString(
            markdown: displayMessage,
            options: AttributedString.MarkdownParsingOptions(
                interpretedSyntax: .inlineOnlyPreservingWhitespace
            )
        )
    }
}

private struct ChatGPTHandoffHeader: View {
    @ObservedObject var model: CompanionAppModel

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "bubble.left.and.text.bubble.right")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(TrayColors.textSecondary)

            VStack(alignment: .leading, spacing: 1) {
                Text("ChatGPT quick bar")
                    .font(.system(size: 12, weight: .semibold))
                Text(model.chatGPTQuickBar.status)
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 0)

            Button {
                model.useChatGPTAPI()
            } label: {
                Text("API")
                    .font(.system(size: 10, weight: .semibold))
                    .padding(.horizontal, 10)
                    .frame(height: 22)
                    .trayCapsuleControl(isInteractive: true)
            }
            .buttonStyle(.plain)
            .help("Switch to Companion API mode")

            Button {
                model.chatGPTQuickBar.close()
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 9, weight: .bold))
                    .frame(width: 22, height: 22)
                    .trayCircleControl(isInteractive: true)
            }
            .buttonStyle(.plain)
            .help("Close ChatGPT quick bar")
        }
        .padding(.horizontal, 11)
        .padding(.vertical, 8)
        .traySurface(cornerRadius: 20)
    }
}

private struct ChatGPTAppHandoffWell: View {
    @ObservedObject var model: CompanionAppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: "bubble.left.and.text.bubble.right")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)

                Text("ChatGPT quick bar")
                    .font(.system(size: 12, weight: .semibold))

                Spacer(minLength: 0)

                Button {
                    model.useChatGPTAPI()
                } label: {
                    Text("API")
                        .font(.system(size: 10, weight: .semibold))
                        .padding(.horizontal, 10)
                        .frame(height: 22)
                        .trayCapsuleControl(isInteractive: true)
                }
                .buttonStyle(.plain)
                .help("Switch to Companion API mode")

                Button {
                    model.chatGPTQuickBar.close()
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 9, weight: .bold))
                        .frame(width: 22, height: 22)
                        .trayCircleControl(isInteractive: true)
                }
                .buttonStyle(.plain)
                .help("Close ChatGPT quick bar")
            }

            ZStack {
                HandoffWellBackground(cornerRadius: 26)

                VStack(spacing: 6) {
                    Image(systemName: "arrow.up.left.and.arrow.down.right")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(TrayColors.textSecondary)

                    Text("Use the ChatGPT quick bar directly")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(TrayColors.textPrimary)

                    Text(model.chatGPTQuickBar.status)
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(TrayColors.textSecondary)
                        .multilineTextAlignment(.center)
                        .lineLimit(2)
                        .frame(maxWidth: 320)
                }
                .allowsHitTesting(false)
            }
            .frame(height: PetWindowMetrics.chatGPTQuickBarHeight)
            .allowsHitTesting(false)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .trayRoundedControl(cornerRadius: 20)
    }
}

private struct HandoffWellBackground: View {
    var cornerRadius: CGFloat

    var body: some View {
        if #available(macOS 26.0, *) {
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .fill(Color.clear)
                .glassEffect(.clear, in: .rect(cornerRadius: cornerRadius))
                .overlay {
                    RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                        .stroke(Color.primary.opacity(0.16), lineWidth: 1)
                        .allowsHitTesting(false)
                }
        } else {
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .fill(.thinMaterial)
                .overlay {
                    RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                        .stroke(.separator.opacity(0.4), lineWidth: 1)
                        .allowsHitTesting(false)
                }
        }
    }
}

private struct ProcessTargetChip: View {
    var systemName: String
    var summary: String
    var clear: () -> Void

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: systemName)
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(TrayColors.textSecondary)

            Text(summary)
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(TrayColors.textSecondary)
                .lineLimit(1)

            Spacer(minLength: 0)

            Button(action: clear) {
                Image(systemName: "xmark")
                    .font(.system(size: 9, weight: .bold))
                    .frame(width: 18, height: 18)
                    .trayCircleControl(isInteractive: true)
            }
            .buttonStyle(.plain)
            .help("Clear process target")
        }
        .padding(.leading, 10)
        .padding(.trailing, 6)
        .frame(height: 24)
        .trayCapsuleControl()
    }
}

private struct CodexAccessibilityNotice: View {
    var fix: () -> Void

    var body: some View {
        HStack(spacing: 7) {
            Image(systemName: "exclamationmark.shield")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(TrayColors.textSecondary)

            Text("Reply fallback needs Accessibility")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(TrayColors.textSecondary)
                .lineLimit(1)

            Spacer(minLength: 0)

            Button(action: fix) {
                Text("Fix")
                    .font(.system(size: 10, weight: .semibold))
                    .padding(.horizontal, 9)
                    .frame(height: 23)
                    .trayCapsuleControl(isSelected: true, isInteractive: true)
            }
            .buttonStyle(.plain)
            .help("Open Accessibility settings for Codex Companion")
        }
        .padding(.leading, 10)
        .padding(.trailing, 6)
        .frame(height: 30)
        .trayRoundedControl(cornerRadius: 15)
    }
}

private enum TrayColors {
    static let surface = Color.black.opacity(0.50)
    static let recessed = Color.black.opacity(0.24)
    static let control = Color.black.opacity(0.24)
    static let selectedControl = Color.white.opacity(0.13)
    static let border = Color.primary.opacity(0.14)
    static let focusBorder = Color.primary.opacity(0.24)
    static let glassSurfaceFill = Color.black.opacity(0.42)
    static let glassSurfaceTint = Color.black.opacity(0.18)
    static let glassControlFill = Color.black.opacity(0.18)
    static let glassSelectedFill = Color.white.opacity(0.11)
    static let glassBorder = Color.primary.opacity(0.16)
    static let glassSelectedBorder = Color.primary.opacity(0.30)
    static let textPrimary = Color.primary
    static let textSecondary = Color.secondary

    static func controlFill(isSelected: Bool) -> Color {
        isSelected ? selectedControl : control
    }
}

private enum TrayAnimation {
    static let quick = Animation.easeOut(duration: 0.14)
    static let hover = Animation.smooth(duration: 0.16, extraBounce: 0.05)
    static let routeHover = Animation.smooth(duration: 0.12, extraBounce: 0.02)
    static let routeMorph = Animation.smooth(duration: 0.18, extraBounce: 0.03)
    static let panel = Animation.smooth(duration: 0.18, extraBounce: 0.03)
    static let glassMorph = Animation.smooth(duration: 0.20, extraBounce: 0.06)
    static let materialize = Animation.spring(response: 0.38, dampingFraction: 0.76, blendDuration: 0.08)
}

private struct ChatGPTModelMenu: View {
    private let compactSelectorWidth: CGFloat = 78
    private let compactMenuWidth: CGFloat = 178

    @Binding var selection: ChatGPTModel
    var deliveryMode: ChatGPTDeliveryMode
    @Binding var isExpanded: Bool
    var opensUpward = false
    var isCompact = false
    @Namespace private var modelGlassNamespace

    var body: some View {
        if isCompact {
            compactBody
        } else {
            VStack(alignment: .leading, spacing: 6) {
                if opensUpward {
                    expandedOptionsList
                }

                selectorButton

                if !opensUpward {
                    expandedOptionsList
                }
            }
            .animation(TrayAnimation.glassMorph, value: isExpanded)
            .zIndex(30)
        }
    }

    private var compactBody: some View {
        ZStack(alignment: .bottomTrailing) {
            if isExpanded {
                optionsList
                    .frame(width: compactMenuWidth)
                    .offset(y: -36)
                    .transition(.asymmetric(
                        insertion: .opacity
                            .combined(with: .scale(scale: 0.97, anchor: .bottomTrailing)),
                        removal: .opacity
                            .combined(with: .scale(scale: 0.99, anchor: .bottomTrailing))
                    ))
                    .zIndex(2)
            }

            selectorButton
                .frame(width: compactSelectorWidth)
                .zIndex(3)
        }
        .frame(width: compactSelectorWidth, height: 30, alignment: .bottomTrailing)
        .animation(TrayAnimation.glassMorph, value: isExpanded)
        .zIndex(35)
    }

    private var selectorButton: some View {
        Button {
            withAnimation(TrayAnimation.glassMorph) {
                isExpanded.toggle()
            }
        } label: {
            selectorLabel
        }
        .buttonStyle(.plain)
        .help("Choose ChatGPT model")
    }

    @ViewBuilder
    private var expandedOptionsList: some View {
        if isExpanded {
            optionsList
                .transition(.asymmetric(
                    insertion: .opacity
                        .combined(with: .scale(scale: 0.985, anchor: opensUpward ? .bottom : .top)),
                    removal: .opacity
                        .combined(with: .scale(scale: 0.995, anchor: opensUpward ? .bottom : .top))
                ))
        }
    }

    private var optionsList: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(ChatGPTModel.allCases) { model in
                Button {
                    selection = model
                    withAnimation(TrayAnimation.glassMorph) {
                        isExpanded = false
                    }
                } label: {
                    HStack(spacing: 7) {
                        Image(systemName: selection == model ? "checkmark.circle.fill" : "circle")
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundStyle(selection == model ? TrayColors.textPrimary : TrayColors.textSecondary)
                            .frame(width: 14)

                        VStack(alignment: .leading, spacing: 1) {
                            Text(model.shortTitle)
                                .font(.system(size: 11, weight: .semibold))
                            Text(model.costNote)
                                .font(.system(size: 9, weight: .medium))
                                .foregroundStyle(TrayColors.textSecondary)
                                .lineLimit(1)
                        }

                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 9)
                    .frame(height: 34)
                    .trayRoundedControl(
                        cornerRadius: 14,
                        isSelected: selection == model,
                        isInteractive: true,
                        glassID: selection == model ? "chatgpt-model-selected" : nil,
                        glassNamespace: modelGlassNamespace
                    )
                    .contentShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .trayPopoverSurface(cornerRadius: 18)
        .shadow(color: .black.opacity(0.34), radius: isCompact ? 16 : 12, y: 8)
    }

    private var selectorLabel: some View {
        HStack(spacing: isCompact ? 5 : 8) {
            if !isCompact {
                Image(systemName: "sparkles")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)
            }

            if isCompact {
                Text(compactTitle(for: selection))
                    .font(.system(size: 11, weight: .semibold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.85)
            } else {
                Text(selection.shortTitle)
                    .font(.system(size: 11, weight: .semibold))
                    .lineLimit(1)

                Text("· \(deliveryMode.shortTitle)")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 0)

            Image(systemName: "chevron.down")
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(TrayColors.textSecondary)
                .rotationEffect(isExpanded ? .degrees(180) : .zero)
        }
        .padding(.horizontal, isCompact ? 7 : 10)
        .frame(height: 30)
        .trayRoundedControl(
            cornerRadius: isCompact ? 13 : 14,
            isSelected: true,
            isInteractive: true,
            glassID: "chatgpt-model-selector",
            glassNamespace: modelGlassNamespace
        )
        .contentShape(RoundedRectangle(cornerRadius: isCompact ? 13 : 14, style: .continuous))
    }

    private func compactTitle(for model: ChatGPTModel) -> String {
        switch model {
        case .gpt55:
            return "Mini"
        case .gpt55Thinking:
            return "Think"
        case .gpt55Pro:
            return "High"
        }
    }
}

private struct InlineSendButton: View {
    var isSending: Bool
    var help: String
    var glassNamespace: Namespace.ID
    var longPressAction: (() -> Void)?
    var action: () -> Void
    @State private var isHovering = false
    @State private var suppressNextTap = false

    var body: some View {
        Button {
            if suppressNextTap {
                suppressNextTap = false
                return
            }
            action()
        } label: {
            Image(systemName: isSending ? "hourglass" : "arrow.up")
                .font(.system(size: 11, weight: .bold))
                .frame(width: 30, height: 30)
                .trayCircleControl(
                    isSelected: true,
                    isInteractive: true,
                    glassID: "quickbar-inline-send",
                    glassNamespace: glassNamespace
                )
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(help)
        .accessibilityIdentifier(help)
        .accessibilityAddTraits(.isButton)
        .help(help)
        .keyboardShortcut(.return, modifiers: [])
        .simultaneousGesture(
            LongPressGesture(minimumDuration: 0.45)
                .onEnded { _ in
                    guard let longPressAction else { return }
                    suppressNextTap = true
                    longPressAction()
                }
        )
        .scaleEffect(x: isHovering ? 1.014 : 1, y: isHovering ? 0.992 : 1)
        .onHover { hovering in
            withAnimation(TrayAnimation.hover) {
                isHovering = hovering
            }
        }
    }
}

private struct ChatDeliveryPicker: View {
    @ObservedObject var model: CompanionAppModel
    var dismiss: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Chat model")
                .font(.system(size: 12, weight: .semibold))
                .padding(.horizontal, 4)

            deliveryButton(
                title: "On-device Apple model",
                detail: "Private · no API usage",
                isSelected: model.selectedChatGPTDeliveryMode == .onDevice
            ) {
                model.useOnDeviceChat()
                dismiss()
            }

            Divider()

            ForEach(ChatGPTModel.allCases) { chatModel in
                deliveryButton(
                    title: chatModel.shortTitle,
                    detail: "OpenAI API · \(chatModel.costNote)",
                    isSelected: model.selectedChatGPTDeliveryMode == .openAIAPI
                        && model.selectedChatGPTModel == chatModel
                ) {
                    model.selectedChatGPTModel = chatModel
                    model.useChatGPTAPI()
                    dismiss()
                }
            }
        }
        .padding(10)
        .frame(width: 238)
    }

    private func deliveryButton(
        title: String,
        detail: String,
        isSelected: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 11, weight: .semibold))
                    .frame(width: 15)

                VStack(alignment: .leading, spacing: 1) {
                    Text(title)
                        .font(.system(size: 11, weight: .semibold))
                    Text(detail)
                        .font(.system(size: 9, weight: .medium))
                        .foregroundStyle(.secondary)
                }

                Spacer(minLength: 0)
            }
            .padding(.horizontal, 8)
            .frame(height: 38)
            .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
    }
}

private struct RoutePillButton: View {
    var title: String
    var isSelected: Bool
    var usesInteractiveGlass = false
    var glassID: String?
    var glassNamespace: Namespace.ID?
    var horizontalPadding: CGFloat = 8
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(isSelected ? TrayColors.textPrimary : TrayColors.textSecondary)
                .lineLimit(1)
                .frame(height: 30)
                .padding(.horizontal, horizontalPadding)
                .trayCapsuleControl(
                    isSelected: isSelected,
                    isInteractive: true,
                    usesInteractiveGlass: usesInteractiveGlass,
                    glassID: glassID,
                    glassNamespace: glassNamespace
                )
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityIdentifier(title)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction(named: Text(title), action)
        .help("Route through \(title)")
        .scaleEffect(x: isHovering ? 1.004 : 1, y: isHovering ? 0.999 : 1)
        .onHover { hovering in
            withAnimation(TrayAnimation.routeHover) {
                isHovering = hovering
            }
        }
    }
}

private struct TrayIconButton: View {
    var systemName: String
    var help: String
    var isPrimary = false
    var glassID: String?
    var glassNamespace: Namespace.ID?
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 12, weight: .semibold))
                .frame(width: 31, height: 30)
                .trayRoundedControl(
                    cornerRadius: 13,
                    isSelected: isPrimary,
                    isInteractive: true,
                    glassID: glassID,
                    glassNamespace: glassNamespace
                )
                .contentShape(RoundedRectangle(cornerRadius: 13, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(help)
        .accessibilityIdentifier(help)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction(named: Text(help), action)
        .help(help)
        .scaleEffect(x: isHovering ? 1.014 : 1, y: isHovering ? 0.992 : 1)
        .onHover { hovering in
            withAnimation(TrayAnimation.hover) {
                isHovering = hovering
            }
        }
    }
}

private struct ProcessLoadingCard: View {
    var body: some View {
        HStack(spacing: 9) {
            ProgressView()
                .controlSize(.small)
                .frame(width: 18, height: 18)

            VStack(alignment: .leading, spacing: 2) {
                Text("Loading Codex processes")
                    .font(.system(size: 12, weight: .semibold))

                Text("Checking recent work")
                    .font(.system(size: 10))
                    .foregroundStyle(TrayColors.textSecondary)
            }

            Spacer(minLength: 0)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, minHeight: 42, alignment: .leading)
        .trayRoundedControl(cornerRadius: 24)
    }
}

private struct ProcessCard: View {
    var item: CodexProcessItem
    var isHovering: Bool
    var attentionAccent: PetAttentionAccent?
    var hoverChanged: (Bool) -> Void
    var inlineComposer: AnyView?
    var inlineComposerHeight: CGFloat
    var isApproving: Bool
    var goalControl: CodexGoalControlState?
    var isUpdatingGoal: Bool
    var goalControlError: String?
    var openGoal: () -> Void
    var dismissGoal: () -> Void
    var beginGoalEditing: () -> Void
    var cancelGoalEditing: () -> Void
    var updateGoalDraft: (String) -> Void
    var saveGoal: () -> Void
    var resumeGoal: () -> Void
    var reply: () -> Void
    var steer: () -> Void
    var approveOnce: () -> Void
    var approveSimilar: () -> Void
    var tellCodex: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(alignment: .center, spacing: 9) {
                Image(systemName: item.iconName)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(TrayColors.textSecondary)
                    .frame(width: 15)

                VStack(alignment: .leading, spacing: 2) {
                    Text(item.title)
                        .font(.system(size: 12, weight: .semibold))
                        .lineLimit(1)

                    goalBadge

                    ProcessMessageText(
                        subtitle: item.subtitle,
                        fullMessage: item.fullMessage,
                        isExpanded: isHovering
                    )
                }

                Spacer(minLength: 0)

                ProcessStatusBadge(status: item.status)
            }

            if showsApprovalActions {
                HStack(spacing: 4) {
                    ProcessActionButton(
                        title: "Approve once",
                        systemName: "checkmark",
                        accessibilityLabel: "Approve once for \(item.title)",
                        isEnabled: !isApproving,
                        isCompact: true,
                        action: approveOnce
                    )
                    ProcessActionButton(
                        title: "Approve similar",
                        systemName: "checkmark.shield",
                        accessibilityLabel: "Approve similar commands for \(item.title)",
                        isEnabled: !isApproving,
                        isCompact: true,
                        action: approveSimilar
                    )
                    ProcessActionButton(
                        title: "Tell Codex",
                        systemName: "text.bubble",
                        accessibilityLabel: "Tell Codex something else for \(item.title)",
                        isEnabled: !isApproving,
                        isCompact: true,
                        action: tellCodex
                    )
                }
                .frame(height: 24, alignment: .top)
                .transition(.opacity.combined(with: .move(edge: .top)))
            } else if !processActions.isEmpty {
                HStack(spacing: 6) {
                    if processActions.contains(.reply) {
                        ProcessActionButton(
                            title: "Reply",
                            systemName: "arrowshape.turn.up.left",
                            accessibilityLabel: "Reply to \(item.title)",
                            action: reply
                        )
                    }
                    if processActions.contains(.steer) {
                        ProcessActionButton(
                            title: "Steer",
                            systemName: "arrow.turn.down.right",
                            accessibilityLabel: "Steer \(item.title)",
                            action: steer
                        )
                    }
                    Spacer(minLength: 0)
                }
                .frame(height: 24, alignment: .top)
                .transition(.opacity.combined(with: .move(edge: .top)))
            }

            if let inlineComposer {
                inlineComposer
                    .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .frame(
            height: PetWindowMetrics.processRowHeight(
                for: item,
                showsActions: showsAnyActions,
                inlineComposerHeight: inlineComposerHeight
            ),
            alignment: .leading
        )
        .frame(maxWidth: .infinity, alignment: .leading)
        .trayRoundedControl(
            cornerRadius: 22,
            isSelected: isHovering,
            tint: cardTint
        )
        .contentShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        .onHover { hovering in
            withAnimation(TrayAnimation.hover) {
                hoverChanged(hovering)
            }
        }
        .animation(TrayAnimation.hover, value: showsAnyActions)
        .animation(TrayAnimation.panel, value: attentionAccent)
        .animation(TrayAnimation.panel, value: item.runtimeStatus)
        .scaleEffect(isHovering ? 1.004 : 1)
        .help(item.fullMessage)
    }

    private var processActions: Set<CompanionProcessAction> {
        CompanionPresentationPolicy.processActions(
            status: item.status,
            isHovered: isHovering,
            canTargetCodexThread: item.canTargetCodexThread
        )
    }

    private var showsApprovalActions: Bool {
        isHovering
            && item.status != .failed
            && item.runtimeStatus == .waitingOnApproval
            && item.canTargetCodexThread
    }

    private var showsAnyActions: Bool {
        showsApprovalActions || !processActions.isEmpty
    }

    private var cardTint: Color? {
        CompanionPresentationPolicy.processAccent(
            status: item.status,
            runtimeStatus: item.runtimeStatus,
            attentionAccent: attentionAccent
        )?.color
    }

    @ViewBuilder
    private var goalBadge: some View {
        if item.goalStatus == .active {
            TimelineView(.periodic(from: .now, by: 1)) { context in
                if let summary = CodexProcessStore.goalDisplaySummary(for: item, at: context.date) {
                    goalButton(summary: summary)
                }
            }
        } else if let summary = CodexProcessStore.goalDisplaySummary(for: item) {
            goalButton(summary: summary)
        }
    }

    private func goalButton(summary: String) -> some View {
        Button(action: openGoal) {
            GoalDurationBadge(summary: summary, isReached: item.hasReachedGoal)
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .help("Open goal controls")
        .popover(isPresented: goalPopoverPresented, arrowEdge: .bottom) {
            if let goalControl, goalControl.threadID == item.threadID {
                GoalControlPopover(
                    state: goalControl,
                    isUpdating: isUpdatingGoal,
                    errorMessage: goalControlError,
                    updateDraft: updateGoalDraft,
                    beginEditing: beginGoalEditing,
                    cancelEditing: cancelGoalEditing,
                    save: saveGoal,
                    resume: resumeGoal,
                    dismiss: dismissGoal
                )
            }
        }
    }

    private var goalPopoverPresented: Binding<Bool> {
        Binding(
            get: { goalControl?.threadID == item.threadID },
            set: { isPresented in
                if !isPresented {
                    dismissGoal()
                }
            }
        )
    }

}

private struct GoalDurationBadge: View {
    var summary: String
    var isReached: Bool

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: isReached ? "target" : "clock")
                .font(.system(size: 8, weight: .bold))
            Text(summary)
                .font(.system(size: 9, weight: .semibold))
                .lineLimit(1)
        }
        .foregroundStyle(isReached ? Color.green.opacity(0.92) : TrayColors.textPrimary.opacity(0.74))
        .padding(.horizontal, 6)
        .frame(height: 13)
        .background(
            (isReached ? Color.green.opacity(0.14) : Color.white.opacity(0.08)),
            in: Capsule()
        )
        .overlay {
            Capsule()
                .stroke(isReached ? Color.green.opacity(0.28) : Color.white.opacity(0.10), lineWidth: 1)
                .allowsHitTesting(false)
        }
        .help(summary)
    }
}

private struct ProcessMessageText: View {
    var subtitle: String
    var fullMessage: String
    var isExpanded: Bool

    var body: some View {
        ZStack(alignment: .topLeading) {
            Text(subtitle)
                .font(.system(size: 10))
                .foregroundStyle(TrayColors.textSecondary)
                .lineLimit(1)
                .opacity(isExpanded ? 0 : 1)

            Text(fullMessage)
                .font(.system(size: 10))
                .foregroundStyle(TrayColors.textSecondary)
                .lineLimit(1)
                .opacity(isExpanded ? 1 : 0)
        }
        .frame(height: 14, alignment: .topLeading)
        .clipped()
    }
}

private struct ProcessStatusBadge: View {
    var status: CodexProcessItem.Status

    var body: some View {
        ZStack {
            StatusGlassCircle(borderColor: borderColor)

            switch status {
            case .running:
                ProgressView()
                    .controlSize(.small)
                    .frame(width: 16, height: 16)
            case .completed:
                Image(systemName: "checkmark")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(Color.green)
            case .failed:
                Image(systemName: "xmark")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(Color.red)
            case .waiting:
                Image(systemName: "exclamationmark")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(Color.yellow)
            }
        }
        .help(helpText)
    }

    private var borderColor: Color {
        switch status {
        case .running:
            return .white.opacity(0.18)
        case .completed:
            return .green.opacity(0.55)
        case .failed:
            return .red.opacity(0.55)
        case .waiting:
            return .yellow.opacity(0.55)
        }
    }

    private var helpText: String {
        switch status {
        case .running:
            return "Still working"
        case .completed:
            return "Completed"
        case .failed:
            return "Failed or disconnected"
        case .waiting:
            return "Needs your attention"
        }
    }
}

private struct StatusGlassCircle: View {
    var borderColor: Color

    var body: some View {
        if #available(macOS 26.0, *) {
            Circle()
                .fill(TrayColors.glassControlFill)
                .frame(width: 24, height: 24)
                .overlay {
                    Circle()
                        .stroke(borderColor)
                        .allowsHitTesting(false)
                }
        } else {
            Circle()
                .fill(.thinMaterial)
                .frame(width: 24, height: 24)
                .overlay {
                    Circle()
                        .stroke(borderColor)
                        .allowsHitTesting(false)
                }
        }
    }
}

private struct ProcessActionButton: View {
    var title: String
    var systemName: String
    var accessibilityLabel: String
    var isEnabled = true
    var isCompact = false
    var action: () -> Void
    @State private var isHovering = false

    var body: some View {
        Button {
            CodexSendLog.append("companion process action tapped label=\(accessibilityLabel)")
            action()
        } label: {
            HStack(spacing: isCompact ? 5 : 8) {
                Image(systemName: systemName)
                    .font(.system(size: 9, weight: .bold))
                Text(title)
                    .font(.system(size: isCompact ? 9 : 10, weight: .semibold))
                    .lineLimit(1)
            }
            .foregroundStyle(TrayColors.textPrimary)
            .padding(.horizontal, isCompact ? 7 : 9)
            .frame(height: 24)
            .trayCapsuleControl(isSelected: true, isInteractive: true)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .disabled(!isEnabled)
        .opacity(isEnabled ? 1 : 0.55)
        .help(accessibilityLabel)
        .accessibilityLabel(accessibilityLabel)
        .accessibilityIdentifier(accessibilityLabel)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction(named: Text(title)) {
            CodexSendLog.append("companion process action accessibility label=\(accessibilityLabel)")
            action()
        }
        .scaleEffect(x: isHovering ? 1.014 : 1, y: isHovering ? 0.992 : 1)
        .onHover { hovering in
            withAnimation(TrayAnimation.hover) {
                isHovering = hovering
            }
        }
    }
}

private extension CodexProcessItem {
    var iconName: String {
        switch kind {
        case .job:
            return "gearshape.2"
        case .thread:
            return "text.bubble"
        case .notice:
            return "info.circle"
        }
    }
}

private extension View {
    @ViewBuilder
    func traySurface(cornerRadius: CGFloat) -> some View {
        if #available(macOS 26.0, *) {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(
                    TrayColors.glassSurfaceFill,
                    in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                )
                .glassEffect(
                    .clear.tint(TrayColors.glassSurfaceTint),
                    in: .rect(cornerRadius: cornerRadius)
                )
                .glassEffectTransition(.materialize)
                .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .nativeLiquidGlassEdges(cornerRadius: cornerRadius)
        } else {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .nativeLiquidGlassEdges(cornerRadius: cornerRadius)
        }
    }

    @ViewBuilder
    func trayPopoverSurface(cornerRadius: CGFloat) -> some View {
        if #available(macOS 26.0, *) {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(
                    TrayColors.glassSurfaceFill,
                    in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                )
                .glassEffect(.clear.tint(Color.black.opacity(0.28)).interactive(), in: .rect(cornerRadius: cornerRadius))
                .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .nativeLiquidGlassEdges(cornerRadius: cornerRadius)
        } else {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .nativeLiquidGlassEdges(cornerRadius: cornerRadius)
        }
    }

    @ViewBuilder
    func trayRoundedControl(
        cornerRadius: CGFloat,
        isSelected: Bool = false,
        isInteractive: Bool = false,
        glassID: String? = nil,
        glassNamespace: Namespace.ID? = nil,
        tint: Color? = nil
    ) -> some View {
        if #available(macOS 26.0, *) {
            if isInteractive, let glassID, let glassNamespace {
                self
                    .foregroundStyle(TrayColors.textPrimary)
                    .glassEffect(
                        (isSelected ? Glass.clear.tint(TrayColors.glassSelectedFill) : Glass.clear).interactive(),
                        in: .rect(cornerRadius: cornerRadius)
                    )
                    .glassEffectID(glassID, in: glassNamespace)
                    .trayControlOverlay(cornerRadius: cornerRadius, isSelected: isSelected, tint: tint)
            } else {
                self
                    .foregroundStyle(TrayColors.textPrimary)
                    .trayControlOverlay(cornerRadius: cornerRadius, isSelected: isSelected, tint: tint)
            }
        } else {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(.thinMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .background(TrayColors.controlFill(isSelected: isSelected), in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
                .trayControlOverlay(cornerRadius: cornerRadius, isSelected: isSelected, tint: tint)
        }
    }

    @ViewBuilder
    func trayCapsuleControl(
        isSelected: Bool = false,
        isInteractive: Bool = false,
        usesInteractiveGlass: Bool = true,
        glassID: String? = nil,
        glassNamespace: Namespace.ID? = nil
    ) -> some View {
        if #available(macOS 26.0, *) {
            if isInteractive, let glassID, let glassNamespace {
                if usesInteractiveGlass {
                    self
                        .foregroundStyle(TrayColors.textPrimary)
                        .glassEffect(
                            (isSelected ? Glass.clear.tint(TrayColors.glassSelectedFill) : Glass.clear).interactive(),
                            in: .capsule
                        )
                        .glassEffectID(glassID, in: glassNamespace)
                        .trayCapsuleOverlay(isSelected: isSelected)
                } else {
                    self
                        .foregroundStyle(TrayColors.textPrimary)
                        .glassEffect(
                            isSelected ? Glass.clear.tint(TrayColors.glassSelectedFill) : Glass.clear,
                            in: .capsule
                        )
                        .glassEffectID(glassID, in: glassNamespace)
                        .trayCapsuleOverlay(isSelected: isSelected)
                }
            } else {
                self
                    .foregroundStyle(TrayColors.textPrimary)
                    .trayCapsuleOverlay(isSelected: isSelected)
            }
        } else {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(.thinMaterial, in: Capsule())
                .background(TrayColors.controlFill(isSelected: isSelected), in: Capsule())
                .trayCapsuleOverlay(isSelected: isSelected)
        }
    }

    @ViewBuilder
    func trayCircleControl(
        isSelected: Bool = false,
        isInteractive: Bool = false,
        glassID: String? = nil,
        glassNamespace: Namespace.ID? = nil
    ) -> some View {
        if #available(macOS 26.0, *) {
            if isInteractive, let glassID, let glassNamespace {
                self
                    .foregroundStyle(TrayColors.textPrimary)
                    .glassEffect(
                        (isSelected ? Glass.clear.tint(TrayColors.glassSelectedFill) : Glass.clear).interactive(),
                        in: .circle
                    )
                    .glassEffectID(glassID, in: glassNamespace)
                    .trayCircleOverlay(isSelected: isSelected)
            } else {
                self
                    .foregroundStyle(TrayColors.textPrimary)
                    .trayCircleOverlay(isSelected: isSelected)
            }
        } else {
            self
                .foregroundStyle(TrayColors.textPrimary)
                .background(.thinMaterial, in: Circle())
                .background(TrayColors.controlFill(isSelected: isSelected), in: Circle())
                .trayCircleOverlay(isSelected: isSelected)
        }
    }

    func nativeLiquidGlassEdges(cornerRadius: CGFloat) -> some View {
        self
            .overlay {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .stroke(
                        LinearGradient(
                            colors: [
                                Color.primary.opacity(0.20),
                                Color.primary.opacity(0.08),
                                Color.clear,
                            ],
                            startPoint: .topLeading,
                            endPoint: .bottomTrailing
                        ),
                        lineWidth: 1
                    )
                    .allowsHitTesting(false)
            }
            .overlay(alignment: .topLeading) {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(
                        LinearGradient(
                            colors: [
                                Color.primary.opacity(0.025),
                                Color.primary.opacity(0.01),
                                Color.clear,
                            ],
                            startPoint: .topLeading,
                            endPoint: .center
                        )
                    )
                    .padding(1)
                    .allowsHitTesting(false)
            }
    }

    func trayControlOverlay(
        cornerRadius: CGFloat,
        isSelected: Bool,
        tint: Color? = nil
    ) -> some View {
        self
            .background(
                tint?.opacity(isSelected ? 0.23 : 0.16)
                    ?? (isSelected ? TrayColors.glassSelectedFill : TrayColors.glassControlFill),
                in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
            )
            .overlay {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .stroke(
                        tint?.opacity(isSelected ? 0.68 : 0.48)
                            ?? (isSelected ? TrayColors.glassSelectedBorder : TrayColors.glassBorder),
                        lineWidth: 1
                    )
                    .allowsHitTesting(false)
            }
    }

    func trayCapsuleOverlay(isSelected: Bool) -> some View {
        self
            .background(isSelected ? TrayColors.glassSelectedFill : TrayColors.glassControlFill, in: Capsule())
            .overlay {
                Capsule()
                    .stroke(isSelected ? TrayColors.glassSelectedBorder : TrayColors.glassBorder, lineWidth: 1)
                    .allowsHitTesting(false)
            }
    }

    func trayCircleOverlay(isSelected: Bool) -> some View {
        self
            .background(isSelected ? TrayColors.glassSelectedFill : TrayColors.glassControlFill, in: Circle())
            .overlay {
                Circle()
                    .stroke(isSelected ? TrayColors.glassSelectedBorder : TrayColors.glassBorder, lineWidth: 1)
                    .allowsHitTesting(false)
            }
    }
}

private extension PetAttentionAccent {
    var color: Color {
        switch self {
        case .blue: return .blue
        case .yellow: return .yellow
        case .green: return .green
        case .indigo: return .indigo
        case .red: return .red
        }
    }
}
