import Foundation

enum RouteMode: String, CaseIterable, Identifiable, Codable {
    case smart
    case chatGPT
    case codex

    var id: String { rawValue }

    static let selectableCases: [RouteMode] = [.chatGPT, .codex]

    var title: String {
        switch self {
        case .smart: "Smart"
        case .chatGPT: "Chat"
        case .codex: "Codex"
        }
    }
}

enum ChatGPTModel: String, CaseIterable, Identifiable, Codable {
    case gpt55
    case gpt55Thinking
    case gpt55Pro

    var id: String { rawValue }

    var title: String {
        switch self {
        case .gpt55:
            return "GPT-5.4 mini"
        case .gpt55Thinking:
            return "GPT-5.4 Thinking"
        case .gpt55Pro:
            return "GPT-5.4 High"
        }
    }

    var shortTitle: String {
        switch self {
        case .gpt55:
            return "5.4 mini"
        case .gpt55Thinking:
            return "5.4 Thinking"
        case .gpt55Pro:
            return "5.4 High"
        }
    }

    var accountModeNote: String {
        "\(title) can be selected for the Companion UI, but using your ChatGPT account requires the ChatGPT app or website. Hidden in-menu account calls are not supported."
    }

    var apiModelID: String {
        switch self {
        case .gpt55:
            return "gpt-5.4-mini"
        case .gpt55Thinking, .gpt55Pro:
            return "gpt-5.4"
        }
    }

    var reasoningEffort: String {
        switch self {
        case .gpt55:
            return "low"
        case .gpt55Thinking:
            return "high"
        case .gpt55Pro:
            return "xhigh"
        }
    }

    var verbosity: String {
        switch self {
        case .gpt55:
            return "low"
        case .gpt55Thinking, .gpt55Pro:
            return "medium"
        }
    }

    var costNote: String {
        switch self {
        case .gpt55:
            return "lower cost"
        case .gpt55Thinking:
            return "more reasoning"
        case .gpt55Pro:
            return "highest cost"
        }
    }
}

enum ChatGPTDeliveryMode: String, CaseIterable, Identifiable, Codable {
    case onDevice
    case appHandoff
    case openAIAPI

    var id: String { rawValue }

    var title: String {
        switch self {
        case .onDevice:
            return "On-device Apple model"
        case .appHandoff:
            return "ChatGPT app handoff"
        case .openAIAPI:
            return "OpenAI API"
        }
    }

    var shortTitle: String {
        switch self {
        case .onDevice:
            return "On-device"
        case .appHandoff:
            return "App handoff"
        case .openAIAPI:
            return "API"
        }
    }

    var description: String {
        switch self {
        case .onDevice:
            return "Answers privately on this Mac without an API key or Codex usage."
        case .appHandoff:
            return "Uses your signed-in ChatGPT app quick bar. No API key or API billing."
        case .openAIAPI:
            return "Answers inside Companion through your OpenAI API key."
        }
    }
}

enum RouteDestination: String, Codable {
    case chatGPT
    case codex

    var title: String {
        switch self {
        case .chatGPT: "ChatGPT"
        case .codex: "Codex"
        }
    }
}

struct ChatGPTMenuResponse: Identifiable, Equatable {
    let id = UUID()
    var model: ChatGPTModel
    var sourceTitle: String? = nil
    var prompt: String
    var message: String
    var usageSummary: String?

    var displayTitle: String {
        sourceTitle ?? model.title
    }
}

struct RouteHistoryItem: Identifiable, Codable, Hashable {
    var id: UUID
    var prompt: String
    var destination: RouteDestination
    var createdAt: Date
}

struct RouteResult {
    var destination: RouteDestination
    var message: String
    var succeeded = true
}
