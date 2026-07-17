import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTAccountConnectionServiceTests {
    @Test
    func inspectorReportsDeclaredSurfacesWithoutTreatingThemAsAChatBridge() throws {
        let fixture = try ChatGPTAppFixture.make()
        defer { try? FileManager.default.removeItem(at: fixture) }

        let detector = ChatGPTAccountCapabilityDetector(applicationURL: fixture)
        let inspection = detector.inspect()

        #expect(inspection.isInstalled)
        #expect(inspection.bundleIdentifier == "com.openai.codex")
        #expect(inspection.version == "26.707.71524")
        #expect(inspection.build == "5263")
        #expect(inspection.declaredURLSchemes == ["chatgpt", "codex"])
        #expect(inspection.xpcServiceIdentifiers == ["com.openai.codex.internal-helper"])
        #expect(inspection.appExtensionIdentifiers == ["com.openai.codex.shortcuts"])
        #expect(inspection.hasAppIntentsMetadata)
        #expect(inspection.hasBundledCodexExecutable)

        let availability = ChatGPTAccountConnectionService(detector: detector).availability()
        guard case .unsupported(let unsupported) = availability else {
            Issue.record("Declared app surfaces must not be promoted to a request/stream bridge.")
            return
        }
        #expect(unsupported.reason == .noSupportedNormalChatTransport)
        #expect(
            Set(unsupported.missingBridge.requiredCapabilities)
                == Set(ChatGPTAccountBridgeRequirement.allCases)
        )
        #expect(unsupported.inspection == inspection)
    }

    @Test
    func missingApplicationHasADistinctTypedUnsupportedReason() {
        let missingURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("app")
        let service = ChatGPTAccountConnectionService(
            detector: ChatGPTAccountCapabilityDetector(applicationURL: missingURL)
        )

        guard case .unsupported(let unsupported) = service.availability() else {
            Issue.record("A missing ChatGPT application cannot be account-chat capable.")
            return
        }

        #expect(unsupported.reason == .chatGPTApplicationNotInstalled)
        #expect(!unsupported.inspection.isInstalled)
        #expect(unsupported.inspection.applicationURL == missingURL)
    }

    @Test
    func existingNonApplicationPathIsReportedAsInvalid() throws {
        let fileURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
        try Data("not an app".utf8).write(to: fileURL)
        defer { try? FileManager.default.removeItem(at: fileURL) }

        let service = ChatGPTAccountConnectionService(
            detector: ChatGPTAccountCapabilityDetector(applicationURL: fileURL)
        )
        guard case .unsupported(let unsupported) = service.availability() else {
            Issue.record("A regular file cannot be a ChatGPT application bundle.")
            return
        }

        #expect(unsupported.reason == .invalidChatGPTApplicationBundle)
        #expect(unsupported.inspection.pathExists)
        #expect(!unsupported.inspection.isInstalled)
    }

    @Test
    func unrelatedApplicationBundleIsReportedAsInvalid() throws {
        let fixture = try ChatGPTAppFixture.make(bundleIdentifier: "com.example.not-chatgpt")
        defer { try? FileManager.default.removeItem(at: fixture) }

        let service = ChatGPTAccountConnectionService(
            detector: ChatGPTAccountCapabilityDetector(applicationURL: fixture)
        )
        guard case .unsupported(let unsupported) = service.availability() else {
            Issue.record("An unrelated application must not be classified as ChatGPT.")
            return
        }

        #expect(unsupported.reason == .invalidChatGPTApplicationBundle)
        #expect(unsupported.inspection.bundleIdentifier == "com.example.not-chatgpt")
        #expect(!unsupported.inspection.isInstalled)
    }

    @Test
    func nonExecutableCodexPathIsNotReportedAsBundledCLI() throws {
        let fixture = try ChatGPTAppFixture.make(codexIsExecutable: false)
        defer { try? FileManager.default.removeItem(at: fixture) }

        let inspection = ChatGPTAccountCapabilityDetector(
            applicationURL: fixture
        ).inspect()

        #expect(inspection.isInstalled)
        #expect(!inspection.hasBundledCodexExecutable)
    }

    @Test
    func unsupportedStreamFailsWithTheSameTypedResult() async {
        let missingURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("app")
        let service = ChatGPTAccountConnectionService(
            detector: ChatGPTAccountCapabilityDetector(applicationURL: missingURL)
        )
        guard case .unsupported(let expected) = service.availability() else {
            Issue.record("Expected an unsupported account connection.")
            return
        }

        do {
            for try await _ in service.stream(
                ChatGPTAccountChatRequest(prompt: "Hello from Companion")
            ) {
                Issue.record("An unsupported bridge must not yield chat events.")
            }
            Issue.record("An unsupported bridge must finish by throwing.")
        } catch let error as ChatGPTAccountConnectionError {
            #expect(error == .unsupported(expected))
        } catch {
            Issue.record("Expected ChatGPTAccountConnectionError, got \(error).")
        }
    }

    @Test
    func explicitlyRegisteredBridgeExposesModelsAgentsAndStreamEvents() async throws {
        let capabilities = ChatGPTAccountCapabilities(
            transportID: "documented-account-bridge",
            transportName: "Documented account bridge",
            supportsStreaming: true,
            supportsConversationContinuation: true,
            models: [
                .init(id: "model-a", displayName: "Model A", isDefault: true),
                .init(id: "model-b", displayName: "Model B", isDefault: false),
            ],
            agents: [
                .init(id: "agent-a", displayName: "Agent A", description: "A published agent")
            ]
        )
        let bridge = StubChatGPTAccountBridge(
            capabilities: capabilities,
            events: [
                .conversationStarted(id: "conversation-1"),
                .textDelta("Hello"),
                .completed,
            ],
            recorder: ChatGPTAccountRequestRecorder()
        )
        let service = ChatGPTAccountConnectionService(
            detector: ChatGPTAccountCapabilityDetector(
                applicationURL: URL(fileURLWithPath: "/Applications/ChatGPT.app")
            ),
            bridge: bridge
        )

        #expect(service.availability() == .available(capabilities))

        let request = ChatGPTAccountChatRequest(
            conversationID: "conversation-existing",
            prompt: "Use the selected account features",
            modelID: "model-b",
            agentID: "agent-a"
        )
        var events: [ChatGPTAccountStreamEvent] = []
        for try await event in service.stream(request) {
            events.append(event)
        }

        #expect(events == bridge.events)
        #expect(bridge.recorder?.requests == [request])
    }
}

private struct StubChatGPTAccountBridge: ChatGPTAccountBridgeTransport {
    var capabilities: ChatGPTAccountCapabilities
    var events: [ChatGPTAccountStreamEvent]
    var recorder: ChatGPTAccountRequestRecorder? = nil

    func stream(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error> {
        recorder?.record(request)
        return AsyncThrowingStream { continuation in
            for event in events {
                continuation.yield(event)
            }
            continuation.finish()
        }
    }
}

private final class ChatGPTAccountRequestRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recordedRequests: [ChatGPTAccountChatRequest] = []

    var requests: [ChatGPTAccountChatRequest] {
        lock.withLock { recordedRequests }
    }

    func record(_ request: ChatGPTAccountChatRequest) {
        lock.withLock {
            recordedRequests.append(request)
        }
    }
}

private enum ChatGPTAppFixture {
    static func make(
        bundleIdentifier: String = "com.openai.codex",
        codexIsExecutable: Bool = true
    ) throws -> URL {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
            .appendingPathExtension("app")
        let contents = root.appendingPathComponent("Contents", isDirectory: true)
        try FileManager.default.createDirectory(at: contents, withIntermediateDirectories: true)

        try writePlist(
            [
                "CFBundleIdentifier": bundleIdentifier,
                "CFBundleShortVersionString": "26.707.71524",
                "CFBundleVersion": "5263",
                "CFBundleURLTypes": [
                    ["CFBundleURLSchemes": ["codex", "chatgpt", "codex"]]
                ],
            ],
            to: contents.appendingPathComponent("Info.plist")
        )

        try writeNestedBundle(
            root: contents.appendingPathComponent("XPCServices/Internal.xpc"),
            identifier: "com.openai.codex.internal-helper"
        )
        try writeNestedBundle(
            root: contents.appendingPathComponent("PlugIns/Shortcuts.appex"),
            identifier: "com.openai.codex.shortcuts"
        )

        let resources = contents.appendingPathComponent("Resources", isDirectory: true)
        try FileManager.default.createDirectory(
            at: resources.appendingPathComponent("Metadata.appintents", isDirectory: true),
            withIntermediateDirectories: true
        )
        let codexURL = resources.appendingPathComponent("codex")
        FileManager.default.createFile(
            atPath: codexURL.path,
            contents: Data()
        )
        if codexIsExecutable {
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o755],
                ofItemAtPath: codexURL.path
            )
        }
        return root
    }

    private static func writeNestedBundle(root: URL, identifier: String) throws {
        let contents = root.appendingPathComponent("Contents", isDirectory: true)
        try FileManager.default.createDirectory(at: contents, withIntermediateDirectories: true)
        try writePlist(
            ["CFBundleIdentifier": identifier],
            to: contents.appendingPathComponent("Info.plist")
        )
    }

    private static func writePlist(_ value: [String: Any], to url: URL) throws {
        let data = try PropertyListSerialization.data(
            fromPropertyList: value,
            format: .xml,
            options: 0
        )
        try data.write(to: url)
    }
}
