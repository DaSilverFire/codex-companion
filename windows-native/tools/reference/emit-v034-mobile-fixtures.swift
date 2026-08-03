import CryptoKit
import Foundation

private let sourceCommit =
    "35b9ded3a96e9b4fd2787cbd7ef1e8859264ff1b"

private struct EnvelopeHeader: Codable {
    let version: Int
    let channelID: String
    let senderID: String
    let sequence: UInt64
    let sentAtMilliseconds: Int64
}

private struct FixturePayload: Codable {
    let instant: Date
    let message: String
}

private struct EncodedBytes: Codable {
    let hex: String
    let base64: String

    init(_ data: Data) {
        hex = data.lowercaseHex
        base64 = data.base64EncodedString()
    }
}

private struct RelayFixture: Codable {
    let sourceCommit: String
    let fixedInputSHA256: String
    let channelID: String
    let headerUTF8: String
    let header: EncodedBytes
    let payloadUTF8: String
    let payload: EncodedBytes
    let nonce: EncodedBytes
    let ciphertext: EncodedBytes
    let tag: EncodedBytes
    let combined: EncodedBytes
}

private enum FixtureError: Error, CustomStringConvertible {
    case invalidArguments
    case invalidExistingFixture
    case fixedInputsChanged

    var description: String {
        switch self {
        case .invalidArguments:
            return "usage: swift emit-v034-mobile-fixtures.swift OUTPUT.json"
        case .invalidExistingFixture:
            return "the existing fixture does not contain fixedInputSHA256"
        case .fixedInputsChanged:
            return "refusing to overwrite a fixture generated from different fixed inputs"
        }
    }
}

private extension Data {
    var lowercaseHex: String {
        map { String(format: "%02x", $0) }.joined()
    }

    var base64URL: String {
        base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

private func sortedEncoder(prettyPrinted: Bool = false) -> JSONEncoder {
    let encoder = JSONEncoder()
    encoder.outputFormatting = prettyPrinted
        ? [.prettyPrinted, .sortedKeys]
        : [.sortedKeys]
    return encoder
}

private func makeFixture() throws -> RelayFixture {
    let secret = Data((0..<32).map(UInt8.init))
    let senderID = "iphone-fixture"
    let sequence: UInt64 = 42
    let sentAtMilliseconds: Int64 = 1_700_000_000_123
    let nonceData = Data((0..<12).map(UInt8.init))
    let payload = try sortedEncoder().encode(
        FixturePayload(
            instant: Date(
                timeIntervalSinceReferenceDate: 0
            ),
            message: "v0.3.4"
        )
    )

    let channelDigest = HMAC<SHA256>.authenticationCode(
        for: Data("codex-companion-relay-channel-v1".utf8),
        using: SymmetricKey(data: secret)
    )
    let channelID = Data(channelDigest.prefix(24)).base64URL
    let header = try sortedEncoder().encode(
        EnvelopeHeader(
            version: 1,
            channelID: channelID,
            senderID: senderID,
            sequence: sequence,
            sentAtMilliseconds: sentAtMilliseconds
        )
    )

    let nonce = try ChaChaPoly.Nonce(data: nonceData)
    let sealed = try ChaChaPoly.seal(
        payload,
        using: SymmetricKey(data: secret),
        nonce: nonce,
        authenticating: header
    )

    let descriptor =
        "sourceCommit=\(sourceCommit);"
        + "secret=\(secret.lowercaseHex);"
        + "senderID=\(senderID);"
        + "sequence=\(sequence);"
        + "sentAtMilliseconds=\(sentAtMilliseconds);"
        + "nonce=\(nonceData.lowercaseHex);"
        + "payload=\(String(decoding: payload, as: UTF8.self))"
    let inputHash = Data(
        SHA256.hash(data: Data(descriptor.utf8))
    ).lowercaseHex

    return RelayFixture(
        sourceCommit: sourceCommit,
        fixedInputSHA256: inputHash,
        channelID: channelID,
        headerUTF8: String(decoding: header, as: UTF8.self),
        header: EncodedBytes(header),
        payloadUTF8: String(decoding: payload, as: UTF8.self),
        payload: EncodedBytes(payload),
        nonce: EncodedBytes(nonceData),
        ciphertext: EncodedBytes(sealed.ciphertext),
        tag: EncodedBytes(sealed.tag),
        combined: EncodedBytes(sealed.combined)
    )
}

private func writeFixture(_ fixture: RelayFixture, to outputURL: URL) throws {
    var output = try sortedEncoder(prettyPrinted: true).encode(fixture)
    output.append(0x0A)

    if FileManager.default.fileExists(atPath: outputURL.path) {
        let existing = try Data(contentsOf: outputURL)
        let json = try JSONSerialization.jsonObject(with: existing)
        guard let object = json as? [String: Any],
              let existingHash = object["fixedInputSHA256"] as? String
        else {
            throw FixtureError.invalidExistingFixture
        }
        guard existingHash == fixture.fixedInputSHA256 else {
            throw FixtureError.fixedInputsChanged
        }
        if existing == output {
            return
        }
    }

    try FileManager.default.createDirectory(
        at: outputURL.deletingLastPathComponent(),
        withIntermediateDirectories: true
    )
    try output.write(to: outputURL, options: .atomic)
}

do {
    guard CommandLine.arguments.count == 2 else {
        throw FixtureError.invalidArguments
    }
    let outputURL = URL(
        fileURLWithPath: CommandLine.arguments[1]
    )
    try writeFixture(makeFixture(), to: outputURL)
} catch {
    FileHandle.standardError.write(
        Data("error: \(error)\n".utf8)
    )
    exit(1)
}
