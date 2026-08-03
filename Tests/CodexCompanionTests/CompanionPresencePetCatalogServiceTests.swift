import CoreGraphics
import CryptoKit
import Foundation
import ImageIO
import Testing
import UniformTypeIdentifiers
@testable import CodexCompanion

@Suite
struct CompanionPresencePetCatalogServiceTests {
    @Test
    func indexesValidPackagesInStableOrderAndAdvertisesSelectedPet() async throws {
        let fixture = try PresencePetFixture()
        defer { fixture.cleanup() }
        let zeta = try fixture.makePet(
            id: "zeta",
            displayName: "Zeta",
            packageID: "zeta-mobile-v1"
        )
        let alpha = try fixture.makePet(
            id: "alpha",
            displayName: "Alpha",
            packageID: "alpha-mobile-v1"
        )
        let service = CompanionPresencePetCatalogService(
            snapshotProvider: {
                CompanionPresencePetSnapshot(
                    pets: [zeta, alpha],
                    selectedPetID: alpha.id
                )
            }
        )

        let presentation = await service.refresh()

        #expect(presentation.selectedDesktopPetID == "alpha")
        #expect(presentation.catalog.map(\.displayName) == ["Alpha", "Zeta"])
        #expect(presentation.catalog.map(\.packageID) == [
            "alpha-mobile-v1",
            "zeta-mobile-v1",
        ])
    }

    @Test
    func rejectsMetadataHashMismatchAndInvalidAtlasGeometry() async throws {
        let fixture = try PresencePetFixture()
        defer { fixture.cleanup() }
        var wrongHash = try fixture.makePet(id: "wrong-hash", displayName: "Wrong Hash")
        wrongHash.mobilePresence?.contentHash = String(repeating: "f", count: 64)
        let rejectedHashPet = wrongHash
        let wrongGeometry = try fixture.makePet(
            id: "wrong-geometry",
            displayName: "Wrong Geometry",
            mutateManifest: { manifest in
                var atlas = try #require(manifest["atlas"] as? [String: Any])
                atlas["columns"] = 13
                manifest["atlas"] = atlas
            }
        )
        let service = CompanionPresencePetCatalogService(
            snapshotProvider: {
                CompanionPresencePetSnapshot(
                    pets: [rejectedHashPet, wrongGeometry],
                    selectedPetID: rejectedHashPet.id
                )
            }
        )

        let presentation = await service.refresh()

        #expect(presentation.catalog.isEmpty)
        #expect(presentation.selectedDesktopPetID == "wrong-hash")
    }

    @Test
    func servesExactManifestAndClampsChunksTo192KiB() async throws {
        let fixture = try PresencePetFixture()
        defer { fixture.cleanup() }
        let pet = try fixture.makePet(id: "alpha", displayName: "Alpha")
        let service = fixture.makeService(pets: [pet], selectedPetID: pet.id)
        _ = await service.refresh()
        let metadata = try #require(pet.mobilePresence)

        let manifest = try await service.manifest(
            packageID: metadata.packageID,
            contentHash: metadata.contentHash
        )
        let chunk = try await service.chunk(
            packageID: metadata.packageID,
            contentHash: metadata.contentHash,
            fileName: "atlas.png",
            offset: 0,
            requestedLength: Int.max
        )

        #expect(manifest.packageID == metadata.packageID)
        #expect(manifest.contentHash == metadata.contentHash)
        #expect(chunk.data.count == 196_608)
        #expect(chunk.nextOffset == 196_608)
        #expect(!chunk.isComplete)
    }

    @Test
    func rejectsStaleHashesInvalidOffsetsTraversalAndSymlinkSwaps() async throws {
        let fixture = try PresencePetFixture()
        defer { fixture.cleanup() }
        let pet = try fixture.makePet(id: "alpha", displayName: "Alpha")
        let service = fixture.makeService(pets: [pet], selectedPetID: pet.id)
        _ = await service.refresh()
        let metadata = try #require(pet.mobilePresence)

        await expectCatalogError(.staleContentHash) {
            _ = try await service.manifest(
                packageID: metadata.packageID,
                contentHash: String(repeating: "0", count: 64)
            )
        }
        await expectCatalogError(.invalidRange) {
            _ = try await service.chunk(
                packageID: metadata.packageID,
                contentHash: metadata.contentHash,
                fileName: "atlas.png",
                offset: Int.max,
                requestedLength: 1
            )
        }
        await expectCatalogError(.invalidFileName) {
            _ = try await service.chunk(
                packageID: metadata.packageID,
                contentHash: metadata.contentHash,
                fileName: "../atlas.png",
                offset: 0,
                requestedLength: 1
            )
        }

        let atlas = metadata.directoryURL.appendingPathComponent("atlas.png")
        let escaped = fixture.root.appendingPathComponent("escaped-atlas.png")
        try FileManager.default.moveItem(at: atlas, to: escaped)
        try FileManager.default.createSymbolicLink(at: atlas, withDestinationURL: escaped)
        await expectCatalogError(.unsafePath) {
            _ = try await service.chunk(
                packageID: metadata.packageID,
                contentHash: metadata.contentHash,
                fileName: "atlas.png",
                offset: 0,
                requestedLength: 1
            )
        }
    }

    @Test
    func bridgeAdvertisesCatalogAndServesManifestAndChunkOperations() async throws {
        let fixture = try PresencePetFixture()
        defer { fixture.cleanup() }
        let pet = try fixture.makePet(id: "alpha", displayName: "Alpha")
        let service = fixture.makeService(pets: [pet], selectedPetID: pet.id)
        let server = CodexCompanionMobileBridgeServer(
            presencePetCatalogService: service
        )
        let metadata = try #require(pet.mobilePresence)

        let handshake = await server.handle(
            CompanionBridgeRequest(operation: .handshake)
        )
        let manifest = await server.handle(
            CompanionBridgeRequest(
                operation: .loadPresencePetManifest,
                presencePetPackageID: metadata.packageID,
                presencePetContentHash: metadata.contentHash
            )
        )
        let chunk = await server.handle(
            CompanionBridgeRequest(
                operation: .loadPresencePetChunk,
                presencePetPackageID: metadata.packageID,
                presencePetContentHash: metadata.contentHash,
                presencePetFileName: "thumbnail.png",
                presencePetOffset: 0,
                presencePetLength: Int.max
            )
        )

        #expect(handshake.succeeded)
        #expect(handshake.features?.contains(.presencePetPackageV1) == true)
        #expect(handshake.selectedDesktopPetID == "alpha")
        #expect(handshake.presencePetCatalog?.count == 1)
        #expect(manifest.presencePetManifest?.packageID == metadata.packageID)
        #expect(chunk.presencePetChunk?.fileName == "thumbnail.png")
        #expect(chunk.presencePetChunk?.isComplete == true)
    }
}

private func expectCatalogError(
    _ expected: CompanionPresencePetCatalogError,
    operation: () async throws -> Void
) async {
    do {
        try await operation()
        Issue.record("Expected \(expected)")
    } catch let error as CompanionPresencePetCatalogError {
        #expect(error == expected)
    } catch {
        Issue.record("Unexpected error: \(error)")
    }
}

private struct PresencePetFixture {
    let root: URL

    init() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("PresencePetFixture-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    func cleanup() {
        try? FileManager.default.removeItem(at: root)
    }

    func makePet(
        id: String,
        displayName: String,
        packageID: String? = nil,
        mutateManifest: ((inout [String: Any]) throws -> Void)? = nil
    ) throws -> PetDefinition {
        let petDirectory = root.appendingPathComponent(id, isDirectory: true)
        let packageDirectory = petDirectory.appendingPathComponent("mobile-presence", isDirectory: true)
        try FileManager.default.createDirectory(
            at: packageDirectory,
            withIntermediateDirectories: true
        )
        try Self.writeFixturePackage(
            to: packageDirectory,
            petID: id,
            displayName: displayName,
            packageID: packageID ?? "\(id)-mobile-v1"
        )

        let manifestURL = packageDirectory.appendingPathComponent("manifest.json")
        var manifest = try #require(
            JSONSerialization.jsonObject(with: Data(contentsOf: manifestURL)) as? [String: Any]
        )
        manifest["petID"] = id
        manifest["displayName"] = displayName
        manifest["packageID"] = packageID ?? "\(id)-mobile-v1"
        try mutateManifest?(&manifest)
        manifest.removeValue(forKey: "contentHash")
        let canonical = try JSONSerialization.data(
            withJSONObject: manifest,
            options: [.sortedKeys, .withoutEscapingSlashes]
        )
        let contentHash = SHA256.hash(data: canonical).map { String(format: "%02x", $0) }.joined()
        manifest["contentHash"] = contentHash
        try JSONSerialization.data(
            withJSONObject: manifest,
            options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        ).write(to: manifestURL)

        return PetDefinition(
            id: "custom:\(id)",
            displayName: displayName,
            description: "Test pet",
            spritesheetURL: petDirectory.appendingPathComponent("spritesheet.webp"),
            spriteColumns: 16,
            spriteRows: 12,
            animationFrameCounts: [:],
            mobilePresence: PetDefinition.MobilePresence(
                directoryURL: packageDirectory,
                packageID: manifest["packageID"] as! String,
                contentHash: contentHash
            ),
            source: .custom(petDirectory)
        )
    }

    func makeService(
        pets: [PetDefinition],
        selectedPetID: String?
    ) -> CompanionPresencePetCatalogService {
        CompanionPresencePetCatalogService(
            snapshotProvider: {
                CompanionPresencePetSnapshot(
                    pets: pets,
                    selectedPetID: selectedPetID
                )
            }
        )
    }

    private static func writeFixturePackage(
        to directory: URL,
        petID: String,
        displayName: String,
        packageID: String
    ) throws {
        let cellWidth = 256
        let cellHeight = 256
        let columns = 4
        let rows = CompanionPresencePetState.allCases.count
        let atlasURL = directory.appendingPathComponent("atlas.png")
        let thumbnailURL = directory.appendingPathComponent("thumbnail.png")
        try writeNoisePNG(
            to: atlasURL,
            width: cellWidth * columns,
            height: cellHeight * rows
        )
        try writeNoisePNG(
            to: thumbnailURL,
            width: cellWidth,
            height: cellHeight,
            noisy: false
        )

        let atlasData = try Data(contentsOf: atlasURL)
        let thumbnailData = try Data(contentsOf: thumbnailURL)
        let fileRecord: (String, Data) -> [String: Any] = { name, data in
            [
                "name": name,
                "sha256": SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined(),
                "byteCount": data.count,
            ]
        }
        var manifest: [String: Any] = [
            "schemaVersion": 1,
            "packageID": packageID,
            "petID": petID,
            "displayName": displayName,
            "assetVersion": "test-1",
            "atlas": [
                "file": fileRecord("atlas.png", atlasData),
                "cellWidth": cellWidth,
                "cellHeight": cellHeight,
                "columns": columns,
                "rows": rows,
            ],
            "thumbnail": fileRecord("thumbnail.png", thumbnailData),
            "animations": CompanionPresencePetState.allCases.enumerated().map { index, state in
                [
                    "state": state.rawValue,
                    "row": index,
                    "frameCount": columns,
                    "frameDurationsMilliseconds": Array(repeating: 120, count: columns),
                    "posterFrame": 0,
                ] as [String: Any]
            },
        ]
        let canonical = try JSONSerialization.data(
            withJSONObject: manifest,
            options: [.sortedKeys, .withoutEscapingSlashes]
        )
        manifest["contentHash"] = SHA256.hash(data: canonical)
            .map { String(format: "%02x", $0) }
            .joined()
        let manifestData = try JSONSerialization.data(
            withJSONObject: manifest,
            options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        )
        try manifestData.write(to: directory.appendingPathComponent("manifest.json"))
    }

    private static func writeNoisePNG(
        to url: URL,
        width: Int,
        height: Int,
        noisy: Bool = true
    ) throws {
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        var state: UInt32 = 0xC0DE_CAFE
        for index in 0..<(width * height) where noisy {
            let offset = index * 4
            state ^= state << 13
            state ^= state >> 17
            state ^= state << 5
            pixels[offset] = UInt8(truncatingIfNeeded: state)
            pixels[offset + 1] = UInt8(truncatingIfNeeded: state >> 8)
            pixels[offset + 2] = UInt8(truncatingIfNeeded: state >> 16)
            pixels[offset + 3] = index == 0 ? 0 : 255
        }

        let data = Data(pixels)
        guard
            let provider = CGDataProvider(data: data as CFData),
            let image = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                provider: provider,
                decode: nil,
                shouldInterpolate: false,
                intent: .defaultIntent
            ),
            let destination = CGImageDestinationCreateWithURL(
                url as CFURL,
                UTType.png.identifier as CFString,
                1,
                nil
            )
        else {
            throw CocoaError(.fileWriteUnknown)
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw CocoaError(.fileWriteUnknown)
        }
    }
}
