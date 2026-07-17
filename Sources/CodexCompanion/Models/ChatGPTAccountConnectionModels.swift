import Foundation

/// Internal Companion value models. Their synthesized `Codable` representation is not an
/// OpenAI endpoint or wire-protocol contract.
struct ChatGPTAccountModelCapability: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var displayName: String
    var isDefault: Bool
}

struct ChatGPTAccountAgentCapability: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var displayName: String
    var description: String
}

struct ChatGPTAccountCapabilities: Codable, Equatable, Sendable {
    var transportID: String
    var transportName: String
    var supportsStreaming: Bool
    var supportsConversationContinuation: Bool
    var models: [ChatGPTAccountModelCapability]
    var agents: [ChatGPTAccountAgentCapability]
}

struct ChatGPTAccountChatRequest: Codable, Equatable, Sendable {
    var conversationID: String?
    var prompt: String
    var modelID: String?
    var agentID: String?

    init(
        conversationID: String? = nil,
        prompt: String,
        modelID: String? = nil,
        agentID: String? = nil
    ) {
        self.conversationID = conversationID
        self.prompt = prompt
        self.modelID = modelID
        self.agentID = agentID
    }
}

enum ChatGPTAccountStreamEvent: Codable, Equatable, Sendable {
    case conversationStarted(id: String)
    case textDelta(String)
    case completed
}

struct ChatGPTAccountAppInspection: Codable, Equatable, Sendable {
    var applicationURL: URL
    var pathExists: Bool
    /// Structural recognition only; account authorization must come from a documented bridge.
    var isInstalled: Bool
    var bundleIdentifier: String?
    var version: String?
    var build: String?
    var declaredURLSchemes: [String]
    var xpcServiceIdentifiers: [String]
    var appExtensionIdentifiers: [String]
    var hasAppIntentsMetadata: Bool
    var hasBundledCodexExecutable: Bool
}

enum ChatGPTAccountBridgeRequirement: String, CaseIterable, Codable, Hashable, Sendable {
    /// A public contract is what makes a visible internal surface safe for a third-party client.
    case documentedThirdPartyContract
    case userAuthorizedAccountAccess
    case accountEntitlementDiscovery
    case modelDiscoveryAndSelection
    case agentDiscoveryAndSelection
    case conversationSendAndContinuation
    case incrementalResponseStreaming
    case cancellationAndTerminalStatus
}

struct ChatGPTAccountMissingBridge: Codable, Equatable, Sendable {
    var requiredCapabilities: [ChatGPTAccountBridgeRequirement]

    init(
        requiredCapabilities: [ChatGPTAccountBridgeRequirement]
            = ChatGPTAccountBridgeRequirement.allCases
    ) {
        self.requiredCapabilities = requiredCapabilities
    }

    /// The integration contract Companion still needs; this deliberately names no imaginary endpoint.
    var summary: String {
        "Normal ChatGPT subscription chat needs an OpenAI-documented, user-authorized "
            + "third-party bridge that returns account entitlements plus model and agent "
            + "capabilities, then supports conversation send/continuation, incremental response "
            + "events, cancellation, and terminal status. URL handoff, private XPC services, and "
            + "the Codex app-server do not satisfy that normal-Chat contract."
    }
}

enum ChatGPTAccountUnsupportedReason: String, Codable, Equatable, Sendable {
    case chatGPTApplicationNotInstalled
    case invalidChatGPTApplicationBundle
    case noSupportedNormalChatTransport
}

struct ChatGPTAccountUnsupported: Codable, Equatable, Sendable {
    var reason: ChatGPTAccountUnsupportedReason
    var inspection: ChatGPTAccountAppInspection
    var missingBridge: ChatGPTAccountMissingBridge
}

enum ChatGPTAccountAvailability: Codable, Equatable, Sendable {
    case available(ChatGPTAccountCapabilities)
    case unsupported(ChatGPTAccountUnsupported)
}

enum ChatGPTAccountConnectionError: Error, Equatable, Sendable {
    case unsupported(ChatGPTAccountUnsupported)
}

extension ChatGPTAccountConnectionError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .unsupported(let result):
            switch result.reason {
            case .chatGPTApplicationNotInstalled:
                return "ChatGPT is not installed at \(result.inspection.applicationURL.path)."
            case .invalidChatGPTApplicationBundle:
                return "The item at \(result.inspection.applicationURL.path) is not a recognized ChatGPT application bundle."
            case .noSupportedNormalChatTransport:
                return result.missingBridge.summary
            }
        }
    }
}
