import SwiftUI

struct CompanionLiquidGlassMenuSurface: ViewModifier {
    var cornerRadius: CGFloat

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            GlassEffectContainer(spacing: 8) {
                content
                    .glassEffect(
                        .regular.interactive(),
                        in: .rect(cornerRadius: cornerRadius)
                    )
                    .glassEffectTransition(.materialize)
            }
            .presentationBackground(.clear)
        } else {
            content
                .background(
                    .ultraThinMaterial,
                    in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                )
        }
    }
}

extension View {
    func companionLiquidGlassMenuSurface(cornerRadius: CGFloat) -> some View {
        modifier(CompanionLiquidGlassMenuSurface(cornerRadius: cornerRadius))
    }
}
