import Foundation

enum PetAnimationState: String, CaseIterable, Identifiable {
    case idle
    case runningRight = "running-right"
    case runningLeft = "running-left"
    case waving
    case jumping
    case failed
    case waiting
    case running
    case review
    case goalComplete = "goal-complete"
    case thinking
    case talking

    var id: String { rawValue }

    var title: String {
        switch self {
        case .idle: "Idle"
        case .runningRight: "Run Right"
        case .runningLeft: "Run Left"
        case .waving: "Wave"
        case .jumping: "Jump"
        case .failed: "Failed"
        case .waiting: "Waiting"
        case .running: "Running"
        case .review: "Review"
        case .goalComplete: "Goal Complete"
        case .thinking: "Thinking"
        case .talking: "Talking"
        }
    }

    var rowIndex: Int {
        switch self {
        case .idle: 0
        case .runningRight: 1
        case .runningLeft: 2
        case .waving: 3
        case .jumping: 4
        case .failed: 5
        case .waiting: 6
        case .running: 7
        case .review: 8
        case .goalComplete: 9
        case .thinking: 10
        case .talking: 11
        }
    }

    var previewFrameDuration: TimeInterval {
        switch self {
        case .idle: 0.16
        case .runningRight, .runningLeft, .running: 0.08
        case .jumping: 0.09
        case .waving: 0.12
        case .failed: 0.15
        case .waiting, .review: 0.14
        case .goalComplete: 0.10
        case .thinking: 0.13
        case .talking: 0.075
        }
    }

    var finalFrameDuration: TimeInterval {
        switch self {
        case .idle, .waiting, .review: 0.24
        case .runningRight, .runningLeft, .running: 0.08
        case .waving, .jumping: 0.20
        case .failed: 0.24
        case .goalComplete: 0.18
        case .thinking: 0.18
        case .talking: 0.08
        }
    }

    var loopsContinuously: Bool {
        switch self {
        case .idle, .runningRight, .runningLeft, .running, .thinking, .talking:
            true
        default:
            false
        }
    }

    func frameTiming(frameCount: Int) -> (base: TimeInterval, final: TimeInterval) {
        let count = max(1, frameCount)
        guard count > 12 else {
            return (previewFrameDuration, finalFrameDuration)
        }

        let finalHold = max(previewFrameDuration, finalFrameDuration)
        let bodyDuration = max(0, targetCycleDuration - finalHold)
        let base = max(minimumFrameDuration, bodyDuration / Double(max(1, count - 1)))
        return (base, finalHold)
    }

    private var targetCycleDuration: TimeInterval {
        switch self {
        case .idle:
            return 5.2
        case .runningRight, .runningLeft, .running:
            return 2.65
        case .waving:
            return 3.6
        case .jumping:
            return 2.75
        case .failed:
            return 4.8
        case .waiting:
            return 5.0
        case .review:
            return 4.4
        case .goalComplete:
            return 3.9
        case .thinking:
            return 3.6
        case .talking:
            return 1.8
        }
    }

    private var minimumFrameDuration: TimeInterval {
        switch self {
        case .runningRight, .runningLeft, .running, .talking:
            return 0.055
        case .jumping, .goalComplete:
            return 0.065
        default:
            return 0.08
        }
    }

    static let idleDefaultFrameCount = 6
}
