import Foundation

struct CodexAutomaticGoalContinuationRecord: Codable, Equatable, Sendable {
    enum Stage: String, Codable, Sendable {
        case planned
        case handedOff
    }

    var id: UUID
    var eventKey: String
    var threadID: String
    var goalID: String?
    var sourceProfileID: UUID?
    var targetProfileID: UUID
    var stage: Stage
}

final class CodexAutomaticGoalContinuationStore: @unchecked Sendable {
    static let pendingRecordKey =
        "com.silverfire.codexcompanion.pending-automatic-goal-continuation"
    static let completedEventKeysKey =
        "com.silverfire.codexcompanion.completed-automatic-goal-continuations"

    private let defaults: UserDefaults
    private let lock = NSLock()

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var pendingRecord: CodexAutomaticGoalContinuationRecord? {
        lock.withLock {
            defaults.data(forKey: Self.pendingRecordKey).flatMap {
                try? JSONDecoder().decode(
                    CodexAutomaticGoalContinuationRecord.self,
                    from: $0
                )
            }
        }
    }

    func hasCompleted(eventKey: String) -> Bool {
        lock.withLock {
            Set(defaults.stringArray(forKey: Self.completedEventKeysKey) ?? [])
                .contains(eventKey)
        }
    }

    func savePending(_ record: CodexAutomaticGoalContinuationRecord) {
        lock.withLock {
            guard let data = try? JSONEncoder().encode(record) else { return }
            defaults.set(data, forKey: Self.pendingRecordKey)
        }
    }

    func discardPending(id: UUID) {
        lock.withLock {
            guard
                let data = defaults.data(forKey: Self.pendingRecordKey),
                let record = try? JSONDecoder().decode(
                    CodexAutomaticGoalContinuationRecord.self,
                    from: data
                ),
                record.id == id
            else { return }
            defaults.removeObject(forKey: Self.pendingRecordKey)
        }
    }

    func markCompleted(_ record: CodexAutomaticGoalContinuationRecord) {
        lock.withLock {
            var keys = defaults.stringArray(forKey: Self.completedEventKeysKey) ?? []
            keys.removeAll { $0 == record.eventKey }
            keys.append(record.eventKey)
            defaults.set(Array(keys.suffix(120)), forKey: Self.completedEventKeysKey)

            guard
                let data = defaults.data(forKey: Self.pendingRecordKey),
                let pending = try? JSONDecoder().decode(
                    CodexAutomaticGoalContinuationRecord.self,
                    from: data
                ),
                pending.id == record.id
            else { return }
            defaults.removeObject(forKey: Self.pendingRecordKey)
        }
    }
}

enum CodexAutomaticGoalContinuationPolicy {
    static func isEligible(_ item: CodexProcessItem) -> Bool {
        guard
            item.kind == .thread,
            item.goalStatus == .usageLimited,
            item.canSwitchCodexAccount,
            item.activeTurnID == nil,
            item.rolloutURL != nil,
            item.goalObjective?.trimmingCharacters(in: .whitespacesAndNewlines)
                .isEmpty == false
        else {
            return false
        }

        switch item.runtimeStatus {
        case .idle, .notLoaded:
            return true
        case .active, .waitingOnApproval, .waitingOnUserInput, .systemError, nil:
            return false
        }
    }

    static func eventKey(for item: CodexProcessItem) -> String? {
        guard
            isEligible(item),
            let threadID = normalized(item.threadID),
            let goalIdentity = normalized(item.goalID) ?? normalized(item.goalObjective),
            let goalUpdatedAt = item.goalUpdatedAt
        else {
            return nil
        }

        let episode = String(
            Int64((goalUpdatedAt.timeIntervalSince1970 * 1_000).rounded())
        )
        return [threadID, goalIdentity, episode].joined(separator: "|")
    }

    static func orderedTargetProfiles(
        _ profiles: [CodexAccountProfile],
        after currentProfileID: UUID?
    ) -> [CodexAccountProfile] {
        guard
            let currentProfileID,
            let index = profiles.firstIndex(where: { $0.id == currentProfileID })
        else {
            return profiles
        }

        let after = profiles.index(after: index)
        return Array(profiles[after...]) + Array(profiles[..<index])
    }

    static func hasAvailableCodexUsage(_ snapshot: CodexUsageSnapshot) -> Bool {
        let limit = snapshot.rateLimitsByLimitID?["codex"] ?? snapshot.rateLimits
        guard limit.limitReached != true else { return false }

        let windows = [limit.primaryWindow, limit.secondaryWindow].compactMap { $0 }
        guard !windows.isEmpty else { return false }
        return windows.allSatisfy { $0.remainingPercent > 0 }
    }

    private static func normalized(_ value: String?) -> String? {
        let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? nil : trimmed
    }
}
