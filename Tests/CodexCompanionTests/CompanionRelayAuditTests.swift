import Foundation
import Testing
@testable import CodexCompanion

@Suite("Companion relay audit")
struct CompanionRelayAuditTests {
    @Test("failure details are single-line and bounded")
    func sanitizesFailureDetails() {
        let result = CompanionRelayAudit.sanitizedFailure(
            "  first line\nsecond\tline with detail  ",
            maximumLength: 20
        )

        #expect(result == "first line second li")
        #expect(!result.contains("\n"))
    }

    @Test("repeated audit events are throttled until the interval elapses")
    func throttlesRepeatedEvents() {
        let throttle = CompanionRelayAuditLogThrottle(
            minimumInterval: 60,
            maximumEntryCount: 2
        )
        let start = Date(timeIntervalSince1970: 1_000)

        #expect(throttle.shouldRecord(key: "device-a", now: start))
        #expect(!throttle.shouldRecord(key: "device-a", now: start.addingTimeInterval(59)))
        #expect(throttle.shouldRecord(key: "device-a", now: start.addingTimeInterval(60)))
    }

    @Test("audit throttle keeps a bounded key set")
    func boundsTrackedKeys() {
        let throttle = CompanionRelayAuditLogThrottle(
            minimumInterval: 60,
            maximumEntryCount: 2
        )
        let start = Date(timeIntervalSince1970: 1_000)

        #expect(throttle.shouldRecord(key: "device-a", now: start))
        #expect(throttle.shouldRecord(key: "device-b", now: start.addingTimeInterval(1)))
        #expect(throttle.shouldRecord(key: "device-c", now: start.addingTimeInterval(2)))
        #expect(throttle.shouldRecord(key: "device-a", now: start.addingTimeInterval(3)))
    }
}
