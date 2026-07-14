import SwiftUI

struct ShadowRiggedPetView: View {
    var state: PetAnimationState
    var speedScale: Double = 1

    var body: some View {
        TimelineView(.animation) { context in
            let time = context.date.timeIntervalSinceReferenceDate / max(0.45, min(3.0, speedScale))
            let pose = ShadowRigPose(state: state, time: time)

            ZStack {
                ShadowTailView(pose: pose)
                    .offset(x: pose.tailOffset.width, y: pose.tailOffset.height)

                ShadowRearLegsView(pose: pose)
                    .offset(x: pose.bodyOffset.width, y: pose.bodyOffset.height)

                ShadowBodyView(pose: pose)
                    .offset(x: pose.bodyOffset.width, y: pose.bodyOffset.height)

                ShadowFrontLegsView(pose: pose)
                    .offset(x: pose.bodyOffset.width, y: pose.bodyOffset.height)

                ShadowHeadView(pose: pose)
                    .offset(x: pose.headOffset.width, y: pose.headOffset.height)

                if pose.showsSparkles {
                    ShadowSparklesView(time: time)
                        .transition(.opacity)
                }
            }
            .frame(width: 100, height: 108)
            .scaleEffect(x: pose.flipX, y: 1, anchor: .center)
            .animation(.smooth(duration: 0.18), value: state)
        }
        .frame(width: 100, height: 108)
    }
}

private struct ShadowRigPose {
    var bodyOffset: CGSize = .zero
    var bodyScale = CGSize(width: 1, height: 1)
    var bodyRotation: Angle = .zero
    var headOffset: CGSize = .zero
    var headRotation: Angle = .zero
    var leftFrontLegOffset: CGSize = .zero
    var rightFrontLegOffset: CGSize = .zero
    var leftRearLegOffset: CGSize = .zero
    var rightRearLegOffset: CGSize = .zero
    var raisedPaw: CGFloat = 0
    var raisedPawSide: CGFloat = 1
    var blink: CGFloat = 0
    var happyEyes = false
    var sadEyes = false
    var mouthOpen = false
    var tears: CGFloat = 0
    var earDroop: CGFloat = 0
    var tailKind: ShadowTailKind = .curl
    var tailWave: CGFloat = 0
    var tailOffset: CGSize = .zero
    var flipX: CGFloat = 1
    var showsSparkles = false

    init(state: PetAnimationState, time: TimeInterval) {
        let twoPi = Double.pi * 2
        let idleBob = CGFloat(sin(time * twoPi / 3.8)) * 1.2
        let idleTail = CGFloat(sin(time * twoPi / 2.9))
        bodyOffset = CGSize(width: 0, height: idleBob)
        headOffset = CGSize(width: 0, height: idleBob * 0.7)
        tailWave = idleTail
        blink = Self.blinkAmount(time: time, period: 4.8, width: 0.10)

        switch state {
        case .idle:
            tailKind = .curl
            bodyScale = CGSize(width: 1, height: 1 + CGFloat(sin(time * twoPi / 4.2)) * 0.015)

        case .waiting:
            tailKind = .softCurl
            tailWave = CGFloat(sin(time * twoPi / 3.6)) * 0.7
            blink = max(
                Self.blinkAmount(time: time, period: 4.4, width: 0.11),
                Self.blinkAmount(time: time + 0.16, period: 4.4, width: 0.10),
                Self.blinkAmount(time: time + 0.31, period: 4.4, width: 0.09)
            )
            headRotation = .degrees(CGFloat(sin(time * twoPi / 6.0)) * 2.5)

        case .runningRight, .runningLeft, .running:
            let step = CGFloat(sin(time * twoPi * 3.2))
            let lift = abs(step)
            bodyOffset = CGSize(width: CGFloat(sin(time * twoPi * 1.6)) * 1.2, height: -lift * 3.2)
            headOffset = CGSize(width: step * 1.2, height: -lift * 2.0)
            bodyScale = CGSize(width: 1.03 + lift * 0.03, height: 0.96 - lift * 0.02)
            bodyRotation = .degrees(step * 2.5)
            headRotation = .degrees(step * 3.0)
            leftFrontLegOffset = CGSize(width: step * 4.5, height: -max(0, step) * 4.5)
            rightFrontLegOffset = CGSize(width: -step * 4.5, height: -max(0, -step) * 4.5)
            leftRearLegOffset = CGSize(width: -step * 3.5, height: -max(0, -step) * 3.5)
            rightRearLegOffset = CGSize(width: step * 3.5, height: -max(0, step) * 3.5)
            tailKind = .highSwish
            tailWave = step
            if state == .runningLeft {
                flipX = -1
            }

        case .jumping:
            let progress = (time.truncatingRemainder(dividingBy: 1.7)) / 1.7
            let arc = CGFloat(sin(progress * Double.pi))
            let crouch = progress < 0.18 ? CGFloat(1 - progress / 0.18) : 0
            bodyOffset = CGSize(width: CGFloat(progress - 0.5) * 7, height: -arc * 22 + crouch * 4)
            headOffset = CGSize(width: CGFloat(progress - 0.5) * 5, height: -arc * 24 + crouch * 3)
            bodyScale = CGSize(width: 1.05 - arc * 0.08 + crouch * 0.08, height: 0.94 + arc * 0.16 - crouch * 0.08)
            leftFrontLegOffset = CGSize(width: -3, height: -arc * 9 + crouch * 3)
            rightFrontLegOffset = CGSize(width: 3, height: -arc * 9 + crouch * 3)
            leftRearLegOffset = CGSize(width: -2, height: crouch * 5)
            rightRearLegOffset = CGSize(width: 2, height: crouch * 5)
            tailKind = .lifted
            tailWave = CGFloat(sin(time * twoPi * 2.1)) * 0.6
            mouthOpen = arc > 0.65

        case .waving:
            let wave = CGFloat(sin(time * twoPi * 1.8))
            bodyOffset = CGSize(width: 0, height: idleBob * 0.5)
            headOffset = CGSize(width: 0, height: idleBob * 0.4)
            raisedPaw = 1
            raisedPawSide = 1
            rightFrontLegOffset = CGSize(width: wave * 2.5, height: -16 - abs(wave) * 1.5)
            tailKind = .curl
            tailWave = idleTail * 0.45
            happyEyes = wave > 0.55
            mouthOpen = wave > 0.55

        case .failed:
            bodyOffset = CGSize(width: 0, height: 2)
            headOffset = CGSize(width: 0, height: 4 + CGFloat(sin(time * twoPi / 2.6)) * 0.8)
            bodyScale = CGSize(width: 1.04, height: 0.94)
            headRotation = .degrees(CGFloat(sin(time * twoPi / 4.0)) * 1.5)
            blink = 1
            sadEyes = true
            tears = 1
            earDroop = 1
            tailKind = .low
            tailWave = CGFloat(sin(time * twoPi / 4.5)) * 0.35
            leftFrontLegOffset = CGSize(width: -1.5, height: 3)
            rightFrontLegOffset = CGSize(width: 1.5, height: 3)

        case .review:
            tailKind = .questionCurl
            tailWave = CGFloat(sin(time * twoPi / 2.2)) * 0.5
            headRotation = .degrees(CGFloat(sin(time * twoPi / 3.2)) * 6)
            headOffset = CGSize(width: CGFloat(sin(time * twoPi / 3.2)) * 1.5, height: idleBob)
            blink = Self.blinkAmount(time: time, period: 5.2, width: 0.10)

        case .goalComplete:
            let beat = CGFloat(sin(time * twoPi * 1.9))
            let bounce = abs(beat)
            bodyOffset = CGSize(width: beat * 3.2, height: -bounce * 6.5)
            headOffset = CGSize(width: beat * 2.0, height: -bounce * 7.0)
            bodyScale = CGSize(width: 1.02 + bounce * 0.04, height: 0.97 - bounce * 0.03)
            raisedPaw = 1
            raisedPawSide = beat >= 0 ? 1 : -1
            leftFrontLegOffset = raisedPawSide < 0 ? CGSize(width: -5, height: -15) : CGSize(width: 1, height: -4)
            rightFrontLegOffset = raisedPawSide > 0 ? CGSize(width: 5, height: -15) : CGSize(width: -1, height: -4)
            happyEyes = true
            mouthOpen = true
            tailKind = .highSwish
            tailWave = beat
            showsSparkles = true

        case .thinking, .talking:
            break
        }
    }

    private static func blinkAmount(time: TimeInterval, period: TimeInterval, width: TimeInterval) -> CGFloat {
        let phase = time.truncatingRemainder(dividingBy: period)
        let distance = min(abs(phase), abs(period - phase))
        guard distance < width else { return 0 }
        return CGFloat(1 - distance / width)
    }
}

private enum ShadowTailKind {
    case curl
    case softCurl
    case highSwish
    case lifted
    case low
    case questionCurl
}

private enum ShadowRigColors {
    static let outline = Color.black.opacity(0.92)
    static let fur = Color(red: 0.10, green: 0.105, blue: 0.10)
    static let furLight = Color(red: 0.16, green: 0.165, blue: 0.16)
    static let furDark = Color(red: 0.055, green: 0.058, blue: 0.055)
    static let innerEar = Color(red: 0.42, green: 0.40, blue: 0.43)
    static let eye = Color(red: 1.0, green: 0.86, blue: 0.05)
    static let eyeGlow = Color(red: 1.0, green: 0.94, blue: 0.24)
    static let blush = Color(red: 0.82, green: 0.28, blue: 0.38)
    static let tear = Color(red: 0.35, green: 0.70, blue: 1.0)
}

private struct ShadowBodyView: View {
    var pose: ShadowRigPose

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 21, style: .continuous)
                .fill(ShadowRigColors.outline)
                .frame(width: 43, height: 48)
                .rotationEffect(pose.bodyRotation)
                .scaleEffect(x: pose.bodyScale.width, y: pose.bodyScale.height)

            RoundedRectangle(cornerRadius: 19, style: .continuous)
                .fill(
                    LinearGradient(
                        colors: [ShadowRigColors.furLight, ShadowRigColors.fur, ShadowRigColors.furDark],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
                .frame(width: 38, height: 43)
                .rotationEffect(pose.bodyRotation)
                .scaleEffect(x: pose.bodyScale.width, y: pose.bodyScale.height)
                .overlay(alignment: .bottom) {
                    Capsule()
                        .fill(Color.black.opacity(0.20))
                        .frame(width: 20, height: 8)
                        .offset(y: -4)
                }
        }
        .position(x: 50, y: 70)
    }
}

private struct ShadowHeadView: View {
    var pose: ShadowRigPose

    var body: some View {
        ZStack {
            ShadowEarView(side: -1, droop: pose.earDroop)
                .offset(x: -21, y: -22)
            ShadowEarView(side: 1, droop: pose.earDroop)
                .offset(x: 21, y: -22)

            RoundedRectangle(cornerRadius: 20, style: .continuous)
                .fill(ShadowRigColors.outline)
                .frame(width: 59, height: 49)

            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(
                    RadialGradient(
                        colors: [ShadowRigColors.furLight, ShadowRigColors.fur, ShadowRigColors.furDark],
                        center: .topLeading,
                        startRadius: 4,
                        endRadius: 48
                    )
                )
                .frame(width: 53, height: 44)

            ShadowEyesView(pose: pose)
                .offset(y: -3)

            ShadowMouthView(open: pose.mouthOpen, sad: pose.sadEyes)
                .offset(y: 9)

            if pose.tears > 0 {
                ShadowTearsView()
                    .offset(y: 8)
            }
        }
        .rotationEffect(pose.headRotation)
        .position(x: 50, y: 42)
    }
}

private struct ShadowEarView: View {
    var side: CGFloat
    var droop: CGFloat

    var body: some View {
        ZStack {
            Triangle()
                .fill(ShadowRigColors.outline)
                .frame(width: 22, height: 27)
            Triangle()
                .fill(ShadowRigColors.fur)
                .frame(width: 17, height: 22)
                .offset(y: 2)
            Triangle()
                .fill(ShadowRigColors.innerEar)
                .frame(width: 9, height: 13)
                .offset(y: 5)
        }
        .rotationEffect(.degrees(side * (16 + droop * 22)))
        .offset(y: droop * 5)
    }
}

private struct ShadowEyesView: View {
    var pose: ShadowRigPose

    var body: some View {
        HStack(spacing: 13) {
            eye(side: -1)
            eye(side: 1)
        }
    }

    @ViewBuilder
    private func eye(side: CGFloat) -> some View {
        if pose.happyEyes {
            HappyEyeShape()
                .stroke(ShadowRigColors.outline, style: StrokeStyle(lineWidth: 4, lineCap: .round))
                .frame(width: 12, height: 8)
        } else if pose.sadEyes {
            SadEyeShape(side: side)
                .stroke(ShadowRigColors.outline, style: StrokeStyle(lineWidth: 4, lineCap: .round))
                .frame(width: 12, height: 8)
        } else {
            ZStack {
                Ellipse()
                    .fill(ShadowRigColors.outline)
                    .frame(width: 16, height: 20)
                Ellipse()
                    .fill(ShadowRigColors.eyeGlow)
                    .frame(width: 12, height: 16)
                Circle()
                    .fill(Color.black)
                    .frame(width: 6, height: 9)
                    .offset(x: side * 1.0)
                Circle()
                    .fill(Color.white.opacity(0.75))
                    .frame(width: 3, height: 3)
                    .offset(x: -2, y: -4)
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(ShadowRigColors.furDark)
                    .frame(width: 16, height: max(1, 20 * pose.blink))
                    .offset(y: -10 + (10 * pose.blink))
            }
            .frame(width: 16, height: 20)
        }
    }
}

private struct ShadowMouthView: View {
    var open: Bool
    var sad: Bool

    var body: some View {
        ZStack {
            Circle()
                .fill(ShadowRigColors.outline)
                .frame(width: 4, height: 3)
                .offset(y: -3)

            if sad {
                SadMouthShape()
                    .stroke(ShadowRigColors.outline, style: StrokeStyle(lineWidth: 2.2, lineCap: .round))
                    .frame(width: 13, height: 8)
                    .offset(y: 2)
            } else {
                SmileShape()
                    .stroke(ShadowRigColors.outline, style: StrokeStyle(lineWidth: 2.1, lineCap: .round))
                    .frame(width: 16, height: 8)
                    .offset(y: 1)

                if open {
                    Capsule()
                        .fill(ShadowRigColors.blush)
                        .frame(width: 7, height: 6)
                        .offset(y: 5)
                }
            }
        }
        .frame(width: 20, height: 14)
    }
}

private struct ShadowTearsView: View {
    var body: some View {
        HStack(spacing: 24) {
            Capsule()
                .fill(ShadowRigColors.tear.opacity(0.85))
                .frame(width: 4, height: 12)
                .offset(y: 4)
            Capsule()
                .fill(ShadowRigColors.tear.opacity(0.85))
                .frame(width: 4, height: 12)
                .offset(y: 7)
        }
    }
}

private struct ShadowFrontLegsView: View {
    var pose: ShadowRigPose

    var body: some View {
        ZStack {
            paw(x: 39, y: 82, offset: pose.leftFrontLegOffset, raised: pose.raisedPaw > 0 && pose.raisedPawSide < 0)
            paw(x: 61, y: 82, offset: pose.rightFrontLegOffset, raised: pose.raisedPaw > 0 && pose.raisedPawSide > 0)
        }
    }

    private func paw(x: CGFloat, y: CGFloat, offset: CGSize, raised: Bool) -> some View {
        ZStack {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(ShadowRigColors.outline)
                .frame(width: raised ? 13 : 12, height: raised ? 25 : 22)
            RoundedRectangle(cornerRadius: 7, style: .continuous)
                .fill(ShadowRigColors.fur)
                .frame(width: raised ? 9 : 8, height: raised ? 21 : 18)
                .offset(y: 1)
        }
        .rotationEffect(.degrees(raised ? (pose.raisedPawSide * -22) : 0))
        .position(x: x + offset.width, y: y + offset.height)
    }
}

private struct ShadowRearLegsView: View {
    var pose: ShadowRigPose

    var body: some View {
        ZStack {
            rearPaw(x: 34, y: 94, offset: pose.leftRearLegOffset)
            rearPaw(x: 66, y: 94, offset: pose.rightRearLegOffset)
        }
    }

    private func rearPaw(x: CGFloat, y: CGFloat, offset: CGSize) -> some View {
        ZStack {
            Capsule()
                .fill(ShadowRigColors.outline)
                .frame(width: 20, height: 13)
            Capsule()
                .fill(ShadowRigColors.fur)
                .frame(width: 16, height: 9)
                .offset(y: -1)
        }
        .position(x: x + offset.width, y: y + offset.height)
    }
}

private struct ShadowTailView: View {
    var pose: ShadowRigPose

    var body: some View {
        ZStack {
            ShadowTailShape(kind: pose.tailKind, wave: pose.tailWave)
                .stroke(ShadowRigColors.outline, style: StrokeStyle(lineWidth: 13, lineCap: .round, lineJoin: .round))
            ShadowTailShape(kind: pose.tailKind, wave: pose.tailWave)
                .stroke(ShadowRigColors.fur, style: StrokeStyle(lineWidth: 8, lineCap: .round, lineJoin: .round))
        }
        .frame(width: 100, height: 108)
    }
}

private struct ShadowSparklesView: View {
    var time: TimeInterval

    var body: some View {
        let pulse = CGFloat(abs(sin(time * Double.pi * 2.4)))
        ZStack {
            sparkle(x: 24, y: 27, size: 8 + pulse * 3)
            sparkle(x: 78, y: 23, size: 7 + (1 - pulse) * 3)
            sparkle(x: 85, y: 54, size: 5 + pulse * 2)
        }
        .foregroundStyle(ShadowRigColors.eye.opacity(0.9))
    }

    private func sparkle(x: CGFloat, y: CGFloat, size: CGFloat) -> some View {
        SparkleShape()
            .fill(ShadowRigColors.eye.opacity(0.82))
            .frame(width: size, height: size)
            .position(x: x, y: y)
    }
}

private struct ShadowTailShape: Shape {
    var kind: ShadowTailKind
    var wave: CGFloat

    func path(in rect: CGRect) -> Path {
        var path = Path()
        let start = CGPoint(x: rect.midX + 18, y: rect.midY + 29)
        path.move(to: start)

        switch kind {
        case .curl:
            path.addCurve(
                to: CGPoint(x: rect.midX + 32 + wave * 3, y: rect.midY + 4 + wave * 2),
                control1: CGPoint(x: rect.midX + 37, y: rect.midY + 22),
                control2: CGPoint(x: rect.midX + 42, y: rect.midY + 5)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 18 + wave * 4, y: rect.midY + 1),
                control1: CGPoint(x: rect.midX + 22, y: rect.midY - 12),
                control2: CGPoint(x: rect.midX + 8, y: rect.midY - 7)
            )
        case .softCurl:
            path.addCurve(
                to: CGPoint(x: rect.midX + 35 + wave * 2, y: rect.midY + 8),
                control1: CGPoint(x: rect.midX + 38, y: rect.midY + 26),
                control2: CGPoint(x: rect.midX + 42, y: rect.midY + 11)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 23 + wave * 3, y: rect.midY + 5),
                control1: CGPoint(x: rect.midX + 31, y: rect.midY - 1),
                control2: CGPoint(x: rect.midX + 24, y: rect.midY - 1)
            )
        case .highSwish:
            path.addCurve(
                to: CGPoint(x: rect.midX + 36, y: rect.midY - 17 + wave * 6),
                control1: CGPoint(x: rect.midX + 34, y: rect.midY + 17),
                control2: CGPoint(x: rect.midX + 28 + wave * 8, y: rect.midY - 7)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 20 + wave * 5, y: rect.midY - 25),
                control1: CGPoint(x: rect.midX + 47, y: rect.midY - 28),
                control2: CGPoint(x: rect.midX + 32, y: rect.midY - 37)
            )
        case .lifted:
            path.addCurve(
                to: CGPoint(x: rect.midX + 33 + wave * 3, y: rect.midY - 19),
                control1: CGPoint(x: rect.midX + 38, y: rect.midY + 14),
                control2: CGPoint(x: rect.midX + 42 + wave * 3, y: rect.midY - 8)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 25 + wave * 4, y: rect.midY - 31),
                control1: CGPoint(x: rect.midX + 27, y: rect.midY - 25),
                control2: CGPoint(x: rect.midX + 25, y: rect.midY - 29)
            )
        case .low:
            path.addCurve(
                to: CGPoint(x: rect.midX + 39 + wave * 4, y: rect.midY + 38),
                control1: CGPoint(x: rect.midX + 32, y: rect.midY + 34),
                control2: CGPoint(x: rect.midX + 35, y: rect.midY + 40)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 47 + wave * 2, y: rect.midY + 32),
                control1: CGPoint(x: rect.midX + 43, y: rect.midY + 35),
                control2: CGPoint(x: rect.midX + 45, y: rect.midY + 34)
            )
        case .questionCurl:
            path.addCurve(
                to: CGPoint(x: rect.midX + 32 + wave * 2, y: rect.midY - 17),
                control1: CGPoint(x: rect.midX + 41, y: rect.midY + 20),
                control2: CGPoint(x: rect.midX + 44, y: rect.midY - 9)
            )
            path.addCurve(
                to: CGPoint(x: rect.midX + 15 + wave * 4, y: rect.midY - 9),
                control1: CGPoint(x: rect.midX + 23, y: rect.midY - 30),
                control2: CGPoint(x: rect.midX + 6, y: rect.midY - 23)
            )
        }

        return path
    }
}

private struct Triangle: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

private struct SmileShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addCurve(
            to: CGPoint(x: rect.minX + 2, y: rect.midY),
            control1: CGPoint(x: rect.midX - 2, y: rect.midY + 5),
            control2: CGPoint(x: rect.minX + 2, y: rect.midY + 5)
        )
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addCurve(
            to: CGPoint(x: rect.maxX - 2, y: rect.midY),
            control1: CGPoint(x: rect.midX + 2, y: rect.midY + 5),
            control2: CGPoint(x: rect.maxX - 2, y: rect.midY + 5)
        )
        return path
    }
}

private struct SadMouthShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX + 2, y: rect.maxY - 1))
        path.addQuadCurve(
            to: CGPoint(x: rect.maxX - 2, y: rect.maxY - 1),
            control: CGPoint(x: rect.midX, y: rect.minY)
        )
        return path
    }
}

private struct HappyEyeShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX + 1, y: rect.midY + 2))
        path.addQuadCurve(
            to: CGPoint(x: rect.maxX - 1, y: rect.midY + 2),
            control: CGPoint(x: rect.midX, y: rect.minY)
        )
        return path
    }
}

private struct SadEyeShape: Shape {
    var side: CGFloat

    func path(in rect: CGRect) -> Path {
        var path = Path()
        if side < 0 {
            path.move(to: CGPoint(x: rect.minX + 1, y: rect.minY + 1))
            path.addLine(to: CGPoint(x: rect.maxX - 1, y: rect.maxY - 1))
        } else {
            path.move(to: CGPoint(x: rect.maxX - 1, y: rect.minY + 1))
            path.addLine(to: CGPoint(x: rect.minX + 1, y: rect.maxY - 1))
        }
        return path
    }
}

private struct SparkleShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.midX + rect.width * 0.18, y: rect.midY - rect.height * 0.18))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.midX + rect.width * 0.18, y: rect.midY + rect.height * 0.18))
        path.addLine(to: CGPoint(x: rect.midX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.midX - rect.width * 0.18, y: rect.midY + rect.height * 0.18))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.midX - rect.width * 0.18, y: rect.midY - rect.height * 0.18))
        path.closeSubpath()
        return path
    }
}
