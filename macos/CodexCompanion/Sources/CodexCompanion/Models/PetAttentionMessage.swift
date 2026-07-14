import Foundation

struct PetProcessSnapshot: Equatable, Sendable {
    var status: CodexProcessItem.Status
    var goalStatus: CodexGoalStatus?
    var goalID: String?
    var fullMessage: String

    init(item: CodexProcessItem) {
        status = item.status
        goalStatus = item.goalStatus
        goalID = item.goalID
        fullMessage = item.fullMessage
    }
}

struct PetAttentionMessage: Identifiable, Equatable, Sendable {
    enum Kind: Int, Equatable, Sendable {
        case response = 1
        case attention = 2
        case completion = 3
        case goal = 4
        case failure = 5
    }

    var id = UUID()
    var kind: Kind
    var title: String
    var detail: String
    var processTitle: String
    var processID: String
    var threadID: String?
    var reactionContext: PetReactionContext

    var supportingText: String {
        processTitle.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    func replacingTitle(_ title: String) -> PetAttentionMessage {
        var message = self
        message.title = title
        return message
    }

    static func appearance(current: CodexProcessItem) -> PetAttentionMessage? {
        guard current.kind != .notice else { return nil }

        if current.status == .failed {
            return message(
                kind: .failure,
                event: .failure,
                detail: current.fullMessage,
                current: current
            )
        }

        if current.goalStatus == .active {
            return message(
                kind: .goal,
                event: .goalStarted,
                detail: current.goalObjective ?? current.title,
                current: current
            )
        }

        if current.status == .waiting {
            return message(
                kind: .attention,
                event: .approval,
                detail: current.fullMessage,
                current: current
            )
        }

        if current.status == .completed {
            return message(
                kind: .completion,
                event: current.goalStatus == .complete ? .goalCompletion : .completion,
                detail: current.fullMessage,
                current: current
            )
        }

        return nil
    }

    static func transition(
        previous: PetProcessSnapshot,
        current: CodexProcessItem
    ) -> PetAttentionMessage? {
        guard current.kind != .notice else { return nil }

        if current.status == .failed, previous.status != .failed {
            return message(
                kind: .failure,
                event: .failure,
                detail: current.fullMessage,
                current: current
            )
        }

        if current.goalStatus == .active,
           previous.goalStatus != .active || previous.goalID != current.goalID {
            return message(
                kind: .goal,
                event: .goalStarted,
                detail: current.goalObjective ?? current.title,
                current: current
            )
        }

        if current.status == .completed, previous.status != .completed {
            return message(
                kind: .completion,
                event: current.goalStatus == .complete ? .goalCompletion : .completion,
                detail: current.fullMessage,
                current: current
            )
        }

        if current.status == .waiting, previous.status == .running {
            return message(
                kind: .attention,
                event: .approval,
                detail: current.fullMessage,
                current: current
            )
        }

        let oldMessage = previous.fullMessage.trimmingCharacters(in: .whitespacesAndNewlines)
        let newMessage = current.fullMessage.trimmingCharacters(in: .whitespacesAndNewlines)
        if !newMessage.isEmpty, newMessage != oldMessage {
            return message(
                kind: .response,
                event: .response,
                detail: newMessage,
                current: current
            )
        }

        return nil
    }

    private static func message(
        kind: Kind,
        event: PetReactionEvent,
        detail: String,
        current: CodexProcessItem
    ) -> PetAttentionMessage {
        let detail = detail.trimmingCharacters(in: .whitespacesAndNewlines)
        let processTitle = current.title.trimmingCharacters(in: .whitespacesAndNewlines)
        let goalObjective = current.goalObjective?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let reactionContext = PetReactionContext(
            event: event,
            processID: current.id,
            processTitle: processTitle,
            detail: detail,
            goalObjective: goalObjective?.isEmpty == false ? goalObjective : nil
        )

        return PetAttentionMessage(
            kind: kind,
            title: PetReactionCopy.fallback(for: reactionContext, excluding: []),
            detail: detail,
            processTitle: processTitle,
            processID: current.id,
            threadID: current.threadID,
            reactionContext: reactionContext
        )
    }

}

enum PetAttentionAccent: Equatable, Sendable {
    case blue
    case yellow
    case green
    case indigo
    case red
}

struct PetAttentionHighlight: Equatable, Sendable {
    var processID: String
    var kind: PetAttentionMessage.Kind

    init(message: PetAttentionMessage) {
        processID = message.processID
        kind = message.kind
    }

    var accent: PetAttentionAccent {
        switch kind {
        case .response: return .blue
        case .attention: return .yellow
        case .completion: return .green
        case .goal: return .indigo
        case .failure: return .red
        }
    }
}
