import Foundation

enum CompanionRelayAudit {
    static func sanitizedFailure(_ value: String, maximumLength: Int = 240) -> String {
        let singleLine = value
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")
        guard singleLine.count > maximumLength else { return singleLine }
        return String(singleLine.prefix(maximumLength))
    }
}

final class CompanionRelayAuditLogThrottle: @unchecked Sendable {
    private let minimumInterval: TimeInterval
    private let maximumEntryCount: Int
    private let lock = NSLock()
    private var lastRecordedAtByKey: [String: Date] = [:]

    init(minimumInterval: TimeInterval = 60, maximumEntryCount: Int = 64) {
        self.minimumInterval = max(0, minimumInterval)
        self.maximumEntryCount = max(1, maximumEntryCount)
    }

    func shouldRecord(key: String, now: Date = Date()) -> Bool {
        lock.withLock {
            if let lastRecordedAt = lastRecordedAtByKey[key],
               now.timeIntervalSince(lastRecordedAt) < minimumInterval {
                return false
            }

            lastRecordedAtByKey[key] = now
            if lastRecordedAtByKey.count > maximumEntryCount,
               let oldest = lastRecordedAtByKey.min(by: { $0.value < $1.value })?.key {
                lastRecordedAtByKey.removeValue(forKey: oldest)
            }
            return true
        }
    }
}

private extension NSLock {
    func withLock<Value>(_ operation: () throws -> Value) rethrows -> Value {
        lock()
        defer { unlock() }
        return try operation()
    }
}
