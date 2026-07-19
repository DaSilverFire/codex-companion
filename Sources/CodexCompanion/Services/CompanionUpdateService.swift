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

    func isNewer(thanVersion currentVersion: String, build currentBuild: Int) -> Bool {
        let ordering: ComparisonResult
        if let releaseVersion = CompanionReleaseVersion(version),
           let installedVersion = CompanionReleaseVersion(currentVersion)
        {
            if releaseVersion < installedVersion {
                ordering = .orderedAscending
            } else if releaseVersion > installedVersion {
                ordering = .orderedDescending
            } else {
                ordering = .orderedSame
            }
        } else {
            ordering = version.compare(
                currentVersion,
                options: [.numeric, .caseInsensitive]
            )
        }

        switch ordering {
        case .orderedDescending:
            return true
        case .orderedAscending:
            return false
        case .orderedSame:
            return build > currentBuild
        }
    }
}

private struct CompanionReleaseVersion: Comparable {
    private enum Identifier: Comparable {
        case number(Int)
        case text(String)

        static func < (lhs: Identifier, rhs: Identifier) -> Bool {
            switch (lhs, rhs) {
            case let (.number(left), .number(right)):
                left < right
            case (.number, .text):
                true
            case (.text, .number):
                false
            case let (.text(left), .text(right)):
                left < right
            }
        }
    }

    private let core: [Int]
    private let prerelease: [Identifier]?

    init?(_ value: String) {
        let withoutMetadata = value.split(
            separator: "+",
            maxSplits: 1,
            omittingEmptySubsequences: false
        )[0]
        let versionParts = withoutMetadata.split(
            separator: "-",
            maxSplits: 1,
            omittingEmptySubsequences: false
        )
        let coreParts = versionParts[0].split(
            separator: ".",
            omittingEmptySubsequences: false
        )
        guard coreParts.count >= 2 else { return nil }
        let parsedCore = coreParts.compactMap { Int($0) }
        guard parsedCore.count == coreParts.count else { return nil }
        core = parsedCore

        if versionParts.count == 1 {
            prerelease = nil
        } else {
            let identifiers = versionParts[1].split(
                separator: ".",
                omittingEmptySubsequences: false
            )
            guard !identifiers.isEmpty,
                  identifiers.allSatisfy({ !$0.isEmpty })
            else { return nil }
            prerelease = identifiers.map { identifier in
                if let number = Int(identifier) {
                    return .number(number)
                }
                return .text(String(identifier))
            }
        }
    }

    static func < (lhs: CompanionReleaseVersion, rhs: CompanionReleaseVersion) -> Bool {
        let componentCount = max(lhs.core.count, rhs.core.count)
        for index in 0 ..< componentCount {
            let left = index < lhs.core.count ? lhs.core[index] : 0
            let right = index < rhs.core.count ? rhs.core[index] : 0
            if left != right {
                return left < right
            }
        }

        switch (lhs.prerelease, rhs.prerelease) {
        case (nil, nil):
            return false
        case (.some, nil):
            return true
        case (nil, .some):
            return false
        case let (.some(left), .some(right)):
            for index in 0 ..< min(left.count, right.count) {
                if left[index] != right[index] {
                    return left[index] < right[index]
                }
            }
            return left.count < right.count
        }
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

enum CompanionUpdateArtifactError: Error, Equatable, LocalizedError {
    case insecureDownloadURL
    case invalidFile
    case sizeMismatch
    case digestMismatch

    var errorDescription: String? {
        switch self {
        case .insecureDownloadURL:
            "The update download must use HTTPS."
        case .invalidFile:
            "The downloaded update could not be read."
        case .sizeMismatch:
            "The downloaded update size does not match the signed manifest."
        case .digestMismatch:
            "The downloaded update checksum does not match the signed manifest."
        }
    }
}

enum CompanionUpdateInstallationError: Error, Equatable, LocalizedError {
    case invalidInstaller

    var errorDescription: String? {
        switch self {
        case .invalidInstaller:
            "The verified update is not a Codex Companion disk image."
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

struct CompanionUpdateArtifactValidator {
    func validate(
        fileURL: URL,
        manifest: CompanionUpdateManifest
    ) throws -> URL {
        guard fileURL.isFileURL,
              let data = try? Data(contentsOf: fileURL, options: .mappedIfSafe)
        else {
            throw CompanionUpdateArtifactError.invalidFile
        }
        guard Int64(data.count) == manifest.size else {
            throw CompanionUpdateArtifactError.sizeMismatch
        }

        let digest = SHA256.hash(data: data)
            .map { String(format: "%02x", $0) }
            .joined()
        guard digest.caseInsensitiveCompare(manifest.sha256) == .orderedSame else {
            throw CompanionUpdateArtifactError.digestMismatch
        }
        return fileURL
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
    case downloading(CompanionUpdateManifest)
    case readyToInstall(CompanionUpdateManifest, URL)
    case installing(CompanionUpdateManifest)
    case failed(String)
}

protocol CompanionUpdateDataLoading: Sendable {
    func data(from url: URL) async throws -> Data
}

protocol CompanionUpdateArtifactDownloading: Sendable {
    func download(from url: URL) async throws -> URL
}

@MainActor
protocol CompanionUpdateInstalling {
    func beginInstallation(
        fileURL: URL,
        manifest: CompanionUpdateManifest
    ) throws
}

struct CompanionUpdateInstaller: CompanionUpdateInstalling {
    static let helperScript = #"""
    #!/bin/bash
    set -euo pipefail

    DMG_PATH="$1"
    EXPECTED_SHA256="$2"
    EXPECTED_SIZE="$3"
    MOUNT_DIR="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/codex-companion-update-mount.XXXXXX")"

    cleanup() {
      /usr/bin/hdiutil detach "$MOUNT_DIR" -quiet >/dev/null 2>&1 || true
      /bin/rmdir "$MOUNT_DIR" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT INT TERM

    [[ -f "$DMG_PATH" ]] || { echo "Verified update is missing." >&2; exit 1; }
    ACTUAL_SIZE="$(/usr/bin/stat -f '%z' "$DMG_PATH")"
    [[ "$ACTUAL_SIZE" == "$EXPECTED_SIZE" ]] || { echo "Verified update size changed." >&2; exit 1; }
    ACTUAL_SHA256="$(/usr/bin/shasum -a 256 "$DMG_PATH" | /usr/bin/awk '{print $1}')"
    [[ "$ACTUAL_SHA256" == "$EXPECTED_SHA256" ]] || { echo "Verified update checksum changed." >&2; exit 1; }

    /usr/bin/hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_DIR" "$DMG_PATH" -quiet
    INSTALLER="$MOUNT_DIR/Install Codex Companion.command"
    [[ -x "$INSTALLER" ]] || { echo "Release installer is missing." >&2; exit 1; }
    if [[ -x "$MOUNT_DIR/.install_release.sh" ]]; then
      INSTALLER="$MOUNT_DIR/.install_release.sh"
    fi

    # The signed update manifest authenticates ad-hoc release payloads. The
    # installer still validates the app's code signature before replacement.
    # Older release images wrapped this transactional script in an interactive
    # first-install warning, which is intentionally bypassed only here.
    CODEX_COMPANION_ALLOW_ADHOC=1 \
      CODEX_COMPANION_INSTALL_RELAUNCH_AGENT="${CODEX_COMPANION_INSTALL_RELAUNCH_AGENT:-1}" \
      /bin/bash "$INSTALLER"
    """#

    private let fileManager: FileManager

    init(fileManager: FileManager = .default) {
        self.fileManager = fileManager
    }

    func beginInstallation(
        fileURL: URL,
        manifest: CompanionUpdateManifest
    ) throws {
        guard fileURL.isFileURL,
              fileURL.pathExtension.caseInsensitiveCompare("dmg") == .orderedSame
        else {
            throw CompanionUpdateInstallationError.invalidInstaller
        }

        let helperDirectory = fileManager.temporaryDirectory
            .appendingPathComponent("CodexCompanionUpdates", isDirectory: true)
            .appendingPathComponent("Install-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: helperDirectory, withIntermediateDirectories: true)
        let helperURL = helperDirectory.appendingPathComponent("install-update.sh", isDirectory: false)
        try Data(Self.helperScript.utf8).write(to: helperURL, options: .atomic)
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: helperURL.path
        )

        let logDirectory = fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/Codex Companion", isDirectory: true)
        try fileManager.createDirectory(at: logDirectory, withIntermediateDirectories: true)
        let logURL = logDirectory.appendingPathComponent("update-install.log", isDirectory: false)
        if !fileManager.fileExists(atPath: logURL.path) {
            fileManager.createFile(atPath: logURL.path, contents: nil)
        }
        let logHandle = try FileHandle(forWritingTo: logURL)
        try logHandle.seekToEnd()

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/nohup")
        process.arguments = [
            "/bin/bash",
            helperURL.path,
            fileURL.path,
            manifest.sha256.lowercased(),
            String(manifest.size),
        ]
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = logHandle
        process.standardError = logHandle
        do {
            try process.run()
            try logHandle.close()
        } catch {
            try? logHandle.close()
            try? fileManager.removeItem(at: helperDirectory)
            throw error
        }
    }
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

private struct URLSessionUpdateArtifactDownloader: CompanionUpdateArtifactDownloading {
    func download(from url: URL) async throws -> URL {
        let (temporaryURL, response) = try await URLSession.shared.download(from: url)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw URLError(.badServerResponse)
        }

        let fileManager = FileManager.default
        let directory = fileManager.temporaryDirectory
            .appendingPathComponent("CodexCompanionUpdates", isDirectory: true)
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
        let fileName = url.lastPathComponent.isEmpty ? "Codex-Companion-Update.dmg" : url.lastPathComponent
        let destination = directory.appendingPathComponent(fileName, isDirectory: false)
        try fileManager.moveItem(at: temporaryURL, to: destination)
        return destination
    }
}

@MainActor
final class CompanionUpdateService: ObservableObject {
    @Published private(set) var state: CompanionUpdateState = .idle

    let configuration: CompanionUpdateConfiguration
    private let loader: any CompanionUpdateDataLoading
    private let validator: CompanionUpdateValidator
    private let artifactDownloader: any CompanionUpdateArtifactDownloading
    private let artifactValidator: CompanionUpdateArtifactValidator
    private let installer: any CompanionUpdateInstalling
    private var downloadedArtifactURL: URL?

    init(
        configuration: CompanionUpdateConfiguration = CompanionUpdateConfiguration(),
        loader: any CompanionUpdateDataLoading = URLSessionUpdateDataLoader(),
        validator: CompanionUpdateValidator = CompanionUpdateValidator(),
        artifactDownloader: any CompanionUpdateArtifactDownloading = URLSessionUpdateArtifactDownloader(),
        artifactValidator: CompanionUpdateArtifactValidator = CompanionUpdateArtifactValidator(),
        installer: (any CompanionUpdateInstalling)? = nil
    ) {
        self.configuration = configuration
        self.loader = loader
        self.validator = validator
        self.artifactDownloader = artifactDownloader
        self.artifactValidator = artifactValidator
        self.installer = installer ?? CompanionUpdateInstaller()
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
            state = manifest.isNewer(
                thanVersion: configuration.currentVersion,
                build: configuration.currentBuild
            )
                ? .available(manifest)
                : .upToDate
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func downloadAvailableUpdate() async {
        guard case let .available(manifest) = state,
              let downloadURL = URL(string: manifest.downloadURL),
              downloadURL.scheme == "https"
        else {
            if case .available = state {
                state = .failed(CompanionUpdateArtifactError.insecureDownloadURL.localizedDescription)
            }
            return
        }

        removeDownloadedArtifact()
        state = .downloading(manifest)
        do {
            let fileURL = try await artifactDownloader.download(from: downloadURL)
            do {
                let verifiedURL = try artifactValidator.validate(
                    fileURL: fileURL,
                    manifest: manifest
                )
                downloadedArtifactURL = verifiedURL
                state = .readyToInstall(manifest, verifiedURL)
            } catch {
                try? FileManager.default.removeItem(at: fileURL.deletingLastPathComponent())
                throw error
            }
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func installReadyUpdate() {
        guard case let .readyToInstall(manifest, fileURL) = state else { return }

        do {
            try installer.beginInstallation(fileURL: fileURL, manifest: manifest)
            state = .installing(manifest)
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    private func removeDownloadedArtifact() {
        guard let downloadedArtifactURL else { return }
        try? FileManager.default.removeItem(at: downloadedArtifactURL.deletingLastPathComponent())
        self.downloadedArtifactURL = nil
    }
}
