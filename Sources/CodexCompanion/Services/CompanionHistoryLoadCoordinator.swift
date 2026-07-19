import Foundation

struct CompanionHistoryLoadKey: Hashable, Sendable {
    var threadID: String
    var cursor: String?
    var limit: Int
}

struct CompanionHistorySnapshot: Equatable, Sendable {
    var messages: [CompanionBridgeMessage]
    var nextCursor: String?
    var timelineItems: [CompanionBridgeTimelineItem]
    var revision: String
    var timelineNextCursor: String?
    var subagents: [CompanionBridgeSubagent]
    var contextUsage: CompanionBridgeContextUsage?
}

actor CompanionHistoryLoadCoordinator {
    private struct CacheEntry {
        var snapshot: CompanionHistorySnapshot
        var expiresAt: Date
    }

    private let cacheLifetime: TimeInterval
    private let maximumCacheEntryCount: Int
    private let now: @Sendable () -> Date
    private var inFlight: [CompanionHistoryLoadKey: Task<CompanionHistorySnapshot, Error>] = [:]
    private var cache: [CompanionHistoryLoadKey: CacheEntry] = [:]

    init(
        cacheLifetime: TimeInterval = 0.75,
        maximumCacheEntryCount: Int = 8,
        now: @escaping @Sendable () -> Date = { Date() }
    ) {
        self.cacheLifetime = max(0, cacheLifetime)
        self.maximumCacheEntryCount = max(1, maximumCacheEntryCount)
        self.now = now
    }

    func load(
        key: CompanionHistoryLoadKey,
        operation: @escaping @Sendable () throws -> CompanionHistorySnapshot
    ) async throws -> CompanionHistorySnapshot {
        let currentDate = now()
        removeExpiredEntries(at: currentDate)

        if let entry = cache[key], entry.expiresAt > currentDate {
            return entry.snapshot
        }
        if let task = inFlight[key] {
            return try await task.value
        }

        let task = Task.detached(priority: .userInitiated) {
            try autoreleasepool {
                try operation()
            }
        }
        inFlight[key] = task

        do {
            let snapshot = try await task.value
            inFlight[key] = nil
            cache[key] = CacheEntry(
                snapshot: snapshot,
                expiresAt: now().addingTimeInterval(cacheLifetime)
            )
            trimCacheIfNeeded()
            return snapshot
        } catch {
            inFlight[key] = nil
            throw error
        }
    }

    private func removeExpiredEntries(at date: Date) {
        cache = cache.filter { $0.value.expiresAt > date }
    }

    private func trimCacheIfNeeded() {
        guard cache.count > maximumCacheEntryCount else { return }
        let overflowCount = cache.count - maximumCacheEntryCount
        for key in cache.sorted(by: { $0.value.expiresAt < $1.value.expiresAt })
            .prefix(overflowCount)
            .map(\.key) {
            cache[key] = nil
        }
    }
}
