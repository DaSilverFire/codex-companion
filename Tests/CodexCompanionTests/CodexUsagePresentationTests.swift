import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexUsagePresentationTests {
    @Test
    func phaseKeepsExistingUsageVisibleDuringRefresh() {
        #expect(CodexUsagePresentationPolicy.phase(
            isLoading: true,
            hasSnapshot: true,
            hasConfirmation: false
        ) == .usage)
        #expect(CodexUsagePresentationPolicy.phase(
            isLoading: true,
            hasSnapshot: false,
            hasConfirmation: false
        ) == .loading)
    }

    @Test
    func resetConfirmationTakesPrecedenceOverUsageRows() {
        #expect(CodexUsagePresentationPolicy.phase(
            isLoading: false,
            hasSnapshot: true,
            hasConfirmation: true
        ) == .confirmation)
    }

    @Test
    func usageControlsUseStableDimensions() {
        #expect(CodexUsagePresentationMetrics.popoverWidth == 292)
        #expect(CodexUsagePresentationMetrics.iconButtonSize == 32)
        #expect(CodexUsagePresentationMetrics.resetRowHeight == 40)
        #expect(CodexUsagePresentationMetrics.confirmationButtonHeight == 32)
        #expect(CodexUsagePresentationMetrics.confirmationButtonWidth == 112)
    }

    @Test
    func usagePopoverOffersManualNewWorkAccountSelection() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexUsagePopover.swift"))

        #expect(source.contains("@StateObject private var profileSwitcher"))
        #expect(source.contains("@StateObject private var profileUsageStore"))
        #expect(source.contains("Picker(\"New work account\""))
        #expect(source.contains("profileSwitcher.selectProfile(id: profileID)"))
        #expect(source.contains("await profileUsageStore.refresh(for: profile)"))
        #expect(source.contains("New Codex work uses this account."))
    }

    @Test
    func usageProfileDefaultsLoadTheApplicationPersistentDomain() throws {
        let suiteName = "CodexUsagePresentationTests.\(UUID().uuidString)"
        let suite = try #require(UserDefaults(suiteName: suiteName))
        defer {
            suite.removePersistentDomain(forName: suiteName)
        }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        suite.set(
            try JSONEncoder().encode([profile]),
            forKey: CodexAccountProfileStore.profilesKey
        )

        let resolvedDefaults = CodexUsageProfileDefaults.applicationDefaults(
            bundleIdentifier: suiteName
        )
        let store = CodexAccountProfileStore(defaults: resolvedDefaults)

        #expect(store.profiles == [profile])
    }

    @Test
    func popoverUsesNativeGlassWithoutOpaqueBacking() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexUsagePopover.swift"))
        let surfaceSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CompanionLiquidGlassMenuSurface.swift"))

        #expect(source.contains("companionLiquidGlassMenuSurface"))
        #expect(surfaceSource.contains("GlassEffectContainer"))
        #expect(surfaceSource.contains(".presentationBackground(.clear)"))
        #expect(surfaceSource.contains(".regular.interactive()"))
        #expect(surfaceSource.contains(".glassEffectTransition(.materialize)"))
        #expect(source.contains("CodexUsagePresentationMetrics.iconButtonSize"))
        #expect(!surfaceSource.contains(".glassEffect(.clear"))
        #expect(!surfaceSource.contains("Color.black.opacity(0.40)"))
        #expect(!surfaceSource.contains(".clear.tint(Color.black"))
    }

    @Test
    func usagePanelReplacesTheSystemPopoverPlacement() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))

        #expect(source.contains("CodexUsagePanel.shared.toggle"))
        #expect(!source.contains(
            ".popover(isPresented: $isUsagePresented, arrowEdge: .bottom)"
        ))
        #expect(source.contains("model.shouldShowPetMenuButton || isUsagePresented"))
    }

    @Test
    func usagePanelAppliesItsFrameBeforeFirstPresentation() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexUsagePanel.swift"))
        let repositionStart = try #require(source.range(of: "    func reposition() {"))
        let presentStart = try #require(source.range(of: "    private func present("))
        let repositionSource = source[repositionStart.lowerBound..<presentStart.lowerBound]

        #expect(!repositionSource.contains("panel.isVisible"))
    }

    @Test
    func usagePanelUsesABoundedScrollableViewportWhenPetSafeSpaceIsShort() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexUsagePanel.swift"))

        #expect(source.contains("NSScrollView"))
        #expect(source.contains("scrollView.documentView = hostingView"))
        #expect(source.contains("scrollView.hasVerticalScroller = true"))
        #expect(source.contains("scrollView.autohidesScrollers = true"))
        #expect(source.contains("panel.contentMinSize = frame.size"))
        #expect(source.contains("panel.contentMaxSize = frame.size"))
        #expect(source.contains("height: max(contentSize.height, viewportSize.height)"))
        #expect(source.contains("scrollView.contentView.scroll(to: .zero)"))
        #expect(source.contains("override func layout()"))
        #expect(source.contains("scheduleContentSizeUpdate()"))
    }

    @Test
    func chatSendDoesNotOwnTheModelPickerGesture() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let quickBarSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/QuickBarTrayView.swift"))
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))

        #expect(!quickBarSource.contains("LongPressGesture"))
        #expect(!quickBarSource.contains("longPressAction"))
        #expect(contentSource.contains("private var chatModelButton"))
        #expect(contentSource.contains("ChatDeliveryPanel.shared.toggle"))
        #expect(!contentSource.contains(".popover(isPresented: $isChatModelPickerPresented"))
    }

    @Test
    func dedicatedModelAndUsageMenusShareNativeLiquidGlassSurface() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let quickBarSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/QuickBarTrayView.swift"))
        let usageSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CodexUsagePopover.swift"))
        let surfaceSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/CompanionLiquidGlassMenuSurface.swift"))

        #expect(quickBarSource.contains("companionLiquidGlassMenuSurface"))
        #expect(usageSource.contains("companionLiquidGlassMenuSurface"))
        #expect(surfaceSource.contains("GlassEffectContainer"))
        #expect(surfaceSource.contains(".regular.interactive()"))
        #expect(surfaceSource.contains(".glassEffectTransition(.materialize)"))
        #expect(surfaceSource.contains(".presentationBackground(.clear)"))
    }

    @Test
    func chatModelMenuUsesACompactServiceFirstPresentation() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/QuickBarTrayView.swift"))

        #expect(source.contains("width: ChatDeliveryPresentationMetrics.panelWidth"))
        #expect(source.contains("cornerRadius: CodexUsagePresentationMetrics.cornerRadius"))
        #expect(source.contains("Label(\"Chat model\", systemImage: \"sparkles\")"))
        #expect(source.contains("Picker(\"Chat service\""))
        #expect(source.contains("selectedDeliverySection"))
        #expect(!source.contains(".frame(maxHeight: 360)"))
    }

    @Test
    func chatModelMenuUsesTheSameUpwardPanelPolicyAsUsage() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let panelSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ChatDeliveryPanel.swift"))
        let contentSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))
        let quickBarSource = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/QuickBarTrayView.swift"))

        #expect(panelSource.contains("CodexUsagePanelPositioningPolicy.frame"))
        #expect(panelSource.contains("PetTrayPanel.shared.usagePresentationGeometry"))
        #expect(panelSource.contains("ChatDeliveryPresentationMetrics.panelWidth"))
        #expect(contentSource.contains("ChatDeliveryPanel.shared.toggle"))
        #expect(quickBarSource.contains("ChatDeliveryPanel.shared.toggle"))
        #expect(!quickBarSource.contains(".popover(isPresented: $isPresented, arrowEdge: .bottom)"))
    }
}
