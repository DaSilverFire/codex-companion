import CryptoKit
import Foundation

struct CompanionUpdateManifest: Codable, Equatable, Sendable {
    let schemaVersion: Int
    let version: String
    var build: Int
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

    func isNewer(thanBuild currentBuild: Int) -> Bool {
        build > currentBuild
    }
}

enum CompanionUpdateValidationError: Error, Equatable, LocalizedError {
    case malformedManifest
    case unsupportedSchema
    case insecureDownloadURL
    case invalidDigest
    case invalidSize
    case invalidPublicKey
    case invalidSignature

    var errorDescription: String? {
        switch self {
        case .malformedManifest:
            "The update manifest could not be decoded."
        case .unsupportedSchema:
            "The update manifest schema is unsupported."
        case .insecureDownloadURL:
            "The update download must use HTTPS."
        case .invalidDigest:
            "The update manifest contains an invalid SHA-256 digest."
        case .invalidSize:
            "The update manifest contains an invalid installer size."
        case .invalidPublicKey:
            "The update signing key is invalid."
        case .invalidSignature:
            "The update manifest signature is invalid."
        }
    }
}

struct CompanionUpdateValidator {
    func validate(
        manifestData: Data,
        publicKeyBase64: String
    ) throws -> CompanionUpdateManifest {
        guard let manifest = try? JSONDecoder().decode(CompanionUpdateManifest.self, from: manifestData) else {
            throw CompanionUpdateValidationError.malformedManifest
        }
        guard manifest.schemaVersion == 1 else {
            throw CompanionUpdateValidationError.unsupportedSchema
        }
        guard let downloadURL = URL(string: manifest.downloadURL), downloadURL.scheme == "https" else {
            throw CompanionUpdateValidationError.insecureDownloadURL
        }
        guard manifest.sha256.count == 64,
              manifest.sha256.allSatisfy({ $0.isHexDigit })
        else {
            throw CompanionUpdateValidationError.invalidDigest
        }
        guard manifest.size > 0 else {
            throw CompanionUpdateValidationError.invalidSize
        }
        guard let publicKeyData = Data(base64Encoded: publicKeyBase64),
              let publicKey = try? Curve25519.Signing.PublicKey(rawRepresentation: publicKeyData)
        else {
            throw CompanionUpdateValidationError.invalidPublicKey
        }
        guard let signature = Data(base64Encoded: manifest.signature),
              publicKey.isValidSignature(signature, for: manifest.canonicalPayload())
        else {
            throw CompanionUpdateValidationError.invalidSignature
        }
        return manifest
    }
}

struct CompanionUpdateConfiguration: Equatable, Sendable {
    static let manifestURLInfoKey = "CodexCompanionUpdateManifestURL"
    static let publicKeyInfoKey = "CodexCompanionUpdatePublicKey"

    let manifestURL: URL?
    let publicKeyBase64: String?
    let currentVersion: String
    let currentBuild: Int

    init(
        manifestURL: URL?,
        publicKeyBase64: String?,
        currentVersion: String,
        currentBuild: Int
    ) {
        self.manifestURL = manifestURL
        self.publicKeyBase64 = publicKeyBase64
        self.currentVersion = currentVersion
        self.currentBuild = currentBuild
    }

    init(bundle: Bundle = .main) {
        let manifestString = bundle.object(forInfoDictionaryKey: Self.manifestURLInfoKey) as? String
        manifestURL = manifestString.flatMap(URL.init(string:))
        publicKeyBase64 = bundle.object(forInfoDictionaryKey: Self.publicKeyInfoKey) as? String
        currentVersion = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
        if let buildString = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String {
            currentBuild = Int(buildString) ?? 0
        } else if let buildNumber = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? NSNumber {
            currentBuild = buildNumber.intValue
        } else {
            currentBuild = 0
        }
    }

    var isConfigured: Bool {
        manifestURL != nil && !(publicKeyBase64?.isEmpty ?? true)
    }
}

enum CompanionUpdateState: Equatable, Sendable {
    case idle
    case checking
    case unavailable(String)
    case upToDate
    case available(CompanionUpdateManifest)
    case failed(String)
}

protocol CompanionUpdateDataLoading: Sendable {
    func data(from url: URL) async throws -> Data
}

private struct URLSessionUpdateDataLoader: CompanionUpdateDataLoading {
    func data(from url: URL) async throws -> Data {
        let (data, response) = try await URLSession.shared.data(from: url)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw URLError(.badServerResponse)
        }
        return data
    }
}

@MainActor
final class CompanionUpdateService: ObservableObject {
    @Published private(set) var state: CompanionUpdateState = .idle

    let configuration: CompanionUpdateConfiguration
    private let loader: any CompanionUpdateDataLoading
    private let validator: CompanionUpdateValidator

    init(
        configuration: CompanionUpdateConfiguration = CompanionUpdateConfiguration(),
        loader: any CompanionUpdateDataLoading = URLSessionUpdateDataLoader(),
        validator: CompanionUpdateValidator = CompanionUpdateValidator()
    ) {
        self.configuration = configuration
        self.loader = loader
        self.validator = validator
    }

    func checkForUpdates() async {
        guard let manifestURL = configuration.manifestURL,
              let publicKey = configuration.publicKeyBase64,
              !publicKey.isEmpty
        else {
            state = .unavailable("Release feed and signing key are not configured.")
            return
        }

        state = .checking
        do {
            let data = try await loader.data(from: manifestURL)
            let manifest = try validator.validate(
                manifestData: data,
                publicKeyBase64: publicKey
            )
            state = manifest.isNewer(thanBuild: configuration.currentBuild)
                ? .available(manifest)
                : .upToDate
        } catch {
            state = .failed(error.localizedDescription)
        }
    }
}
