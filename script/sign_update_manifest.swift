#!/usr/bin/env swift

import CryptoKit
import Foundation

struct UpdateManifest: Codable {
    let schemaVersion: Int
    let version: String
    let build: Int
    let minimumSystemVersion: String
    let publishedAt: String
    let downloadURL: String
    let sha256: String
    let size: Int64
    var signature: String

    func canonicalPayload() -> Data {
        Data(
            [
                String(schemaVersion),
                version,
                String(build),
                minimumSystemVersion,
                publishedAt,
                downloadURL,
                sha256.lowercased(),
                String(size),
            ]
            .joined(separator: "\n")
            .utf8
        )
    }
}

enum ManifestSigningError: Error, LocalizedError {
    case missingArgument(String)
    case invalidArgument(String)
    case missingPrivateKey
    case invalidPrivateKey
    case publicKeyMismatch

    var errorDescription: String? {
        switch self {
        case let .missingArgument(argument):
            "Missing required argument: \(argument)"
        case let .invalidArgument(argument):
            "Invalid argument: \(argument)"
        case .missingPrivateKey:
            "CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64 is not set."
        case .invalidPrivateKey:
            "The update private key must be a base64-encoded 32-byte Curve25519 signing key."
        case .publicKeyMismatch:
            "CODEX_COMPANION_UPDATE_PUBLIC_KEY does not match the private signing key."
        }
    }
}

func arguments() throws -> [String: String] {
    var result: [String: String] = [:]
    var index = 1
    while index < CommandLine.arguments.count {
        let key = CommandLine.arguments[index]
        guard key.hasPrefix("--"), index + 1 < CommandLine.arguments.count else {
            throw ManifestSigningError.invalidArgument(key)
        }
        result[key] = CommandLine.arguments[index + 1]
        index += 2
    }
    return result
}

func required(_ key: String, from values: [String: String]) throws -> String {
    guard let value = values[key], !value.isEmpty else {
        throw ManifestSigningError.missingArgument(key)
    }
    return value
}

do {
    let values = try arguments()
    let version = try required("--version", from: values)
    let buildString = try required("--build", from: values)
    let minimumSystemVersion = try required("--minimum-system-version", from: values)
    let publishedAt = try required("--published-at", from: values)
    let downloadURL = try required("--download-url", from: values)
    let sha256 = try required("--sha256", from: values).lowercased()
    let sizeString = try required("--size", from: values)
    let outputPath = try required("--output", from: values)

    guard let build = Int(buildString), build > 0 else {
        throw ManifestSigningError.invalidArgument("--build")
    }
    guard let size = Int64(sizeString), size > 0 else {
        throw ManifestSigningError.invalidArgument("--size")
    }
    guard URL(string: downloadURL)?.scheme == "https" else {
        throw ManifestSigningError.invalidArgument("--download-url")
    }
    guard sha256.count == 64, sha256.allSatisfy(\.isHexDigit) else {
        throw ManifestSigningError.invalidArgument("--sha256")
    }
    guard let privateKeyBase64 = ProcessInfo.processInfo.environment["CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64"] else {
        throw ManifestSigningError.missingPrivateKey
    }
    guard let privateKeyData = Data(base64Encoded: privateKeyBase64),
          let privateKey = try? Curve25519.Signing.PrivateKey(rawRepresentation: privateKeyData)
    else {
        throw ManifestSigningError.invalidPrivateKey
    }

    if let expectedPublicKey = ProcessInfo.processInfo.environment["CODEX_COMPANION_UPDATE_PUBLIC_KEY"],
       !expectedPublicKey.isEmpty,
       expectedPublicKey != privateKey.publicKey.rawRepresentation.base64EncodedString()
    {
        throw ManifestSigningError.publicKeyMismatch
    }

    var manifest = UpdateManifest(
        schemaVersion: 1,
        version: version,
        build: build,
        minimumSystemVersion: minimumSystemVersion,
        publishedAt: publishedAt,
        downloadURL: downloadURL,
        sha256: sha256,
        size: size,
        signature: ""
    )
    manifest.signature = try privateKey.signature(for: manifest.canonicalPayload()).base64EncodedString()

    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
    var encoded = try encoder.encode(manifest)
    encoded.append(0x0A)
    try encoded.write(to: URL(fileURLWithPath: outputPath), options: .atomic)
    print("Wrote signed update manifest to \(outputPath)")
} catch {
    FileHandle.standardError.write(Data("Update manifest signing failed: \(error.localizedDescription)\n".utf8))
    exit(1)
}
