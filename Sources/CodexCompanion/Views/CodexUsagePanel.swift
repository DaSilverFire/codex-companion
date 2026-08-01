import AppKit
import SwiftUI

enum CodexUsagePanelPositioningPolicy {
    static let screenMargin: CGFloat = 8

    static func frame(
        contentSize: CGSize,
        processTrayFrame: NSRect,
        petWindowFrame: NSRect,
        visibleFrame: NSRect
    ) -> NSRect {
        let width = min(contentSize.width, visibleFrame.width - screenMargin * 2)
        let safeBottomY = max(
            processTrayFrame.minY,
            petWindowFrame.maxY + screenMargin
        )
        let availableHeight = max(
            1,
            visibleFrame.maxY - safeBottomY - screenMargin
        )
        let height = min(contentSize.height, availableHeight)
        var origin = NSPoint(
            x: processTrayFrame.midX - width / 2,
            y: safeBottomY
        )
        origin.x = min(
            max(origin.x, visibleFrame.minX + screenMargin),
            visibleFrame.maxX - width - screenMargin
        )
        origin.y = max(origin.y, visibleFrame.minY + screenMargin)
        return NSRect(origin: origin, size: CGSize(width: width, height: height))
    }
}

@MainActor
final class CodexUsagePanel {
    static let shared = CodexUsagePanel()

    private var panel: CodexUsageWindow?
    private var hostingView: CodexUsageHostingView?
    private var scrollView: NSScrollView?
    private var presentationChanged: (@MainActor (Bool) -> Void)?
    private var contentSize = CGSize(
        width: CodexUsagePresentationMetrics.popoverWidth,
        height: 260
    )
    private var isResizing = false

    private init() {}

    func toggle(
        store: CodexRateLimitStore,
        selectionChanged: @escaping @MainActor () -> Void,
        presentationChanged: @escaping @MainActor (Bool) -> Void
    ) {
        if panel?.isVisible == true {
            dismiss()
        } else {
            present(
                store: store,
                selectionChanged: selectionChanged,
                presentationChanged: presentationChanged
            )
        }
    }

    func dismiss() {
        guard panel?.isVisible == true else {
            presentationChanged?(false)
            presentationChanged = nil
            return
        }
        panel?.orderOut(nil)
        presentationChanged?(false)
        presentationChanged = nil
    }

    func reposition() {
        guard
            let panel,
            let geometry = PetTrayPanel.shared.usagePresentationGeometry
        else { return }
        let frame = CodexUsagePanelPositioningPolicy.frame(
            contentSize: contentSize,
            processTrayFrame: geometry.processTrayFrame,
            petWindowFrame: geometry.petWindowFrame,
            visibleFrame: geometry.visibleFrame
        )
        panel.contentMinSize = frame.size
        panel.contentMaxSize = frame.size
        if panel.frame != frame {
            panel.setFrame(frame, display: true, animate: false)
        }
        updateViewport(frame.size)
    }

    private func present(
        store: CodexRateLimitStore,
        selectionChanged: @escaping @MainActor () -> Void,
        presentationChanged: @escaping @MainActor (Bool) -> Void
    ) {
        guard PetTrayPanel.shared.usagePresentationGeometry != nil else {
            presentationChanged(false)
            return
        }

        self.presentationChanged = presentationChanged
        let panel = panel ?? makePanel()
        self.panel = panel
        let rootView = AnyView(
            CodexUsagePopover(
                store: store,
                defaults: CodexUsageProfileDefaults.applicationDefaults(),
                selectionChanged: selectionChanged
            )
            .fixedSize(horizontal: false, vertical: true)
        )

        if let hostingView {
            hostingView.rootView = rootView
        } else {
            let hostingView = CodexUsageHostingView(rootView: rootView)
            hostingView.frame = NSRect(origin: .zero, size: contentSize)
            hostingView.isFlipped = true
            hostingView.contentSizeDidChange = { [weak self] in
                self?.resizeToFit()
            }
            let scrollView = NSScrollView(frame: NSRect(origin: .zero, size: contentSize))
            scrollView.drawsBackground = false
            scrollView.hasHorizontalScroller = false
            scrollView.hasVerticalScroller = true
            scrollView.autohidesScrollers = true
            scrollView.scrollerStyle = .overlay
            scrollView.horizontalScrollElasticity = .none
            scrollView.verticalScrollElasticity = .automatic
            scrollView.autoresizingMask = [.width, .height]
            scrollView.documentView = hostingView
            self.hostingView = hostingView
            self.scrollView = scrollView
            panel.contentView = scrollView
        }

        hostingView?.needsLayout = true
        hostingView?.layoutSubtreeIfNeeded()
        resizeToFit()
        reposition()
        scrollToTop()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        if let hostingView {
            panel.makeFirstResponder(hostingView)
        }
        presentationChanged(true)
        DispatchQueue.main.async { [weak self] in
            self?.scrollToTop()
        }
    }

    private func updateViewport(_ viewportSize: CGSize) {
        guard let scrollView, let hostingView else { return }
        scrollView.frame = NSRect(origin: .zero, size: viewportSize)
        let documentSize = CGSize(
            width: viewportSize.width,
            height: max(contentSize.height, viewportSize.height)
        )
        if hostingView.frame.size != documentSize {
            hostingView.frame = NSRect(origin: .zero, size: documentSize)
        }
        scrollView.tile()
    }

    private func scrollToTop() {
        guard let scrollView else { return }
        scrollView.contentView.scroll(to: .zero)
        scrollView.reflectScrolledClipView(scrollView.contentView)
    }

    private func resizeToFit() {
        guard !isResizing, let hostingView else { return }
        isResizing = true
        defer { isResizing = false }

        hostingView.layoutSubtreeIfNeeded()
        let fittingSize = hostingView.fittingSize
        let measuredHeight = ceil(max(1, fittingSize.height))
        let nextSize = CGSize(
            width: CodexUsagePresentationMetrics.popoverWidth,
            height: measuredHeight
        )
        guard abs(contentSize.height - nextSize.height) > 0.5 else { return }
        contentSize = nextSize
        reposition()
    }

    private func makePanel() -> CodexUsageWindow {
        let panel = CodexUsageWindow(
            contentRect: NSRect(origin: .zero, size: contentSize),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        panel.title = "Codex usage and resets"
        panel.level = NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue + 1)
        panel.collectionBehavior.formUnion([.canJoinAllSpaces, .fullScreenAuxiliary])
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.ignoresMouseEvents = false
        panel.acceptsMouseMovedEvents = true
        return panel
    }
}

@MainActor
private final class CodexUsageWindow: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}

@MainActor
private final class CodexUsageHostingView: NSHostingView<AnyView> {
    var contentSizeDidChange: (@MainActor () -> Void)?
    private var contentSizeUpdateScheduled = false

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }

    override func layout() {
        super.layout()
        scheduleContentSizeUpdate()
    }

    override func invalidateIntrinsicContentSize() {
        super.invalidateIntrinsicContentSize()
        scheduleContentSizeUpdate()
    }

    private func scheduleContentSizeUpdate() {
        guard !contentSizeUpdateScheduled else { return }
        contentSizeUpdateScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            contentSizeUpdateScheduled = false
            contentSizeDidChange?()
        }
    }
}
