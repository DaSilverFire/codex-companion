import Foundation

enum CodexTaskLifecycleStatus: Equatable, Sendable {
    case active
    case completed
    case failed
}

struct CodexTaskLifecycleState: Equatable, Sendable {
    var status: CodexTaskLifecycleStatus
    var turnID: String?

    var isActive: Bool {
        status == .active
    }
}

enum CodexTaskLifecycleParser {
    static func state(from data: Data) -> CodexTaskLifecycleState? {
        guard
            let raw = try? JSONSerialization.jsonObject(with: data),
            let root = raw as? [String: Any],
            root["type"] as? String == "event_msg",
            let payload = root["payload"] as? [String: Any],
            let type = payload["type"] as? String
        else {
            return nil
        }
        let turnID = payload["turn_id"] as? String
        switch type {
        case "task_started", "turn_started":
            return CodexTaskLifecycleState(status: .active, turnID: turnID)
        case "task_complete", "task_completed", "turn_complete", "turn_completed":
            return CodexTaskLifecycleState(status: .completed, turnID: turnID)
        case "task_aborted", "task_failed", "turn_aborted", "turn_failed":
            return CodexTaskLifecycleState(status: .failed, turnID: turnID)
        default:
            return nil
        }
    }
}
