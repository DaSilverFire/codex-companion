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
}
