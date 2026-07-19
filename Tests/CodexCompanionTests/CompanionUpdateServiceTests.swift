import CryptoKit
import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionUpdateServiceTests {
    @Test
    func validatorAcceptsSignedHTTPSManifest() throws {
        let privateKey = Curve25519.Signing.PrivateKey()
        var manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "1.2.0",
            build: 12,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-14T00:00:00Z",
            downloadURL: "https://github.com/silverfire/codex-companion/releases/download/v1.2.0/Codex-Companion-1.2.0.dmg",
            sha256: String(repeating: "a", count: 64),
            size: 42_000,
            signature: ""
        )
        manifest.signature = try privateKey.signature(for: manifest.canonicalPayload()).base64EncodedString()

        let encoded = try JSONEncoder().encode(manifest)
        let verified = try CompanionUpdateValidator().validate(
            manifestData: encoded,
            publicKeyBase64: privateKey.publicKey.rawRepresentation.base64EncodedString()
        )

        #expect(verified == manifest)
    }

    @Test
    func validatorRejectsTamperedManifest() throws {
        let privateKey = Curve25519.Signing.PrivateKey()
        var manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "1.2.0",
            build: 12,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-14T00:00:00Z",
            downloadURL: "https://github.com/silverfire/codex-companion/releases/download/v1.2.0/Codex-Companion-1.2.0.dmg",
            sha256: String(repeating: "a", count: 64),
            size: 42_000,
            signature: ""
        )
        manifest.signature = try privateKey.signature(for: manifest.canonicalPayload()).base64EncodedString()
        manifest.build = 13

        let encoded = try JSONEncoder().encode(manifest)
        #expect(throws: CompanionUpdateValidationError.invalidSignature) {
            try CompanionUpdateValidator().validate(
                manifestData: encoded,
                publicKeyBase64: privateKey.publicKey.rawRepresentation.base64EncodedString()
            )
        }
    }

    @Test
    func artifactValidatorRejectsChecksumMismatch() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let artifact = directory.appendingPathComponent("Codex-Companion.dmg")
        let data = Data("verified update".utf8)
        try data.write(to: artifact)
        let manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "0.3.4",
            build: 1,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-18T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion.dmg",
            sha256: String(repeating: "0", count: 64),
            size: Int64(data.count),
            signature: "signature"
        )

        #expect(throws: CompanionUpdateArtifactError.digestMismatch) {
            try CompanionUpdateArtifactValidator().validate(
                fileURL: artifact,
                manifest: manifest
            )
        }
    }

    @Test
    func releaseComparisonUsesMonotonicBuildNumber() {
        let manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "1.2.0",
            build: 12,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-14T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion.dmg",
            sha256: String(repeating: "b", count: 64),
            size: 1,
            signature: "signature"
        )

        #expect(manifest.isNewer(thanBuild: 11))
        #expect(!manifest.isNewer(thanBuild: 12))
        #expect(!manifest.isNewer(thanBuild: 13))
    }

    @Test
    func releaseComparisonUsesVersionBeforeBuildNumber() {
        let manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "0.3.4",
            build: 1,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-18T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion.dmg",
            sha256: String(repeating: "b", count: 64),
            size: 1,
            signature: "signature"
        )

        #expect(manifest.isNewer(thanVersion: "0.3.3", build: 99))
        #expect(!manifest.isNewer(thanVersion: "0.3.5", build: 0))
        #expect(manifest.isNewer(thanVersion: "0.3.4", build: 0))
        #expect(!manifest.isNewer(thanVersion: "0.3.4", build: 1))
    }

    @Test
    func stableReleaseSortsAfterPrerelease() {
        let stable = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "0.3.4",
            build: 1,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-18T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion.dmg",
            sha256: String(repeating: "b", count: 64),
            size: 1,
            signature: "signature"
        )

        #expect(stable.isNewer(thanVersion: "0.3.4-beta.2", build: 99))
    }

    @MainActor
    @Test
    func serviceReportsExplicitUnavailableStateWithoutReleaseConfiguration() async {
        let service = CompanionUpdateService(
            configuration: CompanionUpdateConfiguration(
                manifestURL: nil,
                publicKeyBase64: nil,
                currentVersion: "1.0",
                currentBuild: 1
            )
        )

        await service.checkForUpdates()

        #expect(service.state == .unavailable("Release feed and signing key are not configured."))
    }

    @MainActor
    @Test
    func serviceDownloadsAndExposesOnlyVerifiedInstaller() async throws {
        let privateKey = Curve25519.Signing.PrivateKey()
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let artifact = directory.appendingPathComponent("Codex-Companion-0.3.4-1-universal.dmg")
        let artifactData = Data("signed installer".utf8)
        try artifactData.write(to: artifact)
        let digest = SHA256.hash(data: artifactData)
            .map { String(format: "%02x", $0) }
            .joined()
        var manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "0.3.4",
            build: 1,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-18T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion-0.3.4-1-universal.dmg",
            sha256: digest,
            size: Int64(artifactData.count),
            signature: ""
        )
        manifest.signature = try privateKey.signature(for: manifest.canonicalPayload()).base64EncodedString()
        let service = CompanionUpdateService(
            configuration: CompanionUpdateConfiguration(
                manifestURL: URL(string: "https://example.com/update.json"),
                publicKeyBase64: privateKey.publicKey.rawRepresentation.base64EncodedString(),
                currentVersion: "0.3.3",
                currentBuild: 99
            ),
            loader: UpdateDataLoaderStub(data: try JSONEncoder().encode(manifest)),
            artifactDownloader: UpdateArtifactDownloaderStub(fileURL: artifact)
        )

        await service.checkForUpdates()
        #expect(service.state == .available(manifest))
        await service.downloadAvailableUpdate()

        guard case let .readyToInstall(verifiedManifest, verifiedURL) = service.state else {
            Issue.record("Expected a verified installer state")
            return
        }
        #expect(verifiedManifest == manifest)
        #expect(verifiedURL == artifact)
    }

    @MainActor
    @Test
    func serviceHandsOnlyReadyVerifiedArtifactToInstaller() async throws {
        let privateKey = Curve25519.Signing.PrivateKey()
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let artifact = directory.appendingPathComponent("Codex-Companion-0.3.4-1-universal.dmg")
        let artifactData = Data("signed installer".utf8)
        try artifactData.write(to: artifact)
        let digest = SHA256.hash(data: artifactData)
            .map { String(format: "%02x", $0) }
            .joined()
        var manifest = CompanionUpdateManifest(
            schemaVersion: 1,
            version: "0.3.4",
            build: 1,
            minimumSystemVersion: "14.0",
            publishedAt: "2026-07-18T00:00:00Z",
            downloadURL: "https://example.com/Codex-Companion-0.3.4-1-universal.dmg",
            sha256: digest,
            size: Int64(artifactData.count),
            signature: ""
        )
        manifest.signature = try privateKey.signature(for: manifest.canonicalPayload()).base64EncodedString()
        let installer = UpdateInstallerSpy()
        let service = CompanionUpdateService(
            configuration: CompanionUpdateConfiguration(
                manifestURL: URL(string: "https://example.com/update.json"),
                publicKeyBase64: privateKey.publicKey.rawRepresentation.base64EncodedString(),
                currentVersion: "0.3.3",
                currentBuild: 99
            ),
            loader: UpdateDataLoaderStub(data: try JSONEncoder().encode(manifest)),
            artifactDownloader: UpdateArtifactDownloaderStub(fileURL: artifact),
            installer: installer
        )

        service.installReadyUpdate()
        #expect(installer.invocations.isEmpty)

        await service.checkForUpdates()
        await service.downloadAvailableUpdate()
        service.installReadyUpdate()

        #expect(installer.invocations == [.init(fileURL: artifact, manifest: manifest)])
        #expect(service.state == .installing(manifest))
    }

    @MainActor
    @Test
    func installHelperRechecksSignedArtifactBeforeTransactionalInstaller() {
        let helper = CompanionUpdateInstaller.helperScript

        #expect(helper.contains("/usr/bin/stat -f '%z'"))
        #expect(helper.contains("/usr/bin/shasum -a 256"))
        #expect(helper.contains("/usr/bin/hdiutil attach -readonly -nobrowse"))
        #expect(helper.contains("Install Codex Companion.command"))
        #expect(helper.contains(".install_release.sh"))
        #expect(helper.contains("CODEX_COMPANION_ALLOW_ADHOC=1"))
        #expect(helper.contains("CODEX_COMPANION_INSTALL_RELAUNCH_AGENT=\"${CODEX_COMPANION_INSTALL_RELAUNCH_AGENT:-1}\""))
    }
}

private struct UpdateDataLoaderStub: CompanionUpdateDataLoading {
    let data: Data

    func data(from url: URL) async throws -> Data {
        data
    }
}

private struct UpdateArtifactDownloaderStub: CompanionUpdateArtifactDownloading {
    let fileURL: URL

    func download(from url: URL) async throws -> URL {
        fileURL
    }
}

@MainActor
private final class UpdateInstallerSpy: CompanionUpdateInstalling {
    struct Invocation: Equatable {
        let fileURL: URL
        let manifest: CompanionUpdateManifest
    }

    private(set) var invocations: [Invocation] = []

    func beginInstallation(fileURL: URL, manifest: CompanionUpdateManifest) throws {
        invocations.append(.init(fileURL: fileURL, manifest: manifest))
    }
}
