import AppKit
import Foundation
import SwiftUI
import Testing
@testable import CodexCompanion

@Suite
struct CompanionWindowLifecycleTests {
    @Test
    func petSceneLeavesWindowSizingToWindowConfigurator() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let appSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/App/CodexCompanionApp.swift"))
        let petSceneStart = try #require(appSource.range(
            of: "Window(\"Codex Companion\", id: \"companion\")"
        ))
        let menuBarStart = try #require(appSource.range(
            of: "MenuBarExtra(\"Codex Companion\"",
            range: petSceneStart.upperBound..<appSource.endIndex
        ))
        let petScene = appSource[petSceneStart.lowerBound..<menuBarStart.lowerBound]

        #expect(!petScene.contains(".windowResizability(.contentSize)"))
    }

    @Test
    func trayWindowOnlyAnimatesOriginForPetHoverReflow() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let windowSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/WindowConfigurator.swift"))
        let animationStart = try #require(windowSource.range(
            of: "private func animatePanelOrigin"
        ))
        let animationEnd = try #require(windowSource.range(
            of: "private func springProgress",
            range: animationStart.upperBound..<windowSource.endIndex
        ))
        let originAnimation = windowSource[
            animationStart.lowerBound..<animationEnd.lowerBound
        ]

        #expect(!windowSource.contains("panel.animator().setFrame(frame"))
        #expect(!windowSource.contains("panel.animator().setFrameOrigin(frame.origin)"))
        #expect(windowSource.contains("panel.setFrameOrigin(interpolatedOrigin)"))
        #expect(windowSource.contains("springProgress"))
        #expect(originAnimation.contains("MainActor.assumeIsolated"))
        #expect(!originAnimation.contains("Task { @MainActor in"))
    }

    @Test
    func petControlsAndTrayReflowShareOneSettledSpring() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let windowSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/WindowConfigurator.swift"))

        #expect(PetWindowMetrics.menuMotionResponse >= 0.40)
        #expect(PetWindowMetrics.menuMotionDampingFraction >= 0.86)
        #expect(PetWindowMetrics.menuMotionDuration > PetWindowMetrics.menuMotionResponse)
        #expect(contentSource.contains("response: PetWindowMetrics.menuMotionResponse"))
        #expect(contentSource.contains("dampingFraction: PetWindowMetrics.menuMotionDampingFraction"))
        #expect(windowSource.contains("let response = PetWindowMetrics.menuMotionResponse"))
        #expect(windowSource.contains("let dampingFraction = PetWindowMetrics.menuMotionDampingFraction"))
        #expect(windowSource.contains("let duration = PetWindowMetrics.menuMotionDuration"))
        #expect(!contentSource.contains(".scale(scale: 0.82, anchor: .bottomTrailing)"))
    }

    @Test
    func activeProcessBadgePreservesTheCodexAccentUnderLiquidGlass() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))

        #expect(contentSource.contains("Glass.clear.tint(activeTint)"))
        #expect(contentSource.contains("activeTint.opacity(0.92)"))
        #expect(!contentSource.contains("Glass.clear.tint(activeTint.opacity"))
    }

    @Test
    func petControlsDoNotAnimateCrossWindowTrayStateChanges() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let modeButtonStart = try #require(contentSource.range(of: "private var modeButton"))
        let petSpriteStart = try #require(contentSource.range(
            of: "private var petSprite",
            range: modeButtonStart.upperBound..<contentSource.endIndex
        ))
        let menuArrowStart = try #require(contentSource.range(
            of: "private var menuArrowButton",
            range: petSpriteStart.upperBound..<contentSource.endIndex
        ))
        let menuGlyphStart = try #require(contentSource.range(
            of: "private var menuButtonGlyph",
            range: menuArrowStart.upperBound..<contentSource.endIndex
        ))

        let modeButton = contentSource[modeButtonStart.lowerBound..<petSpriteStart.lowerBound]
        let menuArrow = contentSource[menuArrowStart.lowerBound..<menuGlyphStart.lowerBound]
        #expect(!modeButton.contains("withAnimation(menuToggleAnimation)"))
        #expect(!menuArrow.contains("withAnimation(menuToggleAnimation)"))
    }

    @Test
    func petMenuControlsStayInsideTheFixedPetWindow() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let controlStart = try #require(contentSource.range(of: "menuControlHoverSurface"))
        let contextMenuStart = try #require(contentSource.range(
            of: ".contextMenu",
            range: controlStart.upperBound..<contentSource.endIndex
        ))
        let controlPlacement = contentSource[controlStart.lowerBound..<contextMenuStart.lowerBound]

        #expect(!controlPlacement.contains("y: -"))
    }

    @Test
    func menuArrowHoverSurfaceOverlapsThePetTrackingRegion() {
        let arrowHoverFrame = PetWindowMetrics.menuArrowHoverFrame
        let petTrackingFrame = PetWindowMetrics.petDragHandleFrame
        let overlap = arrowHoverFrame.intersection(petTrackingFrame)

        #expect(!overlap.isNull)
        #expect(overlap.width > 0)
        #expect(overlap.height >= 10)
        #expect(PetWindowMetrics.menuArrowVisualFrame.maxY < PetWindowMetrics.petArtworkTop)
        #expect(PetWindowMetrics.petArtworkTop - PetWindowMetrics.menuArrowVisualFrame.maxY <= 2)
    }

    @Test
    func openMenuUtilityButtonsStayInsideThePetWindow() {
        let cluster = PetWindowMetrics.menuControlClusterHoverFrame(isQuickBarOpen: true)

        #expect(cluster.minX >= 0)
        #expect(cluster.maxX <= PetWindowMetrics.petSize.width)
        #expect(PetWindowMetrics.menuUtilityButtonWidth == PetWindowMetrics.menuControlHitSize.width)
        #expect(
            cluster.width
                == PetWindowMetrics.menuControlHitSize.width * 3
                    + PetWindowMetrics.menuControlSpacing * 2
        )
    }

    @Test
    func processAndChatTraysUseTheSameThreeControlSlots() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let clusterStart = try #require(contentSource.range(of: "private var menuControlCluster"))
        let hoverStart = try #require(contentSource.range(
            of: "private var menuControlHoverSurface",
            range: clusterStart.upperBound..<contentSource.endIndex
        ))
        let cluster = contentSource[clusterStart.lowerBound..<hoverStart.lowerBound]

        #expect(cluster.contains("usageButton"))
        #expect(cluster.contains("modeButton"))
        #expect(cluster.contains("menuArrowButton"))
        #expect(!cluster.contains("if model.isCodexProcessTrayVisible"))
    }

    @Test
    func petHoverExplicitlyReflowsTheOpenTrayPanel() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let windowSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/WindowConfigurator.swift"))

        #expect(contentSource.contains(".onChange(of: shouldShowPetMenuControls)"))
        #expect(contentSource.contains("PetTrayPanel.shared.setMenuControlsVisible(isVisible)"))
        #expect(windowSource.contains("func setMenuControlsVisible(_ isVisible: Bool)"))
        #expect(windowSource.contains(
            "PetTrayPanel.shared.setMenuControlsVisible(model.shouldShowPetMenuButton)"
        ))
        #expect(windowSource.contains("animated: true"))
    }

    @Test
    func screenSpaceHoverFramesPreserveTheArrowToPetBridge() {
        let window = CGRect(x: 500, y: 300, width: 124, height: 164)
        let pet = PetWindowMetrics.petDragHandleScreenFrame(in: window)
        let controls = PetWindowMetrics.menuControlClusterScreenFrame(
            in: window,
            isQuickBarOpen: false
        )
        let overlap = pet.intersection(controls)

        #expect(!overlap.isNull)
        #expect(overlap.height >= 10)
        #expect(controls.maxX <= window.maxX)
    }

    @Test
    func hiddenMenuArrowRemovesItsGlassWhileKeepingTheHoverBridgeMounted() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let surfaceStart = try #require(contentSource.range(
            of: "private var menuControlHoverSurface"
        ))
        let usageStart = try #require(contentSource.range(
            of: "private var usageButton",
            range: surfaceStart.upperBound..<contentSource.endIndex
        ))
        let hoverSurface = contentSource[surfaceStart.lowerBound..<usageStart.lowerBound]

        #expect(hoverSurface.contains("menuControlCluster"))
        #expect(hoverSurface.contains("if shouldShowPetMenuControls"))
        #expect(hoverSurface.contains("Color.clear"))
        #expect(hoverSurface.contains(".onHover"))
        #expect(!hoverSurface.contains(".opacity(shouldShowPetMenuControls ? 1 : 0)"))
        #expect(hoverSurface.contains(".allowsHitTesting(shouldShowPetMenuControls)"))
    }

    @Test
    func petWindowKeepsAStableControlRowAboveThePet() {
        let closedSize = PetWindowMetrics.contentSize(
            isQuickBarOpen: false,
            showProcesses: true
        )
        let openSize = PetWindowMetrics.contentSize(
            isQuickBarOpen: true,
            showProcesses: true
        )

        #expect(openSize.width == closedSize.width)
        #expect(openSize == closedSize)
        #expect(closedSize.height == PetWindowMetrics.petSize.height + PetWindowMetrics.menuControlRowHeight)
    }

    @Test @MainActor
    func trayHostingViewUsesOnlyThePanelControlledFrame() {
        let hostingView = NSHostingView(rootView: Text("Companion"))

        PetTrayHostingViewSizingPolicy.apply(to: hostingView)

        #expect(hostingView.sizingOptions.isEmpty)
    }

    @Test @MainActor
    func trayHostingViewRelayoutsWhenAnOpenProcessListChangesSize() {
        let hostingView = NSHostingView(rootView: Text("Companion"))
        hostingView.frame = NSRect(x: 0, y: 0, width: 292, height: 94)

        let didResize = PetTrayHostingViewSizingPolicy.updateFrame(
            of: hostingView,
            to: CGSize(width: 292, height: 240)
        )

        #expect(didResize)
        #expect(hostingView.frame.size == CGSize(width: 292, height: 240))
    }

    @Test @MainActor
    func petWindowRootHostingViewCannotResizeTheAppKitOwnedWindow() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 124, height: 124),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )
        let container = NSView(frame: window.contentView?.bounds ?? .zero)
        let hostingView = NSHostingView(rootView: Text("Companion"))
        hostingView.sizingOptions = [.preferredContentSize]
        container.addSubview(hostingView)
        window.contentView = container

        PetWindowHostingViewSizingPolicy.apply(to: window)

        #expect(hostingView.sizingOptions.isEmpty)
    }

    @Test @MainActor
    func missingStableFrameIsSeededAfterFinalPetLayout() {
        let defaultsKey = CompanionWindowFramePersistence.storageKey
        let appKitDefaultsKey = "NSWindow Frame \(CompanionWindowFramePersistence.autosaveName)"
        UserDefaults.standard.removeObject(forKey: defaultsKey)
        UserDefaults.standard.removeObject(forKey: appKitDefaultsKey)
        defer {
            UserDefaults.standard.removeObject(forKey: defaultsKey)
            UserDefaults.standard.removeObject(forKey: appKitDefaultsKey)
        }

        let window = NSWindow(
            contentRect: NSRect(x: 120, y: 140, width: 124, height: 156),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )

        let restoredExistingFrame = CompanionWindowFramePersistence.restore(window)
        window.setFrameOrigin(NSPoint(x: 360, y: 420))
        CompanionWindowFramePersistence.seedIfNeeded(
            window,
            restoredExistingFrame: restoredExistingFrame
        )

        #expect(!restoredExistingFrame)
        #expect(UserDefaults.standard.string(forKey: defaultsKey) != nil)
    }

    @Test
    func petWindowRestorationUsesAStableNameAndRunsOncePerWindow() {
        #expect(!CompanionWindowFramePersistence.autosaveName.isEmpty)

        let firstWindow = NSObject()
        let secondWindow = NSObject()
        var tracker = CompanionWindowRestorationTracker()

        let firstAttempt = tracker.shouldRestore(firstWindow)
        let repeatedAttempt = tracker.shouldRestore(firstWindow)
        let secondWindowAttempt = tracker.shouldRestore(secondWindow)

        #expect(firstAttempt)
        #expect(!repeatedAttempt)
        #expect(secondWindowAttempt)
    }

    @Test
    func petUsesOneWindowAndShowActionsReuseIt() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let appSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/App/CodexCompanionApp.swift"))
        let menuSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CompanionMenuBarView.swift"))
        let commandsSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/App/CompanionCommands.swift"))
        let modelSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Stores/CompanionAppModel.swift"))

        #expect(appSource.contains("Window(\"Codex Companion\", id: \"companion\")"))
        #expect(!appSource.contains("WindowGroup(\"Codex Companion\", id: \"companion\")"))
        #expect(!menuSource.contains("openWindow(id: \"companion\")"))
        #expect(!commandsSource.contains("openWindow(id: \"companion\")"))
        #expect(modelSource.contains("isPetHovered = false"))
        #expect(modelSource.contains("isPetPointerHovered = false"))
        #expect(modelSource.contains("PetWindowRoamer.shared.setInteractionHold(false)"))
    }
}
