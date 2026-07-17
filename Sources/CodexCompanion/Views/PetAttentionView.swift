import AppKit
import SwiftUI

struct PetAttentionView: View {
    @ObservedObject var model: CompanionAppModel

    var body: some View {
        Group {
            if let message = model.attentionMessage {
                Button {
                    model.openAttentionMessage()
                } label: {
                    HStack(alignment: .top, spacing: 9) {
                        Image(systemName: message.kind.symbolName)
                            .font(.system(size: 12, weight: .bold))
                            .foregroundStyle(message.kind.tint)
                            .frame(width: 16)
                            .padding(.top, 1)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(message.title)
                                .font(.system(size: 11, weight: .semibold))
                                .lineLimit(3)
                                .fixedSize(horizontal: false, vertical: true)
                            Text(message.supportingText)
                                .font(.system(size: 9, weight: .medium))
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.tail)
                                .help(message.supportingText)
                        }

                        Spacer(minLength: 2)

                        Image(systemName: "chevron.right")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundStyle(.secondary)
                    }
                    .padding(.horizontal, 9)
                    .padding(.vertical, 7)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                }
                .buttonStyle(.plain)
                .attentionSurface(tint: message.kind.tint)
                .help(message.supportingText)
                .accessibilityLabel("\(message.title). \(message.supportingText)")
                .accessibilityHint("Open Codex processes")
                .transition(.opacity.combined(with: .scale(scale: 0.96, anchor: .bottom)))
            }
        }
        .padding(3)
        .animation(.smooth(duration: 0.20, extraBounce: 0.08), value: model.attentionMessage?.id)
    }
}

enum PetAttentionLayout {
    static let minimumSize = CGSize(width: 190, height: 52)
    static let maximumWidth: CGFloat = 286
    static let maximumHeight: CGFloat = 112
    static let screenMargin: CGFloat = 8

    private static let horizontalTextChrome: CGFloat = 58
    private static let verticalChrome: CGFloat = 18
    private static let maximumSupportingTextWidth: CGFloat = 170
    private static let titleFont = NSFont.systemFont(ofSize: 11, weight: .semibold)
    private static let supportingFont = NSFont.systemFont(ofSize: 9, weight: .medium)

    static func panelSize(
        for message: PetAttentionMessage,
        visibleFrame: NSRect? = nil
    ) -> CGSize {
        let maximumPanelWidth = min(
            maximumWidth,
            max(minimumSize.width, (visibleFrame?.width ?? maximumWidth) - 2 * screenMargin)
        )
        let unwrappedTextWidth = max(
            singleLineWidth(message.title, font: titleFont),
            min(
                maximumSupportingTextWidth,
                singleLineWidth(message.supportingText, font: supportingFont)
            )
        )
        let width = min(
            maximumPanelWidth,
            max(minimumSize.width, ceil(unwrappedTextWidth + horizontalTextChrome))
        )
        let textWidth = max(1, width - horizontalTextChrome)
        let titleHeight = wrappedHeight(message.title, font: titleFont, width: textWidth)
        let supportingHeight = wrappedHeight(
            message.supportingText,
            font: supportingFont,
            width: textWidth
        )
        let singleSupportingLineHeight = min(
            supportingHeight,
            ceil(supportingFont.ascender - supportingFont.descender + supportingFont.leading)
        )
        let desiredHeight = ceil(max(18, titleHeight + 2 + singleSupportingLineHeight) + verticalChrome)
        let maximumPanelHeight = max(
            minimumSize.height,
            min(
                maximumHeight,
                (visibleFrame?.height ?? desiredHeight) - 2 * screenMargin
            )
        )

        return CGSize(
            width: width,
            height: min(maximumPanelHeight, max(minimumSize.height, desiredHeight))
        )
    }

    private static func singleLineWidth(_ text: String, font: NSFont) -> CGFloat {
        (text as NSString).size(withAttributes: [.font: font]).width
    }

    private static func wrappedHeight(_ text: String, font: NSFont, width: CGFloat) -> CGFloat {
        guard !text.isEmpty else { return 0 }
        return ceil((text as NSString).boundingRect(
            with: CGSize(width: width, height: .greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading],
            attributes: [.font: font]
        ).height)
    }
}

@MainActor
final class PetAttentionPanel {
    static let shared = PetAttentionPanel()

    private weak var anchorWindow: NSWindow?
    private weak var model: CompanionAppModel?
    private var panel: NSPanel?
    private var hostingView: PetAttentionHostingView?
    private var panelSize = PetAttentionLayout.minimumSize

    private init() {}

    func update(anchorWindow: NSWindow, model: CompanionAppModel?, isShown: Bool) {
        self.anchorWindow = anchorWindow
        self.model = model

        guard isShown, let model, let message = model.attentionMessage else {
            panel?.orderOut(nil)
            return
        }

        let visibleFrame = anchorWindow.screen?.visibleFrame ?? NSScreen.main?.visibleFrame
        panelSize = PetAttentionLayout.panelSize(for: message, visibleFrame: visibleFrame)
        let panel = panel ?? makePanel()
        self.panel = panel
        if let hostingView {
            if hostingView.model !== model {
                hostingView.rootView = AnyView(PetAttentionView(model: model))
                hostingView.model = model
            }
        } else {
            let hostingView = PetAttentionHostingView(
                rootView: AnyView(PetAttentionView(model: model))
            )
            hostingView.model = model
            hostingView.frame = NSRect(origin: .zero, size: panelSize)
            hostingView.autoresizingMask = [.width, .height]
            self.hostingView = hostingView
            panel.contentView = hostingView
        }

        hostingView?.frame = NSRect(origin: .zero, size: panelSize)
        panel.setFrame(positionedFrame(for: anchorWindow, size: panelSize), display: true, animate: false)
        panel.orderFront(nil)
        model.attentionMessageDidBecomeVisible(message.id)
    }

    func reposition() {
        guard let panel, panel.isVisible, let anchorWindow else { return }
        panel.setFrame(positionedFrame(for: anchorWindow, size: panelSize), display: true, animate: false)
    }

    private func makePanel() -> NSPanel {
        let panel = NSPanel(
            contentRect: NSRect(origin: .zero, size: panelSize),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        panel.title = "Codex Companion Notice"
        panel.level = .floating
        panel.collectionBehavior.formUnion([.canJoinAllSpaces, .fullScreenAuxiliary])
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.ignoresMouseEvents = false
        return panel
    }

    private func positionedFrame(for anchorWindow: NSWindow, size: CGSize) -> NSRect {
        let anchor = anchorWindow.frame
        let visibleFrame = anchorWindow.screen?.visibleFrame ?? NSScreen.main?.visibleFrame
        var origin = NSPoint(
            x: anchor.midX - size.width / 2,
            y: anchor.maxY + 5
        )

        if let visibleFrame {
            let margin = PetAttentionLayout.screenMargin
            origin.x = min(
                max(origin.x, visibleFrame.minX + margin),
                visibleFrame.maxX - size.width - margin
            )
            if origin.y + size.height > visibleFrame.maxY - margin {
                origin.y = anchor.minY - size.height - 5
            }
            origin.y = min(
                max(origin.y, visibleFrame.minY + margin),
                visibleFrame.maxY - size.height - margin
            )
        }
        return NSRect(origin: origin, size: size)
    }
}

private final class PetAttentionHostingView: NSHostingView<AnyView> {
    weak var model: CompanionAppModel?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }
}

private extension PetAttentionMessage.Kind {
    var symbolName: String {
        switch self {
        case .response: return "text.bubble.fill"
        case .attention: return "exclamationmark.bubble.fill"
        case .goal: return "target"
        case .completion: return "checkmark.circle.fill"
        case .failure: return "xmark.circle.fill"
        }
    }

    var tint: Color {
        switch self {
        case .response: return .secondary
        case .attention: return .yellow
        case .goal: return .blue
        case .completion: return .green
        case .failure: return .red
        }
    }
}

private extension View {
    @ViewBuilder
    func attentionSurface(tint: Color) -> some View {
        if #available(macOS 26.0, *) {
            self
                .background(
                    Color.black.opacity(0.40),
                    in: RoundedRectangle(cornerRadius: 16, style: .continuous)
                )
                .glassEffect(
                    .clear.tint(tint.opacity(0.10)).interactive(),
                    in: .rect(cornerRadius: 16)
                )
                .overlay {
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .stroke(tint.opacity(0.32), lineWidth: 1)
                        .allowsHitTesting(false)
                }
        } else {
            self
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .stroke(tint.opacity(0.32), lineWidth: 1)
                        .allowsHitTesting(false)
                }
        }
    }
}
