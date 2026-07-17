import Foundation

struct CodexProcessItem: Identifiable, Hashable, Codable, Sendable {
    enum Kind: String, Codable, Sendable {
        case job
        case thread
        case notice
    }

    enum Status: String, Codable, Sendable {
        case running
        case completed
        case failed
        case waiting
    }

    typealias GoalStatus = CodexGoalStatus

    var id: String
    var kind: Kind
    var title: String
    var subtitle: String
    var fullMessage: String
    var updatedAt: Date?
    var startedAt: Date?
    var status: Status
    var threadID: String?
    var cwd: String?
    var activeTurnID: String? = nil
    var goalID: String?
    var goalObjective: String?
    var goalStatus: GoalStatus?
    var goalTokenBudget: Int? = nil
    var goalElapsedSeconds: Int?
    var goalTimerReferenceDate: Date?
    var runtimeStatus: CodexThreadRuntimeStatus? = nil

    var isActive: Bool {
        status == .running
    }

    var hasReachedGoal: Bool {
        goalStatus == .complete
    }

    var canTargetCodexThread: Bool {
        guard kind != .notice, let threadID else { return false }
        return threadID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false
    }

    var canUseAsDefaultCodexTarget: Bool {
        canTargetCodexThread && !isCompanionDevelopmentThread
    }

    var isCompanionDevelopmentThread: Bool {
        let normalizedTitle = title.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let normalizedCwd = (cwd ?? "").replacingOccurrences(of: "\\", with: "/").lowercased()
        return normalizedTitle == "codex companion"
            || normalizedCwd.hasSuffix("/skill-installer-hatch-pet")
    }
}

@MainActor
final class CodexProcessStore: ObservableObject {
    struct HandledFailure: Codable, Equatable, Sendable {
        var processID: String
        var threadID: String?
        var failureUpdatedAt: Date?
    }

    struct FailureReconciliation: Equatable {
        var items: [CodexProcessItem]
        var unresolvedFailures: [String: CodexProcessItem]
        var handledFailures: [String: HandledFailure]
    }

    typealias GoalReader = @Sendable ([String]) throws -> [String: CodexGoalSnapshot?]
    typealias GoalWriter = @Sendable (
        _ threadID: String,
        _ objective: String?,
        _ status: CodexGoalStatus?,
        _ tokenBudget: Int?
    ) throws -> CodexGoalSnapshot

    @Published private(set) var items: [CodexProcessItem] = []
    @Published private(set) var isLoading = false
    @Published private(set) var isLoadingGoals = false
    @Published private(set) var errorMessage: String?
    private var needsRefreshAfterCurrentLoad = false
    private var lastRefreshFinishedAt: Date?
    private var lastGoalRefreshFinishedAt: Date?
    private var goalRevision = 0
    private var unresolvedFailures: [String: CodexProcessItem]
    private var handledFailures: [String: HandledFailure]
    private let defaults: UserDefaults
    private let readGoals: GoalReader
    private let writeGoal: GoalWriter

    init(
        readGoals: @escaping GoalReader = { threadIDs in
            try CodexAppServerControlService.shared.readGoals(threadIDs: threadIDs)
        },
        writeGoal: @escaping GoalWriter = { threadID, objective, status, tokenBudget in
            try CodexAppServerControlService.shared.setGoal(
                threadID: threadID,
                objective: objective,
                status: status,
                tokenBudget: tokenBudget
            )
        },
        defaults: UserDefaults = .standard
    ) {
        self.defaults = defaults
        self.unresolvedFailures = Self.loadUnresolvedFailures(from: defaults)
        self.handledFailures = Self.loadHandledFailures(from: defaults)
        self.readGoals = readGoals
        self.writeGoal = writeGoal
    }

    func refresh() {
        guard !isLoading else {
            needsRefreshAfterCurrentLoad = true
            return
        }
        isLoading = true
        errorMessage = nil

        Task.detached(priority: .utility) {
            let result = Self.loadItems()
            await MainActor.run {
                let shouldRefreshAgain = self.needsRefreshAfterCurrentLoad
                self.needsRefreshAfterCurrentLoad = false
                switch result {
                case .success(let items):
                    let cachedByThreadID = self.items.reduce(into: [String: CodexProcessItem]()) { result, item in
                        guard let threadID = item.threadID, item.goalStatus != nil else { return }
                        result[threadID] = item
                    }
                    let goalMergedItems = items.map { item in
                        guard
                            self.lastGoalRefreshFinishedAt != nil,
                            let threadID = item.threadID,
                            let cached = cachedByThreadID[threadID]
                        else {
                            return item
                        }
                        return Self.preservingCachedGoal(from: cached, in: item)
                    }
                    let failureReconciliation = Self.reconcileFailures(
                        cached: self.unresolvedFailures,
                        handled: self.handledFailures,
                        refreshed: goalMergedItems
                    )
                    self.unresolvedFailures = failureReconciliation.unresolvedFailures
                    self.handledFailures = failureReconciliation.handledFailures
                    self.items = failureReconciliation.items
                    self.persistFailureState()
                    self.errorMessage = nil
                case .failure(let error):
                    self.items = Self.itemsAfterRefreshFailure(
                        current: self.items,
                        error: error
                    )
                    self.errorMessage = error.localizedDescription
                }
                self.lastRefreshFinishedAt = Date()
                self.isLoading = false
                self.refreshGoalsIfStale()
                if shouldRefreshAgain {
                    self.refresh()
                }
            }
        }
    }

    func refreshIfStale(maxAge: TimeInterval = 8) {
        guard !items.isEmpty else {
            refresh()
            return
        }
        guard let lastRefreshFinishedAt else {
            refresh()
            return
        }
        guard Date().timeIntervalSince(lastRefreshFinishedAt) >= maxAge else { return }
        refresh()
    }

    func refreshGoalsIfStale(maxAge: TimeInterval = 60) {
        guard !isLoadingGoals else { return }
        guard let lastGoalRefreshFinishedAt else {
            refreshGoals()
            return
        }
        guard Date().timeIntervalSince(lastGoalRefreshFinishedAt) >= maxAge else { return }
        refreshGoals()
    }

    func refreshGoals() {
        guard !isLoadingGoals else { return }
        let threadIDs = Array(Set(items.compactMap {
            $0.threadID?.trimmingCharacters(in: .whitespacesAndNewlines).nilIfEmpty
        })).sorted()
        guard !threadIDs.isEmpty else { return }

        isLoadingGoals = true
        let startedAtRevision = goalRevision
        let readGoals = self.readGoals
        Task.detached(priority: .utility) {
            let result = Result { try readGoals(threadIDs) }
            await MainActor.run {
                guard Self.shouldApplyGoalRefresh(
                    startedAtRevision: startedAtRevision,
                    currentRevision: self.goalRevision
                ) else {
                    self.isLoadingGoals = false
                    return
                }
                switch result {
                case .success(let goals):
                    self.items = self.items.map { item in
                        guard
                            let threadID = item.threadID,
                            goals.keys.contains(threadID)
                        else {
                            return item
                        }
                        if let goal = goals[threadID] ?? nil {
                            return Self.applying(goal: goal, to: item)
                        }
                        return Self.clearingGoal(from: item)
                    }
                    self.errorMessage = nil
                case .failure(let error):
                    self.errorMessage = error.localizedDescription
                }
                self.lastGoalRefreshFinishedAt = Date()
                self.isLoadingGoals = false
            }
        }
    }

    func markFailureHandled(processID: String, threadID: String) {
        let matchingFailures = Array(unresolvedFailures.values) + items.filter {
            $0.status == .failed
        }
        let handled = matchingFailures.filter {
            $0.id == processID || $0.threadID == threadID
        }
        guard !handled.isEmpty else { return }

        for failure in handled {
            Self.removeMatchingFailure(failure, from: &unresolvedFailures)
            Self.replaceMatchingHandledFailure(
                with: HandledFailure(
                    processID: failure.id,
                    threadID: failure.threadID,
                    failureUpdatedAt: failure.updatedAt
                ),
                in: &handledFailures
            )
        }
        items.removeAll {
            $0.status == .failed && ($0.id == processID || $0.threadID == threadID)
        }
        persistFailureState()
        refresh()
    }

    @discardableResult
    func setGoal(
        threadID: String,
        objective: String?,
        status: CodexGoalStatus?,
        tokenBudget: Int?
    ) async throws -> CodexGoalSnapshot {
        let writeGoal = self.writeGoal
        let goal = try await Task.detached(priority: .userInitiated) {
            try writeGoal(threadID, objective, status, tokenBudget)
        }.value

        goalRevision += 1
        items = items.map { item in
            guard item.threadID == goal.threadID else { return item }
            return Self.applying(goal: goal, to: item)
        }
        lastGoalRefreshFinishedAt = Date()
        return goal
    }

    nonisolated static func shouldApplyGoalRefresh(
        startedAtRevision: Int,
        currentRevision: Int
    ) -> Bool {
        startedAtRevision == currentRevision
    }

    nonisolated static func preservingCachedGoal(
        from cached: CodexProcessItem,
        in refreshed: CodexProcessItem
    ) -> CodexProcessItem {
        guard cached.goalStatus != nil, cached.threadID == refreshed.threadID else {
            return refreshed
        }
        var merged = refreshed
        merged.goalID = cached.goalID
        merged.goalObjective = cached.goalObjective
        merged.goalStatus = cached.goalStatus
        merged.goalTokenBudget = cached.goalTokenBudget
        merged.goalElapsedSeconds = cached.goalElapsedSeconds
        merged.goalTimerReferenceDate = cached.goalTimerReferenceDate
        merged.startedAt = cached.startedAt
        merged.status = threadStatus(isFresh: true, goalStatus: cached.goalStatus)
        merged.subtitle = threadSubtitle(
            status: merged.status,
            goalStatus: cached.goalStatus,
            goalElapsedSeconds: cached.goalElapsedSeconds,
            goalTimerReferenceDate: cached.goalTimerReferenceDate,
            updatedAt: refreshed.updatedAt,
            fallback: refreshed.cwd ?? ""
        )
        return applyingRuntimePresentation(to: merged)
    }

    nonisolated static func applying(
        goal: CodexGoalSnapshot,
        to item: CodexProcessItem
    ) -> CodexProcessItem {
        var updated = item
        let createdAt = Date(timeIntervalSince1970: TimeInterval(goal.createdAt))
        let goalUpdatedAt = Date(timeIntervalSince1970: TimeInterval(goal.updatedAt))

        updated.goalID = String(goal.createdAt)
        updated.goalObjective = goal.objective
        updated.goalStatus = goal.status
        updated.goalTokenBudget = goal.tokenBudget
        updated.goalElapsedSeconds = goal.timeUsedSeconds
        updated.goalTimerReferenceDate = goal.status == .active ? goalUpdatedAt : nil
        updated.startedAt = createdAt
        updated.status = threadStatus(isFresh: true, goalStatus: goal.status)
        updated.subtitle = threadSubtitle(
            status: updated.status,
            goalStatus: goal.status,
            goalElapsedSeconds: goal.timeUsedSeconds,
            goalTimerReferenceDate: updated.goalTimerReferenceDate,
            updatedAt: item.updatedAt,
            fallback: item.cwd ?? ""
        )
        return applyingRuntimePresentation(to: updated)
    }

    nonisolated private static func clearingGoal(
        from item: CodexProcessItem
    ) -> CodexProcessItem {
        var cleared = item
        cleared.goalID = nil
        cleared.goalObjective = nil
        cleared.goalStatus = nil
        cleared.goalTokenBudget = nil
        cleared.goalElapsedSeconds = nil
        cleared.goalTimerReferenceDate = nil
        cleared.startedAt = nil
        let isFresh = item.updatedAt.map {
            Date().timeIntervalSince($0) < Self.threadRunningFreshnessWindow
        } ?? false
        cleared.status = threadStatus(isFresh: isFresh, goalStatus: nil)
        cleared.subtitle = threadSubtitle(
            status: cleared.status,
            goalStatus: nil,
            goalElapsedSeconds: nil,
            goalTimerReferenceDate: nil,
            updatedAt: item.updatedAt,
            fallback: item.cwd ?? ""
        )
        return applyingRuntimePresentation(to: cleared)
    }

    nonisolated private static func loadItems() -> Result<[CodexProcessItem], Error> {
        let runtimeStatuses = (try? CodexSharedThreadRuntimeReader().read()) ?? [:]
        var output = recentJobItems()
        output.append(contentsOf: applyingRuntimeStatuses(
            runtimeStatuses,
            to: recentThreadItems(runtimeThreadIDs: Set(runtimeStatuses.keys))
        ))
        output = sidebarOrderedItems(output)

        if output.isEmpty {
            output.append(
                CodexProcessItem(
                    id: "no-processes",
                    kind: .notice,
                    title: "No active Codex processes",
                    subtitle: "Nothing running right now",
                    fullMessage: "Open a Codex thread or start a job and it will appear here.",
                    updatedAt: nil,
                    startedAt: nil,
                    status: .waiting,
                    threadID: nil,
                    cwd: nil,
                    goalID: nil,
                    goalObjective: nil,
                    goalStatus: nil,
                    goalElapsedSeconds: nil,
                    goalTimerReferenceDate: nil
                )
            )
        }

        return .success(retainedProcessItems(output))
    }

    nonisolated static func applyingRuntimeStatuses(
        _ statuses: [String: CodexThreadRuntimeStatus],
        to items: [CodexProcessItem]
    ) -> [CodexProcessItem] {
        items.map { item in
            guard let threadID = item.threadID, let runtimeStatus = statuses[threadID] else {
                return item
            }
            var updated = item
            updated.runtimeStatus = runtimeStatus
            return applyingRuntimePresentation(to: updated)
        }
    }

    nonisolated private static func applyingRuntimePresentation(
        to item: CodexProcessItem
    ) -> CodexProcessItem {
        guard let runtimeStatus = item.runtimeStatus else { return item }
        var updated = item
        switch runtimeStatus {
            case .waitingOnApproval:
                updated.status = .waiting
                updated.subtitle = "Needs your approval"
                updated.fullMessage = "This task is waiting for your approval."
            case .waitingOnUserInput:
                updated.status = .waiting
                updated.subtitle = "Needs your answer"
                updated.fullMessage = "This task is waiting for your input."
            case .active:
                updated.status = .running
            case .systemError:
                updated.status = .failed
            case .idle, .notLoaded:
                break
            }
        return updated
    }

    nonisolated private static func processPriority(_ status: CodexProcessItem.Status) -> Int {
        switch status {
        case .waiting: return 0
        case .running: return 1
        case .failed: return 2
        case .completed: return 3
        }
    }

    nonisolated static func retainedProcessItems(
        _ sortedItems: [CodexProcessItem],
        now: Date = Date()
    ) -> [CodexProcessItem] {
        Array(sortedItems.filter { item in
            shouldRetainProcess(item, now: now)
        }.prefix(10))
    }

    nonisolated static func reconcileFailures(
        cached: [String: CodexProcessItem],
        handled: [String: HandledFailure] = [:],
        refreshed: [CodexProcessItem]
    ) -> FailureReconciliation {
        var handledFailures = handled
        var output: [CodexProcessItem] = []
        var unresolved: [String: CodexProcessItem] = [:]

        for item in refreshed {
            if item.kind != .notice,
               item.status == .failed,
               suppressesHandledFailure(item, handled: &handledFailures)
            {
                continue
            }
            output.append(item)
            if item.kind != .notice && item.status == .failed {
                replaceMatchingFailure(with: item, in: &unresolved)
            }
        }

        for (id, failedItem) in cached {
            if suppressesHandledFailure(failedItem, handled: &handledFailures) {
                continue
            }
            let matchingIndex = output.firstIndex {
                $0.id == id || (
                    failedItem.threadID != nil
                        && $0.threadID == failedItem.threadID
                )
            }
            guard let matchingIndex else {
                guard !unresolved.values.contains(where: {
                    failuresReferToSameProcess($0, failedItem)
                }) else { continue }
                output.append(failedItem)
                unresolved[id] = failedItem
                continue
            }

            let refreshedItem = output[matchingIndex]
            if refreshedItem.status == .failed {
                replaceMatchingFailure(with: refreshedItem, in: &unresolved)
            } else if failureIsResolved(failedItem, by: refreshedItem) {
                removeMatchingFailure(failedItem, from: &unresolved)
            } else {
                output[matchingIndex] = failedItem
                replaceMatchingFailure(with: failedItem, in: &unresolved)
            }
        }

        output = sidebarOrderedItems(output)

        return FailureReconciliation(
            items: retainedProcessItems(output),
            unresolvedFailures: unresolved,
            handledFailures: handledFailures
        )
    }

    nonisolated private static func suppressesHandledFailure(
        _ failure: CodexProcessItem,
        handled: inout [String: HandledFailure]
    ) -> Bool {
        let matches = handled.filter { _, candidate in
            handledFailure(candidate, refersTo: failure)
        }
        guard !matches.isEmpty else { return false }

        if matches.values.contains(where: { failureIsNewer(failure, than: $0) }) {
            for id in matches.keys {
                handled.removeValue(forKey: id)
            }
            return false
        }
        return true
    }

    nonisolated static func sidebarOrderedItems(
        _ items: [CodexProcessItem],
        homeDirectory: URL = FileManager.default.homeDirectoryForCurrentUser,
        pendingApprovalThreadIDs: Set<String>? = nil,
        promotionTracker: CodexApprovalPromotionTracker = .shared,
        now: Date = Date()
    ) -> [CodexProcessItem] {
        let pendingThreadIDs = pendingApprovalThreadIDs
            ?? Set(CodexDesktopApprovalLogReader().pendingApprovals().keys)
        let promotedThreadIDs = promotionTracker.promotedThreadIDs(
            pendingThreadIDs: pendingThreadIDs,
            now: now
        )
        let ordering = CodexSidebarOrderingSnapshot.load(homeDirectory: homeDirectory)
        return items.sorted { lhs, rhs in
            ordering.orders(
                .init(
                    threadID: lhs.threadID,
                    cwd: lhs.cwd,
                    statusRank: processPriority(lhs.status),
                    updatedAt: lhs.updatedAt,
                    isAttentionPromoted: lhs.threadID.map(promotedThreadIDs.contains) ?? false
                ),
                before: .init(
                    threadID: rhs.threadID,
                    cwd: rhs.cwd,
                    statusRank: processPriority(rhs.status),
                    updatedAt: rhs.updatedAt,
                    isAttentionPromoted: rhs.threadID.map(promotedThreadIDs.contains) ?? false
                )
            )
        }
    }

    nonisolated private static func failureIsNewer(
        _ failure: CodexProcessItem,
        than handled: HandledFailure
    ) -> Bool {
        guard let failureUpdatedAt = failure.updatedAt else { return false }
        guard let handledUpdatedAt = handled.failureUpdatedAt else { return true }
        return failureUpdatedAt > handledUpdatedAt
    }

    nonisolated private static func handledFailure(
        _ handled: HandledFailure,
        refersTo failure: CodexProcessItem
    ) -> Bool {
        if handled.processID == failure.id {
            return true
        }
        guard let handledThreadID = handled.threadID, let failureThreadID = failure.threadID else {
            return false
        }
        return handledThreadID == failureThreadID
    }

    nonisolated private static func replaceMatchingHandledFailure(
        with handled: HandledFailure,
        in failures: inout [String: HandledFailure]
    ) {
        let matchingIDs = failures.compactMap { id, candidate in
            if candidate.processID == handled.processID {
                return id
            }
            guard let candidateThreadID = candidate.threadID, let handledThreadID = handled.threadID else {
                return nil
            }
            return candidateThreadID == handledThreadID ? id : nil
        }
        for id in matchingIDs {
            failures.removeValue(forKey: id)
        }
        failures[handled.processID] = handled
    }

    nonisolated private static func replaceMatchingFailure(
        with failure: CodexProcessItem,
        in failures: inout [String: CodexProcessItem]
    ) {
        removeMatchingFailure(failure, from: &failures)
        failures[failure.id] = failure
    }

    nonisolated private static func removeMatchingFailure(
        _ failure: CodexProcessItem,
        from failures: inout [String: CodexProcessItem]
    ) {
        let matchingIDs = failures.compactMap { id, candidate in
            failuresReferToSameProcess(candidate, failure) ? id : nil
        }
        for id in matchingIDs {
            failures.removeValue(forKey: id)
        }
    }

    nonisolated private static func failuresReferToSameProcess(
        _ lhs: CodexProcessItem,
        _ rhs: CodexProcessItem
    ) -> Bool {
        if lhs.id == rhs.id {
            return true
        }
        guard let lhsThreadID = lhs.threadID, let rhsThreadID = rhs.threadID else {
            return false
        }
        return lhsThreadID == rhsThreadID
    }

    nonisolated private static func failureIsResolved(
        _ failure: CodexProcessItem,
        by refreshed: CodexProcessItem
    ) -> Bool {
        switch refreshed.runtimeStatus {
        case .active, .waitingOnApproval, .waitingOnUserInput:
            return true
        case .systemError:
            return false
        case .idle, .notLoaded, nil:
            break
        }

        guard let failureDate = failure.updatedAt, let refreshedDate = refreshed.updatedAt else {
            return false
        }
        return refreshedDate > failureDate
    }

    nonisolated private static func loadUnresolvedFailures(
        from defaults: UserDefaults
    ) -> [String: CodexProcessItem] {
        guard
            let data = defaults.data(forKey: unresolvedFailuresDefaultsKey),
            let items = try? JSONDecoder().decode([CodexProcessItem].self, from: data)
        else {
            return [:]
        }
        return items.reduce(into: [String: CodexProcessItem]()) { result, item in
            result[item.id] = item
        }
    }

    nonisolated private static func loadHandledFailures(
        from defaults: UserDefaults
    ) -> [String: HandledFailure] {
        guard
            let data = defaults.data(forKey: handledFailuresDefaultsKey),
            let items = try? JSONDecoder().decode([HandledFailure].self, from: data)
        else {
            return [:]
        }
        return items.reduce(into: [String: HandledFailure]()) { result, item in
            result[item.processID] = item
        }
    }

    private func persistFailureState() {
        let items = unresolvedFailures.values.sorted { $0.id < $1.id }
        if let data = try? JSONEncoder().encode(items) {
            defaults.set(data, forKey: Self.unresolvedFailuresDefaultsKey)
        }

        let handled = handledFailures.values.sorted { $0.processID < $1.processID }
        if let data = try? JSONEncoder().encode(handled) {
            defaults.set(data, forKey: Self.handledFailuresDefaultsKey)
        }
    }

    nonisolated private static func shouldRetainProcess(
        _ item: CodexProcessItem,
        now: Date
    ) -> Bool {
        guard item.status == .completed else { return true }
        guard item.goalStatus != .complete else { return true }
        guard let updatedAt = item.updatedAt else { return false }

        let displayWindow = item.kind == .thread
            ? currentThreadWindow
            : completedProcessDisplayWindow
        return now.timeIntervalSince(updatedAt) < displayWindow
    }

    nonisolated static func itemsAfterRefreshFailure(
        current: [CodexProcessItem],
        error: Error
    ) -> [CodexProcessItem] {
        guard !current.contains(where: { $0.kind != .notice }) else {
            return current
        }
        return [refreshFailureNotice(error: error)]
    }

    nonisolated private static func refreshFailureNotice(
        error: Error
    ) -> CodexProcessItem {
        CodexProcessItem(
            id: "codex-process-error",
            kind: .notice,
            title: "Could not load Codex processes",
            subtitle: "Refresh failed",
            fullMessage: error.localizedDescription,
            updatedAt: nil,
            startedAt: nil,
            status: .failed,
            threadID: nil,
            cwd: nil,
            goalID: nil,
            goalObjective: nil,
            goalStatus: nil,
            goalElapsedSeconds: nil,
            goalTimerReferenceDate: nil
        )
    }

    nonisolated private static func recentJobItems() -> [CodexProcessItem] {
        let sessionNames = sessionThreadNames()
        let query = """
        select id, name, status, updated_at, instruction, coalesce(last_error, ''), started_at, created_at,
               coalesce((
                   select assigned_thread_id
                   from agent_job_items
                   where job_id = agent_jobs.id
                     and assigned_thread_id is not null
                     and assigned_thread_id != ''
                   order by updated_at desc
                   limit 1
               ), '')
        from agent_jobs
        where lower(status) in ('running', 'pending', 'active', 'queued', 'in_progress')
           or lower(status) in ('failed', 'failure', 'error', 'disconnected', 'cancelled', 'canceled')
           or (lower(status) not in (
                   'running', 'pending', 'active', 'queued', 'in_progress',
                   'failed', 'failure', 'error', 'disconnected', 'cancelled', 'canceled'
               )
               and updated_at >= strftime('%s', 'now') - \(Int(Self.completedProcessDisplayWindow)))
        order by updated_at desc
        limit 50;
        """

        return sqliteRows(query: query).compactMap { columns in
            guard columns.count >= 9 else { return nil }
            let updatedAt = date(fromUnixString: columns[3])
            let startedAt = date(fromUnixString: columns[6]) ?? date(fromUnixString: columns[7])
            let status = processStatus(from: columns[2])
            guard isCurrentJob(status: status, updatedAt: updatedAt) else { return nil }
            let instruction = columns[4].trimmingCharacters(in: .whitespacesAndNewlines)
            let error = columns[5].trimmingCharacters(in: .whitespacesAndNewlines)
            let assignedThreadID = columns[8].trimmingCharacters(in: .whitespacesAndNewlines)
            let title = chatDisplayTitle(
                threadID: assignedThreadID,
                databaseTitle: columns[1],
                cwd: "",
                firstMessage: instruction,
                sessionNames: sessionNames,
                fallback: "Codex job"
            )
            let message = status == .failed && !error.isEmpty
                ? error
                : instruction.isEmpty ? "Codex job status: \(columns[2].displayTitle)" : instruction
            return CodexProcessItem(
                id: "job-\(columns[0])",
                kind: .job,
                title: title,
                subtitle: jobSubtitle(status: columns[2], updatedAt: updatedAt),
                fullMessage: message,
                updatedAt: updatedAt,
                startedAt: startedAt,
                status: status,
                threadID: assignedThreadID.isEmpty ? nil : assignedThreadID,
                cwd: nil,
                goalID: nil,
                goalObjective: nil,
                goalStatus: nil,
                goalElapsedSeconds: nil,
                goalTimerReferenceDate: nil
            )
        }
    }

    nonisolated private static func recentThreadItems(
        runtimeThreadIDs: Set<String> = []
    ) -> [CodexProcessItem] {
        let sessionNames = sessionThreadNames()
        guard hasTable(named: "thread_goals") else {
            return recentThreadItemsWithoutGoals(
                sessionNames: sessionNames,
                runtimeThreadIDs: runtimeThreadIDs
            )
        }

        let runtimeClause = runtimeThreadPredicate(runtimeThreadIDs)

        let query = """
        select t.id, t.title, t.cwd, t.updated_at, t.first_user_message, t.rollout_path,
               coalesce(g.goal_id, ''), coalesce(g.objective, ''), coalesce(g.status, ''),
               coalesce(g.time_used_seconds, ''), coalesce(g.created_at_ms, ''), coalesce(g.updated_at_ms, '')
        from threads t
        left join thread_goals g on g.thread_id = t.id
        where t.archived = 0
          and t.source not like '{"subagent":%'
          and (
                t.updated_at >= strftime('%s', 'now') - \(Int(Self.currentThreadWindow))
             or lower(coalesce(g.status, '')) in ('active', 'paused', 'blocked', 'usage_limited', 'usageLimited', 'budget_limited', 'budgetLimited')
             or (lower(coalesce(g.status, '')) = 'complete'
                 and coalesce(g.updated_at_ms, 0) >= (strftime('%s', 'now') * 1000) - \(Int(Self.recentGoalCompletionDisplayWindow * 1000)))
             \(runtimeClause)
          )
        order by max(t.updated_at, coalesce(g.updated_at_ms, 0) / 1000) desc
        limit 50;
        """

        return sqliteRows(query: query).compactMap { columns in
            guard columns.count >= 12 else { return nil }
            let updatedAt = date(fromUnixString: columns[3])
            let isFresh = updatedAt.map {
                Date().timeIntervalSince($0) < Self.threadRunningFreshnessWindow
            } ?? false
            let firstMessage = columns[4].trimmingCharacters(in: .whitespacesAndNewlines)
            let cwd = columns[2].trimmingCharacters(in: .whitespacesAndNewlines)
            let title = chatDisplayTitle(
                threadID: columns[0],
                databaseTitle: columns[1],
                cwd: cwd,
                firstMessage: firstMessage,
                sessionNames: sessionNames,
                fallback: "Codex thread"
            )
            let rolloutSnapshot = latestRolloutSnapshot(fromRolloutPath: columns[5])
            let fallbackMessage = firstMessage.isEmpty ? (cwd.isEmpty ? title : cwd) : firstMessage
            let goalStatus = displayedGoalStatus(
                from: columns[8],
                updatedAtMilliseconds: columns[11]
            )
            let goalElapsedSeconds = goalElapsedSeconds(
                status: goalStatus,
                timeUsedSeconds: columns[9],
                createdAtMilliseconds: columns[10],
                updatedAtMilliseconds: columns[11]
            )
            let goalTimerReferenceDate = goalTimerReferenceDate(
                status: goalStatus,
                timeUsedSeconds: columns[9],
                createdAtMilliseconds: columns[10],
                updatedAtMilliseconds: columns[11]
            )
            let status = threadStatus(isFresh: isFresh, goalStatus: goalStatus)
            let subtitle = threadSubtitle(
                status: status,
                goalStatus: goalStatus,
                goalElapsedSeconds: goalElapsedSeconds,
                goalTimerReferenceDate: goalTimerReferenceDate,
                updatedAt: updatedAt,
                fallback: cwd
            )
            return CodexProcessItem(
                id: "thread-\(columns[0])",
                kind: .thread,
                title: title,
                subtitle: subtitle,
                fullMessage: rolloutSnapshot.assistantMessage ?? fallbackMessage,
                updatedAt: updatedAt,
                startedAt: goalStatus == nil ? nil : date(fromMillisecondsString: columns[10]),
                status: status,
                threadID: columns[0],
                cwd: cwd.isEmpty ? nil : cwd,
                activeTurnID: rolloutSnapshot.turnID,
                goalID: goalStatus == nil || columns[6].isEmpty ? nil : columns[6],
                goalObjective: goalStatus == nil || columns[7].isEmpty ? nil : columns[7],
                goalStatus: goalStatus,
                goalElapsedSeconds: goalElapsedSeconds,
                goalTimerReferenceDate: goalTimerReferenceDate
            )
        }
    }

    nonisolated private static func recentThreadItemsWithoutGoals(
        sessionNames: [String: String],
        runtimeThreadIDs: Set<String>
    ) -> [CodexProcessItem] {
        let runtimeClause = runtimeThreadPredicate(runtimeThreadIDs)
        let query = """
        select t.id, t.title, t.cwd, t.updated_at, t.first_user_message, t.rollout_path
        from threads t
        where t.archived = 0
          and t.source not like '{"subagent":%'
          and (
                t.updated_at >= strftime('%s', 'now') - \(Int(Self.currentThreadWindow))
                \(runtimeClause)
          )
        order by t.updated_at desc
        limit 50;
        """

        return sqliteRows(query: query).compactMap { columns in
            guard columns.count >= 6 else { return nil }
            let updatedAt = date(fromUnixString: columns[3])
            let isFresh = updatedAt.map {
                Date().timeIntervalSince($0) < Self.threadRunningFreshnessWindow
            } ?? false
            let firstMessage = columns[4].trimmingCharacters(in: .whitespacesAndNewlines)
            let cwd = columns[2].trimmingCharacters(in: .whitespacesAndNewlines)
            let title = chatDisplayTitle(
                threadID: columns[0],
                databaseTitle: columns[1],
                cwd: cwd,
                firstMessage: firstMessage,
                sessionNames: sessionNames,
                fallback: "Codex thread"
            )
            let rolloutSnapshot = latestRolloutSnapshot(fromRolloutPath: columns[5])
            let fallbackMessage = firstMessage.isEmpty ? (cwd.isEmpty ? title : cwd) : firstMessage
            return CodexProcessItem(
                id: "thread-\(columns[0])",
                kind: .thread,
                title: title,
                subtitle: isFresh ? "Working now" : relativeSubtitle(for: updatedAt, fallback: cwd),
                fullMessage: rolloutSnapshot.assistantMessage ?? fallbackMessage,
                updatedAt: updatedAt,
                startedAt: nil,
                status: isFresh ? .running : .completed,
                threadID: columns[0],
                cwd: cwd.isEmpty ? nil : cwd,
                activeTurnID: rolloutSnapshot.turnID,
                goalID: nil,
                goalObjective: nil,
                goalStatus: nil,
                goalElapsedSeconds: nil,
                goalTimerReferenceDate: nil
            )
        }
    }

    nonisolated private static func runtimeThreadPredicate(_ threadIDs: Set<String>) -> String {
        let quoted = threadIDs.sorted().map { rawID in
            "'\(rawID.replacingOccurrences(of: "'", with: "''"))'"
        }
        guard !quoted.isEmpty else { return "" }
        return "or t.id in (\(quoted.joined(separator: ", ")))"
    }

    nonisolated private static func chatDisplayTitle(
        threadID: String,
        databaseTitle: String,
        cwd: String,
        firstMessage: String,
        sessionNames: [String: String],
        fallback: String
    ) -> String {
        let candidateTitles = [
            sessionNames[threadID],
            databaseTitle
        ]

        for candidate in candidateTitles {
            guard
                let title = candidate?.trimmingCharacters(in: .whitespacesAndNewlines),
                !title.isEmpty,
                !looksLikeInitialPrompt(title, firstMessage: firstMessage)
            else { continue }
            return title
        }

        if let folderTitle = folderDisplayTitle(from: cwd) {
            return folderTitle
        }

        if let title = databaseTitle
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .shortenedProcessTitle {
            return title
        }

        return fallback
    }

    nonisolated private static func sessionThreadNames() -> [String: String] {
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("session_index.jsonl")
        guard
            FileManager.default.fileExists(atPath: url.path),
            let data = try? Data(contentsOf: url),
            let text = String(data: data, encoding: .utf8)
        else { return [:] }

        var names: [String: String] = [:]
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            guard
                let jsonData = String(line).data(using: .utf8),
                let rawObject = try? JSONSerialization.jsonObject(with: jsonData),
                let object = rawObject as? [String: Any],
                let id = object["id"] as? String,
                let name = object["thread_name"] as? String
            else { continue }

            let trimmedName = name.trimmingCharacters(in: .whitespacesAndNewlines)
            if !id.isEmpty, !trimmedName.isEmpty {
                names[id] = trimmedName
            }
        }
        return names
    }

    nonisolated private static func looksLikeInitialPrompt(_ title: String, firstMessage: String) -> Bool {
        let normalizedTitle = title.normalizedPromptComparison
        let normalizedFirstMessage = firstMessage.normalizedPromptComparison
        guard !normalizedTitle.isEmpty else { return true }
        if !normalizedFirstMessage.isEmpty
            && (normalizedFirstMessage.hasPrefix(normalizedTitle) || normalizedTitle.hasPrefix(normalizedFirstMessage)) {
            return true
        }
        return normalizedTitle.count > 64
    }

    nonisolated private static func folderDisplayTitle(from cwd: String) -> String? {
        let trimmed = cwd.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        let lastComponent = URL(fileURLWithPath: trimmed).lastPathComponent
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !lastComponent.isEmpty else { return nil }
        return lastComponent
            .replacingOccurrences(of: "-", with: " ")
            .replacingOccurrences(of: "_", with: " ")
            .capitalized
    }

    nonisolated private static func hasTable(named tableName: String) -> Bool {
        let escapedName = tableName.replacingOccurrences(of: "'", with: "''")
        let query = "select 1 from sqlite_master where type = 'table' and name = '\(escapedName)' limit 1;"
        return sqliteRows(query: query).isEmpty == false
    }

    nonisolated private static func sqliteRows(query: String) -> [[String]] {
        let database = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("state_5.sqlite")
        guard FileManager.default.fileExists(atPath: database.path) else { return [] }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")
        let columnSeparator = "\u{1f}"
        let rowSeparator = "\u{1e}"
        process.arguments = [
            "-separator", columnSeparator,
            "-newline", rowSeparator,
            database.path,
            query,
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return []
        }

        guard process.terminationStatus == 0 else { return [] }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let output = String(decoding: data, as: UTF8.self)
        return output
            .split(separator: Character(rowSeparator), omittingEmptySubsequences: true)
            .map { line in line.split(separator: Character(columnSeparator), omittingEmptySubsequences: false).map(String.init) }
    }

    nonisolated private static func date(fromUnixString raw: String) -> Date? {
        guard let seconds = Double(raw), seconds > 0 else { return nil }
        return Date(timeIntervalSince1970: seconds)
    }

    nonisolated private static func date(fromMillisecondsString raw: String) -> Date? {
        guard let milliseconds = Double(raw), milliseconds > 0 else { return nil }
        return Date(timeIntervalSince1970: milliseconds / 1000)
    }

    nonisolated private static func relativeSubtitle(for date: Date?, fallback: String) -> String {
        guard let date else {
            return fallback.isEmpty ? "Recent thread" : fallback
        }

        let elapsed = max(0, Date().timeIntervalSince(date))
        if elapsed < 60 {
            return "Updated just now"
        }
        if elapsed < 60 * 60 {
            return "Updated \(Int(elapsed / 60))m ago"
        }
        if elapsed < 24 * 60 * 60 {
            return "Updated \(Int(elapsed / 3600))h ago"
        }
        return "Updated \(Int(elapsed / 86400))d ago"
    }

    nonisolated private static func processStatus(from rawStatus: String) -> CodexProcessItem.Status {
        switch rawStatus.lowercased() {
        case "running", "pending", "active", "queued", "in_progress":
            return .running
        case "completed", "complete", "succeeded", "success", "done":
            return .completed
        case "failed", "failure", "error", "disconnected", "cancelled", "canceled":
            return .failed
        default:
            return .waiting
        }
    }

    nonisolated private static func goalStatus(from rawStatus: String) -> CodexProcessItem.GoalStatus? {
        switch rawStatus.lowercased() {
        case "active":
            return .active
        case "paused":
            return .paused
        case "blocked":
            return .blocked
        case "usage_limited", "usagelimited":
            return .usageLimited
        case "budget_limited", "budgetlimited":
            return .budgetLimited
        case "complete":
            return .complete
        default:
            return nil
        }
    }

    nonisolated private static func displayedGoalStatus(
        from rawStatus: String,
        updatedAtMilliseconds: String
    ) -> CodexProcessItem.GoalStatus? {
        guard let status = goalStatus(from: rawStatus) else { return nil }
        guard status == .complete else { return status }
        guard let updatedAt = date(fromMillisecondsString: updatedAtMilliseconds) else { return nil }
        guard Date().timeIntervalSince(updatedAt) < recentGoalCompletionDisplayWindow else { return nil }
        return status
    }

    nonisolated private static func goalElapsedSeconds(
        status: CodexProcessItem.GoalStatus?,
        timeUsedSeconds rawTimeUsedSeconds: String,
        createdAtMilliseconds rawCreatedAtMilliseconds: String,
        updatedAtMilliseconds rawUpdatedAtMilliseconds: String
    ) -> Int? {
        guard status != nil else { return nil }
        let usedSeconds = max(0, Int(Double(rawTimeUsedSeconds) ?? 0))
        return usedSeconds
    }

    nonisolated private static func goalTimerReferenceDate(
        status: CodexProcessItem.GoalStatus?,
        timeUsedSeconds rawTimeUsedSeconds: String,
        createdAtMilliseconds rawCreatedAtMilliseconds: String,
        updatedAtMilliseconds rawUpdatedAtMilliseconds: String
    ) -> Date? {
        guard status == .active else { return nil }
        let usedSeconds = max(0, Int(Double(rawTimeUsedSeconds) ?? 0))
        if usedSeconds > 0 {
            return date(fromMillisecondsString: rawUpdatedAtMilliseconds)
        }
        return date(fromMillisecondsString: rawCreatedAtMilliseconds)
    }

    nonisolated private static func threadStatus(
        isFresh: Bool,
        goalStatus: CodexProcessItem.GoalStatus?
    ) -> CodexProcessItem.Status {
        switch goalStatus {
        case .active:
            return .running
        case .paused, .blocked, .usageLimited, .budgetLimited:
            return .waiting
        case .complete:
            return .completed
        case nil:
            return isFresh ? .running : .completed
        }
    }

    nonisolated private static func threadSubtitle(
        status: CodexProcessItem.Status,
        goalStatus: CodexProcessItem.GoalStatus?,
        goalElapsedSeconds: Int?,
        goalTimerReferenceDate: Date?,
        updatedAt: Date?,
        fallback: String
    ) -> String {
        guard let goalStatus else {
            return status == .running ? "Working now" : relativeSubtitle(for: updatedAt, fallback: fallback)
        }

        let duration = formatElapsedDuration(liveGoalElapsedSeconds(
            baseSeconds: goalElapsedSeconds ?? 0,
            referenceDate: goalTimerReferenceDate,
            status: goalStatus,
            at: Date()
        ))
        switch goalStatus {
        case .active:
            return "Goal running \(duration)"
        case .paused:
            return "Goal paused at \(duration)"
        case .blocked:
            return "Goal blocked at \(duration)"
        case .usageLimited:
            return "Goal usage limited at \(duration)"
        case .budgetLimited:
            return "Goal budget limited at \(duration)"
        case .complete:
            return "Goal reached in \(duration)"
        }
    }

    nonisolated private static func jobSubtitle(status: String, updatedAt: Date?) -> String {
        let label = status.isEmpty ? "Waiting" : status.displayTitle
        guard let updatedAt else { return label }
        return "\(label) - \(relativeSubtitle(for: updatedAt, fallback: "recently"))"
    }

    nonisolated static func formatElapsedDuration(_ seconds: Int) -> String {
        let seconds = max(0, seconds)
        if seconds < 60 {
            return "\(seconds)s"
        }
        if seconds < 60 * 60 {
            return "\(seconds / 60)m \(seconds % 60)s"
        }
        let hours = seconds / 3600
        let minutes = (seconds % 3600) / 60
        return minutes == 0 ? "\(hours)h" : "\(hours)h \(minutes)m"
    }

    nonisolated static func liveGoalElapsedSeconds(
        baseSeconds: Int,
        referenceDate: Date?,
        status: CodexProcessItem.GoalStatus?,
        at date: Date
    ) -> Int {
        guard status == .active, let referenceDate else {
            return max(0, baseSeconds)
        }
        return max(0, baseSeconds + Int(date.timeIntervalSince(referenceDate)))
    }

    nonisolated static func goalDisplaySummary(for item: CodexProcessItem, at date: Date = Date()) -> String? {
        guard let goalStatus = item.goalStatus, let baseSeconds = item.goalElapsedSeconds else { return nil }
        let seconds = liveGoalElapsedSeconds(
            baseSeconds: baseSeconds,
            referenceDate: item.goalTimerReferenceDate,
            status: goalStatus,
            at: date
        )
        let duration = formatElapsedDuration(seconds)
        switch goalStatus {
        case .active:
            return "Goal \(duration)"
        case .paused:
            return "Paused \(duration)"
        case .blocked:
            return "Blocked \(duration)"
        case .usageLimited:
            return "Usage \(duration)"
        case .budgetLimited:
            return "Budget \(duration)"
        case .complete:
            return "Reached \(duration)"
        }
    }

    private struct RolloutSnapshot {
        var assistantMessage: String?
        var turnID: String?
    }

    nonisolated private static func latestRolloutSnapshot(
        fromRolloutPath rawPath: String
    ) -> RolloutSnapshot {
        let url = rolloutURL(from: rawPath)
        guard FileManager.default.fileExists(atPath: url.path) else { return RolloutSnapshot() }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return RolloutSnapshot() }
        defer {
            try? handle.close()
        }

        let newline = Data([0x0A])
        let fileSize = (try? handle.seekToEnd()) ?? 0
        guard fileSize > 0 else { return RolloutSnapshot() }

        let readLength = min(fileSize, UInt64(Self.maxRolloutTailBytes))
        let startOffset = fileSize - readLength
        do {
            try handle.seek(toOffset: startOffset)
        } catch {
            return RolloutSnapshot()
        }

        var tail = handle.readDataToEndOfFile()
        if startOffset > 0 {
            guard let firstNewline = tail.firstRange(of: newline) else { return RolloutSnapshot() }
            tail.removeSubrange(tail.startIndex..<firstNewline.upperBound)
        }

        var snapshot = RolloutSnapshot()
        for line in tail.split(separator: 0x0A, omittingEmptySubsequences: true).reversed() {
            guard line.count <= Self.maxRolloutLineBytes else { continue }
            let data = Data(line)
            if snapshot.assistantMessage == nil {
                snapshot.assistantMessage = assistantMessage(fromLine: data)
            }
            if snapshot.turnID == nil {
                snapshot.turnID = turnID(fromLine: data)
            }
            if snapshot.assistantMessage != nil, snapshot.turnID != nil {
                break
            }
        }

        return snapshot
    }

    nonisolated private static func rolloutURL(from rawPath: String) -> URL {
        if rawPath.hasPrefix("/") {
            return URL(fileURLWithPath: rawPath)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent(rawPath)
    }

    nonisolated private static func assistantMessage(fromLine data: Data) -> String? {
        guard !data.isEmpty, data.count <= Self.maxRolloutLineBytes else { return nil }
        guard let line = String(data: data, encoding: .utf8) else { return nil }
        guard line.contains(#""type":"response_item""#) || line.contains(#""type":"event_msg""#) else { return nil }
        guard let jsonData = line.data(using: .utf8) else { return nil }
        guard
            let object = try? JSONSerialization.jsonObject(with: jsonData),
            let root = object as? [String: Any],
            let payload = root["payload"] as? [String: Any]
        else {
            return nil
        }

        if
            root["type"] as? String == "response_item",
            payload["type"] as? String == "message",
            payload["role"] as? String == "assistant"
        {
            return normalizedAssistantText(fromContent: payload["content"])
        }

        if
            root["type"] as? String == "event_msg",
            payload["type"] as? String == "agent_message",
            let message = payload["message"] as? String
        {
            return normalizedMessage(message)
        }

        return nil
    }

    nonisolated private static func turnID(fromLine data: Data) -> String? {
        guard !data.isEmpty, data.count <= Self.maxRolloutLineBytes else { return nil }
        guard let line = String(data: data, encoding: .utf8), line.contains(#""turn_id""#) else {
            return nil
        }
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let root = object as? [String: Any],
            let payload = root["payload"] as? [String: Any]
        else {
            return nil
        }

        if let turnID = payload["turn_id"] as? String, !turnID.isEmpty {
            return turnID
        }
        if
            let metadata = payload["internal_chat_message_metadata_passthrough"] as? [String: Any],
            let turnID = metadata["turn_id"] as? String,
            !turnID.isEmpty
        {
            return turnID
        }
        return nil
    }

    nonisolated private static func normalizedAssistantText(fromContent content: Any?) -> String? {
        if let text = content as? String {
            return normalizedMessage(text)
        }

        guard let fragments = content as? [[String: Any]] else { return nil }
        let text = fragments.compactMap { fragment -> String? in
            guard fragment["type"] as? String == "output_text" else { return nil }
            return fragment["text"] as? String
        }
        .joined(separator: "\n")
        return normalizedMessage(text)
    }

    nonisolated private static func normalizedMessage(_ message: String) -> String? {
        let trimmed = message.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        return trimmed
    }

    nonisolated static func isCurrentJob(status: CodexProcessItem.Status, updatedAt: Date?) -> Bool {
        switch status {
        case .running, .waiting:
            return true
        case .completed:
            guard let updatedAt else { return false }
            return Date().timeIntervalSince(updatedAt) < completedProcessDisplayWindow
        case .failed:
            return true
        }
    }

    nonisolated private static let threadRunningFreshnessWindow: TimeInterval = 3 * 60
    nonisolated private static let completedProcessDisplayWindow: TimeInterval = 5 * 60
    nonisolated private static let currentThreadWindow: TimeInterval =
        threadRunningFreshnessWindow + completedProcessDisplayWindow
    nonisolated private static let unresolvedFailuresDefaultsKey =
        "CodexCompanion.unresolvedFailedProcesses.v1"
    nonisolated private static let handledFailuresDefaultsKey =
        "CodexCompanion.handledFailedProcesses.v1"
    nonisolated private static let recentGoalCompletionDisplayWindow: TimeInterval = 30 * 60
    nonisolated private static let maxRolloutTailBytes = 8 * 1024 * 1024
    nonisolated private static let maxRolloutLineBytes = 512 * 1024
}

private enum CodexProcessStoreError: LocalizedError {
    case databaseMissing
    case sqliteFailed(String)

    var errorDescription: String? {
        switch self {
        case .databaseMissing:
            return "Codex state database was not found."
        case .sqliteFailed(let message):
            return message
        }
    }
}

private extension String {
    var nilIfEmpty: String? {
        isEmpty ? nil : self
    }

    var normalizedPromptComparison: String {
        trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
            .components(separatedBy: .whitespacesAndNewlines)
            .filter { !$0.isEmpty }
            .joined(separator: " ")
    }

    var shortenedProcessTitle: String? {
        let trimmed = trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        guard trimmed.count > 42 else { return trimmed }
        let endIndex = trimmed.index(trimmed.startIndex, offsetBy: 39)
        return "\(trimmed[..<endIndex])..."
    }
}
