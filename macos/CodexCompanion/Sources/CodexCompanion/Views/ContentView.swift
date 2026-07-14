import AppKit
import SwiftUI

struct ContentView: View {
    @ObservedObject var model: CompanionAppModel
    @Namespace private var petMenuGlassNamespace
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var isUsagePresented = false

    var body: some View {
        Group {
            if #available(macOS 26.0, *) {
                GlassEffectContainer(spacing: 4) {
                    petContent
                }
            } else {
                petContent
            }
        }
        .frame(
            width: PetWindowMetrics.petSize.width,
            height: PetWindowMetrics.petSize.height,
            alignment: .bottomLeading
        )
        .animation(.smooth(duration: 0.20, extraBounce: 0.08), value: model.isQuickBarOpen)
        .onChange(of: model.isQuickBarOpen) { _, isOpen in
            if !isOpen {
                isUsagePresented = false
            }
        }
        .onChange(of: model.isCodexProcessTrayVisible) { _, isVisible in
            if !isVisible {
                isUsagePresented = false
            }
        }
    }

    private var petContent: some View {
        ZStack(alignment: .bottomLeading) {
            petSurface
            GoalConfettiView(trigger: model.goalConfettiTrigger)
                .frame(width: PetWindowMetrics.petSize.width, height: PetWindowMetrics.petSize.height)
                .allowsHitTesting(false)
        }
    }

    private var petSurface: some View {
        ZStack(alignment: .topTrailing) {
            ZStack {
                petSprite

                PetWindowDragHandle()
                    .model(model)
                    .frame(width: 112, height: 112)
            }
            .frame(width: PetWindowMetrics.petSize.width, height: PetWindowMetrics.petSize.height)
            .contentShape(Rectangle())

            if model.shouldShowPetMenuButton {
                menuControlCluster
                    .offset(x: -4, y: -8)
                    .transition(.opacity.combined(with: .scale(scale: 0.88, anchor: .center)))
            }
        }
        .frame(width: PetWindowMetrics.petSize.width, height: PetWindowMetrics.petSize.height)
        .animation(.smooth(duration: 0.18, extraBounce: 0.06), value: model.shouldShowPetMenuButton)
        .contextMenu {
            Button("Open Codex") {
                model.continueCodex()
            }

            if !model.isCodexOnlyMode {
                Button("ChatGPT Menu") {
                    model.sendPrompt(mode: .chatGPT)
                }
            }

            Button(model.isQuickBarOpen ? "Hide Quick Bar" : "Show Quick Bar") {
                model.toggleQuickBar()
            }

            Button("Hide Pet") {
                model.hidePet()
            }

            Divider()

            ForEach(PetAnimationState.allCases) { state in
                Button(state.title) {
                    model.setAnimation(state)
                }
            }

        }
    }

    private var menuControlCluster: some View {
        HStack(spacing: 6) {
            if shouldShowOpenMenuUtilities {
                if model.isCodexProcessTrayVisible {
                    usageButton
                        .transition(menuUtilityTransition)
                }

                modeButton
                    .transition(menuUtilityTransition)
            }

            menuArrowButton
        }
        .animation(menuToggleAnimation, value: shouldShowOpenMenuUtilities)
    }

    private var usageButton: some View {
        Button {
            withAnimation(menuToggleAnimation) {
                isUsagePresented.toggle()
            }
            if isUsagePresented {
                model.rateLimitStore.refreshIfNeeded(maxAge: 10)
            }
        } label: {
            Image(systemName: model.rateLimitStore.isLoading ? "hourglass" : "info.circle")
                .font(.system(size: 13, weight: .semibold))
                .frame(width: 28, height: 28)
                .petGlassCircle(
                    isSelected: isUsagePresented,
                    glassID: "pet-menu-usage",
                    glassNamespace: petMenuGlassNamespace
                )
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .frame(width: 34, height: 40)
        .contentShape(Circle())
        .accessibilityLabel("Codex usage and resets")
        .help("Codex usage and resets")
        .popover(isPresented: $isUsagePresented, arrowEdge: .bottom) {
            CodexUsagePopover(store: model.rateLimitStore)
        }
    }

    private var modeButton: some View {
        Button {
            withAnimation(menuToggleAnimation) {
                if model.isCodexProcessTrayVisible {
                    model.showChatGPT()
                } else {
                    model.showCodexProcesses()
                }
            }
        } label: {
            Image(systemName: model.isCodexProcessTrayVisible ? "bubble.left.and.text.bubble.right" : "list.bullet")
                .font(.system(size: 12, weight: .semibold))
                .frame(width: 28, height: 28)
                .petGlassCircle(
                    isSelected: !model.isCodexProcessTrayVisible,
                    glassID: "pet-menu-mode",
                    glassNamespace: petMenuGlassNamespace
                )
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .frame(width: 34, height: 40)
        .contentShape(Circle())
        .accessibilityLabel(model.isCodexProcessTrayVisible ? "Open local chat" : "Show Codex processes")
        .help(model.isCodexProcessTrayVisible ? "Open local chat" : "Show Codex processes")
    }

    @ViewBuilder
    private var petSprite: some View {
        if let pet = model.petStore.selectedPet {
            PetSpriteView(
                pet: pet,
                state: model.renderedPetState,
                speedScale: model.animationSpeedScale,
                directionalLookFrame: model.directionalLookFrame
            )
            .id(pet.renderIdentity)
            .frame(width: 100, height: 108)
            .padding(8)
        } else {
            Image(systemName: "pawprint")
                .font(.system(size: 46, weight: .regular))
                .foregroundStyle(.secondary)
                .frame(width: 100, height: 108)
                .padding(8)
        }
    }

    private var menuArrowButton: some View {
        Button {
            withAnimation(menuToggleAnimation) {
                model.toggleQuickBar()
            }
        } label: {
            ZStack {
                Circle()
                    .fill(Color.clear)
                    .frame(width: 40, height: 40)

                menuButtonGlyph
                    .frame(width: 28, height: 28)
                    .petGlassCircle(
                        isSelected: model.isQuickBarOpen,
                        isActive: shouldShowActiveProcessBadge,
                        glassID: "pet-menu-toggle",
                        glassNamespace: petMenuGlassNamespace
                    )
            }
            .frame(width: 40, height: 40)
            .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .contentShape(Circle())
        .help(menuButtonHelp)
        .animation(menuToggleAnimation, value: model.isQuickBarOpen)
        .animation(menuToggleAnimation, value: shouldShowActiveProcessBadge)
    }

    private var menuButtonGlyph: some View {
        ZStack {
            Text(activeProcessBadgeText)
                .font(.system(size: activeProcessBadgeText.count > 1 ? 11 : 12, weight: .bold, design: .rounded))
                .monospacedDigit()
                .foregroundStyle(.white)
                .opacity(shouldShowActiveProcessBadge ? 1 : 0)
                .scaleEffect(shouldShowActiveProcessBadge ? 1 : 0.72)
                .rotationEffect(.degrees(shouldShowActiveProcessBadge ? 0 : -18))

            Image(systemName: "chevron.up")
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(.primary)
                .rotationEffect(.degrees(model.isQuickBarOpen ? 180 : 0))
                .opacity(shouldShowActiveProcessBadge ? 0 : 1)
                .scaleEffect(shouldShowActiveProcessBadge ? 0.72 : 1)
        }
    }

    private var menuToggleAnimation: Animation {
        reduceMotion
            ? .easeOut(duration: 0.12)
            : .spring(response: 0.34, dampingFraction: 0.72, blendDuration: 0.08)
    }

    private var menuUtilityTransition: AnyTransition {
        .asymmetric(
            insertion: .offset(x: 34)
                .combined(with: .scale(scale: 0.72, anchor: .trailing))
                .combined(with: .opacity),
            removal: .offset(x: 34)
                .combined(with: .scale(scale: 0.76, anchor: .trailing))
                .combined(with: .opacity)
        )
    }

    private var shouldShowOpenMenuUtilities: Bool {
        model.isQuickBarOpen
    }

    private var shouldShowActiveProcessBadge: Bool {
        !model.isQuickBarOpen && model.activeCodexProcessCount > 0
    }

    private var activeProcessBadgeText: String {
        let count = model.activeCodexProcessCount
        return count > 9 ? "9+" : "\(count)"
    }

    private var menuButtonHelp: String {
        if model.isQuickBarOpen {
            return "Hide menu"
        }
        let count = model.activeCodexProcessCount
        if count == 1 {
            return "Show menu - 1 active Codex process"
        }
        if count > 1 {
            return "Show menu - \(count) active Codex processes"
        }
        return "Show menu"
    }
}

private extension View {
    @ViewBuilder
    func petGlassCircle(
        isSelected: Bool,
        isActive: Bool = false,
        glassID: String,
        glassNamespace: Namespace.ID? = nil
    ) -> some View {
        if #available(macOS 26.0, *) {
            self
                .background(
                    isActive ? Color(red: 0.04, green: 0.14, blue: 0.34).opacity(0.78) : Color.black.opacity(isSelected ? 0.20 : 0.06),
                    in: Circle()
                )
                .glassEffect(
                    petMenuGlass(isSelected: isSelected, isActive: isActive).interactive(),
                    in: .circle
                )
                .modifier(PetMenuGlassID(id: glassID, namespace: glassNamespace))
                .overlay {
                    Circle()
                        .stroke(petMenuBorderColor(isSelected: isSelected, isActive: isActive), lineWidth: 1)
                        .allowsHitTesting(false)
                }
                .shadow(color: isActive ? Color.blue.opacity(0.22) : Color.black.opacity(0.12), radius: 9, y: 3)
        } else {
            self
                .background(.regularMaterial, in: Circle())
                .background(isActive ? Color(red: 0.05, green: 0.16, blue: 0.36).opacity(0.72) : Color.clear, in: Circle())
                .overlay {
                    Circle()
                        .stroke(isActive ? Color.blue.opacity(0.55) : Color.secondary.opacity(0.45))
                        .allowsHitTesting(false)
                }
                .shadow(color: .black.opacity(0.24), radius: 8, y: 3)
        }
    }

    @available(macOS 26.0, *)
    private func petMenuGlass(isSelected: Bool, isActive: Bool) -> Glass {
        if isActive {
            return Glass.clear.tint(Color(red: 0.04, green: 0.14, blue: 0.34).opacity(0.72))
        }
        if isSelected {
            return Glass.clear.tint(Color.primary.opacity(0.06))
        }
        return Glass.clear
    }

    private func petMenuBorderColor(isSelected: Bool, isActive: Bool) -> Color {
        if isActive {
            return Color.blue.opacity(0.58)
        }
        return Color.primary.opacity(isSelected ? 0.30 : 0.16)
    }
}

@available(macOS 26.0, *)
private struct PetMenuGlassID: ViewModifier {
    var id: String
    var namespace: Namespace.ID?

    func body(content: Content) -> some View {
        if let namespace {
            content
                .glassEffectID(id, in: namespace)
                .glassEffectTransition(.matchedGeometry)
        } else {
            content
        }
    }
}

struct PetWindowDragHandle: NSViewRepresentable {
    var model: CompanionAppModel?

    func makeNSView(context: Context) -> DragHandleView {
        let view = DragHandleView()
        view.model = model
        return view
    }

    func updateNSView(_ nsView: DragHandleView, context: Context) {
        nsView.model = model
    }

    func model(_ model: CompanionAppModel) -> Self {
        var copy = self
        copy.model = model
        return copy
    }
}

@MainActor
final class DragHandleView: NSView {
    weak var model: CompanionAppModel?
    private var dragStartMouse: NSPoint?
    private var dragStartOrigin: NSPoint?
    private var lastMouse: NSPoint?
    private var isHovering = false
    private var isDragging = false
    private var trackingAreaRef: NSTrackingArea?

    override var acceptsFirstResponder: Bool { true }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool {
        true
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingAreaRef {
            removeTrackingArea(trackingAreaRef)
        }

        let trackingArea = NSTrackingArea(
            rect: bounds,
            options: [.activeAlways, .inVisibleRect, .mouseEnteredAndExited, .mouseMoved],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(trackingArea)
        trackingAreaRef = trackingArea
    }

    override func mouseEntered(with event: NSEvent) {
        isHovering = true
        model?.setPetHovering(true)
        PetWindowRoamer.shared.setInteractionHold(true)
    }

    override func mouseMoved(with event: NSEvent) {
        if isHovering, !isDragging {
            model?.setPetHovering(true)
            PetWindowRoamer.shared.setInteractionHold(true)
        }
    }

    override func mouseExited(with event: NSEvent) {
        isHovering = false
        guard !isDragging else { return }
        model?.setPetHovering(false)
        PetWindowRoamer.shared.setInteractionHold(false)
    }

    override func mouseDown(with event: NSEvent) {
        PetWindowRoamer.shared.pauseBriefly()
        PetWindowRoamer.shared.setInteractionHold(true)
        isDragging = true
        model?.beginPetDrag()
        dragStartMouse = NSEvent.mouseLocation
        dragStartOrigin = window?.frame.origin
        lastMouse = dragStartMouse
    }

    override func mouseDragged(with event: NSEvent) {
        guard let window, let dragStartMouse, let dragStartOrigin else { return }
        let current = NSEvent.mouseLocation
        if let lastMouse {
            let frameDX = current.x - lastMouse.x
            let totalDX = current.x - dragStartMouse.x
            let directionDX = abs(frameDX) > 0.12 ? frameDX : totalDX
            model?.updatePetDrag(dx: directionDX, dy: current.y - lastMouse.y)
        }
        lastMouse = current
        let nextOrigin = NSPoint(
            x: dragStartOrigin.x + current.x - dragStartMouse.x,
            y: dragStartOrigin.y + current.y - dragStartMouse.y
        )
        let clampedOrigin = clamp(origin: nextOrigin, windowSize: window.frame.size, mouseLocation: current)
        guard abs(clampedOrigin.x - window.frame.origin.x) > 0.1 || abs(clampedOrigin.y - window.frame.origin.y) > 0.1 else {
            return
        }
        window.setFrameOrigin(clampedOrigin)
        PetAttentionPanel.shared.reposition()
        if model?.isQuickBarOpen == true {
            PetTrayPanel.shared.reposition()
        }
    }

    override func mouseUp(with event: NSEvent) {
        PetWindowRoamer.shared.pauseBriefly()
        isDragging = false
        model?.endPetDrag()
        PetWindowRoamer.shared.setInteractionHold(isHovering)
        dragStartMouse = nil
        dragStartOrigin = nil
        lastMouse = nil
    }

    private func clamp(origin: NSPoint, windowSize: NSSize, mouseLocation: NSPoint) -> NSPoint {
        let screen = NSScreen.screens.first { $0.visibleFrame.contains(mouseLocation) }
            ?? window?.screen
            ?? NSScreen.main
        guard let visibleFrame = screen?.visibleFrame else { return origin }

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
