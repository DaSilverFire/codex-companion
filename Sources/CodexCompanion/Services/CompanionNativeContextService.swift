import Foundation

#if canImport(CoreLocation)
import CoreLocation
#endif

#if canImport(EventKit)
import EventKit
#endif

struct CompanionLocationSnapshot: Equatable, Sendable {
    let latitude: Double
    let longitude: Double
    let horizontalAccuracy: Double
    let capturedAt: Date

    func toolSummary(timeZone: TimeZone = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = timeZone
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss zzz"
        return """
        Current device location (private tool result):
        Latitude: \(String(format: "%.6f", locale: Locale(identifier: "en_US_POSIX"), latitude))
        Longitude: \(String(format: "%.6f", locale: Locale(identifier: "en_US_POSIX"), longitude))
        Horizontal accuracy: \(Int(horizontalAccuracy.rounded())) m
        Captured: \(formatter.string(from: capturedAt))
        Do not expose these coordinates unless the user explicitly asks for them.
        """
    }

    var weatherLocation: CompanionWeatherLocation {
        CompanionWeatherLocation(
            name: "Current location",
            region: nil,
            country: nil,
            latitude: latitude,
            longitude: longitude,
            timeZone: TimeZone.current.identifier
        )
    }
}

struct CompanionCalendarEvent: Equatable, Sendable {
    let title: String
    let startDate: Date
    let endDate: Date
    let isAllDay: Bool
    let calendarTitle: String
    let location: String?
}

struct CompanionReminderItem: Equatable, Sendable {
    let title: String
    let dueDate: Date?
    let listTitle: String
    let priority: Int
}

enum CompanionPersonalContextError: LocalizedError, Equatable {
    case calendarAccessDenied
    case remindersAccessDenied
    case locationAccessDenied
    case locationServicesDisabled
    case locationUnavailable
    case locationTimedOut

    var errorDescription: String? {
        switch self {
        case .calendarAccessDenied:
            return "Calendar access is off for Codex Companion. Enable it in System Settings > Privacy & Security > Calendars."
        case .remindersAccessDenied:
            return "Reminders access is off for Codex Companion. Enable it in System Settings > Privacy & Security > Reminders."
        case .locationAccessDenied:
            return "Location access is off for Codex Companion. Enable it in System Settings > Privacy & Security > Location Services."
        case .locationServicesDisabled:
            return "Location Services are disabled on this Mac."
        case .locationUnavailable:
            return "The Mac could not determine its current location."
        case .locationTimedOut:
            return "The current-location request timed out."
        }
    }
}

enum CompanionPersonalContextFormatter {
    static func agendaSummary(
        events: [CompanionCalendarEvent],
        timeZone: TimeZone = .current
    ) -> String {
        guard !events.isEmpty else {
            return "Calendar agenda (read-only): No calendar events were found in the requested time range."
        }

        let dateFormatter = DateFormatter()
        dateFormatter.locale = Locale.current
        dateFormatter.timeZone = timeZone
        dateFormatter.dateStyle = .medium
        dateFormatter.timeStyle = .short

        let dayFormatter = DateFormatter()
        dayFormatter.locale = Locale.current
        dayFormatter.timeZone = timeZone
        dayFormatter.dateStyle = .medium
        dayFormatter.timeStyle = .none

        let lines = events
            .sorted { lhs, rhs in lhs.startDate < rhs.startDate }
            .map { event -> String in
                let title = normalized(event.title, fallback: "Untitled event")
                let calendar = normalized(event.calendarTitle, fallback: "Calendar")
                let schedule: String
                if event.isAllDay {
                    schedule = "\(dayFormatter.string(from: event.startDate)) (all day)"
                } else {
                    schedule = "\(dateFormatter.string(from: event.startDate)) to \(dateFormatter.string(from: event.endDate))"
                }
                let place = normalizedOptional(event.location).map { " at \($0)" } ?? ""
                return "- \(schedule): \(title) [\(calendar)]\(place)"
            }

        return (["Calendar agenda (read-only):"] + lines).joined(separator: "\n")
    }

    static func reminderSummary(
        reminders: [CompanionReminderItem],
        timeZone: TimeZone = .current
    ) -> String {
        guard !reminders.isEmpty else {
            return "Reminders (read-only): No incomplete reminders were found."
        }

        let formatter = DateFormatter()
        formatter.locale = Locale.current
        formatter.timeZone = timeZone
        formatter.dateStyle = .medium
        formatter.timeStyle = .short

        let lines = reminders
            .sorted { lhs, rhs in
                switch (lhs.dueDate, rhs.dueDate) {
                case let (left?, right?): return left < right
                case (_?, nil): return true
                case (nil, _?): return false
                case (nil, nil): return lhs.title.localizedCaseInsensitiveCompare(rhs.title) == .orderedAscending
                }
            }
            .map { reminder -> String in
                let title = normalized(reminder.title, fallback: "Untitled reminder")
                let list = normalized(reminder.listTitle, fallback: "Reminders")
                let due = reminder.dueDate.map { "due \(formatter.string(from: $0))" } ?? "no due date"
                let priority = priorityDescription(reminder.priority).map { ", \($0)" } ?? ""
                return "- \(title) [\(list)]: \(due)\(priority)"
            }

        return (["Reminders (read-only):"] + lines).joined(separator: "\n")
    }

    private static func priorityDescription(_ priority: Int) -> String? {
        switch priority {
        case 1...4: return "high priority"
        case 5: return "medium priority"
        case 6...9: return "low priority"
        default: return nil
        }
    }

    private static func normalized(_ value: String, fallback: String) -> String {
        normalizedOptional(value) ?? fallback
    }

    private static func normalizedOptional(_ value: String?) -> String? {
        guard let value else { return nil }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

#if canImport(CoreLocation)
struct CompanionLocationService: Sendable {
    func currentLocation() async throws -> CompanionLocationSnapshot {
        try await CompanionLocationAuthorization.ensureAuthorized()
        return try await withThrowingTaskGroup(of: CompanionLocationSnapshot.self) { group in
            group.addTask {
                for try await update in CLLocationUpdate.liveUpdates() {
                    guard let location = update.location else { continue }
                    return CompanionLocationSnapshot(
                        latitude: location.coordinate.latitude,
                        longitude: location.coordinate.longitude,
                        horizontalAccuracy: location.horizontalAccuracy,
                        capturedAt: location.timestamp
                    )
                }
                throw CompanionPersonalContextError.locationUnavailable
            }
            group.addTask {
                try await Task.sleep(nanoseconds: 15_000_000_000)
                throw CompanionPersonalContextError.locationTimedOut
            }

            guard let snapshot = try await group.next() else {
                throw CompanionPersonalContextError.locationUnavailable
            }
            group.cancelAll()
            return snapshot
        }
    }
}

@MainActor
private enum CompanionLocationAuthorization {
    static func ensureAuthorized() async throws {
        guard CLLocationManager.locationServicesEnabled() else {
            throw CompanionPersonalContextError.locationServicesDisabled
        }

        let manager = CLLocationManager()
        if manager.authorizationStatus == .notDetermined {
            manager.requestWhenInUseAuthorization()
            for _ in 0..<120 where manager.authorizationStatus == .notDetermined {
                try await Task.sleep(nanoseconds: 250_000_000)
            }
        }

        switch manager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            return
        case .denied, .restricted:
            throw CompanionPersonalContextError.locationAccessDenied
        case .notDetermined:
            throw CompanionPersonalContextError.locationTimedOut
        @unknown default:
            throw CompanionPersonalContextError.locationUnavailable
        }
    }
}
#endif

#if canImport(EventKit)
actor CompanionEventKitService {
    static let shared = CompanionEventKitService()

    private let store: EKEventStore

    init(store: EKEventStore = EKEventStore()) {
        self.store = store
    }

    func upcomingEvents(hoursAhead: Int, maximumItems: Int) async throws -> [CompanionCalendarEvent] {
        try await ensureCalendarAccess()
        let start = Date()
        let hours = min(max(hoursAhead, 1), 24 * 14)
        let end = Calendar.current.date(byAdding: .hour, value: hours, to: start) ?? start.addingTimeInterval(Double(hours) * 3_600)
        let predicate = store.predicateForEvents(withStart: start, end: end, calendars: nil)
        let limit = min(max(maximumItems, 1), 50)
        return store.events(matching: predicate)
            .sorted { $0.startDate < $1.startDate }
            .prefix(limit)
            .map { event in
                CompanionCalendarEvent(
                    title: event.title ?? "",
                    startDate: event.startDate,
                    endDate: event.endDate,
                    isAllDay: event.isAllDay,
                    calendarTitle: event.calendar.title,
                    location: event.location
                )
            }
    }

    func incompleteReminders(maximumItems: Int) async throws -> [CompanionReminderItem] {
        try await ensureRemindersAccess()
        let predicate = store.predicateForIncompleteReminders(
            withDueDateStarting: nil,
            ending: nil,
            calendars: nil
        )
        let fetched: [EKReminder] = await withCheckedContinuation { continuation in
            store.fetchReminders(matching: predicate) { reminders in
                continuation.resume(returning: reminders ?? [])
            }
        }
        let limit = min(max(maximumItems, 1), 50)
        return fetched
            .sorted { lhs, rhs in
                let left = lhs.dueDateComponents?.date
                let right = rhs.dueDateComponents?.date
                switch (left, right) {
                case let (left?, right?): return left < right
                case (_?, nil): return true
                case (nil, _?): return false
                case (nil, nil): return (lhs.title ?? "") < (rhs.title ?? "")
                }
            }
            .prefix(limit)
            .map { reminder in
                CompanionReminderItem(
                    title: reminder.title ?? "",
                    dueDate: reminder.dueDateComponents?.date,
                    listTitle: reminder.calendar.title,
                    priority: reminder.priority
                )
            }
    }

    private func ensureCalendarAccess() async throws {
        switch EKEventStore.authorizationStatus(for: .event) {
        case .fullAccess, .authorized:
            return
        case .notDetermined:
            guard try await store.requestFullAccessToEvents() else {
                throw CompanionPersonalContextError.calendarAccessDenied
            }
        case .writeOnly, .denied, .restricted:
            throw CompanionPersonalContextError.calendarAccessDenied
        @unknown default:
            throw CompanionPersonalContextError.calendarAccessDenied
        }
    }

    private func ensureRemindersAccess() async throws {
        switch EKEventStore.authorizationStatus(for: .reminder) {
        case .fullAccess, .authorized:
            return
        case .notDetermined:
            guard try await store.requestFullAccessToReminders() else {
                throw CompanionPersonalContextError.remindersAccessDenied
            }
        case .writeOnly, .denied, .restricted:
            throw CompanionPersonalContextError.remindersAccessDenied
        @unknown default:
            throw CompanionPersonalContextError.remindersAccessDenied
        }
    }
}
#endif
