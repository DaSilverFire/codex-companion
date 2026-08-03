import Foundation

enum CompanionIncomingAttachmentUploadError: LocalizedError, Equatable {
    case invalidUpload
    case unauthorizedUpload
    case invalidFilename
    case attachmentTooLarge(String)
    case tooManyAttachments
    case totalPayloadTooLarge
    case chunkTooLarge
    case invalidOffset
    case payloadExceedsDeclaredSize
    case uploadIncomplete
    case uploadAlreadyConsumed

    var errorDescription: String? {
        switch self {
        case .invalidUpload:
            "The attachment upload is unavailable. Add the file again and retry."
        case .unauthorizedUpload:
            "That attachment upload belongs to a different paired device."
        case .invalidFilename:
            "One attachment has an invalid filename."
        case .attachmentTooLarge(let filename):
            "\(filename) is larger than its supported attachment limit."
        case .tooManyAttachments:
            "You can attach up to \(CompanionIncomingAttachmentUploadStore.maximumAttachmentCount) items."
        case .totalPayloadTooLarge:
            "The selected attachments are larger than the 2 GB total limit."
        case .chunkTooLarge:
            "The attachment transfer chunk is too large."
        case .invalidOffset:
            "The attachment transfer arrived out of order."
        case .payloadExceedsDeclaredSize:
            "The attachment contains more data than declared."
        case .uploadIncomplete:
            "The attachment did not finish uploading."
        case .uploadAlreadyConsumed:
            "The attachment was already used by a different request."
        }
    }
}

actor CompanionIncomingAttachmentUploadStore {
    static let maximumAttachmentCount = 10
    static let maximumImageBytes: Int64 = 150 * 1_024 * 1_024
    static let maximumFileBytes: Int64 = 2 * 1_024 * 1_024 * 1_024
    static let maximumTotalBytes: Int64 = 2 * 1_024 * 1_024 * 1_024
    static let maximumChunkBytes = 393_216

    private struct Entry {
        var deviceID: String
        var attachmentID: UUID
        var kind: CompanionBridgeAttachmentKind
        var filename: String
        var mimeType: String?
        var byteCount: Int64
        var bytesReceived: Int64
        var fileURL: URL
        var createdAt: Date
        var consumedRequestID: UUID?
    }

    private let rootURL: URL
    private let fileManager: FileManager
    private var entries: [UUID: Entry] = [:]

    init(
        rootURL: URL = CompanionIncomingAttachmentUploadStore.defaultRootURL(),
        fileManager: FileManager = .default
    ) {
        self.rootURL = rootURL
        self.fileManager = fileManager
    }

    func begin(
        uploadID: UUID,
        attachment: CompanionBridgeAttachment,
        deviceID: String
    ) throws -> CompanionAttachmentUploadProgress {
        guard attachment.uploadID == uploadID,
              attachment.payloadByteCount >= 0,
              attachment.data.isEmpty
        else {
            throw CompanionIncomingAttachmentUploadError.invalidUpload
        }
        _ = try sanitizedFilename(attachment.filename)
        let maximumBytes = attachment.kind == .image
            ? Self.maximumImageBytes
            : Self.maximumFileBytes
        guard attachment.payloadByteCount <= maximumBytes else {
            throw CompanionIncomingAttachmentUploadError.attachmentTooLarge(attachment.filename)
        }

        if let existing = entries[uploadID] {
            guard existing.deviceID == deviceID else {
                throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
            }
            guard existing.attachmentID == attachment.id,
                  existing.kind == attachment.kind,
                  existing.filename == attachment.filename,
                  existing.mimeType == attachment.mimeType,
                  existing.byteCount == attachment.payloadByteCount
            else {
                throw CompanionIncomingAttachmentUploadError.invalidUpload
            }
            return progress(for: existing)
        }

        try fileManager.createDirectory(at: rootURL, withIntermediateDirectories: true)
        try pruneExpiredUploads()
        let activeEntries = entries.values.filter {
            $0.deviceID == deviceID && $0.consumedRequestID == nil
        }
        guard activeEntries.count < Self.maximumAttachmentCount else {
            throw CompanionIncomingAttachmentUploadError.tooManyAttachments
        }
        let activeBytes = activeEntries.reduce(Int64(0)) { partial, entry in
            partial + entry.byteCount
        }
        guard activeBytes <= Self.maximumTotalBytes - attachment.payloadByteCount else {
            throw CompanionIncomingAttachmentUploadError.totalPayloadTooLarge
        }

        let fileURL = rootURL.appendingPathComponent(
            "\(uploadID.uuidString).upload",
            isDirectory: false
        )
        guard fileManager.createFile(atPath: fileURL.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let entry = Entry(
            deviceID: deviceID,
            attachmentID: attachment.id,
            kind: attachment.kind,
            filename: attachment.filename,
            mimeType: attachment.mimeType,
            byteCount: attachment.payloadByteCount,
            bytesReceived: 0,
            fileURL: fileURL,
            createdAt: Date(),
            consumedRequestID: nil
        )
        entries[uploadID] = entry
        return progress(for: entry)
    }

    func append(
        uploadID: UUID,
        deviceID: String,
        offset: Int64,
        data: Data
    ) throws -> CompanionAttachmentUploadProgress {
        guard var entry = entries[uploadID] else {
            throw CompanionIncomingAttachmentUploadError.invalidUpload
        }
        guard entry.deviceID == deviceID else {
            throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
        }
        guard entry.consumedRequestID == nil else {
            throw CompanionIncomingAttachmentUploadError.uploadAlreadyConsumed
        }
        guard data.count <= Self.maximumChunkBytes else {
            throw CompanionIncomingAttachmentUploadError.chunkTooLarge
        }
        guard offset == entry.bytesReceived else {
            throw CompanionIncomingAttachmentUploadError.invalidOffset
        }
        let chunkBytes = Int64(data.count)
        guard chunkBytes <= entry.byteCount - entry.bytesReceived else {
            throw CompanionIncomingAttachmentUploadError.payloadExceedsDeclaredSize
        }
        if !data.isEmpty {
            let handle = try FileHandle(forWritingTo: entry.fileURL)
            defer { try? handle.close() }
            try handle.seek(toOffset: UInt64(offset))
            try handle.write(contentsOf: data)
        }
        entry.bytesReceived += chunkBytes
        entries[uploadID] = entry
        return progress(for: entry)
    }

    func stage(
        _ attachments: [CompanionBridgeAttachment],
        requestID: UUID,
        deviceID: String
    ) throws -> [CodexFollowerAttachment] {
        guard attachments.count <= Self.maximumAttachmentCount else {
            throw CompanionIncomingAttachmentUploadError.tooManyAttachments
        }
        guard Set(attachments.map(\.id)).count == attachments.count,
              Set(attachments.compactMap(\.uploadID)).count == attachments.count
        else {
            throw CompanionIncomingAttachmentUploadError.invalidUpload
        }

        var totalBytes: Int64 = 0
        var resolved: [(uploadID: UUID, entry: Entry, label: String)] = []
        resolved.reserveCapacity(attachments.count)
        for attachment in attachments {
            guard let uploadID = attachment.uploadID,
                  let entry = entries[uploadID]
            else {
                throw CompanionIncomingAttachmentUploadError.invalidUpload
            }
            guard entry.deviceID == deviceID else {
                throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
            }
            guard entry.consumedRequestID == nil || entry.consumedRequestID == requestID else {
                throw CompanionIncomingAttachmentUploadError.uploadAlreadyConsumed
            }
            guard entry.attachmentID == attachment.id,
                  entry.kind == attachment.kind,
                  entry.filename == attachment.filename,
                  entry.mimeType == attachment.mimeType,
                  entry.byteCount == attachment.payloadByteCount,
                  entry.bytesReceived == entry.byteCount
            else {
                throw CompanionIncomingAttachmentUploadError.uploadIncomplete
            }
            let label = try sanitizedFilename(entry.filename)
            totalBytes += entry.byteCount
            guard totalBytes <= Self.maximumTotalBytes else {
                throw CompanionIncomingAttachmentUploadError.totalPayloadTooLarge
            }
            resolved.append((uploadID, entry, label))
        }

        guard !resolved.isEmpty else { return [] }
        let requestDirectory = rootURL
            .appendingPathComponent("staged", isDirectory: true)
            .appendingPathComponent(requestID.uuidString, isDirectory: true)
        try fileManager.createDirectory(at: requestDirectory, withIntermediateDirectories: true)

        return try resolved.map { item in
            var entry = item.entry
            let destination = requestDirectory.appendingPathComponent(
                "\(entry.attachmentID.uuidString)-\(item.label)",
                isDirectory: false
            )
            if entry.fileURL != destination {
                if fileManager.fileExists(atPath: destination.path) {
                    try fileManager.removeItem(at: destination)
                }
                try fileManager.moveItem(at: entry.fileURL, to: destination)
                entry.fileURL = destination
            }
            entry.consumedRequestID = requestID
            entries[item.uploadID] = entry
            return CodexFollowerAttachment(
                id: entry.attachmentID,
                kind: entry.kind,
                label: item.label,
                path: destination.path,
                fsPath: destination.path,
                mimeType: entry.mimeType
            )
        }
    }

    func cancel(uploadID: UUID, deviceID: String) throws {
        guard let entry = entries[uploadID] else { return }
        guard entry.deviceID == deviceID else {
            throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
        }
        guard entry.consumedRequestID == nil else { return }
        entries.removeValue(forKey: uploadID)
        try? fileManager.removeItem(at: entry.fileURL)
    }

    private func progress(for entry: Entry) -> CompanionAttachmentUploadProgress {
        CompanionAttachmentUploadProgress(
            nextOffset: entry.bytesReceived,
            isComplete: entry.bytesReceived == entry.byteCount
        )
    }

    private func sanitizedFilename(_ filename: String) throws -> String {
        let trimmed = filename.trimmingCharacters(in: .whitespacesAndNewlines)
        let lastComponent = URL(fileURLWithPath: trimmed).lastPathComponent
        guard !lastComponent.isEmpty,
              lastComponent != ".",
              lastComponent != "..",
              !lastComponent.contains("\0")
        else {
            throw CompanionIncomingAttachmentUploadError.invalidFilename
        }
        return lastComponent
    }

    private func pruneExpiredUploads(now: Date = Date()) throws {
        let expirationDate = now.addingTimeInterval(-7 * 24 * 60 * 60)
        let expiredEntries = entries.filter { $0.value.createdAt < expirationDate }
        for (uploadID, entry) in expiredEntries {
            entries.removeValue(forKey: uploadID)
            try? fileManager.removeItem(at: entry.fileURL)
        }
        let keys: Set<URLResourceKey> = [.contentModificationDateKey]
        let candidates = (try? fileManager.contentsOfDirectory(
            at: rootURL,
            includingPropertiesForKeys: Array(keys),
            options: [.skipsHiddenFiles]
        )) ?? []
        for candidate in candidates where candidate.pathExtension == "upload" {
            let modifiedAt = try candidate.resourceValues(forKeys: keys).contentModificationDate
            if let modifiedAt, modifiedAt < expirationDate {
                try? fileManager.removeItem(at: candidate)
            }
        }
    }

    private static func defaultRootURL(fileManager: FileManager = .default) -> URL {
        let support = (try? fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )) ?? fileManager.temporaryDirectory
        return support
            .appendingPathComponent("CodexCompanion", isDirectory: true)
            .appendingPathComponent("IncomingAttachments", isDirectory: true)
            .appendingPathComponent("Uploads-v1", isDirectory: true)
    }
}
