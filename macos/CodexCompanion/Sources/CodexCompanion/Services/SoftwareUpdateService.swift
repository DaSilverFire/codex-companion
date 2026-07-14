import AppKit
import CryptoKit
import Foundation

struct CompanionRelease: Equatable, Sendable {
    var version: String
    var pageURL: URL
    var installerURL: URL
    var checksumURL: URL
    var installerName: String
}

enum CompanionUpdateState: Equatable {
    case idle
    case checking
    case current
    case available(CompanionRelease)
    case downloading(CompanionRelease)
    case installerReady(CompanionRelease, URL)
    case failed(String)
}

struct CompanionVersion: Comparable, Equatable {
    private let components: [Int]

    init(_ value: String) {
        let numericPrefix = value
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "vV"))
            .split(separator: "-", maxSplits: 1)
            .first ?? "0"
        components = numericPrefix.split(separator: ".").map { Int($0) ?? 0 }
    }

    static func < (lhs: Self, rhs: Self) -> Bool {
        let count = max(lhs.components.count, rhs.components.count)
        for index in 0..<count {
            let left = index < lhs.components.count ? lhs.components[index] : 0
            let right = index < rhs.components.count ? rhs.components[index] : 0
            if left != right { return left < right }
        }
        return false
    }

    static func == (lhs: Self, rhs: Self) -> Bool {
        !(lhs < rhs) && !(rhs < lhs)
    }
}

struct CompanionReleaseClient: Sendable {
    static let latestReleaseURL = URL(
        string: "https://api.github.com/repos/DaSilverFire/codex-companion/releases/latest"
    )!

    var session: URLSession = .shared

    func latestRelease() async throws -> CompanionRelease {
        var request = URLRequest(url: Self.latestReleaseURL)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("CodexCompanion-Updater", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await session.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse, httpResponse.statusCode == 200 else {
            throw CompanionUpdateError.releaseUnavailable
        }

        let release = try JSONDecoder().decode(GitHubRelease.self, from: data)
        guard let pageURL = URL(string: release.htmlURL) else {
            throw CompanionUpdateError.invalidRelease
        }
        let installer = release.assets.first {
            $0.name.hasSuffix("-macOS-universal.dmg")
        }
        guard
            let installer,
            let installerURL = URL(string: installer.browserDownloadURL),
            let checksum = release.assets.first(where: { $0.name == "\(installer.name).sha256" }),
            let checksumURL = URL(string: checksum.browserDownloadURL)
        else {
            throw CompanionUpdateError.missingInstaller
        }

        return CompanionRelease(
            version: Self.normalizedVersion(release.tagName),
            pageURL: pageURL,
            installerURL: installerURL,
            checksumURL: checksumURL,
            installerName: installer.name
        )
    }

    static func normalizedVersion(_ tag: String) -> String {
        tag.trimmingCharacters(in: CharacterSet(charactersIn: "vV"))
    }

    private struct GitHubRelease: Decodable {
        var tagName: String
        var htmlURL: String
        var assets: [Asset]

        enum CodingKeys: String, CodingKey {
            case tagName = "tag_name"
            case htmlURL = "html_url"
            case assets
        }
    }

    private struct Asset: Decodable {
        var name: String
        var browserDownloadURL: String

        enum CodingKeys: String, CodingKey {
            case name
            case browserDownloadURL = "browser_download_url"
        }
    }
}

@MainActor
final class SoftwareUpdateService: ObservableObject {
    @Published private(set) var state: CompanionUpdateState = .idle

    let currentVersion: String
    private let client: CompanionReleaseClient
    private let session: URLSession

    init(
        currentVersion: String = Bundle.main.object(
            forInfoDictionaryKey: "CFBundleShortVersionString"
        ) as? String ?? "0",
        client: CompanionReleaseClient = CompanionReleaseClient(),
        session: URLSession = .shared
    ) {
        self.currentVersion = currentVersion
        self.client = client
        self.session = session
    }

    var statusText: String {
        switch state {
        case .idle:
            "Check GitHub Releases for a prebuilt update."
        case .checking:
            "Checking for updates..."
        case .current:
            "Codex Companion is up to date."
        case let .available(release):
            "Version \(release.version) is available."
        case let .downloading(release):
            "Downloading version \(release.version)..."
        case .installerReady:
            "The verified installer is ready."
        case let .failed(message):
            message
        }
    }

    var availableRelease: CompanionRelease? {
        switch state {
        case let .available(release), let .downloading(release), let .installerReady(release, _):
            release
        default:
            nil
        }
    }

    var installerURL: URL? {
        guard case let .installerReady(_, url) = state else { return nil }
        return url
    }

    var isBusy: Bool {
        switch state {
        case .checking, .downloading:
            true
        default:
            false
        }
    }

    func checkForUpdates() {
        guard !isBusy else { return }
        state = .checking
        Task {
            do {
                let release = try await client.latestRelease()
                state = CompanionVersion(currentVersion) < CompanionVersion(release.version)
                    ? .available(release)
                    : .current
            } catch {
                state = .failed(error.localizedDescription)
            }
        }
    }

    func downloadInstaller() {
        guard let release = availableRelease, !isBusy else { return }
        state = .downloading(release)
        Task {
            do {
                async let installerDownload = session.data(from: release.installerURL)
                async let checksumDownload = session.data(from: release.checksumURL)
                let ((installerData, installerResponse), (checksumData, checksumResponse)) = try await (
                    installerDownload,
                    checksumDownload
                )
                try Self.requireSuccessfulResponse(installerResponse)
                try Self.requireSuccessfulResponse(checksumResponse)
                try Self.verify(installerData: installerData, checksumData: checksumData)

                let destination = try Self.installerDestination(for: release)
                try installerData.write(to: destination, options: [.atomic])
                state = .installerReady(release, destination)
                NSWorkspace.shared.activateFileViewerSelecting([destination])
                NSWorkspace.shared.open(destination)
            } catch {
                state = .failed(error.localizedDescription)
            }
        }
    }

    func openInstaller() {
        guard let installerURL else { return }
        NSWorkspace.shared.open(installerURL)
    }

    func openReleaseNotes() {
        guard let release = availableRelease else { return }
        NSWorkspace.shared.open(release.pageURL)
    }

    nonisolated static func verify(installerData: Data, checksumData: Data) throws {
        guard
            let checksumText = String(data: checksumData, encoding: .utf8),
            let expected = checksumText.split(whereSeparator: { $0.isWhitespace }).first,
            expected.count == 64
        else {
            throw CompanionUpdateError.invalidChecksum
        }
        let actual = SHA256.hash(data: installerData).map { String(format: "%02x", $0) }.joined()
        guard actual.caseInsensitiveCompare(String(expected)) == .orderedSame else {
            throw CompanionUpdateError.checksumMismatch
        }
    }

    private static func requireSuccessfulResponse(_ response: URLResponse) throws {
        guard let httpResponse = response as? HTTPURLResponse,
              (200..<300).contains(httpResponse.statusCode)
        else {
            throw CompanionUpdateError.downloadFailed
        }
    }

    private static func installerDestination(for release: CompanionRelease) throws -> URL {
        let root = try FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let updateDirectory = root
            .appendingPathComponent("Codex Companion", isDirectory: true)
            .appendingPathComponent("Updates", isDirectory: true)
        try FileManager.default.createDirectory(at: updateDirectory, withIntermediateDirectories: true)
        return updateDirectory.appendingPathComponent(release.installerName)
    }
}

enum CompanionUpdateError: LocalizedError {
    case releaseUnavailable
    case invalidRelease
    case missingInstaller
    case downloadFailed
    case invalidChecksum
    case checksumMismatch

    var errorDescription: String? {
        switch self {
        case .releaseUnavailable:
            "The release service is unavailable."
        case .invalidRelease:
            "The latest release metadata is invalid."
        case .missingInstaller:
            "The latest release has no universal macOS installer."
        case .downloadFailed:
            "The update download failed."
        case .invalidChecksum:
            "The release checksum is invalid."
        case .checksumMismatch:
            "The downloaded installer failed its integrity check."
        }
    }
}
