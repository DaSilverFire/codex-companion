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
}
