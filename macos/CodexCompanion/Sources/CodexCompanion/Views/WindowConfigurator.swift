import AppKit
import CoreGraphics
import QuartzCore
import SwiftUI

enum PetWindowMetrics {
    static let petSize = CGSize(width: 124, height: 124)
    static let trayWidth: CGFloat = 292
    static let chatGPTHandoffTrayWidth: CGFloat = 380
    static let compactTrayHeight: CGFloat = 94
    static let chatGPTResponseTrayHeight: CGFloat = 420
    static let chatGPTResponseModelPickerTrayHeight: CGFloat = 546
    static let chatGPTAppHandoffTrayHeight: CGFloat = 366
    static let chatGPTQuickBarInset: CGFloat = 24
    static let chatGPTQuickBarBottomOffset: CGFloat = 78
    static let chatGPTQuickBarHeight: CGFloat = 230
    static let modelPickerTrayHeight: CGFloat = 328
    static let maxProcessTrayHeight: CGFloat = 416
    static let maximumProcessCardsWithoutScrolling = 3
    static let maxProcessListHeight: CGFloat =
        CGFloat(maximumProcessCardsWithoutScrolling) * 72
        + CGFloat(maximumProcessCardsWithoutScrolling - 1) * 6
        + 2
    static let trayGap: CGFloat = 10

    static func positionedTrayOrigin(
        anchorFrame: NSRect,
        traySize: CGSize,
        visibleFrame: NSRect,
        margin: CGFloat = 8
    ) -> NSPoint {
        let centeredX = anchorFrame.midX - traySize.width / 2
        let clampedX = min(
            max(centeredX, visibleFrame.minX + margin),
            visibleFrame.maxX - traySize.width - margin
        )
        let clampedY = min(
            max(anchorFrame.midY - traySize.height / 2, visibleFrame.minY + margin),
            visibleFrame.maxY - traySize.height - margin
        )
        let aboveY = anchorFrame.maxY + trayGap
        if aboveY + traySize.height <= visibleFrame.maxY - margin {
            return NSPoint(x: clampedX, y: aboveY)
        }

        let leftX = anchorFrame.minX - traySize.width - trayGap
        if leftX >= visibleFrame.minX + margin {
            return NSPoint(x: leftX, y: clampedY)
        }

        let rightX = anchorFrame.maxX + trayGap
        if rightX + traySize.width <= visibleFrame.maxX - margin {
            return NSPoint(x: rightX, y: clampedY)
        }

        let belowY = anchorFrame.minY - traySize.height - trayGap
        if belowY >= visibleFrame.minY + margin {
            return NSPoint(x: clampedX, y: belowY)
        }

        return NSPoint(x: clampedX, y: clampedY)
    }

    static func contentSize(isQuickBarOpen: Bool, showProcesses: Bool) -> CGSize {
        petSize
    }

    static func traySize(
        showProcesses: Bool,
        showModelPicker: Bool = false,
        showChatGPTResponse: Bool = false,
        showChatGPTAppHandoff: Bool = false,
        processItems: [CodexProcessItem] = [],
        isProcessLoading: Bool = false,
        prompt: String = "",
        hasProcessTarget: Bool = false,
        showsAccessibilityNotice: Bool = false,
        targetProcessID: String? = nil,
        expandedProcessID: String? = nil,
        showsCodexSendFeedback: Bool = false
    ) -> CGSize {
        let width: CGFloat = showProcesses ? trayWidth : (showChatGPTAppHandoff ? chatGPTHandoffTrayWidth : trayWidth)
        let height: CGFloat
        if showProcesses {
            height = processTrayHeight(
                processItems: processItems,
                isLoading: isProcessLoading,
                prompt: prompt,
                hasProcessTarget: hasProcessTarget,
                showsAccessibilityNotice: showsAccessibilityNotice,
                targetProcessID: targetProcessID,
                expandedProcessID: expandedProcessID,
                showsCodexSendFeedback: showsCodexSendFeedback
            )
        } else if showChatGPTAppHandoff {
            height = chatGPTAppHandoffTrayHeight
        } else if showChatGPTResponse && showModelPicker {
            height = chatGPTResponseModelPickerTrayHeight
        } else if showChatGPTResponse {
            height = chatGPTResponseTrayHeight
        } else if showModelPicker {
            height = modelPickerTrayHeight
        } else {
            height = compactTrayHeight
        }
        return CGSize(width: width, height: height)
    }

    static func processListHeight(
        for items: [CodexProcessItem],
        isLoading: Bool,
        prompt: String = "",
        hasProcessTarget: Bool = false,
        showsAccessibilityNotice: Bool = false,
        targetProcessID: String? = nil,
        expandedProcessID: String? = nil,
        showsCodexSendFeedback: Bool = false
    ) -> CGFloat {
        if isLoading && items.isEmpty {
            return 46
        }

        return min(
            naturalProcessListHeight(
                for: items,
                prompt: prompt,
                showsAccessibilityNotice: showsAccessibilityNotice,
                targetProcessID: targetProcessID,
                expandedProcessID: expandedProcessID,
                showsCodexSendFeedback: showsCodexSendFeedback
            ),
            availableProcessListHeight(
                showsAccessibilityNotice: showsAccessibilityNotice,
                targetProcessID: targetProcessID
            )
        )
    }

    static func naturalProcessListHeight(
        for items: [CodexProcessItem],
        prompt: String = "",
        showsAccessibilityNotice: Bool = false,
        targetProcessID: String? = nil,
        expandedProcessID: String? = nil,
        showsCodexSendFeedback: Bool = false
    ) -> CGFloat {
        let visibleItems = visibleProcessItems(
            from: items,
            targetProcessID: targetProcessID
        )
        guard !visibleItems.isEmpty else { return 64 }
        return visibleItems.reduce(CGFloat(0)) { total, item in
            let isTargeted = item.id == targetProcessID
            let inlineComposerHeight = isTargeted
                ? inlineProcessComposerHeight(
                    prompt: prompt,
                    showsAccessibilityNotice: showsAccessibilityNotice,
                    showsSendFeedback: showsCodexSendFeedback
                )
                : 0
            return total + processRowHeight(
                for: item,
                showsActions: item.id == expandedProcessID
                    && item.canTargetCodexThread
                    && (item.runtimeStatus == .waitingOnApproval || item.status != .waiting),
                inlineComposerHeight: inlineComposerHeight
            )
        } + CGFloat(max(0, visibleItems.count - 1)) * 6 + 2
    }

    static func visibleProcessItems(
        from items: [CodexProcessItem],
        targetProcessID: String?
    ) -> [CodexProcessItem] {
        items
    }

    static func processListNeedsScrolling(
        for items: [CodexProcessItem],
        isLoading: Bool,
        prompt: String = "",
        hasProcessTarget: Bool = false,
        showsAccessibilityNotice: Bool = false,
        targetProcessID: String? = nil,
        expandedProcessID: String? = nil,
        showsCodexSendFeedback: Bool = false
    ) -> Bool {
        guard !(isLoading && items.isEmpty) else { return false }
        return naturalProcessListHeight(
            for: items,
            prompt: prompt,
            showsAccessibilityNotice: showsAccessibilityNotice,
            targetProcessID: targetProcessID,
            expandedProcessID: expandedProcessID,
            showsCodexSendFeedback: showsCodexSendFeedback
        ) > processListHeight(
            for: items,
            isLoading: isLoading,
            prompt: prompt,
            hasProcessTarget: hasProcessTarget,
            showsAccessibilityNotice: showsAccessibilityNotice,
            targetProcessID: targetProcessID,
            expandedProcessID: expandedProcessID,
            showsCodexSendFeedback: showsCodexSendFeedback
        )
    }

    static func promptFieldHeight(for prompt: String) -> CGFloat {
        let lineCount = estimatedPromptLineCount(for: prompt)
        return min(68, 38 + CGFloat(lineCount - 1) * 15)
    }

    static func inlineProcessComposerHeight(
        prompt: String,
        showsAccessibilityNotice: Bool,
        showsSendFeedback: Bool = false
    ) -> CGFloat {
        let targetHeaderHeight: CGFloat = 22
        let promptHeight = promptFieldHeight(for: prompt)
        let noticeHeight: CGFloat = showsAccessibilityNotice ? 38 + 6 : 0
        let feedbackHeight: CGFloat = showsSendFeedback ? 30 + 6 : 0
        return targetHeaderHeight + 6 + promptHeight + noticeHeight + feedbackHeight
    }

    private static func processTrayHeight(
        processItems: [CodexProcessItem],
        isLoading: Bool,
        prompt: String,
        hasProcessTarget: Bool,
        showsAccessibilityNotice: Bool,
        targetProcessID: String?,
        expandedProcessID: String?,
        showsCodexSendFeedback: Bool
    ) -> CGFloat {
        let processPanelHeight = processListHeight(
            for: processItems,
            isLoading: isLoading,
            prompt: prompt,
            hasProcessTarget: hasProcessTarget,
            showsAccessibilityNotice: showsAccessibilityNotice,
            targetProcessID: targetProcessID,
            expandedProcessID: expandedProcessID,
            showsCodexSendFeedback: showsCodexSendFeedback
        ) + processHeaderAndPaddingHeight
        let footerHeight = processFooterHeight(
            showsAccessibilityNotice: showsAccessibilityNotice,
            targetProcessID: targetProcessID
        )
        let outerPadding: CGFloat = 14 + 14
        return min(maxProcessTrayHeight, processPanelHeight + footerHeight + outerPadding)
    }

    private static let processHeaderAndPaddingHeight: CGFloat = 16

    private static func availableProcessListHeight(
        showsAccessibilityNotice: Bool,
        targetProcessID: String?
    ) -> CGFloat {
        let footerHeight = processFooterHeight(
            showsAccessibilityNotice: showsAccessibilityNotice,
            targetProcessID: targetProcessID
        )
        let outerPadding: CGFloat = 14 + 14
        let available = maxProcessTrayHeight
            - processHeaderAndPaddingHeight
            - footerHeight
            - outerPadding
        return max(46, min(maxProcessListHeight, available))
    }

    private static func processFooterHeight(
        showsAccessibilityNotice: Bool,
        targetProcessID: String?
    ) -> CGFloat {
        showsAccessibilityNotice && targetProcessID == nil ? 38 + 6 : 0
    }

    static func processRowHeight(
        for item: CodexProcessItem,
        showsActions: Bool = false,
        inlineComposerHeight: CGFloat = 0
    ) -> CGFloat {
        let collapsedHeight: CGFloat = item.goalStatus == nil ? 58 : 72
        let actionsHeight: CGFloat = item.canTargetCodexThread && showsActions ? 29 : 0
        let composerHeight = inlineComposerHeight > 0 ? inlineComposerHeight + 5 : 0
        return collapsedHeight + actionsHeight + composerHeight
    }

    private static func estimatedPromptLineCount(for prompt: String) -> Int {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return 1 }
        let wrappedLines = trimmed
            .split(separator: "\n", omittingEmptySubsequences: false)
            .map { max(1, Int(ceil(Double($0.count) / 34.0))) }
            .reduce(0, +)
        return min(3, max(1, wrappedLines))
    }
}

enum CompanionWindowStyle {
    case pet
    case utility
}

struct WindowConfigurator: NSViewRepresentable {
    var style: CompanionWindowStyle = .utility
    var isQuickBarOpen = false
    var isCodexProcessTrayVisible = false
    var isChatGPTModelPickerExpanded = false
    var isChatGPTMenuResponseVisible = false
    var isChatGPTAppHandoffVisible = false
    var isPetVisible = true
    var hasAttentionMessage = false
    var model: CompanionAppModel?

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async {
            configure(window: view.window)
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        DispatchQueue.main.async {
            configure(window: nsView.window)
        }
    }

    private func configure(window: NSWindow?) {
        guard let window else { return }
        window.title = "Codex Companion"
        window.level = .floating
        window.collectionBehavior.formUnion([.canJoinAllSpaces, .fullScreenAuxiliary])
        switch style {
        case .pet:
            window.isMovableByWindowBackground = false
        case .utility:
            window.isMovableByWindowBackground = true
        }

        switch style {
        case .pet:
            window.titleVisibility = .hidden
            window.titlebarAppearsTransparent = true
            window.setFrameAutosaveName("")
            window.styleMask = isChatGPTAppHandoffVisible
                ? [.borderless, .nonactivatingPanel]
                : [.borderless]
            window.isOpaque = false
            window.backgroundColor = .clear
            window.hasShadow = false
            clearWindowSurfaces(window)
            window.standardWindowButton(.closeButton)?.isHidden = true
            window.standardWindowButton(.miniaturizeButton)?.isHidden = true
            window.standardWindowButton(.zoomButton)?.isHidden = true
            anchorResizePetWindow(window)
            let pausesRoaming = CompanionPresentationPolicy.pausesRoaming(
                isQuickBarOpen: isQuickBarOpen,
                isPetVisible: isPetVisible,
                hasAttentionMessage: hasAttentionMessage
            )
            PetWindowRoamer.shared.update(
                window: window,
                isRoamingPaused: pausesRoaming,
                model: model
            )
            PetTrayPanel.shared.update(
                anchorWindow: window,
                model: model,
                isShown: isPetVisible && isQuickBarOpen
            )
            PetAttentionPanel.shared.update(
                anchorWindow: window,
                model: model,
                isShown: isPetVisible && !isQuickBarOpen && model?.attentionMessage != nil
            )
            if isPetVisible {
                if !window.isVisible {
                    window.orderFront(nil)
                }
            } else {
                window.orderOut(nil)
            }
        case .utility:
            window.standardWindowButton(.zoomButton)?.isHidden = true
        }
    }

    private func clearWindowSurfaces(_ window: NSWindow) {
        window.contentView?.wantsLayer = true
        window.contentView?.layer?.backgroundColor = NSColor.clear.cgColor
        window.contentView?.layer?.isOpaque = false
        window.contentView?.layer?.masksToBounds = false
        clearSubviewSurfaces(window.contentView)
        clearSubviewSurfaces(window.contentView?.superview)
    }

    private func clearSubviewSurfaces(_ view: NSView?) {
        guard let view else { return }
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.clear.cgColor
        view.layer?.isOpaque = false
        view.layer?.masksToBounds = false
        view.subviews.forEach(clearSubviewSurfaces)
    }

    private func anchorResizePetWindow(_ window: NSWindow) {
        let targetContentSize = PetWindowMetrics.contentSize(
            isQuickBarOpen: isQuickBarOpen,
            showProcesses: isCodexProcessTrayVisible
        )
        let targetFrameSize = window.frameRect(forContentRect: NSRect(origin: .zero, size: targetContentSize)).size
        var frame = window.frame

        guard abs(frame.width - targetFrameSize.width) > 0.5 || abs(frame.height - targetFrameSize.height) > 0.5 else {
            return
        }

        frame.size = targetFrameSize
        frame = clamp(frame, to: window.screen?.visibleFrame ?? NSScreen.main?.visibleFrame)
        window.setFrame(frame, display: true, animate: false)
    }

    private func clamp(_ frame: NSRect, to visibleFrame: NSRect?) -> NSRect {
        guard let visibleFrame else { return frame }
        var clamped = frame
        if clamped.minX < visibleFrame.minX {
            clamped.origin.x = visibleFrame.minX
        }
        if clamped.maxX > visibleFrame.maxX {
            clamped.origin.x = visibleFrame.maxX - clamped.width
        }
        if clamped.minY < visibleFrame.minY {
            clamped.origin.y = visibleFrame.minY
        }
        return clamped
    }
}

@MainActor
final class PetTrayPanel {
    static let shared = PetTrayPanel()

    private weak var anchorWindow: NSWindow?
    private weak var model: CompanionAppModel?
    private var panel: NSWindow?
    private var hostingView: FirstMouseHostingView<AnyView>?
    private weak var hostedModel: CompanionAppModel?
    private var currentTraySize = PetWindowMetrics.traySize(showProcesses: false)

    private init() {}

    func update(anchorWindow: NSWindow, model: CompanionAppModel?, isShown: Bool) {
        self.anchorWindow = anchorWindow
        self.model = model

        guard isShown, let model else {
            close()
            return
        }

        let panel = panel ?? makePanel()
        self.panel = panel
        let wasVisible = panel.isVisible
        let isChatGPTHandoff = model.shouldShowChatGPTAppHandoff
        let isModelPickerExpanded = model.shouldExpandQuickBarForChatGPTModelPicker
        let traySize = PetWindowMetrics.traySize(
            showProcesses: model.isCodexProcessTrayVisible,
            showModelPicker: isModelPickerExpanded,
            showChatGPTResponse: model.shouldShowChatGPTMenuResponse,
            showChatGPTAppHandoff: isChatGPTHandoff,
            processItems: model.processStore.items,
            isProcessLoading: model.processStore.isLoading,
            prompt: model.prompt,
            hasProcessTarget: model.activeProcessTarget != nil,
            showsAccessibilityNotice: model.shouldShowCodexAccessibilityNotice,
            targetProcessID: model.activeProcessTarget?.processID,
            expandedProcessID: model.hoveredProcessID,
            showsCodexSendFeedback: model.codexComposerFeedback != nil
        )
        panel.styleMask = isChatGPTHandoff ? [.borderless, .nonactivatingPanel] : [.borderless]
        if let focusableWindow = panel as? FocusableWindow {
            focusableWindow.companionModel = model
            focusableWindow.isCodexProcessTrayVisible = model.isCodexProcessTrayVisible
            focusableWindow.isChatGPTHandoffVisible = isChatGPTHandoff
            focusableWindow.traySize = traySize
        }
        panel.contentMinSize = traySize
        panel.contentMaxSize = traySize

        let previousTraySize = currentTraySize
        let nextFrame = positionedFrame(for: anchorWindow, size: traySize)
        if wasVisible {
            panel.alphaValue = 1
            setPanelFrameIfNeeded(
                panel,
                nextFrame,
                animated: previousTraySize != traySize
            )
            updateHostingView(for: panel, model: model, traySize: traySize)
            panel.orderFront(nil)
        } else {
            panel.alphaValue = 0
            panel.setFrame(nextFrame, display: true, animate: false)
            updateHostingView(for: panel, model: model, traySize: traySize)
            if isChatGPTHandoff {
                panel.orderFrontRegardless()
            } else {
                NSApp.activate(ignoringOtherApps: true)
                panel.makeKeyAndOrderFront(nil)
                if let hostingView {
                    panel.makeFirstResponder(hostingView)
                }
            }
            fadeIn(panel)
        }
        currentTraySize = traySize
        clearPanelSurface(panel)
        model.chatGPTQuickBar.updateHostFrame(model.shouldShowChatGPTAppHandoff ? nextFrame : nil)
    }

    func reposition() {
        guard let panel, panel.isVisible, let anchorWindow else { return }
        let nextFrame = positionedFrame(for: anchorWindow, size: currentTraySize)
        setPanelFrameIfNeeded(panel, nextFrame, animated: false)
        model?.chatGPTQuickBar.updateHostFrame(model?.shouldShowChatGPTAppHandoff == true ? nextFrame : nil)
    }

    private func close() {
        model?.chatGPTQuickBar.updateHostFrame(nil)
        panel?.orderOut(nil)
        panel?.alphaValue = 1
    }

    private func fadeIn(_ panel: NSWindow) {
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            context.timingFunction = CAMediaTimingFunction(name: .easeOut)
            context.allowsImplicitAnimation = true
            panel.animator().alphaValue = 1
        }
    }

    private func makePanel() -> NSWindow {
        let panel = FocusableWindow(
            contentRect: NSRect(origin: .zero, size: PetWindowMetrics.traySize(showProcesses: false)),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        panel.title = "Codex Companion Menu"
        panel.setFrameAutosaveName("")
        panel.level = .floating
        panel.collectionBehavior.formUnion([.canJoinAllSpaces, .fullScreenAuxiliary])
        panel.appearance = nil
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = false
        panel.acceptsMouseMovedEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        return panel
    }

    private func updateHostingView(for panel: NSWindow, model: CompanionAppModel, traySize: CGSize) {
        let needsRootUpdate = hostingView == nil || hostedModel !== model
        let passthroughRect = chatGPTPassthroughRect(for: model, traySize: traySize)

        if let hostingView {
            if !frame(hostingView.frame, equals: NSRect(origin: .zero, size: traySize)) {
                hostingView.frame = NSRect(origin: .zero, size: traySize)
            }
            hostingView.passthroughRect = passthroughRect
            if needsRootUpdate {
                hostingView.rootView = trayRootView(model: model, traySize: traySize)
                hostingView.layoutSubtreeIfNeeded()
            }
            if panel.contentView !== hostingView {
                panel.contentView = hostingView
            }
        } else {
            let hostingView = FirstMouseHostingView(rootView: trayRootView(model: model, traySize: traySize))
            hostingView.autoresizingMask = [.width, .height]
            hostingView.frame = NSRect(origin: .zero, size: traySize)
            hostingView.passthroughRect = passthroughRect
            hostingView.layoutSubtreeIfNeeded()
            self.hostingView = hostingView
            panel.contentView = hostingView
        }

        hostedModel = model
    }

    private func chatGPTPassthroughRect(for model: CompanionAppModel, traySize: CGSize) -> NSRect? {
        guard model.shouldShowChatGPTAppHandoff else { return nil }
        return NSRect(
            x: PetWindowMetrics.chatGPTQuickBarInset,
            y: PetWindowMetrics.chatGPTQuickBarBottomOffset,
            width: traySize.width - PetWindowMetrics.chatGPTQuickBarInset * 2,
            height: PetWindowMetrics.chatGPTQuickBarHeight
        )
    }

    private func trayRootView(model: CompanionAppModel, traySize: CGSize) -> AnyView {
        AnyView(
            QuickBarTrayView(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
        )
    }

    private func positionedFrame(for anchorWindow: NSWindow, size: CGSize? = nil) -> NSRect {
        let anchorFrame = anchorWindow.frame
        let size = size ?? PetWindowMetrics.traySize(
            showProcesses: model?.isCodexProcessTrayVisible ?? false,
            showModelPicker: model?.shouldExpandQuickBarForChatGPTModelPicker ?? false,
            showChatGPTResponse: model?.shouldShowChatGPTMenuResponse ?? false,
            showChatGPTAppHandoff: model?.shouldShowChatGPTAppHandoff ?? false,
            processItems: model?.processStore.items ?? [],
            isProcessLoading: model?.processStore.isLoading ?? false,
            prompt: model?.prompt ?? "",
            hasProcessTarget: model?.activeProcessTarget != nil,
            showsAccessibilityNotice: model?.shouldShowCodexAccessibilityNotice ?? false,
            targetProcessID: model?.activeProcessTarget?.processID,
            expandedProcessID: model?.hoveredProcessID,
            showsCodexSendFeedback: model?.codexComposerFeedback != nil
        )
        let visibleFrame = anchorWindow.screen?.visibleFrame ?? NSScreen.main?.visibleFrame

        var origin = NSPoint(
            x: anchorFrame.midX - size.width / 2,
            y: anchorFrame.maxY + PetWindowMetrics.trayGap
        )

        if let visibleFrame {
            origin = PetWindowMetrics.positionedTrayOrigin(
                anchorFrame: anchorFrame,
                traySize: size,
                visibleFrame: visibleFrame
            )
        }

        return NSRect(origin: origin, size: size)
    }

    private func setPanelFrameIfNeeded(_ panel: NSWindow, _ frame: NSRect, animated: Bool = false) {
        guard !self.frame(panel.frame, equals: frame) else { return }
        guard animated else {
            panel.setFrame(frame, display: true, animate: false)
            return
        }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            context.timingFunction = CAMediaTimingFunction(controlPoints: 0.22, 0.78, 0.24, 1)
            context.allowsImplicitAnimation = true
            panel.animator().setFrame(frame, display: true)
        }
    }

    private func frame(_ lhs: NSRect, equals rhs: NSRect) -> Bool {
        abs(lhs.origin.x - rhs.origin.x) <= 0.5
            && abs(lhs.origin.y - rhs.origin.y) <= 0.5
            && abs(lhs.size.width - rhs.size.width) <= 0.5
            && abs(lhs.size.height - rhs.size.height) <= 0.5
    }

    private func clearPanelSurface(_ panel: NSWindow) {
        panel.contentView?.wantsLayer = true
        panel.contentView?.layer?.backgroundColor = NSColor.clear.cgColor
        panel.contentView?.layer?.isOpaque = false
    }
}

@MainActor
private final class FocusableWindow: NSWindow {
    weak var companionModel: CompanionAppModel?
    var traySize = PetWindowMetrics.traySize(showProcesses: false)
    var isCodexProcessTrayVisible = false
    var isChatGPTHandoffVisible = false

    override var canBecomeKey: Bool { !isChatGPTHandoffVisible }
    override var canBecomeMain: Bool { !isChatGPTHandoffVisible }

    override func mouseDown(with event: NSEvent) {
        if !isChatGPTHandoffVisible {
            makeKey()
        }
        super.mouseDown(with: event)
    }
}

private final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    var passthroughRect: NSRect?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        if let passthroughRect, passthroughRect.contains(point) {
            return nil
        }
        return super.hitTest(point)
    }
}

@MainActor
final class PetWindowRoamer {
    static let shared = PetWindowRoamer()

    private weak var window: NSWindow?
    private weak var model: CompanionAppModel?
    private var timer: Timer?
    private var targetOrigin: NSPoint?
    private var cachedVisibleFrame: NSRect?
    private var lastBoundsRefresh = Date.distantPast
    private var enabled = false
    private var interactionHeld = false
    private var pausedUntil = Date.distantPast
    private var lastTickDate: Date?
    private var motionPrimer = PetRoamingMotionPrimer()
    private var workspaceObserverTokens: [NSObjectProtocol] = []
    private var sessionIsInactive = false
    private let pointsPerSecond: CGFloat = 54
    private let tickInterval: TimeInterval = 1.0 / 12.0

    private init() {
        let notificationCenter = NSWorkspace.shared.notificationCenter
        workspaceObserverTokens.append(
            notificationCenter.addObserver(
                forName: NSWorkspace.sessionDidResignActiveNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                Task { @MainActor in
                    self?.sessionIsInactive = true
                    self?.model?.setRoamingIdle()
                    self?.stop()
                }
            }
        )
        workspaceObserverTokens.append(
            notificationCenter.addObserver(
                forName: NSWorkspace.sessionDidBecomeActiveNotification,
                object: nil,
                queue: .main
            ) { [weak self] _ in
                Task { @MainActor in
                    guard let self else { return }
                    self.sessionIsInactive = false
                    if self.enabled {
                        self.start()
                    }
                }
            }
        )
    }

    func update(window: NSWindow, isRoamingPaused: Bool, model: CompanionAppModel?) {
        self.window = window
        self.model = model
        enabled = !isRoamingPaused

        if enabled, !shouldPauseForInactiveSession() {
            start()
        } else {
            model?.setRoamingIdle()
            stop()
        }
    }

    func pauseBriefly() {
        pausedUntil = Date().addingTimeInterval(1.5)
        targetOrigin = nil
        motionPrimer.reset()
    }

    func setInteractionHold(_ isHeld: Bool) {
        interactionHeld = isHeld
        if isHeld {
            targetOrigin = nil
            motionPrimer.reset()
        }
    }

    private func start() {
        guard timer == nil else { return }
        lastTickDate = Date()
        timer = Timer.scheduledTimer(withTimeInterval: tickInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.tick()
            }
        }
        timer?.tolerance = tickInterval * 0.25
        RunLoop.main.add(timer!, forMode: .common)
    }

    private func stop() {
        timer?.invalidate()
        timer = nil
        targetOrigin = nil
        cachedVisibleFrame = nil
        lastTickDate = nil
        motionPrimer.reset()
        model?.clearDirectionalLook()
    }

    private func tick() {
        guard enabled, let window else { return }
        if shouldPauseForInactiveSession() {
            model?.setRoamingIdle()
            stop()
            return
        }

        let now = Date()
        let deltaTime = max(1.0 / 60.0, min(1.0 / 8.0, now.timeIntervalSince(lastTickDate ?? now)))
        lastTickDate = now

        if interactionHeld {
            model?.setRoamingIdle()
            model?.clearDirectionalLook()
            return
        }
        if Date() < pausedUntil {
            model?.setRoamingIdle()
            model?.updateDirectionalLook(
                pointer: NSEvent.mouseLocation,
                petFrame: window.frame
            )
            return
        }
        guard let visibleFrame = visibleFrame(for: window) else { return }

        if targetOrigin == nil || distance(from: window.frame.origin, to: targetOrigin!) < 4 {
            model?.setRoamingIdle()
            model?.updateDirectionalLook(
                pointer: NSEvent.mouseLocation,
                petFrame: window.frame
            )
            pausedUntil = Date().addingTimeInterval(Double.random(in: 0.45...1.2))
            targetOrigin = randomTarget(in: visibleFrame, windowSize: window.frame.size)
            motionPrimer.reset()
            return
        }

        guard let targetOrigin else { return }
        model?.clearDirectionalLook()
        let origin = window.frame.origin
        let dx = targetOrigin.x - origin.x
        let dy = targetOrigin.y - origin.y
        let length = max(0.001, sqrt(dx * dx + dy * dy))
        let stepDistance = pointsPerSecond * CGFloat(deltaTime)
        let next = NSPoint(
            x: origin.x + dx / length * stepDistance,
            y: origin.y + dy / length * stepDistance
        )
        let clampedNext = clamp(origin: next, windowSize: window.frame.size, visibleFrame: visibleFrame)
        let animationAccepted = model?.setRoamingMotion(
            dx: clampedNext.x - origin.x,
            dy: clampedNext.y - origin.y
        ) ?? false
        guard motionPrimer.shouldMove(animationAccepted: animationAccepted) else {
            return
        }
        window.setFrameOrigin(clampedNext)
        if abs(clampedNext.x - next.x) > 0.5 || abs(clampedNext.y - next.y) > 0.5 {
            self.targetOrigin = nil
            motionPrimer.reset()
        }
        PetTrayPanel.shared.reposition()
        PetAttentionPanel.shared.reposition()
    }

    private func visibleFrame(for window: NSWindow) -> NSRect? {
        let now = Date()
        if let cachedVisibleFrame, now.timeIntervalSince(lastBoundsRefresh) < 1.0 {
            return cachedVisibleFrame
        }

        let nextFrame = (window.screen ?? NSScreen.main)?.visibleFrame
        cachedVisibleFrame = nextFrame
        lastBoundsRefresh = now
        return nextFrame
    }

    private func shouldPauseForInactiveSession() -> Bool {
        sessionIsInactive || Self.isScreenLocked
    }

    private static var isScreenLocked: Bool {
        guard let dictionary = CGSessionCopyCurrentDictionary() as? [String: Any] else {
            return false
        }
        return dictionary["CGSSessionScreenIsLocked"] as? Bool == true
    }

    private func randomTarget(in visibleFrame: NSRect, windowSize: NSSize) -> NSPoint {
        let minX = visibleFrame.minX + 12
        let maxX = max(minX, visibleFrame.maxX - windowSize.width - 12)
        let minY = visibleFrame.minY + 12
        let maxY = max(minY, visibleFrame.maxY - windowSize.height - 12)
        return NSPoint(
            x: CGFloat.random(in: minX...maxX),
            y: CGFloat.random(in: minY...maxY)
        )
    }

    private func distance(from origin: NSPoint, to target: NSPoint) -> CGFloat {
        let dx = target.x - origin.x
        let dy = target.y - origin.y
        return sqrt(dx * dx + dy * dy)
    }

    private func clamp(origin: NSPoint, windowSize: NSSize, visibleFrame: NSRect) -> NSPoint {
        let margin: CGFloat = 6
        let minX = visibleFrame.minX + margin
        let maxX = visibleFrame.maxX - windowSize.width - margin
        let minY = visibleFrame.minY + margin
        let maxY = visibleFrame.maxY - windowSize.height - margin
        return NSPoint(
            x: min(max(origin.x, minX), max(minX, maxX)),
            y: min(max(origin.y, minY), max(minY, maxY))
        )
    }
}
