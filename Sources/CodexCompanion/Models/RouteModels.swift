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
    case gpt56Luna
    case gpt56Terra
    case gpt56Sol

    var id: String { rawValue }

    var title: String {
        switch self {
        case .gpt56Luna:
            return "GPT-5.6 Luna"
        case .gpt56Terra:
            return "GPT-5.6 Terra"
        case .gpt56Sol:
            return "GPT-5.6 Sol"
        }
    }

    var shortTitle: String {
        switch self {
        case .gpt56Luna:
            return "5.6 Luna"
        case .gpt56Terra:
            return "5.6 Terra"
        case .gpt56Sol:
            return "5.6 Sol"
        }
    }

    var accountModeNote: String {
        "\(title) can be selected for the Companion UI, but using your ChatGPT account requires the ChatGPT app or website. Hidden in-menu account calls are not supported."
    }

    var apiModelID: String {
        switch self {
        case .gpt56Luna:
            return "gpt-5.6-luna"
        case .gpt56Terra:
            return "gpt-5.6-terra"
        case .gpt56Sol:
            return "gpt-5.6-sol"
        }
    }

    var reasoningEffort: String {
        switch self {
        case .gpt56Luna:
            return "low"
        case .gpt56Terra:
            return "high"
        case .gpt56Sol:
            return "xhigh"
        }
    }

    var verbosity: String {
        switch self {
        case .gpt56Luna:
            return "low"
        case .gpt56Terra, .gpt56Sol:
            return "medium"
        }
    }

    var costNote: String {
        switch self {
        case .gpt56Luna:
            return "lowest cost"
        case .gpt56Terra:
            return "balanced"
        case .gpt56Sol:
            return "highest capability"
        }
    }

    static func restoringPersistedSelection(_ rawValue: String?) -> ChatGPTModel {
        if let rawValue, let currentModel = ChatGPTModel(rawValue: rawValue) {
            return currentModel
        }

        switch rawValue {
        case "gpt55":
            return .gpt56Luna
        case "gpt55Thinking":
            return .gpt56Terra
        case "gpt55Pro":
            return .gpt56Sol
        default:
            return .gpt56Luna
        }
    }
}

enum ChatGPTDeliveryMode: String, CaseIterable, Identifiable, Codable {
    case onDevice
    case openAIAPI
    case lumoAPI

    var id: String { rawValue }

    var title: String {
        switch self {
        case .onDevice:
            return "On-device Apple model"
        case .openAIAPI:
            return "OpenAI API"
        case .lumoAPI:
            return "Lumo API"
        }
    }

    var shortTitle: String {
        switch self {
        case .onDevice:
            return "On-device"
        case .openAIAPI:
            return "API"
        case .lumoAPI:
            return "Lumo"
        }
    }

    var description: String {
        switch self {
        case .onDevice:
            return "Reasons on this Mac without an API key or Codex usage; live tools contact their data sources when needed."
        case .openAIAPI:
            return "Answers inside Companion through your OpenAI API key."
        case .lumoAPI:
            return "Answers inside Companion through an API key included with Lumo+."
        }
    }

    static func restoringPersistedSelection(_ rawValue: String?) -> ChatGPTDeliveryMode {
        guard let rawValue else { return .onDevice }
        if let currentMode = ChatGPTDeliveryMode(rawValue: rawValue) {
            return currentMode
        }

        // Older builds persisted this mode before the ChatGPT window handoff was removed.
        return .onDevice
    }
}

enum LumoModel: String, CaseIterable, Identifiable, Codable {
    case automatic
    case fast
    case thinking

    var id: String { rawValue }

    var title: String {
        switch self {
        case .automatic:
            return "Lumo Auto"
        case .fast:
            return "Lumo Fast"
        case .thinking:
            return "Lumo Thinking"
        }
    }

    var shortTitle: String {
        switch self {
        case .automatic:
            return "Auto"
        case .fast:
            return "Fast"
        case .thinking:
            return "Thinking"
        }
    }

    var apiModelID: String {
        switch self {
        case .automatic:
            return "auto"
        case .fast:
            return "lumo-basic-v1"
        case .thinking:
            return "lumo-plus-v1"
        }
    }

    var capabilityNote: String {
        switch self {
        case .automatic:
            return "best available model"
        case .fast:
            return "fast responses"
        case .thinking:
            return "deeper reasoning"
        }
    }

    static func restoringPersistedSelection(_ rawValue: String?) -> LumoModel {
        guard let rawValue, let model = LumoModel(rawValue: rawValue) else {
            return .automatic
        }
        return model
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
