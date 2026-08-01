import Foundation

struct CodexQuotaInterruption: Equatable, Sendable {
    var threadID: String
    var turnID: String
    var completedAt: Date?

    var eventKey: String {
        "\(threadID)|\(turnID)"
    }
}

enum CodexQuotaInterruptionInspectionError: Error, Equatable {
    case invalidThreadID
    case missingResponse
    case server(String)
    case invalidResponse
}

enum CodexAutomaticTaskContinuationError: LocalizedError {
    case profileNotSignedIn

    var errorDescription: String? {
        "The Codex profile is not signed in."
    }
}

extension CodexQuotaInterruptionInspectionError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .invalidThreadID:
            return "The Codex task does not have a valid thread identifier."
        case .missingResponse:
            return "Codex did not return the task's latest turn."
        case .server(let message):
            return message
        case .invalidResponse:
            return "Codex returned an unreadable latest-turn response."
        }
    }
}

enum CodexQuotaInterruptionRequestFactory {
    static func latestTurn(id: Int, threadID: String) -> CodexRPCRequest {
        CodexRPCRequest(
            id: id,
            method: "thread/turns/list",
            params: [
                "threadId": threadID,
                "limit": 1,
                "sortDirection": "desc",
                "itemsView": "notLoaded",
            ]
        )
    }
}

enum CodexQuotaInterruptionParser {
    static func interruption(
        threadID: String,
        response: CodexRPCResponse
    ) throws -> CodexQuotaInterruption? {
        let normalizedThreadID = normalized(threadID)
        guard !normalizedThreadID.isEmpty else {
            throw CodexQuotaInterruptionInspectionError.invalidThreadID
        }
        if let error = response.error {
            throw CodexQuotaInterruptionInspectionError.server(error)
        }
        guard
            let result = response.result,
            let turns = result["data"] as? [[String: Any]]
        else {
            throw CodexQuotaInterruptionInspectionError.invalidResponse
        }
        guard let turn = turns.first else { return nil }
        guard
            turn["status"] as? String == "failed",
            let turnID = turn["id"] as? String,
            !normalized(turnID).isEmpty,
            let error = turn["error"] as? [String: Any],
            error["codexErrorInfo"] as? String == "usageLimitExceeded"
        else {
            return nil
        }

        return CodexQuotaInterruption(
            threadID: normalizedThreadID,
            turnID: normalized(turnID),
            completedAt: date(from: turn["completedAt"])
        )
    }

    private static func normalized(_ value: String) -> String {
        value.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func date(from value: Any?) -> Date? {
        if let value = value as? NSNumber {
            return Date(timeIntervalSince1970: value.doubleValue)
        }
        if let value = value as? Double {
            return Date(timeIntervalSince1970: value)
        }
        if let value = value as? Int {
            return Date(timeIntervalSince1970: Double(value))
        }
        return nil
    }
}

struct CodexQuotaInterruptionInspector: Sendable {
    private let clientProvider: any CodexAccountProfileRPCClientProviding

    init(
        clientProvider: any CodexAccountProfileRPCClientProviding =
            CodexAccountProfileRPCClientProvider()
    ) {
        self.clientProvider = clientProvider
    }

    func read(
        threadID: String,
        profile: CodexAccountProfile
    ) throws -> CodexQuotaInterruption? {
        let normalizedThreadID = threadID.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        guard !normalizedThreadID.isEmpty else {
            throw CodexQuotaInterruptionInspectionError.invalidThreadID
        }

        let request = CodexQuotaInterruptionRequestFactory.latestTurn(
            id: 2,
            threadID: normalizedThreadID
        )
        let responses = try clientProvider.client(for: profile).perform([request])
        guard let response = responses[request.id] else {
            throw CodexQuotaInterruptionInspectionError.missingResponse
        }
        return try CodexQuotaInterruptionParser.interruption(
            threadID: normalizedThreadID,
            response: response
        )
    }
}

struct CodexAutomaticTaskContinuationRecord: Codable, Equatable, Sendable {
    enum Stage: String, Codable, Sendable {
        case planned
        case handedOff
    }

    var id: UUID
    var eventKey: String
    var threadID: String
    var sourceProfileID: UUID
    var targetProfileID: UUID
    var stage: Stage
}

final class CodexAutomaticTaskContinuationStore: @unchecked Sendable {
    static let pendingRecordKey =
        "com.silverfire.codexcompanion.pending-automatic-task-continuation"
    static let completedRecordsKey =
        "com.silverfire.codexcompanion.completed-automatic-task-continuations"

    private let defaults: UserDefaults
    private let lock = NSLock()

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    var pendingRecord: CodexAutomaticTaskContinuationRecord? {
        lock.withLock {
            defaults.data(forKey: Self.pendingRecordKey).flatMap {
                try? JSONDecoder().decode(
                    CodexAutomaticTaskContinuationRecord.self,
                    from: $0
                )
            }
        }
    }

    func hasCompleted(eventKey: String) -> Bool {
        completedRecord(eventKey: eventKey) != nil
    }

    func completedRecord(
        eventKey: String
    ) -> CodexAutomaticTaskContinuationRecord? {
        lock.withLock {
            completedRecords().last { $0.eventKey == eventKey }
        }
    }

    func savePending(_ record: CodexAutomaticTaskContinuationRecord) {
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
                    CodexAutomaticTaskContinuationRecord.self,
                    from: data
                ),
                record.id == id
            else {
                return
            }
            defaults.removeObject(forKey: Self.pendingRecordKey)
        }
    }

    func markCompleted(_ record: CodexAutomaticTaskContinuationRecord) {
        lock.withLock {
            var records = completedRecords()
            records.removeAll { $0.eventKey == record.eventKey }
            records.append(record)
            if let data = try? JSONEncoder().encode(Array(records.suffix(120))) {
                defaults.set(data, forKey: Self.completedRecordsKey)
            }

            guard
                let data = defaults.data(forKey: Self.pendingRecordKey),
                let pending = try? JSONDecoder().decode(
                    CodexAutomaticTaskContinuationRecord.self,
                    from: data
                ),
                pending.id == record.id
            else {
                return
            }
            defaults.removeObject(forKey: Self.pendingRecordKey)
        }
    }

    private func completedRecords() -> [CodexAutomaticTaskContinuationRecord] {
        defaults.data(forKey: Self.completedRecordsKey).flatMap {
            try? JSONDecoder().decode(
                [CodexAutomaticTaskContinuationRecord].self,
                from: $0
            )
        } ?? []
    }
}

enum CodexAutomaticTaskContinuationPolicy {
    static func isCandidate(_ item: CodexProcessItem) -> Bool {
        guard
            item.kind == .thread,
            item.status == .failed,
            normalized(item.threadID) != nil,
            item.rolloutURL != nil,
            item.goalStatus == nil,
            normalized(item.goalID) == nil,
            normalized(item.goalObjective) == nil
        else {
            return false
        }

        switch item.runtimeStatus {
        case .idle, .notLoaded, .systemError:
            return true
        case .active, .waitingOnApproval, .waitingOnUserInput, nil:
            return false
        }
    }

    static func hasConfirmedCodexExhaustion(
        _ snapshot: CodexUsageSnapshot
    ) -> Bool {
        let limit = snapshot.rateLimitsByLimitID?["codex"] ?? snapshot.rateLimits
        if limit.limitReached == true {
            return true
        }
        let windows = [limit.primaryWindow, limit.secondaryWindow].compactMap { $0 }
        return windows.contains { $0.remainingPercent <= 0 }
    }

    static func orderedTargetProfiles(
        _ profiles: [CodexAccountProfile],
        after currentProfileID: UUID?
    ) -> [CodexAccountProfile] {
        CodexAutomaticGoalContinuationPolicy.orderedTargetProfiles(
            profiles,
            after: currentProfileID
        )
    }

    static func hasAvailableCodexUsage(_ snapshot: CodexUsageSnapshot) -> Bool {
        CodexAutomaticGoalContinuationPolicy.hasAvailableCodexUsage(snapshot)
    }

    private static func normalized(_ value: String?) -> String? {
        let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? nil : trimmed
    }
}
