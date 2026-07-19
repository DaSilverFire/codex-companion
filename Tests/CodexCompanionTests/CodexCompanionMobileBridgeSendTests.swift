import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeSendTests {
    @Test(arguments: [CodexSendAction.reply, .steer])
    func unavailableModelOverrideDoesNotBlockTheMessage(_ action: CodexSendAction) async throws {
        let recorder = MessageSendRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: FileManager.default.temporaryDirectory
                .appendingPathComponent(UUID().uuidString, isDirectory: true),
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            threadSettingsUpdater: { _, _, _ in .sharedDaemonUnavailable },
            taskMessageSender: {
                await recorder.send(
                    prompt: $0,
                    threadID: $1,
                    cwd: $2,
                    action: $3,
                    expectedTurnID: $4,
                    clientMessageID: $5,
                    attachments: $6
                )
            }
        )
        let request = CompanionBridgeRequest(
            operation: .sendMessage,
            threadID: "thread-existing",
            text: "Keep working",
            sendAction: action == .steer ? .steer : .reply,
            model: "gpt-selected",
            reasoningEffort: "high"
        )

        let response = await server.handle(request)

        #expect(response.succeeded)
        #expect(response.message?.contains("current model") == true)
        let call = try #require(recorder.recordedCalls.first)
        #expect(call.prompt == "Keep working")
        #expect(call.threadID == "thread-existing")
        #expect(call.action == action)
    }

    @Test
    func appliedModelOverrideKeepsTheNormalSuccessMessage() async {
        let recorder = MessageSendRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: FileManager.default.temporaryDirectory
                .appendingPathComponent(UUID().uuidString, isDirectory: true),
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
            threadSettingsUpdater: { _, _, _ in .sent },
            taskMessageSender: {
                await recorder.send(
                    prompt: $0,
                    threadID: $1,
                    cwd: $2,
                    action: $3,
                    expectedTurnID: $4,
                    clientMessageID: $5,
                    attachments: $6
                )
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendMessage,
                threadID: "thread-existing",
                text: "Continue",
                sendAction: .steer,
                model: "gpt-selected"
            )
        )

        #expect(response.succeeded)
        #expect(response.message == "Steered task.")
    }
}

private final class MessageSendRecorder: @unchecked Sendable {
    struct Call: Equatable {
        var prompt: String
        var threadID: String
        var action: CodexSendAction
    }

    private let lock = NSLock()
    private var calls: [Call] = []

    var recordedCalls: [Call] {
        lock.lock()
        defer { lock.unlock() }
        return calls
    }

    func send(
        prompt: String,
        threadID: String,
        cwd: String?,
        action: CodexSendAction,
        expectedTurnID: String?,
        clientMessageID: String,
        attachments: [CodexFollowerAttachment]
    ) async -> CodexAppServerSendOutcome {
        lock.withLock {
            calls.append(Call(prompt: prompt, threadID: threadID, action: action))
        }
        return .sent
    }
}
