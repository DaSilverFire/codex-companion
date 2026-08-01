import Foundation

actor CompanionBridgeRequestCoordinator {
    private struct CacheEntry {
        var response: CompanionBridgeResponse
        var expiresAt: Date
    }

    private let cacheLifetime: TimeInterval
    private let maximumCacheEntryCount: Int
    private let now: @Sendable () -> Date
    private var inFlight: [UUID: Task<CompanionBridgeResponse, Never>] = [:]
    private var cache: [UUID: CacheEntry] = [:]

    init(
        cacheLifetime: TimeInterval = 120,
        maximumCacheEntryCount: Int = 64,
        now: @escaping @Sendable () -> Date = { Date() }
    ) {
        self.cacheLifetime = max(0, cacheLifetime)
        self.maximumCacheEntryCount = max(1, maximumCacheEntryCount)
        self.now = now
    }

    func response(
        for requestID: UUID,
        operation: @escaping @Sendable () async -> CompanionBridgeResponse
    ) async -> CompanionBridgeResponse {
        let currentDate = now()
        removeExpiredEntries(at: currentDate)

        if let entry = cache[requestID], entry.expiresAt > currentDate {
            return entry.response
        }
        if let task = inFlight[requestID] {
            return await task.value
        }

        let task = Task(priority: .userInitiated) {
            await operation()
        }
        inFlight[requestID] = task
        let response = await task.value
        inFlight[requestID] = nil
        cache[requestID] = CacheEntry(
            response: response,
            expiresAt: now().addingTimeInterval(cacheLifetime)
        )
        trimCacheIfNeeded()
        return response
    }

    private func removeExpiredEntries(at date: Date) {
        cache = cache.filter { $0.value.expiresAt > date }
    }

    private func trimCacheIfNeeded() {
        guard cache.count > maximumCacheEntryCount else { return }
        let overflowCount = cache.count - maximumCacheEntryCount
        for requestID in cache
            .sorted(by: { $0.value.expiresAt < $1.value.expiresAt })
            .prefix(overflowCount)
            .map(\.key) {
            cache[requestID] = nil
        }
    }
}
