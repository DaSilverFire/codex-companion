import SwiftUI

struct GoalConfettiView: View {
    var trigger: Int

    @State private var activeTrigger = 0
    @State private var isExpanded = false

    var body: some View {
        GeometryReader { proxy in
            ZStack {
                if activeTrigger > 0 {
                    ForEach(0..<30, id: \.self) { index in
                        confettiPiece(index: index, size: proxy.size)
                    }
                }
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
        .onAppear {
            if trigger > 0 {
                startBurst(trigger)
            }
        }
        .onChange(of: trigger) { _, newValue in
            guard newValue > 0 else { return }
            startBurst(newValue)
        }
    }

    private func confettiPiece(index: Int, size: CGSize) -> some View {
        let vector = particleVector(index: index, trigger: activeTrigger)
        let distance = CGFloat(34 + (index % 7) * 12)
        let start = CGPoint(x: size.width / 2, y: size.height * 0.58)
        let endOffset = CGSize(width: vector.dx * distance, height: vector.dy * distance - CGFloat(index % 5) * 7)
        let pieceSize = CGFloat(5 + index % 3)

        return RoundedRectangle(cornerRadius: 1.5, style: .continuous)
            .fill(confettiColor(index: index))
            .frame(width: pieceSize, height: pieceSize * 1.5)
            .rotationEffect(.degrees(isExpanded ? Double((index * 31) % 360) : 0))
            .position(start)
            .offset(
                x: isExpanded ? endOffset.width : 0,
                y: isExpanded ? endOffset.height : 0
            )
            .opacity(isExpanded ? 0 : 1)
            .scaleEffect(isExpanded ? 0.88 : 1)
            .animation(.easeOut(duration: 1.2), value: isExpanded)
    }

    private func startBurst(_ value: Int) {
        activeTrigger = value
        isExpanded = false

        DispatchQueue.main.async {
            withAnimation(.easeOut(duration: 1.2)) {
                isExpanded = true
            }
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 1.25) {
            guard activeTrigger == value else { return }
            activeTrigger = 0
            isExpanded = false
        }
    }

    private func particleVector(index: Int, trigger: Int) -> CGVector {
        let baseAngle = (Double(index) / 30.0) * Double.pi * 2
        let offset = Double((trigger * 23 + index * 11) % 70) / 100.0
        let angle = baseAngle + offset
        return CGVector(dx: cos(angle), dy: sin(angle) - 0.35)
    }

    private func confettiColor(index: Int) -> Color {
        switch index % 6 {
        case 0:
            return .yellow
        case 1:
            return .green
        case 2:
            return .cyan
        case 3:
            return .pink
        case 4:
            return .orange
        default:
            return .white
        }
    }
}
