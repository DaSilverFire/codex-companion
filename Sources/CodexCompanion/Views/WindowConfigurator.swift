import AppKit
import Combine
import CoreGraphics
import QuartzCore
import SwiftUI

enum PetWindowMetrics {
    static let petSize = CGSize(width: 124, height: 124)
    static let menuControlRowHeight: CGFloat = 40
    static let menuControlHitSize = CGSize(width: 36, height: 48)
    static let menuControlVisualDiameter: CGFloat = 28
    static let menuUtilityButtonWidth: CGFloat = menuControlHitSize.width
    static let menuControlSpacing: CGFloat = 4
    static let menuControlTrailingInset: CGFloat = 4
    static let menuControlVerticalOffset: CGFloat = 8
    static let trayBaselineDrop: CGFloat = 18
    static let trayControlRevealLift: CGFloat = 28
    static let activeProcessBadgeVerticalOffset: CGFloat = -0.5
    static let menuMotionResponse: Double = 0.42
    static let menuMotionDampingFraction: Double = 0.88
    static let menuMotionBlendDuration: Double = 0.10
    static let menuMotionDuration: TimeInterval = 0.62
    static let petDragHandleSize = CGSize(width: 112, height: 112)
    static let petArtworkInset: CGFloat = 8
    static let trayWidth: CGFloat = 292
    static let compactTrayHeight: CGFloat = 94
    static let chatGPTResponseTrayHeight: CGFloat = 420
    static let chatGPTResponseModelPickerTrayHeight: CGFloat = 546
    static let modelPickerTrayHeight: CGFloat = 328
    static let maxProcessTrayHeight: CGFloat = 416
    static let maximumProcessCardsWithoutScrolling = 3
    static let maxProcessListHeight: CGFloat =
        CGFloat(maximumProcessCardsWithoutScrolling) * 72
        + CGFloat(maximumProcessCardsWithoutScrolling - 1) * 6
        + 2
    static let trayGap: CGFloat = 10

    // Frames use the SwiftUI root view's top-leading coordinate space.
    static var menuArrowHoverFrame: CGRect {
        CGRect(
            x: petSize.width - menuControlTrailingInset - menuControlHitSize.width,
            y: menuControlVerticalOffset,
            width: menuControlHitSize.width,
            height: menuControlHitSize.height
        )
    }

    static var menuArrowVisualFrame: CGRect {
        menuArrowHoverFrame.insetBy(
            dx: (menuControlHitSize.width - menuControlVisualDiameter) / 2,
            dy: (menuControlHitSize.height - menuControlVisualDiameter) / 2
        )
    }

    static var petDragHandleFrame: CGRect {
        CGRect(
            x: (petSize.width - petDragHandleSize.width) / 2,
            y: menuControlRowHeight + (petSize.height - petDragHandleSize.height) / 2,
            width: petDragHandleSize.width,
            height: petDragHandleSize.height
        )
    }

    static var petArtworkTop: CGFloat {
        menuControlRowHeight + petArtworkInset
    }

    static func menuControlClusterHoverFrame(isQuickBarOpen: Bool) -> CGRect {
        let utilityCount: CGFloat = isQuickBarOpen ? 2 : 0
        let width = menuControlHitSize.width
            + utilityCount * menuUtilityButtonWidth
            + utilityCount * menuControlSpacing
        return CGRect(
            x: petSize.width - menuControlTrailingInset - width,
            y: menuControlVerticalOffset,
            width: width,
            height: menuControlHitSize.height
        )
    }

    static func screenFrame(fromTopLeadingFrame frame: CGRect, in windowFrame: CGRect) -> CGRect {
        CGRect(
            x: windowFrame.minX + frame.minX,
            y: windowFrame.maxY - frame.maxY,
            width: frame.width,
            height: frame.height
        )
    }

    static func petDragHandleScreenFrame(in windowFrame: CGRect) -> CGRect {
        screenFrame(fromTopLeadingFrame: petDragHandleFrame, in: windowFrame)
    }

    static func menuControlClusterScreenFrame(
        in windowFrame: CGRect,
        isQuickBarOpen: Bool
    ) -> CGRect {
        screenFrame(
            fromTopLeadingFrame: menuControlClusterHoverFrame(isQuickBarOpen: isQuickBarOpen),
            in: windowFrame
        )
    }

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

    static func trayAnchorFrame(
        for petWindowFrame: NSRect,
        areMenuControlsVisible: Bool
    ) -> NSRect {
        let controlRowDrop = areMenuControlsVisible ? 0 : trayControlRevealLift
        return NSRect(
            x: petWindowFrame.minX,
            y: petWindowFrame.minY,
            width: petWindowFrame.width,
            height: max(0, petWindowFrame.height - trayBaselineDrop - controlRowDrop)
        )
    }

    static func contentSize(isQuickBarOpen: Bool, showProcesses: Bool) -> CGSize {
        return CGSize(
            width: petSize.width,
            height: petSize.height + menuControlRowHeight
        )
    }

    static func traySize(
        showProcesses: Bool,
        showModelPicker: Bool = false,
        showChatGPTResponse: Bool = false,
        processItems: [CodexProcessItem] = [],
        isProcessLoading: Bool = false,
        prompt: String = "",
        hasProcessTarget: Bool = false,
        showsAccessibilityNotice: Bool = false,
        targetProcessID: String? = nil,
        expandedProcessID: String? = nil,
        showsCodexSendFeedback: Bool = false
    ) -> CGSize {
        let width = trayWidth
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

enum CompanionWindowFramePersistence {
    static let autosaveName = "CodexCompanionPetWindow"
    static let storageKey = "CodexCompanionPetWindowFrame"

    @MainActor
    @discardableResult
    static func restore(_ window: NSWindow) -> Bool {
        window.setFrameAutosaveName(autosaveName)
        if let rawFrame = UserDefaults.standard.string(forKey: storageKey) {
            let frame = NSRectFromString(rawFrame)
            if !frame.isEmpty, NSScreen.screens.contains(where: { $0.visibleFrame.intersects(frame) }) {
                window.setFrame(frame, display: false)
                return true
            }
        }
        return window.setFrameUsingName(autosaveName)
    }

    @MainActor
    static func save(_ window: NSWindow) {
        window.saveFrame(usingName: autosaveName)
        UserDefaults.standard.set(NSStringFromRect(window.frame), forKey: storageKey)
    }

    @MainActor
    static func seedIfNeeded(_ window: NSWindow, restoredExistingFrame: Bool) {
        guard !restoredExistingFrame else { return }
        save(window)
    }
}

struct CompanionWindowRestorationTracker {
    private var restoredWindowIDs: Set<ObjectIdentifier> = []

    mutating func shouldRestore(_ window: AnyObject) -> Bool {
        restoredWindowIDs.insert(ObjectIdentifier(window)).inserted
    }
}

struct WindowConfigurator: NSViewRepresentable {
    var style: CompanionWindowStyle = .utility
    var isQuickBarOpen = false
    var isCodexProcessTrayVisible = false
    var isChatGPTModelPickerExpanded = false
    var isChatGPTMenuResponseVisible = false
    var isPetVisible = true
    var hasAttentionMessage = false
    var model: CompanionAppModel?

    final class Coordinator {
        var restorationTracker = CompanionWindowRestorationTracker()
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async {
            configure(window: view.window, coordinator: context.coordinator)
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        DispatchQueue.main.async {
            configure(window: nsView.window, coordinator: context.coordinator)
        }
    }

    private func configure(window: NSWindow?, coordinator: Coordinator) {
        guard let window else { return }
        var restoredExistingPetFrame: Bool?
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
            PetWindowHostingViewSizingPolicy.apply(to: window)
            window.titleVisibility = .hidden
            window.titlebarAppearsTransparent = true
            if coordinator.restorationTracker.shouldRestore(window) {
                restoredExistingPetFrame = CompanionWindowFramePersistence.restore(window)
            }
            window.styleMask = [.borderless]
            window.isOpaque = false
            window.backgroundColor = .clear
            window.hasShadow = false
            clearWindowSurfaces(window)
            window.standardWindowButton(.closeButton)?.isHidden = true
            window.standardWindowButton(.miniaturizeButton)?.isHidden = true
            window.standardWindowButton(.zoomButton)?.isHidden = true
            anchorResizePetWindow(window)
            if let restoredExistingPetFrame {
                CompanionWindowFramePersistence.seedIfNeeded(
                    window,
                    restoredExistingFrame: restoredExistingPetFrame
                )
            }
            let pausesRoaming = CompanionPresentationPolicy.pausesRoaming(
                isQuickBarOpen: isQuickBarOpen,
                isPetVisible: isPetVisible,
                hasAttentionMessage: hasAttentionMessage,
                allowsAutonomousMovement: model?.allowsAutonomousPetMovement ?? true
            )
            let allowsDirectionalLookTracking = CompanionPresentationPolicy.allowsDirectionalLookTracking(
                isQuickBarOpen: isQuickBarOpen,
                isPetVisible: isPetVisible,
                hasAttentionMessage: hasAttentionMessage
            )
            let requiresPointerHoverReconciliation =
                CompanionPresentationPolicy.requiresPointerHoverReconciliation(
                    isQuickBarOpen: isQuickBarOpen,
                    isPetVisible: isPetVisible
                )
            PetWindowRoamer.shared.update(
                window: window,
                isRoamingPaused: pausesRoaming,
                isDirectionalLookTrackingAllowed: allowsDirectionalLookTracking,
                isPointerHoverReconciliationRequired: requiresPointerHoverReconciliation,
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
private protocol HostingViewSizingControlling: AnyObject {
    var sizingOptions: NSHostingSizingOptions { get set }
}

extension NSHostingView: HostingViewSizingControlling {}

@MainActor
enum PetWindowHostingViewSizingPolicy {
    static func apply(to window: NSWindow) {
        apply(to: window.contentView)
    }

    private static func apply(to view: NSView?) {
        guard let view else { return }
        if let hostingView = view as? any HostingViewSizingControlling {
            hostingView.sizingOptions = []
        }
        view.subviews.forEach { apply(to: $0) }
    }
}

@MainActor
enum PetTrayHostingViewSizingPolicy {
    static func apply<Content: View>(to hostingView: NSHostingView<Content>) {
        hostingView.sizingOptions = []
    }

    @discardableResult
    static func updateFrame<Content: View>(
        of hostingView: NSHostingView<Content>,
        to size: CGSize
    ) -> Bool {
        let frame = NSRect(origin: .zero, size: size)
        guard hostingView.frame != frame else { return false }
        hostingView.frame = frame
        hostingView.needsLayout = true
        hostingView.layoutSubtreeIfNeeded()
        return true
    }
}

@MainActor
final class PetTrayProcessRefreshObserver {
    private weak var observedStore: CodexProcessStore?
    private var cancellable: AnyCancellable?

    func observe(
        _ store: CodexProcessStore,
        refresh: @escaping @MainActor () -> Void
    ) {
        guard observedStore !== store else { return }
        observedStore = store
        cancellable = store.objectWillChange.sink { _ in
            DispatchQueue.main.async {
                refresh()
            }
        }
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
    private var areMenuControlsVisible = true
    private var originAnimationTimer: Timer?
    private let processRefreshObserver = PetTrayProcessRefreshObserver()

    private init() {}

    func update(anchorWindow: NSWindow, model: CompanionAppModel?, isShown: Bool) {
        self.anchorWindow = anchorWindow
        self.model = model

        if let model {
            processRefreshObserver.observe(model.processStore) { [weak self] in
                guard
                    let self,
                    let anchorWindow = self.anchorWindow,
                    let model = self.model,
                    self.panel?.isVisible == true
                else { return }
                self.update(anchorWindow: anchorWindow, model: model, isShown: true)
            }
        }

        guard isShown, let model else {
            close()
            return
        }

        let panel = panel ?? makePanel()
        self.panel = panel
        let wasVisible = panel.isVisible
        let nextMenuControlsVisible = model.shouldShowPetMenuButton
        let didMenuControlVisibilityChange = wasVisible
            && nextMenuControlsVisible != areMenuControlsVisible
        let isModelPickerExpanded = model.shouldExpandQuickBarForChatGPTModelPicker
        let traySize = PetWindowMetrics.traySize(
            showProcesses: model.isCodexProcessTrayVisible,
            showModelPicker: isModelPickerExpanded,
            showChatGPTResponse: model.shouldShowChatGPTMenuResponse,
            processItems: model.processStore.items,
            isProcessLoading: model.processStore.isLoading,
            prompt: model.prompt,
            hasProcessTarget: model.activeProcessTarget != nil,
            showsAccessibilityNotice: model.shouldShowCodexAccessibilityNotice,
            targetProcessID: model.activeProcessTarget?.processID,
            expandedProcessID: model.hoveredProcessID,
            showsCodexSendFeedback: model.codexComposerFeedback != nil
        )
        panel.styleMask = [.borderless]
        if let focusableWindow = panel as? FocusableWindow {
            focusableWindow.companionModel = model
            focusableWindow.isCodexProcessTrayVisible = model.isCodexProcessTrayVisible
            focusableWindow.traySize = traySize
        }
        panel.contentMinSize = traySize
        panel.contentMaxSize = traySize

        let nextFrame = positionedFrame(
            for: anchorWindow,
            size: traySize,
            areMenuControlsVisible: nextMenuControlsVisible
        )
        if wasVisible {
            panel.alphaValue = 1
            setPanelFrameIfNeeded(
                panel,
                nextFrame,
                animated: didMenuControlVisibilityChange
            )
            updateHostingView(for: panel, model: model, traySize: traySize)
            panel.orderFront(nil)
        } else {
            panel.alphaValue = 0
            panel.setFrame(nextFrame, display: true, animate: false)
            updateHostingView(for: panel, model: model, traySize: traySize)
            NSApp.activate(ignoringOtherApps: true)
            panel.makeKeyAndOrderFront(nil)
            if let hostingView {
                panel.makeFirstResponder(hostingView)
            }
            fadeIn(panel)
        }
        currentTraySize = traySize
        areMenuControlsVisible = nextMenuControlsVisible
        clearPanelSurface(panel)
    }

    func reposition() {
        guard let panel, panel.isVisible, let anchorWindow else { return }
        let nextFrame = positionedFrame(
            for: anchorWindow,
            size: currentTraySize,
            areMenuControlsVisible: areMenuControlsVisible
        )
        setPanelFrameIfNeeded(panel, nextFrame)
    }

    func setMenuControlsVisible(_ isVisible: Bool) {
        guard areMenuControlsVisible != isVisible else { return }
        areMenuControlsVisible = isVisible
        guard let panel, panel.isVisible, let anchorWindow else { return }

        let nextFrame = positionedFrame(
            for: anchorWindow,
            size: currentTraySize,
            areMenuControlsVisible: isVisible
        )
        setPanelFrameIfNeeded(panel, nextFrame, animated: true)
    }

    private func close() {
        stopOriginAnimation()
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

        if let hostingView {
            PetTrayHostingViewSizingPolicy.apply(to: hostingView)
            PetTrayHostingViewSizingPolicy.updateFrame(of: hostingView, to: traySize)
            if needsRootUpdate {
                hostingView.rootView = trayRootView(model: model, traySize: traySize)
                hostingView.layoutSubtreeIfNeeded()
            }
            if panel.contentView !== hostingView {
                panel.contentView = hostingView
            }
        } else {
            let hostingView = FirstMouseHostingView(rootView: trayRootView(model: model, traySize: traySize))
            PetTrayHostingViewSizingPolicy.apply(to: hostingView)
            hostingView.autoresizingMask = [.width, .height]
            hostingView.frame = NSRect(origin: .zero, size: traySize)
            hostingView.layoutSubtreeIfNeeded()
            self.hostingView = hostingView
            panel.contentView = hostingView
        }

        hostedModel = model
    }

    private func trayRootView(model: CompanionAppModel, traySize: CGSize) -> AnyView {
        AnyView(
            QuickBarTrayView(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
        )
    }

    private func positionedFrame(
        for anchorWindow: NSWindow,
        size: CGSize? = nil,
        areMenuControlsVisible: Bool? = nil
    ) -> NSRect {
        let anchorFrame = PetWindowMetrics.trayAnchorFrame(
            for: anchorWindow.frame,
            areMenuControlsVisible: areMenuControlsVisible ?? self.areMenuControlsVisible
        )
        let size = size ?? PetWindowMetrics.traySize(
            showProcesses: model?.isCodexProcessTrayVisible ?? false,
            showModelPicker: model?.shouldExpandQuickBarForChatGPTModelPicker ?? false,
            showChatGPTResponse: model?.shouldShowChatGPTMenuResponse ?? false,
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

    private func setPanelFrameIfNeeded(
        _ panel: NSWindow,
        _ frame: NSRect,
        animated: Bool = false
    ) {
        guard !self.frame(panel.frame, equals: frame) else { return }
        let keepsCurrentSize = abs(panel.frame.width - frame.width) <= 0.5
            && abs(panel.frame.height - frame.height) <= 0.5
        guard animated, keepsCurrentSize else {
            stopOriginAnimation()
            panel.setFrame(frame, display: true, animate: false)
            return
        }
        guard !NSWorkspace.shared.accessibilityDisplayShouldReduceMotion else {
            stopOriginAnimation()
            panel.setFrameOrigin(frame.origin)
            return
        }
        animatePanelOrigin(panel, to: frame.origin)
    }

    private func animatePanelOrigin(_ panel: NSWindow, to targetOrigin: NSPoint) {
        stopOriginAnimation()
        let startOrigin = panel.frame.origin
        let startTime = CACurrentMediaTime()
        let duration = PetWindowMetrics.menuMotionDuration

        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self, weak panel] timer in
            MainActor.assumeIsolated {
                guard let self, let panel, panel === self.panel else {
                    timer.invalidate()
                    return
                }
                let elapsed = CACurrentMediaTime() - startTime
                guard elapsed < duration else {
                    panel.setFrameOrigin(targetOrigin)
                    timer.invalidate()
                    if self.originAnimationTimer === timer {
                        self.originAnimationTimer = nil
                    }
                    return
                }

                let progress = self.springProgress(at: elapsed)
                let interpolatedOrigin = NSPoint(
                    x: startOrigin.x + (targetOrigin.x - startOrigin.x) * progress,
                    y: startOrigin.y + (targetOrigin.y - startOrigin.y) * progress
                )
                panel.setFrameOrigin(interpolatedOrigin)
            }
        }
        timer.tolerance = 1.0 / 240.0
        originAnimationTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    private func springProgress(at elapsed: TimeInterval) -> CGFloat {
        let response = PetWindowMetrics.menuMotionResponse
        let dampingFraction = PetWindowMetrics.menuMotionDampingFraction
        let angularFrequency = 2 * Double.pi / response
        let dampedScale = sqrt(1 - dampingFraction * dampingFraction)
        let dampedFrequency = angularFrequency * dampedScale
        let decay = exp(-dampingFraction * angularFrequency * elapsed)
        let displacement = decay * (
            cos(dampedFrequency * elapsed)
                + dampingFraction / dampedScale * sin(dampedFrequency * elapsed)
        )
        return CGFloat(1 - displacement)
    }

    private func stopOriginAnimation() {
        originAnimationTimer?.invalidate()
        originAnimationTimer = nil
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

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }

    override func mouseDown(with event: NSEvent) {
        makeKey()
        super.mouseDown(with: event)
    }
}

private final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
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
    private var isRoamingEnabled = false
    private var isDirectionalLookTrackingAllowed = false
    private var isPointerHoverReconciliationRequired = false
    private var interactionHeld = false
    private var pausedUntil = Date.distantPast
    private var lastTickDate: Date?
    private var lastFrameSaveDate = Date.distantPast
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
                    self?.interactionHeld = false
                    self?.model?.clearPetHoverState()
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
                    if self.shouldRunTimer {
                        self.start()
                    }
                }
            }
        )
    }

    func update(
        window: NSWindow,
        isRoamingPaused: Bool,
        isDirectionalLookTrackingAllowed: Bool,
        isPointerHoverReconciliationRequired: Bool,
        model: CompanionAppModel?
    ) {
        self.window = window
        self.model = model
        isRoamingEnabled = !isRoamingPaused
        self.isDirectionalLookTrackingAllowed = isDirectionalLookTrackingAllowed
        self.isPointerHoverReconciliationRequired = isPointerHoverReconciliationRequired

        if shouldRunTimer, !shouldPauseForInactiveSession() {
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

    func setAutonomousMovementAllowed(_ isAllowed: Bool) {
        guard !isAllowed else { return }
        isRoamingEnabled = false
        targetOrigin = nil
        motionPrimer.reset()
        model?.setRoamingIdle()
        if (isDirectionalLookTrackingAllowed || isPointerHoverReconciliationRequired),
           !shouldPauseForInactiveSession() {
            start()
        } else {
            stop()
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
        if let window {
            CompanionWindowFramePersistence.save(window)
        }
        timer?.invalidate()
        timer = nil
        targetOrigin = nil
        cachedVisibleFrame = nil
        lastTickDate = nil
        motionPrimer.reset()
        model?.clearDirectionalLook()
    }

    private func tick() {
        guard shouldRunTimer, let window else { return }
        if shouldPauseForInactiveSession() {
            model?.setRoamingIdle()
            stop()
            return
        }

        reconcilePointerHover(for: window)

        let now = Date()
        let deltaTime = max(1.0 / 60.0, min(1.0 / 8.0, now.timeIntervalSince(lastTickDate ?? now)))
        lastTickDate = now

        if interactionHeld {
            model?.setRoamingIdle()
            model?.clearDirectionalLook()
            return
        }
        if !isRoamingEnabled {
            model?.setRoamingIdle()
            if isDirectionalLookTrackingAllowed {
                model?.updateDirectionalLook(
                    pointer: NSEvent.mouseLocation,
                    petFrame: window.frame
                )
            } else {
                model?.clearDirectionalLook()
            }
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
        if now.timeIntervalSince(lastFrameSaveDate) >= 1 {
            CompanionWindowFramePersistence.save(window)
            lastFrameSaveDate = now
        }
        if abs(clampedNext.x - next.x) > 0.5 || abs(clampedNext.y - next.y) > 0.5 {
            self.targetOrigin = nil
            motionPrimer.reset()
        }
        PetTrayPanel.shared.reposition()
        PetAttentionPanel.shared.reposition()
    }

    private func reconcilePointerHover(for window: NSWindow) {
        guard NSEvent.pressedMouseButtons == 0, let model else { return }
        let pointer = NSEvent.mouseLocation
        let isPetHovered = PetWindowMetrics.petDragHandleScreenFrame(in: window.frame)
            .contains(pointer)
        let isMenuHovered = model.shouldShowPetMenuButton
            && PetWindowMetrics.menuControlClusterScreenFrame(
                in: window.frame,
                isQuickBarOpen: model.isQuickBarOpen
            ).contains(pointer)

        model.setPetHovering(isPetHovered)
        model.setPetMenuControlHovering(isMenuHovered)
        interactionHeld = isPetHovered || isMenuHovered
        PetTrayPanel.shared.setMenuControlsVisible(model.shouldShowPetMenuButton)
    }

    private var shouldRunTimer: Bool {
        isRoamingEnabled
            || isDirectionalLookTrackingAllowed
            || isPointerHoverReconciliationRequired
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
