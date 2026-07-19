import Foundation

struct CodexFollowerAttachment: Equatable, Sendable {
    var id: UUID
    var kind: CompanionBridgeAttachmentKind
    var label: String
    var path: String
    var fsPath: String
    var mimeType: String?

    var nativeAttachment: [String: Any] {
        [
            "label": label,
            "path": path,
            "fsPath": fsPath,
        ]
    }

    var inputItem: [String: Any]? {
        guard kind == .image else { return nil }
        return [
            "type": "localImage",
            "path": path,
        ]
    }

    var appServerInputItem: [String: Any] {
        switch kind {
        case .image:
            return [
                "type": "localImage",
                "path": path,
            ]
        case .file:
            return [
                "type": "mention",
                "name": label,
                "path": path,
            ]
        }
    }

    var queuedImageAttachment: [String: Any]? {
        guard kind == .image else { return nil }
        var result: [String: Any] = [
            "id": id.uuidString,
            "src": URL(fileURLWithPath: path).absoluteString,
            "filename": label,
            "localPath": path,
            "uploadStatus": "uploaded",
        ]
        if let mimeType, !mimeType.isEmpty {
            result["mimeType"] = mimeType
        }
        return result
    }
}

enum CompanionIncomingAttachmentStoreError: LocalizedError, Equatable {
    case tooManyAttachments
    case attachmentTooLarge(String)
    case totalPayloadTooLarge
    case invalidFilename

    var errorDescription: String? {
        switch self {
        case .tooManyAttachments:
            "You can attach up to \(CompanionIncomingAttachmentStore.maximumAttachmentCount) items."
        case .attachmentTooLarge(let filename):
            "\(filename) is larger than the 20 MB attachment limit."
        case .totalPayloadTooLarge:
            "The selected attachments are larger than the 50 MB total limit."
        case .invalidFilename:
            "One attachment has an invalid filename."
        }
    }
}

struct CompanionIncomingAttachmentStore {
    static let maximumAttachmentCount = 10
    static let maximumAttachmentBytes = 20 * 1_024 * 1_024
    static let maximumTotalBytes = 50 * 1_024 * 1_024

    private let rootURL: URL
    private let fileManager: FileManager

    init(
        rootURL: URL = CompanionIncomingAttachmentStore.defaultRootURL(),
        fileManager: FileManager = .default
    ) {
        self.rootURL = rootURL
        self.fileManager = fileManager
    }

    func stage(
        _ attachments: [CompanionBridgeAttachment],
        requestID: UUID
    ) throws -> [CodexFollowerAttachment] {
        try Self.validate(attachments)

        guard !attachments.isEmpty else { return [] }
        try fileManager.createDirectory(at: rootURL, withIntermediateDirectories: true)
        try pruneExpiredDirectories()

        let requestDirectory = rootURL.appendingPathComponent(
            requestID.uuidString,
            isDirectory: true
        )
        try fileManager.createDirectory(at: requestDirectory, withIntermediateDirectories: true)

        return try attachments.map { attachment in
            let label = try sanitizedFilename(attachment.filename)
            let storedFilename = "\(attachment.id.uuidString)-\(label)"
            let fileURL = requestDirectory.appendingPathComponent(
                storedFilename,
                isDirectory: false
            )
            try attachment.data.write(to: fileURL, options: .atomic)
            return CodexFollowerAttachment(
                id: attachment.id,
                kind: attachment.kind,
                label: label,
                path: fileURL.path,
                fsPath: fileURL.path,
                mimeType: attachment.mimeType
            )
        }
    }

    static func validate(_ attachments: [CompanionBridgeAttachment]) throws {
        guard attachments.count <= Self.maximumAttachmentCount else {
            throw CompanionIncomingAttachmentStoreError.tooManyAttachments
        }

        var totalBytes = 0
        for attachment in attachments {
            guard attachment.data.count <= Self.maximumAttachmentBytes else {
                throw CompanionIncomingAttachmentStoreError.attachmentTooLarge(attachment.filename)
            }
            totalBytes += attachment.data.count
            guard totalBytes <= Self.maximumTotalBytes else {
                throw CompanionIncomingAttachmentStoreError.totalPayloadTooLarge
            }
        }
    }

    private func sanitizedFilename(_ filename: String) throws -> String {
        let trimmed = filename.trimmingCharacters(in: .whitespacesAndNewlines)
        let lastComponent = URL(fileURLWithPath: trimmed).lastPathComponent
        guard !lastComponent.isEmpty,
              lastComponent != ".",
              lastComponent != "..",
              !lastComponent.contains("\0")
        else {
            throw CompanionIncomingAttachmentStoreError.invalidFilename
        }
        return lastComponent
    }

    private func pruneExpiredDirectories(now: Date = Date()) throws {
        let expirationDate = now.addingTimeInterval(-7 * 24 * 60 * 60)
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .contentModificationDateKey]
        for candidate in try fileManager.contentsOfDirectory(
            at: rootURL,
            includingPropertiesForKeys: Array(keys),
            options: [.skipsHiddenFiles]
        ) {
            let values = try candidate.resourceValues(forKeys: keys)
            guard values.isDirectory == true,
                  let modifiedAt = values.contentModificationDate,
                  modifiedAt < expirationDate
            else { continue }
            try? fileManager.removeItem(at: candidate)
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
    }
}
