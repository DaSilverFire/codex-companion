import Foundation

struct CompanionPairingRecord: Codable, Equatable, Identifiable, Sendable {
    var id: String { deviceID }
    var deviceID: String
    var displayName: String
    var secret: Data
    var pairedAt: Date
    var relayURLString: String? = nil
}

enum CompanionPairingRecordStoreError: Error {
    case invalidSecret
}

final class CompanionPairingRecordStore: @unchecked Sendable {
    private struct Payload: Codable {
        var version = 1
        var records: [CompanionPairingRecord]
    }

    static var defaultFileURL: URL {
        let root = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first ?? FileManager.default.temporaryDirectory
        return root
            .appendingPathComponent("Codex Companion", isDirectory: true)
            .appendingPathComponent("Security", isDirectory: true)
            .appendingPathComponent("paired-devices.json")
    }

    private let fileURL: URL
    private let lock = NSLock()
    private var recordsByID: [String: CompanionPairingRecord]

    init(fileURL: URL = CompanionPairingRecordStore.defaultFileURL) {
        self.fileURL = fileURL
        if let data = try? Data(contentsOf: fileURL),
           let payload = try? JSONDecoder().decode(Payload.self, from: data) {
            recordsByID = Dictionary(uniqueKeysWithValues: payload.records.map { ($0.deviceID, $0) })
        } else {
            recordsByID = [:]
        }
    }

    func record(for deviceID: String) -> CompanionPairingRecord? {
        lock.withLock { recordsByID[deviceID] }
    }

    func records() -> [CompanionPairingRecord] {
        lock.withLock {
            recordsByID.values.sorted { lhs, rhs in
                if lhs.pairedAt != rhs.pairedAt { return lhs.pairedAt < rhs.pairedAt }
                return lhs.displayName.localizedStandardCompare(rhs.displayName) == .orderedAscending
            }
        }
    }

    func save(_ record: CompanionPairingRecord) throws {
        guard record.secret.count >= 32 else {
            throw CompanionPairingRecordStoreError.invalidSecret
        }
        try lock.withLock {
            recordsByID[record.deviceID] = record
            try persistLocked()
        }
    }

    func remove(deviceID: String) throws {
        try lock.withLock {
            recordsByID.removeValue(forKey: deviceID)
            try persistLocked()
        }
    }

    private func persistLocked() throws {
        let directory = fileURL.deletingLastPathComponent()
        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let payload = Payload(records: recordsByID.values.sorted { $0.deviceID < $1.deviceID })
        try encoder.encode(payload).write(to: fileURL, options: .atomic)

        var attributes: [FileAttributeKey: Any] = [.posixPermissions: 0o600]
        #if os(iOS)
        attributes[.protectionKey] = FileProtectionType.completeUntilFirstUserAuthentication
        #endif
        try FileManager.default.setAttributes(attributes, ofItemAtPath: fileURL.path)
    }
}

private extension NSLock {
    func withLock<Value>(_ operation: () throws -> Value) rethrows -> Value {
        lock()
        defer { unlock() }
        return try operation()
    }
}
