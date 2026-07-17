import Foundation

/// Internal adaptation seam for a future OpenAI-documented normal ChatGPT account bridge.
///
/// There is intentionally no production implementation today. In particular, this protocol is
/// not an assertion that ChatGPT's private XPC services, session state, or Codex app-server are a
/// normal ChatGPT request/stream API.
protocol ChatGPTAccountBridgeTransport: Sendable {
    var capabilities: ChatGPTAccountCapabilities { get }

    func stream(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error>
}

struct ChatGPTAccountCapabilityDetector: Sendable {
    static let defaultApplicationURL = URL(fileURLWithPath: "/Applications/ChatGPT.app")
    static let recognizedBundleIdentifiers: Set<String> = [
        "com.openai.codex",
        "com.openai.chat",
    ]

    var applicationURL: URL

    init(applicationURL: URL = Self.defaultApplicationURL) {
        self.applicationURL = applicationURL
    }

    /// Performs static bundle inspection only. It never launches ChatGPT or reads account state.
    func inspect() -> ChatGPTAccountAppInspection {
        let fileManager = FileManager.default
        var isDirectory: ObjCBool = false
        let pathExists = fileManager.fileExists(
            atPath: applicationURL.path,
            isDirectory: &isDirectory
        )
        guard pathExists else {
            return ChatGPTAccountAppInspection(
                applicationURL: applicationURL,
                pathExists: false,
                isInstalled: false,
                bundleIdentifier: nil,
                version: nil,
                build: nil,
                declaredURLSchemes: [],
                xpcServiceIdentifiers: [],
                appExtensionIdentifiers: [],
                hasAppIntentsMetadata: false,
                hasBundledCodexExecutable: false
            )
        }

        let contentsURL = applicationURL.appendingPathComponent("Contents", isDirectory: true)
        let info = Self.infoPlist(
            at: contentsURL.appendingPathComponent("Info.plist", isDirectory: false)
        )
        let bundleIdentifier = info?["CFBundleIdentifier"] as? String
        let isInstalled = isDirectory.boolValue
            && applicationURL.pathExtension.caseInsensitiveCompare("app") == .orderedSame
            && bundleIdentifier.map(Self.recognizedBundleIdentifiers.contains) == true
        let schemes = (info?["CFBundleURLTypes"] as? [[String: Any]] ?? [])
            .flatMap { $0["CFBundleURLSchemes"] as? [String] ?? [] }
            .filter { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            .uniquedAndSorted()

        let resourcesURL = contentsURL.appendingPathComponent("Resources", isDirectory: true)
        let declaredSurfaces = Self.scanDeclaredSurfaces(in: contentsURL)

        let bundledCodexCandidates = [
            resourcesURL.appendingPathComponent("codex", isDirectory: false),
            contentsURL.appendingPathComponent("MacOS/codex", isDirectory: false),
        ]

        return ChatGPTAccountAppInspection(
            applicationURL: applicationURL,
            pathExists: true,
            isInstalled: isInstalled,
            bundleIdentifier: bundleIdentifier,
            version: info?["CFBundleShortVersionString"] as? String,
            build: info?["CFBundleVersion"] as? String,
            declaredURLSchemes: schemes,
            xpcServiceIdentifiers: declaredSurfaces.xpcServiceIdentifiers,
            appExtensionIdentifiers: declaredSurfaces.appExtensionIdentifiers,
            hasAppIntentsMetadata: declaredSurfaces.hasAppIntentsMetadata,
            hasBundledCodexExecutable: bundledCodexCandidates.contains {
                Self.isExecutableRegularFile(at: $0, fileManager: fileManager)
            }
        )
    }

    func unsupportedResult() -> ChatGPTAccountUnsupported {
        let inspection = inspect()
        let reason: ChatGPTAccountUnsupportedReason
        if !inspection.pathExists {
            reason = .chatGPTApplicationNotInstalled
        } else if !inspection.isInstalled {
            reason = .invalidChatGPTApplicationBundle
        } else {
            reason = .noSupportedNormalChatTransport
        }
        return ChatGPTAccountUnsupported(
            reason: reason,
            inspection: inspection,
            missingBridge: ChatGPTAccountMissingBridge()
        )
    }

    private struct DeclaredSurfaceScan {
        var xpcServiceIdentifiers: [String]
        var appExtensionIdentifiers: [String]
        var hasAppIntentsMetadata: Bool
    }

    private static func scanDeclaredSurfaces(in contentsURL: URL) -> DeclaredSurfaceScan {
        guard let enumerator = FileManager.default.enumerator(
            at: contentsURL,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) else {
            return DeclaredSurfaceScan(
                xpcServiceIdentifiers: [],
                appExtensionIdentifiers: [],
                hasAppIntentsMetadata: false
            )
        }

        var xpcServiceIdentifiers: [String] = []
        var appExtensionIdentifiers: [String] = []
        var hasAppIntentsMetadata = false
        for case let url as URL in enumerator {
            switch url.pathExtension.lowercased() {
            case "xpc":
                if let identifier = infoPlist(
                    at: url.appendingPathComponent("Contents/Info.plist", isDirectory: false)
                )?["CFBundleIdentifier"] as? String {
                    xpcServiceIdentifiers.append(identifier)
                }
            case "appex":
                if let identifier = infoPlist(
                    at: url.appendingPathComponent("Contents/Info.plist", isDirectory: false)
                )?["CFBundleIdentifier"] as? String {
                    appExtensionIdentifiers.append(identifier)
                }
            case "appintents", "intentdefinition":
                hasAppIntentsMetadata = true
            default:
                break
            }
        }

        return DeclaredSurfaceScan(
            xpcServiceIdentifiers: xpcServiceIdentifiers.uniquedAndSorted(),
            appExtensionIdentifiers: appExtensionIdentifiers.uniquedAndSorted(),
            hasAppIntentsMetadata: hasAppIntentsMetadata
        )
    }

    private static func infoPlist(at url: URL) -> [String: Any]? {
        guard
            let data = try? Data(contentsOf: url),
            let plist = try? PropertyListSerialization.propertyList(
                from: data,
                options: [],
                format: nil
            )
        else {
            return nil
        }
        return plist as? [String: Any]
    }

    private static func isExecutableRegularFile(
        at url: URL,
        fileManager: FileManager
    ) -> Bool {
        var isDirectory: ObjCBool = false
        return fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory)
            && !isDirectory.boolValue
            && fileManager.isExecutableFile(atPath: url.path)
    }
}

struct ChatGPTAccountConnectionService: Sendable {
    var detector: ChatGPTAccountCapabilityDetector
    private var bridge: (any ChatGPTAccountBridgeTransport)?

    init(
        detector: ChatGPTAccountCapabilityDetector = ChatGPTAccountCapabilityDetector(),
        bridge: (any ChatGPTAccountBridgeTransport)? = nil
    ) {
        self.detector = detector
        self.bridge = bridge
    }

    /// Only an explicitly supplied, documented bridge can make account chat available.
    /// The adapter is registered after its own authorization and capability discovery succeeds;
    /// its capability booleans intentionally expose plan- or transport-specific feature gaps.
    func availability() -> ChatGPTAccountAvailability {
        guard let bridge else {
            return .unsupported(detector.unsupportedResult())
        }
        return .available(bridge.capabilities)
    }

    func stream(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error> {
        guard let bridge else {
            let unsupported = detector.unsupportedResult()
            return AsyncThrowingStream { continuation in
                continuation.finish(
                    throwing: ChatGPTAccountConnectionError.unsupported(unsupported)
                )
            }
        }
        return bridge.stream(request)
    }
}

private extension Array where Element == String {
    func uniquedAndSorted() -> [String] {
        Array(Set(self)).sorted()
    }
}
