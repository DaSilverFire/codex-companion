import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionIncomingAttachmentStoreTests {
    @Test
    func stagesFilesAndImagesWithNativeComposerMetadata() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-incoming-attachments-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let file = CompanionBridgeAttachment(
            kind: .file,
            filename: "../notes.txt",
            mimeType: "text/plain",
            data: Data("notes".utf8)
        )
        let image = CompanionBridgeAttachment(
            kind: .image,
            filename: "shadow.png",
            mimeType: "image/png",
            data: Data([0x89, 0x50, 0x4E, 0x47])
        )

        let staged = try CompanionIncomingAttachmentStore(rootURL: root).stage(
            [file, image],
            requestID: UUID()
        )

        #expect(staged.map(\.label) == ["notes.txt", "shadow.png"])
        #expect(try Data(contentsOf: URL(fileURLWithPath: staged[0].path)) == file.data)
        #expect(try Data(contentsOf: URL(fileURLWithPath: staged[1].path)) == image.data)
        #expect(staged[0].inputItem == nil)
        #expect(staged[1].inputItem?["type"] as? String == "localImage")
        #expect(staged[1].queuedImageAttachment?["mimeType"] as? String == "image/png")
    }

    @Test
    func rejectsMoreThanTenAttachmentsBeforeWriting() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-incoming-attachment-limit-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let attachments = (0...CompanionIncomingAttachmentStore.maximumAttachmentCount).map { index in
            CompanionBridgeAttachment(
                kind: .file,
                filename: "file-\(index).txt",
                data: Data()
            )
        }

        #expect(throws: CompanionIncomingAttachmentStoreError.tooManyAttachments) {
            _ = try CompanionIncomingAttachmentStore(rootURL: root).stage(
                attachments,
                requestID: UUID()
            )
        }
        #expect(!FileManager.default.fileExists(atPath: root.path))
    }

    @Test
    func assemblesBoundedChunksAndStagesAReferenceWithoutInlineBytes() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-streamed-attachments-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let uploadID = UUID()
        let attachmentID = UUID()
        let store = CompanionIncomingAttachmentUploadStore(rootURL: root)
        let metadata = CompanionBridgeAttachment(
            id: attachmentID,
            kind: .file,
            filename: "archive.zip",
            mimeType: "application/zip",
            data: Data(),
            byteCount: 6,
            uploadID: uploadID
        )

        let started = try await store.begin(
            uploadID: uploadID,
            attachment: metadata,
            deviceID: "phone-1"
        )
        #expect(started == CompanionAttachmentUploadProgress(nextOffset: 0, isComplete: false))

        let first = try await store.append(
            uploadID: uploadID,
            deviceID: "phone-1",
            offset: 0,
            data: Data("abc".utf8)
        )
        #expect(first == CompanionAttachmentUploadProgress(nextOffset: 3, isComplete: false))

        let second = try await store.append(
            uploadID: uploadID,
            deviceID: "phone-1",
            offset: 3,
            data: Data("def".utf8)
        )
        #expect(second == CompanionAttachmentUploadProgress(nextOffset: 6, isComplete: true))

        let staged = try await store.stage(
            [metadata],
            requestID: UUID(),
            deviceID: "phone-1"
        )
        #expect(staged.count == 1)
        #expect(staged[0].label == "archive.zip")
        #expect(try Data(contentsOf: URL(fileURLWithPath: staged[0].path)) == Data("abcdef".utf8))
    }

    @Test
    func rejectsOutOfOrderChunksWithoutCorruptingTheUpload() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-streamed-offset-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let uploadID = UUID()
        let store = CompanionIncomingAttachmentUploadStore(rootURL: root)
        let metadata = CompanionBridgeAttachment(
            kind: .file,
            filename: "notes.txt",
            data: Data(),
            byteCount: 5,
            uploadID: uploadID
        )
        _ = try await store.begin(
            uploadID: uploadID,
            attachment: metadata,
            deviceID: "phone-1"
        )

        await #expect(throws: CompanionIncomingAttachmentUploadError.invalidOffset) {
            _ = try await store.append(
                uploadID: uploadID,
                deviceID: "phone-1",
                offset: 2,
                data: Data("no".utf8)
            )
        }

        let accepted = try await store.append(
            uploadID: uploadID,
            deviceID: "phone-1",
            offset: 0,
            data: Data("hello".utf8)
        )
        #expect(accepted.isComplete)
    }

    @Test
    func acceptsTheRelaySafeLargeTransferChunkBudget() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-streamed-chunk-budget-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let uploadID = UUID()
        let store = CompanionIncomingAttachmentUploadStore(rootURL: root)
        let expectedChunkBytes = 393_216
        let metadata = CompanionBridgeAttachment(
            kind: .file,
            filename: "large-transfer.bin",
            data: Data(),
            byteCount: Int64(expectedChunkBytes),
            uploadID: uploadID
        )
        _ = try await store.begin(
            uploadID: uploadID,
            attachment: metadata,
            deviceID: "phone-1"
        )

        let progress = try await store.append(
            uploadID: uploadID,
            deviceID: "phone-1",
            offset: 0,
            data: Data(repeating: 0xA5, count: expectedChunkBytes)
        )

        #expect(progress.isComplete)
        #expect(progress.nextOffset == Int64(expectedChunkBytes))
    }

    @Test
    func rejectsFilesBeyondTwoGigabytesFromMetadataAlone() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-streamed-limit-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let uploadID = UUID()
        let store = CompanionIncomingAttachmentUploadStore(rootURL: root)
        let metadata = CompanionBridgeAttachment(
            kind: .file,
            filename: "too-large.bin",
            data: Data(),
            byteCount: CompanionIncomingAttachmentUploadStore.maximumFileBytes + 1,
            uploadID: uploadID
        )

        await #expect(throws: CompanionIncomingAttachmentUploadError.attachmentTooLarge("too-large.bin")) {
            _ = try await store.begin(
                uploadID: uploadID,
                attachment: metadata,
                deviceID: "phone-1"
            )
        }
        #expect(!FileManager.default.fileExists(atPath: root.path))
    }

    @Test
    func rejectsDuplicateUploadReferencesInsteadOfCrashing() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("companion-streamed-duplicate-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let uploadID = UUID()
        let store = CompanionIncomingAttachmentUploadStore(rootURL: root)
        let metadata = CompanionBridgeAttachment(
            kind: .file,
            filename: "notes.txt",
            data: Data(),
            byteCount: 0,
            uploadID: uploadID
        )
        _ = try await store.begin(
            uploadID: uploadID,
            attachment: metadata,
            deviceID: "phone-1"
        )

        await #expect(throws: CompanionIncomingAttachmentUploadError.invalidUpload) {
            _ = try await store.stage(
                [metadata, metadata],
                requestID: UUID(),
                deviceID: "phone-1"
            )
        }
    }
}
