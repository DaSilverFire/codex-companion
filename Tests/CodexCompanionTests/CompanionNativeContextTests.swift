import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionNativeContextTests {
    private let timeZone = TimeZone(identifier: "America/Indiana/Indianapolis")!

    @Test
    func locationSummaryIncludesCoordinatesAccuracyAndPrivacyBoundary() {
        let snapshot = CompanionLocationSnapshot(
            latitude: 39.7684,
            longitude: -86.1581,
            horizontalAccuracy: 18,
            capturedAt: Date(timeIntervalSince1970: 1_784_297_600)
        )

        let summary = snapshot.toolSummary(timeZone: timeZone)

        #expect(summary.contains("39.768400"))
        #expect(summary.contains("-86.158100"))
        #expect(summary.contains("18 m"))
        #expect(summary.contains("Do not expose these coordinates"))
    }

    @Test
    func agendaSummarySortsEventsAndKeepsCalendarContext() throws {
        let later = CompanionCalendarEvent(
            title: "Design review",
            startDate: Date(timeIntervalSince1970: 1_784_344_400),
            endDate: Date(timeIntervalSince1970: 1_784_348_000),
            isAllDay: false,
            calendarTitle: "Work",
            location: "Studio"
        )
        let earlier = CompanionCalendarEvent(
            title: "Morning focus",
            startDate: Date(timeIntervalSince1970: 1_784_322_800),
            endDate: Date(timeIntervalSince1970: 1_784_326_400),
            isAllDay: false,
            calendarTitle: "Personal",
            location: nil
        )

        let summary = CompanionPersonalContextFormatter.agendaSummary(
            events: [later, earlier],
            timeZone: timeZone
        )

        let morningRange = try #require(summary.range(of: "Morning focus"))
        let reviewRange = try #require(summary.range(of: "Design review"))
        #expect(morningRange.lowerBound < reviewRange.lowerBound)
        #expect(summary.contains("[Personal]"))
        #expect(summary.contains("[Work]"))
        #expect(summary.contains("at Studio"))
        #expect(summary.contains("read-only"))
    }

    @Test
    func reminderSummaryShowsDueAndUndatedItems() {
        let reminders = [
            CompanionReminderItem(
                title: "Submit build",
                dueDate: Date(timeIntervalSince1970: 1_784_351_600),
                listTitle: "Work",
                priority: 1
            ),
            CompanionReminderItem(
                title: "Replace filter",
                dueDate: nil,
                listTitle: "Home",
                priority: 0
            ),
        ]

        let summary = CompanionPersonalContextFormatter.reminderSummary(
            reminders: reminders,
            timeZone: timeZone
        )

        #expect(summary.contains("Submit build"))
        #expect(summary.contains("high priority"))
        #expect(summary.contains("Replace filter"))
        #expect(summary.contains("no due date"))
        #expect(summary.contains("read-only"))
    }

    @Test
    func emptyPersonalDataSummariesAreExplicit() {
        #expect(
            CompanionPersonalContextFormatter.agendaSummary(
                events: [],
                timeZone: timeZone
            ).contains("No calendar events")
        )
        #expect(
            CompanionPersonalContextFormatter.reminderSummary(
                reminders: [],
                timeZone: timeZone
            ).contains("No incomplete reminders")
        )
    }
}
