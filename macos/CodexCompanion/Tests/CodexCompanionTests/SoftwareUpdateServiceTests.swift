import CryptoKit
import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct SoftwareUpdateServiceTests {
    @Test
    func comparesNumericVersions() {
        #expect(CompanionVersion("1.2.9") < CompanionVersion("1.3.0"))
        #expect(CompanionVersion("v1.10") > CompanionVersion("1.9.8"))
        #expect(CompanionVersion("2.0") == CompanionVersion("2.0.0"))
    }

    @Test
    func normalizesReleaseTags() {
        #expect(CompanionReleaseClient.normalizedVersion("v1.4.0") == "1.4.0")
        #expect(CompanionReleaseClient.normalizedVersion("V2.0") == "2.0")
    }

    @Test
    func acceptsMatchingChecksumAndRejectsMismatch() throws {
        let installer = Data("installer".utf8)
        let digest = SHA256.hash(data: installer).map { String(format: "%02x", $0) }.joined()

        try SoftwareUpdateService.verify(
            installerData: installer,
            checksumData: Data("\(digest)  Codex-Companion.dmg\n".utf8)
        )

        #expect(throws: CompanionUpdateError.self) {
            try SoftwareUpdateService.verify(
                installerData: Data("tampered".utf8),
                checksumData: Data("\(digest)  Codex-Companion.dmg\n".utf8)
            )
        }
    }
}
