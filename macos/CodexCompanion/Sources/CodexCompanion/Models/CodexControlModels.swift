import Foundation

enum CodexGoalStatus: String, Codable, CaseIterable, Hashable, Sendable {
    case active
    case paused
    case blocked
    case usageLimited
    case budgetLimited
    case complete

    var canResume: Bool {
        self == .paused || self == .blocked
    }

    var canEdit: Bool {
        self != .complete
    }
}

struct CodexGoalSnapshot: Codable, Equatable, Sendable {
    var threadID: String
    var objective: String
    var status: CodexGoalStatus
    var tokenBudget: Int?
    var tokensUsed: Int
    var timeUsedSeconds: Int
    var createdAt: Int
    var updatedAt: Int

    enum CodingKeys: String, CodingKey {
        case threadID = "threadId"
        case objective
        case status
        case tokenBudget
        case tokensUsed
        case timeUsedSeconds
        case createdAt
        case updatedAt
    }
}

struct CodexGoalControlState: Equatable, Sendable {
    var threadID: String
    var taskTitle: String
    var originalObjective: String
    var draftObjective: String
    var status: CodexGoalStatus
    var tokenBudget: Int?
    var isEditing = false

    var canResume: Bool {
        status.canResume
    }

    var canEdit: Bool {
        status.canEdit
    }

    init?(item: CodexProcessItem) {
        guard
            let threadID = item.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
            !threadID.isEmpty,
            let status = item.goalStatus,
            let objective = item.goalObjective?.trimmingCharacters(in: .whitespacesAndNewlines),
            !objective.isEmpty
        else {
            return nil
        }

        self.threadID = threadID
        taskTitle = item.title
        originalObjective = objective
        draftObjective = objective
        self.status = status
        tokenBudget = item.goalTokenBudget
    }
}

struct CodexGoalMutation: Equatable, Sendable {
    var objective: String?
    var status: CodexGoalStatus?
    var tokenBudget: Int?

    static func editObjective(_ objective: String) -> CodexGoalMutation {
        CodexGoalMutation(
            objective: objective,
            status: nil,
            tokenBudget: nil
        )
    }
}

enum CodexRateLimitResetType: String, Codable, Hashable, Sendable {
    case codexRateLimits
    case unknown
}

enum CodexRateLimitResetCreditStatus: String, Codable, Hashable, Sendable {
    case available
    case redeeming
    case redeemed
    case unknown
}

struct CodexRateLimitResetCredit: Codable, Identifiable, Hashable, Sendable {
    var id: String
    var resetType: CodexRateLimitResetType
    var status: CodexRateLimitResetCreditStatus
    var grantedAt: Double
    var expiresAt: Double?
    var title: String?
    var description: String?

    var expirationDate: Date? {
        expiresAt.map(Date.init(timeIntervalSince1970:))
    }

    var displayTitle: String {
        let trimmed = title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? "Codex usage reset" : trimmed
    }

    var isAvailable: Bool {
        status == .available
    }
}

struct CodexRateLimitResetCreditsSummary: Codable, Equatable, Sendable {
    var availableCount: Int
    var credits: [CodexRateLimitResetCredit]?

    var availableCredits: [CodexRateLimitResetCredit] {
        (credits ?? []).filter(\.isAvailable)
    }
}

enum CodexResetConsumeOutcome: String, Codable, Equatable, Sendable {
    case reset
    case nothingToReset
    case noCredit
    case alreadyRedeemed
}

struct CodexRPCRequest {
    var id: Int
    var method: String
    var params: [String: Any]

    var jsonObject: [String: Any] {
        [
            "id": id,
            "method": method,
            "params": params,
        ]
    }
}

enum CodexControlRequestFactory {
    static func goalGet(id: Int, threadID: String) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "thread/goal/get",
            params: ["threadId": threadID]
        )
    }

    static func goalSet(
        id: Int,
        threadID: String,
        objective: String?,
        status: CodexGoalStatus?,
        tokenBudget: Int?
    ) -> CodexRPCRequest {
        var params: [String: Any] = ["threadId": threadID]
        if let objective {
            params["objective"] = objective
        }
        if let status {
            params["status"] = status.rawValue
        }
        if let tokenBudget {
            params["tokenBudget"] = tokenBudget
        }
        return CodexRPCRequest(id: id, method: "thread/goal/set", params: params)
    }

    static func rateLimitsRead(id: Int) -> CodexRPCRequest {
        CodexRPCRequest(id: id, method: "account/rateLimits/read", params: [:])
    }

    static func consumeReset(
        id: Int,
        creditID: String,
        idempotencyKey: String
    ) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "account/rateLimitResetCredit/consume",
            params: [
                "creditId": creditID,
                "idempotencyKey": idempotencyKey,
            ]
        )
    }

    static func modelsList(id: Int) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "model/list",
            params: ["limit": 100, "includeHidden": false]
        )
    }

    static func skillsList(id: Int, cwd: String) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "skills/list",
            params: ["cwds": [cwd], "forceReload": false]
        )
    }

    static func pluginsList(id: Int, cwd: String) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "plugin/list",
            params: ["cwds": [cwd], "marketplaceKinds": ["local"]]
        )
    }
}
