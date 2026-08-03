import CoreGraphics
import CryptoKit
import Foundation
import ImageIO
import UniformTypeIdentifiers

struct CompanionPresencePetSnapshot: Sendable {
    var pets: [PetDefinition]
    var selectedPetID: String?
}

struct CompanionPresencePetCatalogPresentation: Equatable, Sendable {
    var selectedDesktopPetID: String?
    var catalog: [CompanionPresencePetCatalogEntry]
}

enum CompanionPresencePetCatalogError: Error, Equatable, LocalizedError {
    case unknownPackage
    case staleContentHash
    case invalidFileName
    case invalidRange
    case unsafePath
    case invalidPackage

    var errorDescription: String? {
        switch self {
        case .unknownPackage:
            "That Companion pet package is not available on this Mac."
        case .staleContentHash:
            "The Companion pet package changed. Refresh the pet catalog and try again."
        case .invalidFileName:
            "That file is not part of the Companion pet package."
        case .invalidRange:
            "The requested Companion pet file range is invalid."
        case .unsafePath:
            "The Companion pet package path is unsafe."
        case .invalidPackage:
            "The Companion pet package failed validation."
        }
    }
}

actor CompanionPresencePetCatalogService {
    static let maximumChunkLength = 196_608
    static let maximumPackageBytes = 8 * 1024 * 1024

    typealias SnapshotProvider = @Sendable () -> CompanionPresencePetSnapshot

    private struct IndexedPackage: Sendable {
        var manifest: CompanionPresencePetManifest
        var packageDirectory: URL
        var files: [String: URL]
        var byteCount: Int
    }

    private let snapshotProvider: SnapshotProvider
    private var packagesByID: [String: IndexedPackage] = [:]

    init(snapshotProvider: @escaping SnapshotProvider = {
        let store = PetStore()
        return CompanionPresencePetSnapshot(
            pets: store.pets,
            selectedPetID: store.selectedPet?.id
        )
    }) {
        self.snapshotProvider = snapshotProvider
    }

    func refresh() -> CompanionPresencePetCatalogPresentation {
        let snapshot = snapshotProvider()
        var indexed: [IndexedPackage] = []
        for pet in snapshot.pets where pet.mobilePresence != nil {
            do {
                indexed.append(try Self.validatePackage(for: pet))
            } catch {
                CodexSendLog.append(
                    "presence pet rejected id=\(pet.id) reason=\(error.localizedDescription)"
                )
            }
        }
        indexed.sort { lhs, rhs in
            let nameOrder = lhs.manifest.displayName.localizedCaseInsensitiveCompare(
                rhs.manifest.displayName
            )
            if nameOrder != .orderedSame {
                return nameOrder == .orderedAscending
            }
            return lhs.manifest.packageID < rhs.manifest.packageID
        }

        packagesByID.removeAll(keepingCapacity: true)
        for package in indexed where packagesByID[package.manifest.packageID] == nil {
            packagesByID[package.manifest.packageID] = package
        }
        let catalog = indexed.compactMap { package -> CompanionPresencePetCatalogEntry? in
            guard packagesByID[package.manifest.packageID]?.manifest.contentHash
                    == package.manifest.contentHash
            else {
                return nil
            }
            return CompanionPresencePetCatalogEntry(
                packageID: package.manifest.packageID,
                petID: package.manifest.petID,
                displayName: package.manifest.displayName,
                assetVersion: package.manifest.assetVersion,
                contentHash: package.manifest.contentHash,
                byteCount: package.byteCount,
                thumbnail: package.manifest.thumbnail
            )
        }
        return CompanionPresencePetCatalogPresentation(
            selectedDesktopPetID: snapshot.selectedPetID.map(Self.unqualifiedPetID),
            catalog: catalog
        )
    }

    func manifest(
        packageID: String,
        contentHash: String
    ) throws -> CompanionPresencePetManifest {
        let package = try indexedPackage(packageID: packageID, contentHash: contentHash)
        return package.manifest
    }

    func chunk(
        packageID: String,
        contentHash: String,
        fileName: String,
        offset: Int,
        requestedLength: Int
    ) throws -> CompanionPresencePetChunk {
        let package = try indexedPackage(packageID: packageID, contentHash: contentHash)
        guard let indexedURL = package.files[fileName] else {
            throw CompanionPresencePetCatalogError.invalidFileName
        }
        guard requestedLength > 0, offset >= 0 else {
            throw CompanionPresencePetCatalogError.invalidRange
        }
        let fileURL = try Self.safeRegularFile(
            named: fileName,
            in: package.packageDirectory
        )
        guard fileURL == indexedURL else {
            throw CompanionPresencePetCatalogError.unsafePath
        }
        let values = try fileURL.resourceValues(forKeys: [.fileSizeKey])
        guard let fileSize = values.fileSize, offset <= fileSize else {
            throw CompanionPresencePetCatalogError.invalidRange
        }

        let length = min(Self.maximumChunkLength, requestedLength)
        let handle = try FileHandle(forReadingFrom: fileURL)
        defer { try? handle.close() }
        try handle.seek(toOffset: UInt64(offset))
        let data = try handle.read(upToCount: min(length, fileSize - offset)) ?? Data()
        let nextOffset = offset + data.count
        return CompanionPresencePetChunk(
            packageID: packageID,
            contentHash: contentHash,
            fileName: fileName,
            offset: offset,
            data: data,
            nextOffset: nextOffset,
            isComplete: nextOffset >= fileSize
        )
    }

    private func indexedPackage(
        packageID: String,
        contentHash: String
    ) throws -> IndexedPackage {
        guard let package = packagesByID[packageID] else {
            throw CompanionPresencePetCatalogError.unknownPackage
        }
        guard package.manifest.contentHash == contentHash else {
            throw CompanionPresencePetCatalogError.staleContentHash
        }
        return package
    }

    private static func validatePackage(for pet: PetDefinition) throws -> IndexedPackage {
        guard let metadata = pet.mobilePresence else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }
        let petDirectory: URL
        switch pet.source {
        case .custom(let directory):
            petDirectory = directory
        case .builtIn(let spritesheet):
            petDirectory = spritesheet.deletingLastPathComponent()
        }
        let resolvedPetDirectory = petDirectory.resolvingSymlinksInPath().standardizedFileURL
        let packageDirectory = metadata.directoryURL
            .resolvingSymlinksInPath()
            .standardizedFileURL
        guard contains(packageDirectory, in: resolvedPetDirectory) else {
            throw CompanionPresencePetCatalogError.unsafePath
        }

        let manifestURL = try safeRegularFile(named: "manifest.json", in: packageDirectory)
        let manifestData = try Data(contentsOf: manifestURL, options: .mappedIfSafe)
        let manifest = try JSONDecoder().decode(
            CompanionPresencePetManifest.self,
            from: manifestData
        )
        guard
            manifest.schemaVersion == 1,
            manifest.packageID == metadata.packageID,
            manifest.contentHash == metadata.contentHash,
            manifest.petID == unqualifiedPetID(pet.id),
            !manifest.displayName.isEmpty,
            !manifest.assetVersion.isEmpty,
            manifest.contentHash == contentHash(for: manifestData)
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }

        try validateGeometry(manifest)
        let atlasURL = try validateFile(
            manifest.atlas.file,
            expectedName: "atlas.png",
            in: packageDirectory
        )
        let thumbnailURL = try validateFile(
            manifest.thumbnail,
            expectedName: "thumbnail.png",
            in: packageDirectory
        )
        try validatePNG(
            atlasURL,
            expectedWidth: manifest.atlas.columns * manifest.atlas.cellWidth,
            expectedHeight: manifest.atlas.rows * manifest.atlas.cellHeight
        )
        try validatePNG(
            thumbnailURL,
            expectedWidth: manifest.atlas.cellWidth,
            expectedHeight: manifest.atlas.cellHeight
        )
        let packageBytes = manifestData.count
            + manifest.atlas.file.byteCount
            + manifest.thumbnail.byteCount
        guard packageBytes <= maximumPackageBytes else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }

        return IndexedPackage(
            manifest: manifest,
            packageDirectory: packageDirectory,
            files: [
                manifest.atlas.file.name: atlasURL,
                manifest.thumbnail.name: thumbnailURL,
            ],
            byteCount: packageBytes
        )
    }

    private static func validateGeometry(
        _ manifest: CompanionPresencePetManifest
    ) throws {
        let atlas = manifest.atlas
        guard
            (1...256).contains(atlas.cellWidth),
            (1...256).contains(atlas.cellHeight),
            atlas.columns > 0,
            atlas.rows == CompanionPresencePetState.allCases.count,
            atlas.columns * atlas.cellWidth <= 8_192,
            atlas.rows * atlas.cellHeight <= 8_192,
            manifest.animations.count == CompanionPresencePetState.allCases.count
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }

        var states = Set<CompanionPresencePetState>()
        var rows = Set<Int>()
        var largestFrameCount = 0
        for animation in manifest.animations {
            guard
                states.insert(animation.state).inserted,
                rows.insert(animation.row).inserted,
                (0..<atlas.rows).contains(animation.row),
                (1...min(32, atlas.columns)).contains(animation.frameCount),
                animation.frameDurationsMilliseconds.count == animation.frameCount,
                animation.frameDurationsMilliseconds.allSatisfy({ $0 > 0 }),
                (0..<animation.frameCount).contains(animation.posterFrame)
            else {
                throw CompanionPresencePetCatalogError.invalidPackage
            }
            largestFrameCount = max(largestFrameCount, animation.frameCount)
        }
        guard
            states == Set(CompanionPresencePetState.allCases),
            largestFrameCount == atlas.columns
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }
    }

    private static func validateFile(
        _ record: CompanionPresencePetFile,
        expectedName: String,
        in packageDirectory: URL
    ) throws -> URL {
        guard
            record.name == expectedName,
            record.byteCount >= 0,
            record.sha256.range(of: "^[0-9a-f]{64}$", options: .regularExpression) != nil
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }
        let fileURL = try safeRegularFile(named: expectedName, in: packageDirectory)
        let data = try Data(contentsOf: fileURL, options: .mappedIfSafe)
        guard
            data.count == record.byteCount,
            sha256(data) == record.sha256
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }
        return fileURL
    }

    private static func validatePNG(
        _ url: URL,
        expectedWidth: Int,
        expectedHeight: Int
    ) throws {
        guard
            let source = CGImageSourceCreateWithURL(url as CFURL, nil),
            CGImageSourceGetType(source) as String? == UTType.png.identifier,
            let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
            image.width == expectedWidth,
            image.height == expectedHeight,
            containsTransparentPixel(image)
        else {
            throw CompanionPresencePetCatalogError.invalidPackage
        }
    }

    private static func containsTransparentPixel(_ image: CGImage) -> Bool {
        var pixels = [UInt8](repeating: 0, count: image.width * image.height * 4)
        let drewImage = pixels.withUnsafeMutableBytes { buffer -> Bool in
            guard let context = CGContext(
                data: buffer.baseAddress,
                width: image.width,
                height: image.height,
                bitsPerComponent: 8,
                bytesPerRow: image.width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo.byteOrder32Big.rawValue
                    | CGImageAlphaInfo.premultipliedLast.rawValue
            ) else {
                return false
            }
            context.draw(
                image,
                in: CGRect(x: 0, y: 0, width: image.width, height: image.height)
            )
            return true
        }
        guard drewImage else { return false }
        return stride(from: 3, to: pixels.count, by: 4).contains { pixels[$0] == 0 }
    }

    private static func safeRegularFile(
        named name: String,
        in packageDirectory: URL
    ) throws -> URL {
        guard
            !name.isEmpty,
            !name.contains("/"),
            !name.contains("\\"),
            name != ".",
            name != ".."
        else {
            throw CompanionPresencePetCatalogError.invalidFileName
        }
        let candidate = packageDirectory.appendingPathComponent(name, isDirectory: false)
        let values = try candidate.resourceValues(
            forKeys: [.isRegularFileKey, .isSymbolicLinkKey]
        )
        guard values.isRegularFile == true, values.isSymbolicLink != true else {
            throw CompanionPresencePetCatalogError.unsafePath
        }
        let resolved = candidate.resolvingSymlinksInPath().standardizedFileURL
        guard contains(resolved, in: packageDirectory) else {
            throw CompanionPresencePetCatalogError.unsafePath
        }
        return resolved
    }

    private static func contains(_ child: URL, in parent: URL) -> Bool {
        let parentComponents = parent.standardizedFileURL.pathComponents
        let childComponents = child.standardizedFileURL.pathComponents
        return childComponents.count > parentComponents.count
            && Array(childComponents.prefix(parentComponents.count)) == parentComponents
    }

    private static func contentHash(for manifestData: Data) -> String? {
        guard var object = try? JSONSerialization.jsonObject(with: manifestData) as? [String: Any]
        else {
            return nil
        }
        object.removeValue(forKey: "contentHash")
        guard let canonical = try? JSONSerialization.data(
            withJSONObject: object,
            options: [.sortedKeys, .withoutEscapingSlashes]
        ) else {
            return nil
        }
        return sha256(canonical)
    }

    private static func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    private static func unqualifiedPetID(_ id: String) -> String {
        id.split(separator: ":", maxSplits: 1).last.map(String.init) ?? id
    }
}
