import Foundation

enum CompanionPresencePetState: String, Codable, CaseIterable, Sendable {
    case idle
    case thinking
    case talking
}

struct CompanionPresencePetFile: Codable, Equatable, Sendable {
    var name: String
    var sha256: String
    var byteCount: Int
}

struct CompanionPresencePetAtlas: Codable, Equatable, Sendable {
    var file: CompanionPresencePetFile
    var cellWidth: Int
    var cellHeight: Int
    var columns: Int
    var rows: Int
}

struct CompanionPresencePetAnimation: Codable, Equatable, Sendable {
    var state: CompanionPresencePetState
    var row: Int
    var frameCount: Int
    var frameDurationsMilliseconds: [Int]
    var posterFrame: Int
}

struct CompanionPresencePetManifest: Codable, Equatable, Sendable {
    var schemaVersion: Int
    var packageID: String
    var petID: String
    var displayName: String
    var assetVersion: String
    var atlas: CompanionPresencePetAtlas
    var thumbnail: CompanionPresencePetFile
    var animations: [CompanionPresencePetAnimation]
    var contentHash: String
}

struct CompanionPresencePetCatalogEntry: Codable, Equatable, Identifiable, Sendable {
    var packageID: String
    var petID: String
    var displayName: String
    var assetVersion: String
    var contentHash: String
    var byteCount: Int
    var thumbnail: CompanionPresencePetFile

    var id: String { packageID }
}

struct CompanionPresencePetChunk: Codable, Equatable, Sendable {
    var packageID: String
    var contentHash: String
    var fileName: String
    var offset: Int
    var data: Data
    var nextOffset: Int
    var isComplete: Bool
}
