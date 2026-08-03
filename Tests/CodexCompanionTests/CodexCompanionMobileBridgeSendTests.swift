import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeSendTests {
    @Test
    func duplicateSendMessageRequestIsSubmittedOnlyOnce() async {
        let recorder = MessageSendRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: FileManager.default.temporaryDirectory
                .appendingPathComponent(UUID().uuidString, isDirectory: true),
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
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
            text: "Continue once",
            sendAction: .steer
        )

        async let first = server.handle(request)
        async let duplicate = server.handle(request)
        let responses = await [first, duplicate]

        #expect(responses.allSatisfy { $0.succeeded })
        #expect(recorder.recordedCalls.count == 1)
    }

    @Test
    func relayResponseFallsBackToNearbyWithoutResendingTheRequest() async {
        let recorder = BridgeResponseRouteRecorder(
            nearbyResult: true,
            relayResult: false
        )
        let server = CodexCompanionMobileBridgeServer(
            nearbyResponseSender: { response, context in
                recorder.sendNearby(response: response, context: context)
            },
            relayResponseSender: { response, context in
                await recorder.sendRelay(response: response, context: context)
            }
        )
        let request = CompanionBridgeRequest(
            operation: .sendMessage,
            threadID: "thread-existing",
            text: "Keep working",
            sendAction: .steer
        )

        await server.deliverResponse(
            .success(for: request, message: "Steered task."),
            context: CompanionBridgeRequestContext(
                deviceID: "phone-1",
                relayGeneration: UUID()
            )
        )

        #expect(recorder.routes == ["relay", "nearby"])
        #expect(recorder.responseIDs == [request.id, request.id])
    }

    @Test
    func successfulRelayResponseIsNotDuplicatedOverNearby() async {
        let recorder = BridgeResponseRouteRecorder(
            nearbyResult: true,
            relayResult: true
        )
        let server = CodexCompanionMobileBridgeServer(
            nearbyResponseSender: { response, context in
                recorder.sendNearby(response: response, context: context)
            },
            relayResponseSender: { response, context in
                await recorder.sendRelay(response: response, context: context)
            }
        )
        let request = CompanionBridgeRequest(
            operation: .sendMessage,
            threadID: "thread-existing",
            text: "Keep working",
            sendAction: .steer
        )

        await server.deliverResponse(
            .success(for: request, message: "Steered task."),
            context: CompanionBridgeRequestContext(
                deviceID: "phone-1",
                relayGeneration: UUID()
            )
        )

        #expect(recorder.routes == ["relay"])
        #expect(recorder.responseIDs == [request.id])
    }

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

    @Test
    func clientExpectedTurnIdentityReachesTheNativeSender() async throws {
        let recorder = MessageSendRecorder()
        let archive = CodexMobileTaskArchive(
            homeDirectory: FileManager.default.temporaryDirectory
                .appendingPathComponent(UUID().uuidString, isDirectory: true),
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
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

        let response = await server.handle(CompanionBridgeRequest(
            operation: .sendMessage,
            threadID: "thread-existing",
            text: "Continue",
            sendAction: .steer,
            expectedTurnID: "turn-from-phone"
        ))

        #expect(response.succeeded)
        let call = try #require(recorder.recordedCalls.first)
        #expect(call.expectedTurnID == "turn-from-phone")
    }

    @Test
    func canonicalMessageTargetsTheActiveForkForSettingsAndDelivery() async throws {
        let defaultsName = "CodexCompanionMobileBridgeSendLineageTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let lineages = CodexThreadLineageStore(defaults: defaults)
        #expect(lineages.registerFork(
            sourceThreadID: "canonical-thread",
            destinationThreadID: "physical-fork"
        ) == "canonical-thread")
        let messageRecorder = MessageSendRecorder()
        let settingsRecorder = SettingsUpdateRecorder()
        let server = CodexCompanionMobileBridgeServer(
            archive: CodexMobileTaskArchive(lineageStore: lineages),
            threadSettingsUpdater: { threadID, model, reasoningEffort in
                await settingsRecorder.record(
                    threadID: threadID,
                    model: model,
                    reasoningEffort: reasoningEffort
                )
                return .sent
            },
            taskMessageSender: {
                await messageRecorder.send(
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

        let response = await server.handle(CompanionBridgeRequest(
            operation: .sendMessage,
            threadID: "canonical-thread",
            text: "Continue",
            sendAction: .steer,
            model: "gpt-selected",
            reasoningEffort: "high"
        ))

        #expect(response.succeeded)
        #expect(await settingsRecorder.threadIDs == ["physical-fork"])
        #expect(messageRecorder.recordedCalls.map(\.threadID) == ["physical-fork"])
    }

    @Test
    func streamedAttachmentIsResolvedBeforeSendingTheTaskMessage() async throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("mobile-bridge-streamed-send-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }

        let recorder = MessageSendRecorder()
        let uploadStore = CompanionIncomingAttachmentUploadStore(
            rootURL: root.appendingPathComponent("uploads", isDirectory: true)
        )
        let archive = CodexMobileTaskArchive(
            homeDirectory: root.appendingPathComponent("archive", isDirectory: true),
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(
            archive: archive,
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
            },
            attachmentUploadStore: uploadStore
        )
        let deviceID = "phone-1"
        let context = CompanionBridgeRequestContext(deviceID: deviceID)
        let uploadID = UUID()
        let attachmentID = UUID()
        let bytes = Data("streamed payload".utf8)

        let begin = await server.handle(
            CompanionBridgeRequest(
                operation: .beginAttachmentUpload,
                attachmentUploadID: uploadID,
                attachmentID: attachmentID,
                attachmentKind: .file,
                attachmentFilename: "notes.txt",
                attachmentMimeType: "text/plain",
                attachmentByteCount: Int64(bytes.count)
            ),
            context: context
        )
        #expect(begin.succeeded)

        let uploaded = await server.handle(
            CompanionBridgeRequest(
                operation: .uploadAttachmentChunk,
                attachmentUploadID: uploadID,
                attachmentID: attachmentID,
                attachmentChunkOffset: 0,
                attachmentChunkData: bytes
            ),
            context: context
        )
        #expect(uploaded.succeeded)
        #expect(uploaded.attachmentUploadProgress?.isComplete == true)

        let reference = CompanionBridgeAttachment(
            id: attachmentID,
            kind: .file,
            filename: "notes.txt",
            mimeType: "text/plain",
            data: Data(),
            byteCount: Int64(bytes.count),
            uploadID: uploadID
        )
        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendMessage,
                threadID: "thread-existing",
                text: "Use the file",
                sendAction: .reply,
                attachments: [reference]
            ),
            context: context
        )

        #expect(response.succeeded)
        let call = try #require(recorder.recordedCalls.first)
        let attachment = try #require(call.attachments.first)
        #expect(attachment.label == "notes.txt")
        #expect(try Data(contentsOf: URL(fileURLWithPath: attachment.path)) == bytes)
    }
}

private final class BridgeResponseRouteRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private let nearbyResult: Bool
    private let relayResult: Bool
    private var recordedRoutes: [String] = []
    private var recordedResponseIDs: [UUID] = []

    init(nearbyResult: Bool, relayResult: Bool) {
        self.nearbyResult = nearbyResult
        self.relayResult = relayResult
    }

    var routes: [String] {
        lock.withLock { recordedRoutes }
    }

    var responseIDs: [UUID] {
        lock.withLock { recordedResponseIDs }
    }

    func sendNearby(
        response: CompanionBridgeResponse,
        context: CompanionBridgeRequestContext
    ) -> Bool {
        _ = context
        lock.withLock {
            recordedRoutes.append("nearby")
            recordedResponseIDs.append(response.id)
        }
        return nearbyResult
    }

    func sendRelay(
        response: CompanionBridgeResponse,
        context: CompanionBridgeRequestContext
    ) async -> Bool {
        _ = context
        lock.withLock {
            recordedRoutes.append("relay")
            recordedResponseIDs.append(response.id)
        }
        return relayResult
    }
}

private actor SettingsUpdateRecorder {
    private(set) var threadIDs: [String] = []

    func record(threadID: String, model: String?, reasoningEffort: String?) {
        _ = model
        _ = reasoningEffort
        threadIDs.append(threadID)
    }
}

private final class MessageSendRecorder: @unchecked Sendable {
    struct Call: Equatable {
        var prompt: String
        var threadID: String
        var action: CodexSendAction
        var expectedTurnID: String?
        var attachments: [CodexFollowerAttachment]
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
            calls.append(Call(
                prompt: prompt,
                threadID: threadID,
                action: action,
                expectedTurnID: expectedTurnID,
                attachments: attachments
            ))
        }
        return .sent
    }
}
