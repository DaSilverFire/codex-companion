import AppKit
import SwiftUI

enum ChatDeliveryPresentationMetrics {
    static let panelWidth: CGFloat = 244
    static let initialHeight: CGFloat = 176
}

@MainActor
final class ChatDeliveryPanel {
    static let shared = ChatDeliveryPanel()

    private var panel: ChatDeliveryWindow?
    private var hostingView: ChatDeliveryHostingView?
    private var presentationChanged: (@MainActor (Bool) -> Void)?
    private var contentSize = CGSize(
        width: ChatDeliveryPresentationMetrics.panelWidth,
        height: ChatDeliveryPresentationMetrics.initialHeight
    )
    private var isResizing = false

    private init() {}

    func toggle(
        model: CompanionAppModel,
        presentationChanged: @escaping @MainActor (Bool) -> Void
    ) {
        if panel?.isVisible == true {
            dismiss()
        } else {
            present(model: model, presentationChanged: presentationChanged)
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
        hostingView?.frame = NSRect(origin: .zero, size: frame.size)
    }

    private func present(
        model: CompanionAppModel,
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
            ChatDeliveryPicker(model: model) { [weak self] in
                self?.dismiss()
            }
            .fixedSize(horizontal: false, vertical: true)
        )

        if let hostingView {
            hostingView.rootView = rootView
        } else {
            let hostingView = ChatDeliveryHostingView(rootView: rootView)
            hostingView.frame = NSRect(origin: .zero, size: contentSize)
            hostingView.contentSizeDidChange = { [weak self] in
                self?.resizeToFit()
            }
            self.hostingView = hostingView
            panel.contentView = hostingView
        }

        hostingView?.needsLayout = true
        hostingView?.layoutSubtreeIfNeeded()
        resizeToFit()
        reposition()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        if let hostingView {
            panel.makeFirstResponder(hostingView)
        }
        presentationChanged(true)
    }

    private func resizeToFit() {
        guard !isResizing, let hostingView else { return }
        isResizing = true
        defer { isResizing = false }

        hostingView.layoutSubtreeIfNeeded()
        let measuredHeight = ceil(max(1, hostingView.fittingSize.height))
        let nextSize = CGSize(
            width: ChatDeliveryPresentationMetrics.panelWidth,
            height: measuredHeight
        )
        guard abs(contentSize.height - nextSize.height) > 0.5 else { return }
        contentSize = nextSize
        reposition()
    }

    private func makePanel() -> ChatDeliveryWindow {
        let panel = ChatDeliveryWindow(
            contentRect: NSRect(origin: .zero, size: contentSize),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        panel.title = "Chat service and model"
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
private final class ChatDeliveryWindow: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}

@MainActor
private final class ChatDeliveryHostingView: NSHostingView<AnyView> {
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
