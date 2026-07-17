import Foundation

struct CompanionTrayDestination: Equatable {
    var routeMode: RouteMode
    var isQuickBarOpen: Bool
    var isCodexProcessTrayVisible: Bool
}

struct PetRoamingMotionPrimer {
    private(set) var isPrimed = false

    mutating func shouldMove(animationAccepted: Bool) -> Bool {
        guard animationAccepted else {
            isPrimed = false
            return false
        }
        guard isPrimed else {
            isPrimed = true
            return false
        }
        return true
    }

    mutating func reset() {
        isPrimed = false
    }
}

enum CompanionProcessAction: Hashable {
    case reply
    case steer
}

enum CompanionPresentationPolicy {
    static let attentionDestination = CompanionTrayDestination(
        routeMode: .codex,
        isQuickBarOpen: true,
        isCodexProcessTrayVisible: true
    )

    static func showsProcessActions(
        isHovered: Bool,
        canTargetCodexThread: Bool
    ) -> Bool {
        isHovered && canTargetCodexThread
    }

    static func processActions(
        status: CodexProcessItem.Status,
        isHovered: Bool,
        canTargetCodexThread: Bool
    ) -> Set<CompanionProcessAction> {
        guard showsProcessActions(
            isHovered: isHovered,
            canTargetCodexThread: canTargetCodexThread
        ) else { return [] }

        switch status {
        case .running, .completed:
            return [.reply, .steer]
        case .failed:
            return [.reply]
        case .waiting:
            return []
        }
    }

    static func processAccent(
        status: CodexProcessItem.Status,
        runtimeStatus: CodexThreadRuntimeStatus?,
        attentionAccent: PetAttentionAccent?
    ) -> PetAttentionAccent? {
        if status == .failed {
            return .red
        }
        if runtimeStatus == .waitingOnApproval {
            return .yellow
        }
        return attentionAccent
    }

    static func pausesRoaming(
        isQuickBarOpen: Bool,
        isPetVisible: Bool,
        hasAttentionMessage: Bool,
        allowsAutonomousMovement: Bool
    ) -> Bool {
        !allowsAutonomousMovement || isQuickBarOpen || !isPetVisible || hasAttentionMessage
    }

    static func allowsDirectionalLookTracking(
        isQuickBarOpen: Bool,
        isPetVisible: Bool,
        hasAttentionMessage: Bool
    ) -> Bool {
        !isQuickBarOpen && isPetVisible && !hasAttentionMessage
    }

    static func requiresPointerHoverReconciliation(
        isQuickBarOpen: Bool,
        isPetVisible: Bool
    ) -> Bool {
        isQuickBarOpen && isPetVisible
    }

    static func showsPetMenuControls(
        isQuickBarOpen: Bool,
        hidesUntilHover: Bool,
        isPetHovered: Bool,
        isMenuControlHovered: Bool
    ) -> Bool {
        if isQuickBarOpen {
            return isPetHovered || isMenuControlHovered
        }
        return !hidesUntilHover
            || isPetHovered
            || isMenuControlHovered
    }

    static func acceptsRoamingMotion(
        isPetHovered: Bool,
        isPetDragging: Bool,
        hasInteractionAnimation: Bool
    ) -> Bool {
        !isPetHovered && !isPetDragging && !hasInteractionAnimation
    }

    static func showsComposerSurface(
        isCodexProcessTrayVisible: Bool,
        hasProcessTarget: Bool,
        showsAccessibilityNotice: Bool
    ) -> Bool {
        !isCodexProcessTrayVisible
    }

    static func showsPromptField(
        routeMode: RouteMode,
        isCodexProcessTrayVisible: Bool,
        hasProcessTarget: Bool
    ) -> Bool {
        if routeMode == .codex || isCodexProcessTrayVisible {
            return hasProcessTarget
        }
        return true
    }
}
