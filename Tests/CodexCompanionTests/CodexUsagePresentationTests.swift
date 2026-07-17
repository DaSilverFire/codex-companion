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
    func usagePopoverOpensAboveItsPinnedAnchor() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let source = try String(contentsOf: root
            .appendingPathComponent("Sources/CodexCompanion/Views/ContentView.swift"))

        #expect(source.contains(
            ".popover(isPresented: $isUsagePresented, arrowEdge: .bottom)"
        ))
        #expect(source.contains("model.shouldShowPetMenuButton || isUsagePresented"))
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
        #expect(contentSource.contains("ChatDeliveryPicker(model: model)"))
        #expect(contentSource.contains(".popover(isPresented: $isChatModelPickerPresented"))
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
}
