import Foundation

enum PetSource: Hashable {
    case custom(URL)
    case builtIn(URL)

    var title: String {
        switch self {
        case .custom:
            "Custom"
        case .builtIn:
            "Built-in"
        }
    }
}

struct PetDefinition: Identifiable, Hashable {
    struct DirectionalLookFrames: Hashable {
        var spritesheetURL: URL
        var spriteColumns: Int
        var spriteRows: Int
        var startRow: Int
    }

    var id: String
    var displayName: String
    var description: String
    var spritesheetURL: URL
    var spriteColumns: Int
    var spriteRows: Int
    var animationFrameCounts: [String: Int]
    var directionalLookFrames: DirectionalLookFrames? = nil
    var source: PetSource

    var renderIdentity: String {
        let lookIdentity = directionalLookFrames.map {
            "\($0.spritesheetURL.path)|\($0.spriteColumns)x\($0.spriteRows)|\($0.startRow)"
        } ?? "no-look"
        return "\(id)|\(spritesheetURL.path)|\(spriteColumns)x\(spriteRows)|\(lookIdentity)"
    }

    func frameCount(for state: PetAnimationState) -> Int {
        let resolvedState = resolvedAnimationState(for: state)
        let explicit = animationFrameCounts[state.rawValue]
            ?? animationFrameCounts[resolvedState.rawValue]
        return max(
            1,
            min(spriteColumns, explicit ?? defaultFrameCount(for: resolvedState))
        )
    }

    func hasNativeRow(for state: PetAnimationState) -> Bool {
        state.rowIndex < spriteRows
    }

    func resolvedAnimationState(for state: PetAnimationState) -> PetAnimationState {
        guard !hasNativeRow(for: state) else { return state }

        switch state {
        case .thinking:
            return hasNativeRow(for: .running) ? .running : .idle
        case .talking:
            return hasNativeRow(for: .review) ? .review : .idle
        default:
            return state
        }
    }

    var usesShadowStyle: Bool {
        id.localizedCaseInsensitiveContains("shadow")
            || displayName.localizedCaseInsensitiveContains("shadow")
    }

    private func defaultFrameCount(for state: PetAnimationState) -> Int {
        switch state {
        case .idle: 6
        case .runningRight, .runningLeft: 8
        case .waving: 4
        case .jumping: 5
        case .failed: 8
        case .waiting, .running, .review: 6
        case .goalComplete: 8
        case .thinking, .talking: 16
        }
    }
}

struct PetManifest: Decodable {
    var id: String?
    var displayName: String?
    var description: String?
    var spritesheetPath: String?
    var spriteColumns: Int?
    var spriteRows: Int?
    var animationFrameCounts: [String: Int]?
    var lookSpritesheetPath: String?
    var lookSpriteColumns: Int?
    var lookSpriteRows: Int?
    var lookFrameStartRow: Int?
}
