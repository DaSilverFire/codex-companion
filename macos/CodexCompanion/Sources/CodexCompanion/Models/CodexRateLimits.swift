import Foundation

struct CodexUsageSnapshot: Decodable, Equatable, Sendable {
    var rateLimits: CodexRateLimit
    var rateLimitsByLimitID: [String: CodexRateLimit]?
    var rateLimitResetCredits: CodexRateLimitResetCreditsSummary?

    enum CodingKeys: String, CodingKey {
        case rateLimits
        case rateLimitsByLimitID = "rateLimitsByLimitId"
        case rateLimitResetCredits
    }

    var planType: String? {
        rateLimits.planType
    }

    var availableResetCount: Int {
        max(0, rateLimitResetCredits?.availableCount ?? 0)
    }

    var availableResetCredits: [CodexRateLimitResetCredit] {
        rateLimitResetCredits?.availableCredits ?? []
    }

    var allGroups: [CodexRateLimitGroup] {
        if let rateLimitsByLimitID, !rateLimitsByLimitID.isEmpty {
            return rateLimitsByLimitID
                .map { limitID, rateLimit in
                    CodexRateLimitGroup(
                        limitID: limitID,
                        title: Self.displayTitle(limitID: limitID, rateLimit: rateLimit),
                        rateLimit: rateLimit
                    )
                }
                .sorted { lhs, rhs in
                    if lhs.limitID == "codex" { return true }
                    if rhs.limitID == "codex" { return false }
                    return lhs.title.localizedStandardCompare(rhs.title) == .orderedAscending
                }
        }

        let limitID = rateLimits.limitID ?? "codex"
        return [
            CodexRateLimitGroup(
                limitID: limitID,
                title: Self.displayTitle(limitID: limitID, rateLimit: rateLimits),
                rateLimit: rateLimits
            ),
        ]
    }

    private static func displayTitle(limitID: String, rateLimit: CodexRateLimit) -> String {
        if let limitName = rateLimit.limitName?.trimmingCharacters(in: .whitespacesAndNewlines),
           !limitName.isEmpty {
            return limitName
        }
        return limitID == "codex" ? "Codex" : limitID.displayTitle
    }
}

struct CodexRateLimitGroup: Identifiable, Hashable, Sendable {
    var limitID: String
    var title: String
    var rateLimit: CodexRateLimit

    var id: String { limitID }

    var windows: [CodexRateLimitWindow] {
        [rateLimit.primaryWindow, rateLimit.secondaryWindow].compactMap { $0 }
    }

    var shortWindow: CodexRateLimitWindow? {
        windows
            .filter { $0.limitWindowSeconds < 24 * 60 * 60 }
            .max { $0.limitWindowSeconds < $1.limitWindowSeconds }
    }

    var weeklyWindow: CodexRateLimitWindow? {
        windows
            .filter { $0.limitWindowSeconds >= 24 * 60 * 60 }
            .max { $0.limitWindowSeconds < $1.limitWindowSeconds }
    }
}

struct CodexRateLimit: Decodable, Hashable, Sendable {
    var limitID: String?
    var limitName: String?
    var primary: CodexRateLimitWindow?
    var secondary: CodexRateLimitWindow?
    var planType: String?
    var rateLimitReachedType: String?

    enum CodingKeys: String, CodingKey {
        case limitID = "limitId"
        case limitName
        case primary
        case secondary
        case planType
        case rateLimitReachedType
    }

    var primaryWindow: CodexRateLimitWindow? { primary }
    var secondaryWindow: CodexRateLimitWindow? { secondary }
    var allowed: Bool? { rateLimitReachedType == nil }
    var limitReached: Bool? { rateLimitReachedType != nil }
}

struct CodexRateLimitWindow: Decodable, Hashable, Sendable {
    var usedPercent: Double
    var windowDurationMins: Double?
    var resetsAt: Double?

    var limitWindowSeconds: Double {
        max(0, (windowDurationMins ?? 0) * 60)
    }

    var remainingPercent: Double {
        max(0, min(100, 100 - usedPercent))
    }

    var clampedUsedPercent: Double {
        max(0, min(100, usedPercent))
    }

    var durationLabel: String {
        let seconds = Int(limitWindowSeconds.rounded())

        if seconds >= 7 * 24 * 60 * 60 {
            let weeks = max(1, Int(ceil(Double(seconds) / Double(7 * 24 * 60 * 60))))
            return weeks == 1 ? "Weekly" : "\(weeks) Week"
        }

        if seconds >= 24 * 60 * 60 {
            let days = max(1, Int(ceil(Double(seconds) / Double(24 * 60 * 60))))
            return "\(days)d"
        }

        if seconds >= 60 * 60 {
            let hours = max(1, Int(ceil(Double(seconds) / Double(60 * 60))))
            return "\(hours)h"
        }

        let minutes = max(1, Int(ceil(Double(seconds) / 60)))
        return "\(minutes)m"
    }

    var resetDate: Date? {
        guard let resetsAt else { return nil }
        return Date(timeIntervalSince1970: resetsAt)
    }
}

extension CompanionBridgeUsageSnapshot {
    init(snapshot: CodexUsageSnapshot, updatedAt: Date = Date()) {
        planType = snapshot.planType
        groups = snapshot.allGroups.map { group in
            CompanionBridgeUsageGroup(
                id: group.id,
                title: group.title,
                shortWindow: group.shortWindow.map(CompanionBridgeUsageWindow.init),
                weeklyWindow: group.weeklyWindow.map(CompanionBridgeUsageWindow.init)
            )
        }
        availableResetCount = snapshot.availableResetCount
        availableResetCredits = snapshot.availableResetCredits.map { credit in
            CompanionBridgeResetCredit(
                id: credit.id,
                displayTitle: credit.displayTitle,
                detail: credit.description,
                expiresAt: credit.expirationDate
            )
        }
        self.updatedAt = updatedAt
    }
}

private extension CompanionBridgeUsageWindow {
    init(window: CodexRateLimitWindow) {
        remainingPercent = window.remainingPercent
        durationLabel = window.durationLabel
        resetsAt = window.resetDate
    }
}
