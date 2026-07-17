import SwiftUI

extension View {
    @ViewBuilder
    func clearWindowContainerBackground() -> some View {
        if #available(macOS 15.0, *) {
            containerBackground(.clear, for: .window)
        } else {
            background(Color.clear)
        }
    }
}
