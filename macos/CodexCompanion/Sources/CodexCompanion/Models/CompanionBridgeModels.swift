import Foundation

enum CompanionBridgeProtocol {
    static let version = 1
    static let serviceType = "codex-companion"
    static let defaultTaskPageSize = 20
    static let defaultMessagePageSize = 30
    static let maximumPageSize = 50
}

enum CompanionBridgeOperation: String, Codable, Sendable {
    case handshake
    case listTasks
    case loadMessages
    case sendMessage
    case respondToApproval
    case createTask
    case loadCapabilities
    case sendCasualChat
    case loadUsage
    case consumeUsageReset
}

enum CompanionBridgeSendAction: String, Codable, CaseIterable, Sendable {
    case reply
    case steer
}

enum CompanionBridgeApprovalDecision: String, Codable, CaseIterable, Sendable {
    case approveOnce
    case approveSimilar
    case decline
}

enum CompanionBridgeTaskStatus: String, Codable, Sendable {
    case running
    case waiting
    case completed
    case failed
}

enum CompanionBridgeTaskGroupKind: String, Codable, Sendable {
    case chats
    case project
}

struct CompanionBridgeTaskGroup: Codable, Equatable, Sendable {
    var kind: CompanionBridgeTaskGroupKind
    var title: String
    var path: String?
}

enum CompanionBridgeMessageRole: String, Codable, Sendable {
    case user
    case assistant
}

enum CompanionBridgeTimelineItemKind: String, Codable, Sendable {
    case message
    case reasoning
    case tool
    case status
    case compaction
}

enum CompanionBridgeTimelineItemStatus: String, Codable, Sendable {
    case inProgress
    case completed
    case failed
}

enum CompanionBridgeTimelineItemPhase: String, Codable, Sendable {
    case commentary
    case final
}

enum CompanionBridgeMediaKind: String, Codable, Sendable {
    case image
}

struct CompanionBridgeRequest: Codable, Equatable, Sendable {
    var id: UUID
    var protocolVersion: Int = CompanionBridgeProtocol.version
    var operation: CompanionBridgeOperation
    var cursor: String?
    var limit: Int?
    var threadID: String?
    var text: String?
    var cwd: String?
    var sendAction: CompanionBridgeSendAction?
    var approvalDecision: CompanionBridgeApprovalDecision?
    var model: String?
    var reasoningEffort: String?
    var skillName: String?
    var skillPath: String?
    var chatAgentID: String?
    var resetCreditID: String?
    var idempotencyKey: UUID?

    init(
        id: UUID = UUID(),
        operation: CompanionBridgeOperation,
        cursor: String? = nil,
        limit: Int? = nil,
        threadID: String? = nil,
        text: String? = nil,
        cwd: String? = nil,
        sendAction: CompanionBridgeSendAction? = nil,
        approvalDecision: CompanionBridgeApprovalDecision? = nil,
        model: String? = nil,
        reasoningEffort: String? = nil,
        skillName: String? = nil,
        skillPath: String? = nil,
        chatAgentID: String? = nil,
        resetCreditID: String? = nil,
        idempotencyKey: UUID? = nil
    ) {
        self.id = id
        self.operation = operation
        self.cursor = cursor
        self.limit = limit
        self.threadID = threadID
        self.text = text
        self.cwd = cwd
        self.sendAction = sendAction
        self.approvalDecision = approvalDecision
        self.model = model
        self.reasoningEffort = reasoningEffort
        self.skillName = skillName
        self.skillPath = skillPath
        self.chatAgentID = chatAgentID
        self.resetCreditID = resetCreditID
        self.idempotencyKey = idempotencyKey
    }
}

struct CompanionBridgeResponse: Codable, Equatable, Sendable {
    var id: UUID
    var protocolVersion: Int = CompanionBridgeProtocol.version
    var operation: CompanionBridgeOperation
    var succeeded: Bool
    var errorCode: String?
    var message: String?
    var macName: String?
    var tasks: [CompanionBridgeTask]?
    var messages: [CompanionBridgeMessage]?
    var nextCursor: String?
    var threadID: String?
    var capabilities: CompanionBridgeCapabilities?
    var chatMessage: CompanionBridgeMessage?
    var timelineItems: [CompanionBridgeTimelineItem]?
    var revision: String?
    var timelineNextCursor: String?
    var subagents: [CompanionBridgeSubagent]?
    var contextUsage: CompanionBridgeContextUsage?
    var usageSnapshot: CompanionBridgeUsageSnapshot?

    static func success(
        for request: CompanionBridgeRequest,
        message: String? = nil,
        macName: String? = nil,
        tasks: [CompanionBridgeTask]? = nil,
        messages: [CompanionBridgeMessage]? = nil,
        nextCursor: String? = nil,
        threadID: String? = nil,
        capabilities: CompanionBridgeCapabilities? = nil,
        chatMessage: CompanionBridgeMessage? = nil,
        timelineItems: [CompanionBridgeTimelineItem]? = nil,
        revision: String? = nil,
        timelineNextCursor: String? = nil,
        subagents: [CompanionBridgeSubagent]? = nil,
        contextUsage: CompanionBridgeContextUsage? = nil,
        usageSnapshot: CompanionBridgeUsageSnapshot? = nil
    ) -> CompanionBridgeResponse {
        CompanionBridgeResponse(
            id: request.id,
            operation: request.operation,
            succeeded: true,
            message: message,
            macName: macName,
            tasks: tasks,
            messages: messages,
            nextCursor: nextCursor,
            threadID: threadID,
            capabilities: capabilities,
            chatMessage: chatMessage,
            timelineItems: timelineItems,
            revision: revision,
            timelineNextCursor: timelineNextCursor,
            subagents: subagents,
            contextUsage: contextUsage,
            usageSnapshot: usageSnapshot
        )
    }

    static func failure(
        for request: CompanionBridgeRequest,
        code: String,
        message: String
    ) -> CompanionBridgeResponse {
        CompanionBridgeResponse(
            id: request.id,
            operation: request.operation,
            succeeded: false,
            errorCode: code,
            message: message,
            contextUsage: nil,
            usageSnapshot: nil
        )
    }
}

struct CompanionBridgeContextUsage: Codable, Equatable, Sendable {
    var usedTokens: Int
    var contextWindow: Int

    var fractionUsed: Double {
        guard contextWindow > 0 else { return 0 }
        return min(1, max(0, Double(usedTokens) / Double(contextWindow)))
    }
}

struct CompanionBridgeUsageSnapshot: Codable, Equatable, Sendable {
    var planType: String?
    var groups: [CompanionBridgeUsageGroup]
    var availableResetCount: Int
    var availableResetCredits: [CompanionBridgeResetCredit]
    var updatedAt: Date
}

struct CompanionBridgeUsageGroup: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var title: String
    var shortWindow: CompanionBridgeUsageWindow?
    var weeklyWindow: CompanionBridgeUsageWindow?
}

struct CompanionBridgeUsageWindow: Codable, Equatable, Sendable {
    var remainingPercent: Double
    var durationLabel: String
    var resetsAt: Date?
}

struct CompanionBridgeResetCredit: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var displayTitle: String
    var detail: String?
    var expiresAt: Date?
}

struct CompanionBridgeTask: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var title: String
    var preview: String
    var updatedAt: Date
    var cwd: String?
    var status: CompanionBridgeTaskStatus
    var needsApproval: Bool
    var activeTurnID: String?
    var model: String?
    var reasoningEffort: String?
    var taskGroup: CompanionBridgeTaskGroup? = nil
}

struct CompanionBridgeMessage: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var role: CompanionBridgeMessageRole
    var text: String
    var createdAt: Date?
}

struct CompanionBridgeMedia: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var kind: CompanionBridgeMediaKind
    var mimeType: String
    var data: Data
}

struct CompanionBridgeTimelineItem: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var kind: CompanionBridgeTimelineItemKind
    var status: CompanionBridgeTimelineItemStatus = .completed
    var role: CompanionBridgeMessageRole? = nil
    var title: String? = nil
    var text: String? = nil
    var detail: String? = nil
    var phase: CompanionBridgeTimelineItemPhase? = nil
    var createdAt: Date? = nil
    var turnID: String? = nil
    var callID: String? = nil
    var media: [CompanionBridgeMedia] = []
}

struct CompanionBridgeSubagent: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var name: String
    var title: String
    var role: String?
    var updatedAt: Date
    var status: CompanionBridgeTaskStatus
}

struct CompanionBridgeCapabilities: Codable, Equatable, Sendable {
    var models: [CompanionBridgeModel]
    var skills: [CompanionBridgeSkill]
    var plugins: [CompanionBridgePlugin]
    var chatAgents: [CompanionBridgeChatAgent]
}

struct CompanionBridgeModel: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var model: String
    var displayName: String
    var description: String
    var isDefault: Bool
    var defaultReasoningEffort: String
    var supportedReasoningEfforts: [CompanionBridgeReasoningEffort]
}

struct CompanionBridgeReasoningEffort: Codable, Equatable, Identifiable, Sendable {
    var id: String { value }
    var value: String
    var description: String
}

struct CompanionBridgeSkill: Codable, Equatable, Identifiable, Sendable {
    var id: String { path }
    var name: String
    var displayName: String
    var description: String
    var path: String
    var scope: String
    var defaultPrompt: String?
}

struct CompanionBridgePlugin: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var name: String
    var displayName: String
    var description: String
    var enabled: Bool
    var installed: Bool
}

struct CompanionBridgeChatAgent: Codable, Equatable, Identifiable, Sendable {
    var id: String
    var name: String
    var description: String
    var symbolName: String

    static let builtIns: [CompanionBridgeChatAgent] = [
        .init(
            id: "general",
            name: "General",
            description: "Direct answers and everyday help",
            symbolName: "sparkles"
        ),
        .init(
            id: "explain",
            name: "Explain",
            description: "Clear explanations with useful context",
            symbolName: "text.book.closed"
        ),
        .init(
            id: "plan",
            name: "Plan",
            description: "Practical steps and tradeoffs",
            symbolName: "checklist"
        ),
        .init(
            id: "create",
            name: "Create",
            description: "Ideas, drafts, and alternatives",
            symbolName: "wand.and.stars"
        ),
    ]

    var promptInstruction: String {
        switch id {
        case "explain":
            return "Explain the answer clearly, define unfamiliar terms, and use a short example when useful."
        case "plan":
            return "Turn the request into a practical ordered plan. State important constraints and tradeoffs."
        case "create":
            return "Generate polished ideas or drafts. Offer distinct alternatives when there is more than one good direction."
        default:
            return "Answer directly and concisely."
        }
    }
}
