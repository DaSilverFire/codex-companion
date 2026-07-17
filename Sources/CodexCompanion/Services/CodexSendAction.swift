enum CodexSendAction: Equatable, Sendable {
    case reply
    case steer

    var logName: String {
        switch self {
        case .reply:
            return "reply"
        case .steer:
            return "steer"
        }
    }
}
