import Foundation
import MultipeerConnectivity

typealias CompanionThreadSettingsUpdater = @Sendable (
    _ threadID: String,
    _ model: String?,
    _ reasoningEffort: String?
) async -> CodexAppServerSendOutcome

typealias CompanionTaskMessageSender = @Sendable (
    _ prompt: String,
    _ threadID: String,
    _ cwd: String?,
    _ action: CodexSendAction,
    _ expectedTurnID: String?,
    _ clientMessageID: String,
    _ attachments: [CodexFollowerAttachment]
) async -> CodexAppServerSendOutcome

typealias CompanionAccountProfileUsageReader = @Sendable (
    _ profile: CodexAccountProfile
) throws -> CodexUsageSnapshot

typealias CompanionAccountProfileResetConsumer = @Sendable (
    _ profile: CodexAccountProfile,
    _ creditID: String,
    _ idempotencyKey: UUID
) throws -> CodexResetConsumeOutcome

typealias CompanionThreadAccountProfileIDProvider = @Sendable (_ threadID: String) -> UUID?

typealias CompanionThreadAccountHandoffSubmitter = @Sendable (
    _ threadID: String,
    _ rolloutURL: URL,
    _ hasActiveTurn: Bool,
    _ profile: CodexAccountProfile
) throws -> CodexThreadAccountHandoffResult

struct CompanionBridgeRequestContext: @unchecked Sendable {
    var deviceID: String?
    var nearbyPeer: MCPeerID?
    var relayGeneration: UUID?

    init(
        deviceID: String? = nil,
        nearbyPeer: MCPeerID? = nil,
        relayGeneration: UUID? = nil
    ) {
        self.deviceID = deviceID
        self.nearbyPeer = nearbyPeer
        self.relayGeneration = relayGeneration
    }
}

typealias CompanionNearbyLiveEventSender = @Sendable (
    _ event: CompanionBridgeServerEvent,
    _ deviceID: String
) -> Bool

typealias CompanionRelayLiveEventSender = @Sendable (
    _ event: CompanionBridgeServerEvent,
    _ deviceID: String,
    _ generation: UUID?
) async -> Bool

typealias CompanionNearbyResponseSender = @Sendable (
    _ response: CompanionBridgeResponse,
    _ context: CompanionBridgeRequestContext
) -> Bool

typealias CompanionRelayResponseSender = @Sendable (
    _ response: CompanionBridgeResponse,
    _ context: CompanionBridgeRequestContext
) async -> Bool

final class CodexCompanionMobileBridgeServer: NSObject {
    static let subagentHistoryLimit = CompanionBridgeProtocol.maximumPageSize

    private struct RelayEndpoint {
        var generation: UUID
        var url: URL
        var record: CompanionPairingRecord
        var connection: CompanionRelayConnection
    }

    private static let installationIDKey = "CodexCompanion.macInstallationID.v1"

    private let peerID: MCPeerID
    private let macDeviceID: String
    private lazy var session = MCSession(
        peer: peerID,
        securityIdentity: nil,
        encryptionPreference: .required
    )
    private lazy var advertiser = MCNearbyServiceAdvertiser(
        peer: peerID,
        discoveryInfo: [
            "protocol": String(CompanionBridgeProtocol.version),
            "deviceID": macDeviceID,
        ],
        serviceType: CompanionBridgeProtocol.serviceType
    )
    private let archive: CodexMobileTaskArchive
    private let capabilityService: CodexAppServerCapabilityService
    private let goalControlService: any CodexGoalControlling
    private let onDeviceChatService: any OnDeviceChatServing
    private let openAIChatService: any OpenAIChatServing
    private let lumoChatService: any LumoChatServing
    private let openAIAPIKeyProvider: () -> String?
    private let lumoAPIKeyProvider: () -> String?
    private let threadSettingsUpdater: CompanionThreadSettingsUpdater
    private let taskMessageSender: CompanionTaskMessageSender
    private let accountProfileProvider: CodexAccountProfileProvider
    private let threadAccountProfileIDProvider: CompanionThreadAccountProfileIDProvider
    private let threadAccountHandoffSubmitter: CompanionThreadAccountHandoffSubmitter
    private let accountProfileUsageReader: CompanionAccountProfileUsageReader
    private let accountProfileResetConsumer: CompanionAccountProfileResetConsumer
    private let taskStreamBroker: any CodexTaskStreamBrokerServing
    private let presencePetCatalogService: CompanionPresencePetCatalogService
    private let attachmentUploadStore: CompanionIncomingAttachmentUploadStore
    private let nearbyLiveEventSenderOverride: CompanionNearbyLiveEventSender?
    private let relayLiveEventSenderOverride: CompanionRelayLiveEventSender?
    private let nearbyResponseSenderOverride: CompanionNearbyResponseSender?
    private let relayResponseSenderOverride: CompanionRelayResponseSender?
    private let historyLoadCoordinator = CompanionHistoryLoadCoordinator()
    private let idempotentRequestCoordinator = CompanionBridgeRequestCoordinator()
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let pairingCoordinator: CompanionPairingCoordinator
    private let relaySequenceStore = CompanionRelaySequenceStore()
    private let lifecycleLock = NSLock()
    private let authorizationLock = NSLock()
    private let relayLock = NSLock()
    private let relayAuditLogThrottle = CompanionRelayAuditLogThrottle()
    private var isRunning = false
    private var authorizedDeviceIDByPeerName: [String: String] = [:]
    private var pendingPairingByPeerName: [String: CompanionBridgeInvitation] = [:]
    private var relayEndpointsByDeviceID: [String: RelayEndpoint] = [:]
    private var relayReplayWindowsByDeviceID: [String: CompanionBridgeReplayWindow] = [:]
    private var notificationTokens: [NSObjectProtocol] = []
    private let requestQueue = DispatchQueue(
        label: "com.silverfire.codexcompanion.mobile-bridge",
        qos: .userInitiated
    )

    init(
        archive: CodexMobileTaskArchive = CodexMobileTaskArchive(),
        capabilityService: CodexAppServerCapabilityService = CodexAppServerCapabilityService(),
        goalControlService: any CodexGoalControlling = CodexAppServerControlService.shared,
        onDeviceChatService: any OnDeviceChatServing = OnDeviceChatServiceFactory.make(),
        openAIChatService: any OpenAIChatServing = OpenAIChatService(),
        lumoChatService: any LumoChatServing = LumoChatService(),
        openAIAPIKeyProvider: @escaping () -> String? = { OpenAIAPIKeyStore().load() },
        lumoAPIKeyProvider: @escaping () -> String? = { LumoAPIKeyStore().load() },
        pairingCoordinator: CompanionPairingCoordinator = .shared,
        threadSettingsUpdater: @escaping CompanionThreadSettingsUpdater = { threadID, model, reasoningEffort in
            await CodexFollowerIPCTransport().updateThreadSettings(
                threadID: threadID,
                model: model,
                reasoningEffort: reasoningEffort
            )
        },
        taskMessageSender: @escaping CompanionTaskMessageSender = {
            prompt, threadID, cwd, action, expectedTurnID, clientMessageID, attachments in
            await CodexAppServerSender().submit(
                prompt: prompt,
                threadID: threadID,
                cwd: cwd,
                action: action,
                expectedTurnID: expectedTurnID,
                clientMessageID: clientMessageID,
                onQueued: {},
                attachments: attachments
            )
        },
        accountProfileProvider: @escaping CodexAccountProfileProvider = { profileID in
            CodexAccountProfileStore().profiles.first { $0.id == profileID }
        },
        threadAccountProfileIDProvider: @escaping CompanionThreadAccountProfileIDProvider = { threadID in
            CodexThreadAccountProfileBindingStore().profileID(for: threadID)
        },
        threadAccountHandoffSubmitter: CompanionThreadAccountHandoffSubmitter? = nil,
        accountProfileUsageReader: @escaping CompanionAccountProfileUsageReader = { profile in
            try CodexAccountProfileUsageService().readUsage(for: profile)
        },
        accountProfileResetConsumer: @escaping CompanionAccountProfileResetConsumer = {
            profile, creditID, idempotencyKey in
            try CodexAccountProfileUsageService().consumeReset(
                for: profile,
                creditID: creditID,
                idempotencyKey: idempotencyKey
            )
        },
        taskStreamBroker: (any CodexTaskStreamBrokerServing)? = nil,
        presencePetCatalogService: CompanionPresencePetCatalogService = CompanionPresencePetCatalogService(),
        attachmentUploadStore: CompanionIncomingAttachmentUploadStore = CompanionIncomingAttachmentUploadStore(),
        nearbyLiveEventSender: CompanionNearbyLiveEventSender? = nil,
        relayLiveEventSender: CompanionRelayLiveEventSender? = nil,
        nearbyResponseSender: CompanionNearbyResponseSender? = nil,
        relayResponseSender: CompanionRelayResponseSender? = nil
    ) {
        let computerName = Host.current().localizedName?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        peerID = MCPeerID(displayName: computerName?.isEmpty == false ? computerName! : "Codex Companion Mac")
        macDeviceID = Self.makeInstallationID()
        self.archive = archive
        self.capabilityService = capabilityService
        self.goalControlService = goalControlService
        self.onDeviceChatService = onDeviceChatService
        self.openAIChatService = openAIChatService
        self.lumoChatService = lumoChatService
        self.openAIAPIKeyProvider = openAIAPIKeyProvider
        self.lumoAPIKeyProvider = lumoAPIKeyProvider
        self.pairingCoordinator = pairingCoordinator
        self.threadSettingsUpdater = threadSettingsUpdater
        self.taskMessageSender = taskMessageSender
        self.accountProfileProvider = accountProfileProvider
        self.threadAccountProfileIDProvider = threadAccountProfileIDProvider
        self.threadAccountHandoffSubmitter = threadAccountHandoffSubmitter ?? {
            threadID, rolloutURL, hasActiveTurn, profile in
            try CodexThreadAccountHandoffService().handoff(
                threadID: threadID,
                rolloutURL: rolloutURL,
                hasActiveTurn: hasActiveTurn,
                to: profile
            )
        }
        self.accountProfileUsageReader = accountProfileUsageReader
        self.accountProfileResetConsumer = accountProfileResetConsumer
        let taskStreamArchive = archive
        self.taskStreamBroker = taskStreamBroker ?? CodexTaskStreamBroker(
            clientFactory: { threadID in
                do {
                    let context = try taskStreamArchive.accountHandoffContext(
                        threadID: threadID
                    )
                    CodexAccountRuntimeDiagnostics.append(
                        "task-stream source=rollout-tail thread=\(threadID)"
                    )
                    return CodexRolloutTaskEventClient(
                        rolloutURL: context.rolloutURL,
                        hasActiveTurn: context.hasActiveTurn
                    )
                } catch {
                    let endpoint = try CodexThreadSourceProfileResolver()
                        .resolveTaskStreamEndpoint(for: threadID)
                    CodexAccountRuntimeDiagnostics.append(
                        "task-stream source=profile-daemon-fallback thread=\(threadID)"
                    )
                    return CodexAppServerTaskEventClient(endpoint: endpoint)
                }
            }
        )
        self.presencePetCatalogService = presencePetCatalogService
        self.attachmentUploadStore = attachmentUploadStore
        nearbyLiveEventSenderOverride = nearbyLiveEventSender
        relayLiveEventSenderOverride = relayLiveEventSender
        nearbyResponseSenderOverride = nearbyResponseSender
        relayResponseSenderOverride = relayResponseSender
        encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .millisecondsSince1970
        decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .millisecondsSince1970
        super.init()
        session.delegate = self
        advertiser.delegate = self
    }

    func start() {
        let shouldStart = lifecycleLock.withLock {
            guard !isRunning else { return false }
            isRunning = true
            return true
        }
        guard shouldStart else { return }

        session.delegate = self
        advertiser.delegate = self
        advertiser.startAdvertisingPeer()
        observeRelayConfiguration()
        synchronizeRelayConnections()
        CodexSendLog.append("mobile bridge advertising peer=\(peerID.displayName)")
    }

    func stop() {
        Task { await taskStreamBroker.stop() }
        let shouldStop = lifecycleLock.withLock {
            guard isRunning else { return false }
            isRunning = false
            return true
        }
        guard shouldStop else { return }

        advertiser.stopAdvertisingPeer()
        advertiser.delegate = nil
        session.disconnect()
        session.delegate = nil
        stopObservingRelayConfiguration()
        stopRelayConnections()
        authorizationLock.withLock {
            authorizedDeviceIDByPeerName.removeAll()
            pendingPairingByPeerName.removeAll()
        }
        CodexSendLog.append("mobile bridge stopped")
    }

    func resumeAfterWake() {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        let connections = relayLock.withLock {
            relayEndpointsByDeviceID.values.map(\.connection)
        }
        for connection in connections {
            Task { await connection.resumeAfterWake() }
        }
        synchronizeRelayConnections()
        CodexSendLog.append(
            "mobile bridge recovering relay connections after system wake"
        )
    }

    private func receive(_ data: Data, from peer: MCPeerID) {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        guard isAuthorizedOrPairing(peer) else {
            CodexSendLog.append("mobile bridge rejected unauthorized data peer=\(peer.displayName)")
            return
        }
        requestQueue.async { [weak self] in
            guard let self else { return }
            let request: CompanionBridgeRequest
            do {
                request = try decoder.decode(CompanionBridgeRequest.self, from: data)
            } catch {
                CodexSendLog.append("mobile bridge rejected undecodable request peer=\(peer.displayName)")
                return
            }
            Task {
                let context = self.requestContext(for: peer)
                let response = await self.handle(
                    request,
                    context: context
                )
                await self.deliverResponse(response, context: context)
            }
        }
    }

    func handle(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        await handle(request, context: CompanionBridgeRequestContext())
    }

    func handle(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard request.protocolVersion == CompanionBridgeProtocol.version else {
            return .failure(
                for: request,
                code: "protocol_mismatch",
                message: "Update Codex Companion on the Mac and iPhone."
            )
        }

        do {
            if request.operation != .handshake,
               let pairingPeer = context.nearbyPeer,
               isPairing(pairingPeer) {
                return .failure(
                    for: request,
                    code: "pairing_incomplete",
                    message: "Finish pairing before using this Mac."
                )
            }
            switch request.operation {
            case .handshake:
                let presencePets = await presencePetCatalogService.refresh()
                if let pairingPeer = context.nearbyPeer,
                   let invitation = pendingPairing(pairingPeer) {
                    let record = try pairingCoordinator.completePairing(invitation)
                    authorize(pairingPeer, deviceID: record.deviceID)
                    synchronizeRelayConnections()
                    return .success(
                        for: request,
                        macName: peerID.displayName,
                        macDeviceID: macDeviceID,
                        pairingSecret: record.secret,
                        relayURLString: CompanionRelaySettings.configuredURL()?.absoluteString,
                        features: [.taskStreamV1, .presencePetPackageV1, .attachmentUploadV1],
                        selectedDesktopPetID: presencePets.selectedDesktopPetID,
                        presencePetCatalog: presencePets.catalog
                    )
                }
                return .success(
                    for: request,
                    macName: peerID.displayName,
                    macDeviceID: macDeviceID,
                    relayURLString: CompanionRelaySettings.configuredURL()?.absoluteString,
                    features: [.taskStreamV1, .presencePetPackageV1, .attachmentUploadV1],
                    selectedDesktopPetID: presencePets.selectedDesktopPetID,
                    presencePetCatalog: presencePets.catalog
                )
            case .listTasks:
                let page = try archive.tasks(cursor: request.cursor, limit: request.limit)
                let accountAnnotatedTasks = page.tasks.map { task in
                    var task = task
                    guard let profileID = threadAccountProfileIDProvider(task.id) else {
                        return task
                    }
                    task.accountProfileID = profileID
                    task.accountProfileLabel = accountProfileProvider(profileID)?.label
                    return task
                }
                let runtimeThreadIDsByTaskID = Dictionary(uniqueKeysWithValues:
                    accountAnnotatedTasks.map { ($0.id, archive.runtimeThreadID(for: $0.id)) }
                )
                let runtimeGoals = (try? goalControlService.readGoals(
                    threadIDs: Array(Set(runtimeThreadIDsByTaskID.values))
                )) ?? [:]
                var goals: [String: CodexGoalSnapshot?] = [:]
                for (taskID, runtimeThreadID) in runtimeThreadIDsByTaskID {
                    goals.updateValue(runtimeGoals[runtimeThreadID] ?? nil, forKey: taskID)
                }
                return .success(
                    for: request,
                    tasks: Self.attachingGoals(goals, to: accountAnnotatedTasks),
                    nextCursor: page.nextCursor
                )
            case .loadMessages:
                guard let threadID = request.threadID else {
                    return .failure(for: request, code: "missing_thread", message: "Choose a task first.")
                }
                let cursor = request.cursor
                let limit = min(
                    CompanionBridgeProtocol.maximumPageSize,
                    max(1, request.limit ?? CompanionBridgeProtocol.defaultMessagePageSize)
                )
                let archive = archive
                let snapshot = try await historyLoadCoordinator.load(
                    key: CompanionHistoryLoadKey(
                        threadID: threadID,
                        cursor: cursor,
                        limit: limit
                    )
                ) {
                    let page = try archive.messages(
                        threadID: threadID,
                        cursor: cursor,
                        limit: limit
                    )
                    let timeline = try archive.timeline(
                        threadID: threadID,
                        cursor: cursor,
                        limit: limit
                    )
                    let subagentFamily = try archive.subagentFamily(
                        threadID: threadID,
                        limit: Self.subagentHistoryLimit
                    )
                    return CompanionHistorySnapshot(
                        messages: page.messages,
                        nextCursor: page.nextCursor,
                        timelineItems: timeline.items,
                        revision: timeline.revision,
                        timelineNextCursor: timeline.nextCursor,
                        mainThreadID: subagentFamily.mainThreadID,
                        subagents: subagentFamily.subagents,
                        contextUsage: timeline.contextUsage
                    )
                }
                return .success(
                    for: request,
                    messages: snapshot.messages,
                    nextCursor: snapshot.nextCursor,
                    threadID: threadID,
                    mainThreadID: snapshot.mainThreadID,
                    timelineItems: snapshot.timelineItems,
                    revision: snapshot.revision,
                    timelineNextCursor: snapshot.timelineNextCursor,
                    subagents: snapshot.subagents,
                    contextUsage: snapshot.contextUsage
                )
            case .sendMessage:
                return await idempotentRequestCoordinator.response(for: request.id) { [weak self] in
                    guard let self else {
                        return .failure(
                            for: request,
                            code: "bridge_stopped",
                            message: "Codex Companion stopped before the message could be sent."
                        )
                    }
                    return await self.sendMessage(request, context: context)
                }
            case .respondToApproval:
                return await respondToApproval(request)
            case .createTask:
                return await idempotentRequestCoordinator.response(for: request.id) { [weak self] in
                    guard let self else {
                        return .failure(
                            for: request,
                            code: "bridge_stopped",
                            message: "Codex Companion stopped before the task could be created."
                        )
                    }
                    return await self.createTask(request, context: context)
                }
            case .switchTaskAccount:
                return switchTaskAccount(request)
            case .loadCapabilities:
                let capabilities = try capabilityService.load(cwd: request.cwd)
                return .success(for: request, capabilities: capabilities)
            case .sendCasualChat:
                return await sendCasualChat(request, context: context)
            case .loadUsage:
                return loadUsage(request)
            case .consumeUsageReset:
                return consumeUsageReset(request)
            case .createGoal:
                return createGoal(request)
            case .resumeGoal:
                return resumeGoal(request)
            case .updateGoal:
                return updateGoal(request)
            case .subscribeTaskStream:
                return await subscribeTaskStream(request, context: context)
            case .unsubscribeTaskStream:
                return await unsubscribeTaskStream(request, context: context)
            case .beginAttachmentUpload:
                return await beginAttachmentUpload(request, context: context)
            case .uploadAttachmentChunk:
                return await uploadAttachmentChunk(request, context: context)
            case .cancelAttachmentUpload:
                return await cancelAttachmentUpload(request, context: context)
            case .loadPresencePetManifest:
                return await loadPresencePetManifest(request)
            case .loadPresencePetChunk:
                return await loadPresencePetChunk(request)
            }
        } catch {
            return .failure(
                for: request,
                code: "archive_error",
                message: error.localizedDescription
            )
        }
    }

    private func beginAttachmentUpload(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let deviceID = context.deviceID,
              let uploadID = request.attachmentUploadID,
              let attachmentID = request.attachmentID,
              let kind = request.attachmentKind,
              let filename = request.attachmentFilename,
              let byteCount = request.attachmentByteCount
        else {
            return .failure(
                for: request,
                code: "invalid_attachment_upload",
                message: "The attachment upload metadata is incomplete."
            )
        }
        let attachment = CompanionBridgeAttachment(
            id: attachmentID,
            kind: kind,
            filename: filename,
            mimeType: request.attachmentMimeType,
            data: Data(),
            byteCount: byteCount,
            uploadID: uploadID
        )
        do {
            let progress = try await attachmentUploadStore.begin(
                uploadID: uploadID,
                attachment: attachment,
                deviceID: deviceID
            )
            return .success(for: request, attachmentUploadProgress: progress)
        } catch {
            return attachmentUploadFailure(for: request, error: error)
        }
    }

    private func uploadAttachmentChunk(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let deviceID = context.deviceID,
              let uploadID = request.attachmentUploadID,
              let offset = request.attachmentChunkOffset,
              let data = request.attachmentChunkData
        else {
            return .failure(
                for: request,
                code: "invalid_attachment_upload",
                message: "The attachment transfer chunk is incomplete."
            )
        }
        do {
            let progress = try await attachmentUploadStore.append(
                uploadID: uploadID,
                deviceID: deviceID,
                offset: offset,
                data: data
            )
            return .success(for: request, attachmentUploadProgress: progress)
        } catch {
            return attachmentUploadFailure(for: request, error: error)
        }
    }

    private func cancelAttachmentUpload(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let deviceID = context.deviceID,
              let uploadID = request.attachmentUploadID
        else {
            return .failure(
                for: request,
                code: "invalid_attachment_upload",
                message: "The attachment upload identifier is missing."
            )
        }
        do {
            try await attachmentUploadStore.cancel(uploadID: uploadID, deviceID: deviceID)
            return .success(for: request, message: "Attachment upload cancelled.")
        } catch {
            return attachmentUploadFailure(for: request, error: error)
        }
    }

    private func attachmentUploadFailure(
        for request: CompanionBridgeRequest,
        error: Error
    ) -> CompanionBridgeResponse {
        .failure(
            for: request,
            code: "invalid_attachment_upload",
            message: error.localizedDescription
        )
    }

    private func loadPresencePetManifest(
        _ request: CompanionBridgeRequest
    ) async -> CompanionBridgeResponse {
        guard
            let packageID = request.presencePetPackageID?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !packageID.isEmpty,
            let contentHash = request.presencePetContentHash?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !contentHash.isEmpty
        else {
            return .failure(
                for: request,
                code: "invalid_presence_pet_request",
                message: "Choose a Companion pet package first."
            )
        }
        do {
            let manifest = try await presencePetCatalogService.manifest(
                packageID: packageID,
                contentHash: contentHash
            )
            return .success(for: request, presencePetManifest: manifest)
        } catch {
            return presencePetFailure(for: request, error: error)
        }
    }

    private func loadPresencePetChunk(
        _ request: CompanionBridgeRequest
    ) async -> CompanionBridgeResponse {
        guard
            let packageID = request.presencePetPackageID?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !packageID.isEmpty,
            let contentHash = request.presencePetContentHash?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !contentHash.isEmpty,
            let fileName = request.presencePetFileName?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !fileName.isEmpty,
            let offset = request.presencePetOffset,
            let length = request.presencePetLength
        else {
            return .failure(
                for: request,
                code: "invalid_presence_pet_request",
                message: "The Companion pet download request is incomplete."
            )
        }
        do {
            let chunk = try await presencePetCatalogService.chunk(
                packageID: packageID,
                contentHash: contentHash,
                fileName: fileName,
                offset: offset,
                requestedLength: length
            )
            return .success(for: request, presencePetChunk: chunk)
        } catch {
            return presencePetFailure(for: request, error: error)
        }
    }

    private func presencePetFailure(
        for request: CompanionBridgeRequest,
        error: Error
    ) -> CompanionBridgeResponse {
        let catalogError = error as? CompanionPresencePetCatalogError
        let code: String
        switch catalogError {
        case .unknownPackage, .invalidPackage:
            code = "presence_pet_not_found"
        case .staleContentHash:
            code = "stale_presence_pet"
        case .invalidFileName, .unsafePath:
            code = "invalid_presence_pet_file"
        case .invalidRange:
            code = "invalid_presence_pet_range"
        case nil:
            code = "presence_pet_unavailable"
        }
        return .failure(
            for: request,
            code: code,
            message: error.localizedDescription
        )
    }

    private func subscribeTaskStream(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let deviceID = context.deviceID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !deviceID.isEmpty
        else {
            return .failure(
                for: request,
                code: "unauthorized_device",
                message: "Pair this iPhone with Codex Companion before streaming a task."
            )
        }
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty
        else {
            return .failure(
                for: request,
                code: "missing_thread",
                message: "Choose a task first."
            )
        }
        guard let subscriptionID = request.subscriptionID else {
            return .failure(
                for: request,
                code: "missing_subscription",
                message: "The task stream request is incomplete."
            )
        }

        let canonicalThreadID = archive.canonicalThreadID(for: threadID)
        let runtimeThreadID = archive.runtimeThreadID(for: threadID)
        do {
            let acceptedID = try await taskStreamBroker.subscribe(
                deviceID: deviceID,
                threadID: runtimeThreadID,
                subscriptionID: subscriptionID,
                onEvent: { [weak self] event in
                    var event = event
                    event.threadID = canonicalThreadID
                    await self?.sendLiveEvent(
                        event,
                        deviceID: deviceID,
                        relayGeneration: context.relayGeneration
                    )
                }
            )
            return .success(
                for: request,
                message: "Live task updates connected.",
                threadID: canonicalThreadID,
                subscriptionID: acceptedID
            )
        } catch {
            return .failure(
                for: request,
                code: "stream_unavailable",
                message: error.localizedDescription
            )
        }
    }

    private func unsubscribeTaskStream(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let deviceID = context.deviceID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !deviceID.isEmpty
        else {
            return .failure(
                for: request,
                code: "unauthorized_device",
                message: "Pair this iPhone with Codex Companion before changing a task stream."
            )
        }
        guard let subscriptionID = request.streamID ?? request.subscriptionID else {
            return .failure(
                for: request,
                code: "missing_subscription",
                message: "The task stream request is incomplete."
            )
        }

        await taskStreamBroker.unsubscribe(
            deviceID: deviceID,
            subscriptionID: subscriptionID
        )
        return .success(
            for: request,
            message: "Live task updates disconnected.",
            subscriptionID: subscriptionID
        )
    }

    private func sendCasualChat(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        let text = request.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let requestedAttachments = request.attachments ?? []
        guard !text.isEmpty || !requestedAttachments.isEmpty else {
            return .failure(for: request, code: "invalid_message", message: "Enter a message first.")
        }

        let provider = request.chatProvider ?? .onDevice
        if !requestedAttachments.isEmpty, provider != .onDevice {
            let providerName = provider == .openAIAPI ? "OpenAI API" : "Lumo API"
            return .failure(
                for: request,
                code: "chat_attachments_unsupported",
                message: "\(providerName) chat does not support Companion attachments yet. Choose On-device or remove the attachment."
            )
        }
        let attachments: [CompanionBridgeAttachment]
        do {
            attachments = try await resolveCasualChatAttachments(
                requestedAttachments,
                requestID: request.id,
                context: context
            )
        } catch {
            return .failure(
                for: request,
                code: "invalid_attachment",
                message: error.localizedDescription
            )
        }
        var liveSequence: UInt64 = 0
        do {
            let agent = CompanionBridgeChatAgent.builtIns.first {
                $0.id == request.chatAgentID
            } ?? CompanionBridgeChatAgent.builtIns[0]
            let prompt = """
            Mode: \(agent.name)
            \(agent.promptInstruction)

            User request:
            \(text)
            """

            let stream: AsyncThrowingStream<CompanionChatStreamEvent, Error>
            switch provider {
            case .onDevice:
                stream = onDeviceChatService.stream(
                    prompt: prompt,
                    attachments: attachments,
                    authorizationMode: .remoteClient
                )
            case .openAIAPI:
                guard let apiKey = openAIAPIKeyProvider() else {
                    return .failure(
                        for: request,
                        code: "missing_openai_api_key",
                        message: "Add an OpenAI API key in Codex Companion Settings on the Mac. ChatGPT subscriptions and API billing are separate."
                    )
                }
                let model = request.chatModelID.flatMap(ChatGPTModel.init(rawValue:)) ?? .gpt56Luna
                stream = openAIChatService.stream(
                    prompt: prompt,
                    model: model,
                    apiKey: apiKey
                )
            case .lumoAPI:
                guard let apiKey = lumoAPIKeyProvider() else {
                    return .failure(
                        for: request,
                        code: "missing_lumo_api_key",
                        message: "Add a Lumo API key in Codex Companion Settings on the Mac."
                    )
                }
                let model = request.chatModelID.flatMap(LumoModel.init(rawValue:)) ?? .automatic
                stream = lumoChatService.stream(
                    prompt: prompt,
                    model: model,
                    apiKey: apiKey
                )
            }

            let streamID = request.streamID
            let deviceID = context.deviceID?
                .trimmingCharacters(in: .whitespacesAndNewlines)
            let canPublishLiveEvents = streamID != nil && deviceID?.isEmpty == false
            let turnID = request.id.uuidString
            let itemID = "casual-chat-assistant-\(turnID)"
            var completion: CompanionChatStreamCompletion?

            for try await event in stream {
                let liveEvent: CompanionBridgeLiveEvent?
                switch event {
                case .started:
                    liveSequence += 1
                    liveEvent = CompanionBridgeLiveEvent(
                        channel: .casualChat,
                        streamID: streamID ?? request.id,
                        sequence: liveSequence,
                        turnID: turnID,
                        kind: .turnStarted
                    )
                case .assistantDelta(let delta):
                    liveSequence += 1
                    liveEvent = CompanionBridgeLiveEvent(
                        channel: .casualChat,
                        streamID: streamID ?? request.id,
                        sequence: liveSequence,
                        turnID: turnID,
                        itemID: itemID,
                        kind: .assistantDelta,
                        text: delta
                    )
                case .completed(let finalCompletion):
                    completion = finalCompletion
                    liveSequence += 1
                    liveEvent = CompanionBridgeLiveEvent(
                        channel: .casualChat,
                        streamID: streamID ?? request.id,
                        sequence: liveSequence,
                        turnID: turnID,
                        itemID: itemID,
                        kind: .turnCompleted
                    )
                }

                if canPublishLiveEvents,
                   let liveEvent,
                   let deviceID {
                    await sendLiveEvent(
                        liveEvent,
                        deviceID: deviceID,
                        relayGeneration: context.relayGeneration
                    )
                }
            }

            guard let completion else {
                throw CompanionChatStreamError.missingCompletion
            }
            return .success(
                for: request,
                chatMessage: CompanionBridgeMessage(
                    id: UUID().uuidString,
                    role: .assistant,
                    text: completion.text,
                    createdAt: Date()
                )
            )
        } catch {
            let errorCode: String
            let errorMessage: String
            switch provider {
            case .onDevice:
                let presentation = OnDeviceChatFailurePolicy.bridgePresentation(
                    for: error
                )
                errorCode = presentation.code
                errorMessage = presentation.message
            case .openAIAPI:
                errorCode = "openai_chat_unavailable"
                errorMessage = error.localizedDescription
            case .lumoAPI:
                errorCode = "lumo_chat_unavailable"
                errorMessage = error.localizedDescription
            }
            if let streamID = request.streamID,
               let deviceID = context.deviceID?
                    .trimmingCharacters(in: .whitespacesAndNewlines),
               !deviceID.isEmpty {
                liveSequence += 1
                await sendLiveEvent(
                    CompanionBridgeLiveEvent(
                        channel: .casualChat,
                        streamID: streamID,
                        sequence: liveSequence,
                        turnID: request.id.uuidString,
                        kind: .failed,
                        text: errorMessage,
                        errorCode: errorCode
                    ),
                    deviceID: deviceID,
                    relayGeneration: context.relayGeneration
                )
            }
            return .failure(
                for: request,
                code: errorCode,
                message: errorMessage
            )
        }
    }

    private func loadUsage(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        do {
            let profile: CodexAccountProfile?
            let snapshot: CodexUsageSnapshot
            if let profileID = request.accountProfileID {
                guard let requestedProfile = accountProfileProvider(profileID) else {
                    return unknownAccountProfileFailure(for: request)
                }
                profile = requestedProfile
                snapshot = try accountProfileUsageReader(requestedProfile)
            } else {
                profile = nil
                snapshot = try CodexAppServerControlService.shared.readRateLimits(
                    as: CodexUsageSnapshot.self
                )
            }
            return .success(
                for: request,
                usageSnapshot: CompanionBridgeUsageSnapshot(
                    snapshot: snapshot,
                    accountProfileID: profile?.id,
                    accountProfileLabel: profile?.label
                )
            )
        } catch {
            return .failure(
                for: request,
                code: "usage_unavailable",
                message: error.localizedDescription
            )
        }
    }

    private func switchTaskAccount(
        _ request: CompanionBridgeRequest
    ) -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty
        else {
            return .failure(for: request, code: "missing_thread", message: "Choose a task first.")
        }
        guard let profileID = request.accountProfileID,
              let profile = accountProfileProvider(profileID)
        else {
            return unknownAccountProfileFailure(for: request)
        }

        do {
            let context = try archive.accountHandoffContext(threadID: threadID)
            let sourceRuntimeThreadID = archive.runtimeThreadID(for: threadID)
            let result = try threadAccountHandoffSubmitter(
                threadID,
                context.rolloutURL,
                context.hasActiveTurn,
                profile
            )
            guard !result.threadID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  result.runtimeThreadID != sourceRuntimeThreadID,
                  !result.runtimeThreadID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                  result.profileID == profile.id,
                  result.rolloutURL.standardizedFileURL
                    != context.rolloutURL.standardizedFileURL
            else {
                return .failure(
                    for: request,
                    code: "account_handoff_mismatch",
                    message: "Codex did not create the expected runtime continuation, so the account was not changed."
                )
            }
            return .success(
                for: request,
                message: "Task continued with \(profile.label).",
                threadID: result.threadID,
                accountProfileID: profile.id,
                accountProfileLabel: profile.label
            )
        } catch CodexThreadAccountHandoffError.activeTurn {
            return .failure(
                for: request,
                code: "account_handoff_active",
                message: CodexThreadAccountHandoffError.activeTurn.localizedDescription
            )
        } catch {
            return .failure(
                for: request,
                code: "account_handoff_failed",
                message: error.localizedDescription
            )
        }
    }

    private func consumeUsageReset(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        guard let creditID = request.resetCreditID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !creditID.isEmpty,
              let idempotencyKey = request.idempotencyKey
        else {
            return .failure(
                for: request,
                code: "invalid_reset",
                message: "Choose an available Codex reset first."
            )
        }

        do {
            let profile: CodexAccountProfile?
            let outcome: CodexResetConsumeOutcome
            if let profileID = request.accountProfileID {
                guard let requestedProfile = accountProfileProvider(profileID) else {
                    return unknownAccountProfileFailure(for: request)
                }
                profile = requestedProfile
                outcome = try accountProfileResetConsumer(
                    requestedProfile,
                    creditID,
                    idempotencyKey
                )
            } else {
                profile = nil
                outcome = try CodexAppServerControlService.shared.consumeResetCredit(
                    creditID: creditID,
                    idempotencyKey: idempotencyKey
                )
            }
            let message: String
            switch outcome {
            case .reset:
                if let profile {
                    message = "Codex usage reset applied for \(profile.label)."
                } else {
                    message = "Codex usage reset applied."
                }
            case .nothingToReset:
                message = "There is currently no Codex limit to reset."
            case .noCredit:
                message = "That Codex reset is no longer available."
            case .alreadyRedeemed:
                message = "That Codex reset was already used."
            }
            let refreshed: CodexUsageSnapshot?
            if let profile {
                refreshed = try? accountProfileUsageReader(profile)
            } else {
                refreshed = try? CodexAppServerControlService.shared.readRateLimits(
                    as: CodexUsageSnapshot.self
                )
            }
            let bridgeSnapshot = refreshed.map {
                CompanionBridgeUsageSnapshot(
                    snapshot: $0,
                    accountProfileID: profile?.id,
                    accountProfileLabel: profile?.label
                )
            }
            return .success(
                for: request,
                message: message,
                usageSnapshot: bridgeSnapshot
            )
        } catch {
            return .failure(
                for: request,
                code: "reset_failed",
                message: error.localizedDescription
            )
        }
    }

    private func unknownAccountProfileFailure(
        for request: CompanionBridgeRequest
    ) -> CompanionBridgeResponse {
        .failure(
            for: request,
            code: "unknown_account_profile",
            message: "That Codex account is no longer available on this Mac. Refresh accounts and choose another one."
        )
    }

    private func createGoal(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty,
              let objective = request.goalObjective?.trimmingCharacters(in: .whitespacesAndNewlines),
              !objective.isEmpty,
              request.goalTokenBudget.map({ $0 > 0 }) ?? true
        else {
            return .failure(
                for: request,
                code: "invalid_goal",
                message: "Choose a task and enter a valid goal objective first."
            )
        }

        do {
            let goal = try goalControlService.createGoal(
                threadID: archive.runtimeThreadID(for: threadID),
                objective: objective,
                tokenBudget: request.goalTokenBudget
            )
            return .success(
                for: request,
                message: "Goal created.",
                goal: CompanionBridgeGoal(goal)
            )
        } catch {
            return .failure(
                for: request,
                code: "goal_create_failed",
                message: error.localizedDescription
            )
        }
    }

    private func resumeGoal(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty
        else {
            return .failure(for: request, code: "invalid_goal", message: "Choose a goal first.")
        }

        do {
            let goal = try goalControlService.resumeGoal(
                threadID: archive.runtimeThreadID(for: threadID)
            )
            return .success(
                for: request,
                message: "Goal resumed.",
                goal: CompanionBridgeGoal(goal)
            )
        } catch {
            return .failure(
                for: request,
                code: "goal_resume_failed",
                message: error.localizedDescription
            )
        }
    }

    private func updateGoal(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty,
              let objective = request.goalObjective?.trimmingCharacters(in: .whitespacesAndNewlines),
              !objective.isEmpty
        else {
            return .failure(
                for: request,
                code: "invalid_goal",
                message: "Choose a goal and enter its updated objective first."
            )
        }

        do {
            let goal = try goalControlService.updateGoal(
                threadID: archive.runtimeThreadID(for: threadID),
                objective: objective
            )
            return .success(
                for: request,
                message: "Goal updated.",
                goal: CompanionBridgeGoal(goal)
            )
        } catch {
            return .failure(
                for: request,
                code: "goal_update_failed",
                message: error.localizedDescription
            )
        }
    }

    static func attachingGoals(
        _ goals: [String: CodexGoalSnapshot?],
        to tasks: [CompanionBridgeTask]
    ) -> [CompanionBridgeTask] {
        tasks.map { task in
            guard let goal = goals[task.id] ?? nil else { return task }
            var task = task
            task.goal = CompanionBridgeGoal(goal)
            return task
        }
    }

    private func sendMessage(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty,
              let text = request.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty
        else {
            return .failure(for: request, code: "invalid_message", message: "Enter a message first.")
        }
        let action: CodexSendAction = request.sendAction == .steer ? .steer : .reply
        let runtimeThreadID = archive.runtimeThreadID(for: threadID)
        let task = try? archive.tasks(cursor: nil, limit: CompanionBridgeProtocol.maximumPageSize)
            .tasks.first(where: { $0.id == threadID })
        let model = request.model?.trimmingCharacters(in: .whitespacesAndNewlines)
        let reasoningEffort = request.reasoningEffort?.trimmingCharacters(in: .whitespacesAndNewlines)
        var retainedCurrentSettings = false
        if model?.isEmpty == false || reasoningEffort?.isEmpty == false {
            let settingsOutcome = await threadSettingsUpdater(
                runtimeThreadID,
                model,
                reasoningEffort
            )
            if settingsOutcome != .sent {
                retainedCurrentSettings = true
                CodexSendLog.append(
                    "mobile bridge retained current task settings thread=\(runtimeThreadID) "
                        + "outcome=\(String(describing: settingsOutcome))"
                )
            }
        }
        let stagedAttachments: [CodexFollowerAttachment]
        do {
            stagedAttachments = try await stageTaskAttachments(
                request.attachments ?? [],
                requestID: request.id,
                context: context
            )
        } catch {
            return .failure(
                for: request,
                code: "invalid_attachment",
                message: error.localizedDescription
            )
        }
        let outcome = await taskMessageSender(
            text,
            runtimeThreadID,
            request.cwd ?? task?.cwd,
            action,
            request.expectedTurnID ?? task?.activeTurnID,
            request.clientMessageID,
            stagedAttachments
        )
        switch outcome {
        case .sent:
            let message: String
            if retainedCurrentSettings {
                message = action == .steer
                    ? "Steered task using its current model."
                    : "Reply sent using the task's current model."
            } else {
                message = action == .steer ? "Steered task." : "Reply sent."
            }
            return .success(for: request, message: message)
        case .noActiveTurn:
            return .failure(for: request, code: "no_active_turn", message: "This task is not currently running, so it cannot be steered.")
        case .threadNotLoaded:
            return .failure(
                for: request,
                code: "thread_not_loaded",
                message: "The Mac could not load this task in the background. Your message was not sent."
            )
        case .sharedDaemonUnavailable:
            return .failure(for: request, code: "native_transport_unavailable", message: "ChatGPT's local task connection is unavailable. Your message was not lost.")
        case .timedOut:
            return .failure(for: request, code: "timed_out", message: "Codex did not confirm the message in time.")
        case .failed:
            return .failure(for: request, code: "send_failed", message: "Codex did not accept the message.")
        }
    }

    private func respondToApproval(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        guard let threadID = request.threadID,
              let bridgeDecision = request.approvalDecision
        else {
            return .failure(for: request, code: "invalid_approval", message: "That approval request is unavailable.")
        }
        let decision: CodexApprovalDecision
        switch bridgeDecision {
        case .approveOnce: decision = .approveOnce
        case .approveSimilar: decision = .approveSimilarCommands
        case .decline: decision = .decline
        }
        let outcome = await CodexAppServerApprovalSender().respond(
            threadID: archive.runtimeThreadID(for: threadID),
            decision: decision
        )
        switch outcome {
        case .approved:
            return .success(for: request, message: "Approval sent.")
        case .declined:
            return .success(for: request, message: "Request declined.")
        case .requestNotFound:
            return .failure(for: request, code: "approval_gone", message: "That approval request is no longer active.")
        case .sharedDaemonUnavailable:
            return .failure(
                for: request,
                code: "native_transport_unavailable",
                message: "ChatGPT's native approval connection is unavailable. Refresh the request, then retry."
            )
        case .timedOut:
            return .failure(for: request, code: "approval_timed_out", message: "The approval response could not be confirmed.")
        case .failed:
            return .failure(for: request, code: "approval_failed", message: "Codex did not accept the approval response.")
        }
    }

    private func createTask(
        _ request: CompanionBridgeRequest,
        context: CompanionBridgeRequestContext
    ) async -> CompanionBridgeResponse {
        guard let prompt = request.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !prompt.isEmpty
        else {
            return .failure(for: request, code: "invalid_message", message: "Describe the new task first.")
        }
        let stagedAttachments: [CodexFollowerAttachment]
        do {
            stagedAttachments = try await stageTaskAttachments(
                request.attachments ?? [],
                requestID: request.id,
                context: context
            )
        } catch {
            return .failure(
                for: request,
                code: "invalid_attachment",
                message: error.localizedDescription
            )
        }
        let outcome = await CodexAppServerTaskCreator().create(
            prompt: prompt,
            cwd: request.cwd,
            model: request.model,
            reasoningEffort: request.reasoningEffort,
            skillName: request.skillName,
            skillPath: request.skillPath,
            accountProfileID: request.accountProfileID,
            attachments: stagedAttachments,
            clientMessageID: request.clientMessageID
        )
        return Self.createTaskResponse(for: request, outcome: outcome)
    }

    private func stageTaskAttachments(
        _ attachments: [CompanionBridgeAttachment],
        requestID: UUID,
        context: CompanionBridgeRequestContext
    ) async throws -> [CodexFollowerAttachment] {
        guard Set(attachments.map(\.id)).count == attachments.count else {
            throw CompanionIncomingAttachmentUploadError.invalidUpload
        }
        let uploaded = attachments.filter { $0.uploadID != nil }
        let inline = attachments.filter { $0.uploadID == nil }
        var staged = try CompanionIncomingAttachmentStore().stage(
            inline,
            requestID: requestID
        )
        if !uploaded.isEmpty {
            guard let deviceID = context.deviceID else {
                throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
            }
            staged.append(contentsOf: try await attachmentUploadStore.stage(
                uploaded,
                requestID: requestID,
                deviceID: deviceID
            ))
        }
        let stagedByID = Dictionary(uniqueKeysWithValues: staged.map { ($0.id, $0) })
        return attachments.compactMap { stagedByID[$0.id] }
    }

    private func resolveCasualChatAttachments(
        _ attachments: [CompanionBridgeAttachment],
        requestID: UUID,
        context: CompanionBridgeRequestContext
    ) async throws -> [CompanionBridgeAttachment] {
        guard Set(attachments.map(\.id)).count == attachments.count else {
            throw CompanionIncomingAttachmentUploadError.invalidUpload
        }
        let uploaded = attachments.filter { $0.uploadID != nil }
        let inline = attachments.filter { $0.uploadID == nil }
        try CompanionIncomingAttachmentStore.validate(inline)

        var resolvedByID = Dictionary(uniqueKeysWithValues: inline.map { ($0.id, $0) })
        if !uploaded.isEmpty {
            guard let deviceID = context.deviceID else {
                throw CompanionIncomingAttachmentUploadError.unauthorizedUpload
            }
            let staged = try await attachmentUploadStore.stage(
                uploaded,
                requestID: requestID,
                deviceID: deviceID
            )
            let stagedByID = Dictionary(uniqueKeysWithValues: staged.map { ($0.id, $0) })
            for attachment in uploaded {
                guard let stagedAttachment = stagedByID[attachment.id] else {
                    throw CompanionIncomingAttachmentUploadError.invalidUpload
                }
                resolvedByID[attachment.id] = CompanionBridgeAttachment(
                    id: attachment.id,
                    kind: attachment.kind,
                    filename: attachment.filename,
                    mimeType: attachment.mimeType,
                    data: Data(),
                    byteCount: attachment.payloadByteCount,
                    uploadID: attachment.uploadID,
                    localFileURL: URL(fileURLWithPath: stagedAttachment.path)
                )
            }
        }
        return try attachments.map { attachment in
            guard let resolved = resolvedByID[attachment.id] else {
                throw CompanionIncomingAttachmentUploadError.invalidUpload
            }
            return resolved
        }
    }

    static func createTaskResponse(
        for request: CompanionBridgeRequest,
        outcome: CodexAppServerTaskCreationOutcome
    ) -> CompanionBridgeResponse {
        switch outcome {
        case .created(let threadID):
            return .success(
                for: request,
                message: "New Codex task started.",
                threadID: threadID
            )
        case .sharedDaemonUnavailable:
            return .failure(
                for: request,
                code: "native_transport_unavailable",
                message: "The selected Codex account service is unavailable. The task was not started."
            )
        case .timedOut:
            return .failure(
                for: request,
                code: "timed_out",
                message: "Codex did not confirm the new task in time."
            )
        case .failed:
            return .failure(
                for: request,
                code: "create_failed",
                message: "Codex did not start the new task."
            )
        }
    }

    private func sendLiveEvent(
        _ liveEvent: CompanionBridgeLiveEvent,
        deviceID: String,
        relayGeneration: UUID?
    ) async {
        let serverEvent = CompanionBridgeServerEvent(liveEvent: liveEvent)
        let sentNearby: Bool
        if let nearbyLiveEventSenderOverride {
            sentNearby = nearbyLiveEventSenderOverride(serverEvent, deviceID)
        } else {
            sentNearby = sendNearbyLiveEvent(serverEvent, deviceID: deviceID)
        }
        guard !sentNearby else { return }

        if let relayLiveEventSenderOverride {
            _ = await relayLiveEventSenderOverride(
                serverEvent,
                deviceID,
                relayGeneration
            )
        } else {
            _ = await sendRelayLiveEvent(
                serverEvent,
                deviceID: deviceID,
                generation: relayGeneration
            )
        }
    }

    private func sendNearbyLiveEvent(
        _ event: CompanionBridgeServerEvent,
        deviceID: String
    ) -> Bool {
        guard let peer = session.connectedPeers.first(where: {
            authorizedDeviceID(for: $0) == deviceID
        }) else { return false }

        do {
            let eventEncoder = JSONEncoder()
            eventEncoder.dateEncodingStrategy = .millisecondsSince1970
            let data = try eventEncoder.encode(event)
            try session.send(data, toPeers: [peer], with: .reliable)
            return true
        } catch {
            CodexSendLog.append(
                "mobile bridge live event failed peer=\(peer.displayName) error=\(error.localizedDescription)"
            )
            return false
        }
    }

    private func sendRelayLiveEvent(
        _ event: CompanionBridgeServerEvent,
        deviceID: String,
        generation: UUID?
    ) async -> Bool {
        let endpoint = relayLock.withLock { relayEndpointsByDeviceID[deviceID] }
        guard let endpoint,
              generation == nil || endpoint.generation == generation
        else { return false }

        do {
            let channelID = CompanionBridgeSecurity.channelID(secret: endpoint.record.secret)
            let sequence = relaySequenceStore.next(
                channelID: channelID,
                senderID: macDeviceID
            )
            let envelope = try CompanionBridgeSecurity.seal(
                event,
                secret: endpoint.record.secret,
                senderID: macDeviceID,
                sequence: sequence
            )
            try await endpoint.connection.send(envelope)
            return true
        } catch {
            CodexSendLog.append(
                "mobile relay live event failed device=\(deviceID) error=\(error.localizedDescription)"
            )
            return false
        }
    }

    private func send(_ response: CompanionBridgeResponse, to peer: MCPeerID) -> Bool {
        do {
            let data = try encoder.encode(response)
            try session.send(data, toPeers: [peer], with: .reliable)
            return true
        } catch {
            CodexSendLog.append("mobile bridge response failed peer=\(peer.displayName) error=\(error.localizedDescription)")
            return false
        }
    }

    private func observeRelayConfiguration() {
        guard notificationTokens.isEmpty else { return }
        let center = NotificationCenter.default
        notificationTokens = [
            center.addObserver(
                forName: CompanionPairingCoordinator.pairingStateDidChange,
                object: nil,
                queue: nil
            ) { [weak self] _ in
                self?.synchronizeRelayConnections()
            },
            center.addObserver(
                forName: CompanionRelaySettings.didChange,
                object: nil,
                queue: nil
            ) { [weak self] _ in
                self?.synchronizeRelayConnections()
            },
        ]
    }

    private func stopObservingRelayConfiguration() {
        let center = NotificationCenter.default
        notificationTokens.forEach(center.removeObserver)
        notificationTokens.removeAll()
    }

    private func synchronizeRelayConnections() {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        let records = pairingCoordinator.trustedRecords()
        let configuredURL = CompanionRelaySettings.configuredURL()
        var stopped: [CompanionRelayConnection] = []
        var started: [CompanionRelayConnection] = []

        relayLock.withLock {
            let recordsByID = Dictionary(
                uniqueKeysWithValues: records.map { ($0.deviceID, $0) }
            )
            let removedIDs = relayEndpointsByDeviceID.compactMap { deviceID, endpoint in
                configuredURL == nil
                    || endpoint.url != configuredURL
                    || recordsByID[deviceID]?.secret != endpoint.record.secret
                    ? deviceID
                    : nil
            }
            for deviceID in removedIDs {
                guard let endpoint = relayEndpointsByDeviceID.removeValue(forKey: deviceID)
                else { continue }
                stopped.append(endpoint.connection)
                relayReplayWindowsByDeviceID.removeValue(forKey: deviceID)
            }

            guard let configuredURL else { return }
            for record in records where relayEndpointsByDeviceID[record.deviceID] == nil {
                let generation = UUID()
                let connection = CompanionRelayConnection(
                    url: configuredURL,
                    channelID: CompanionBridgeSecurity.channelID(secret: record.secret),
                    endpointID: macDeviceID,
                    stateHandler: { [weak self] state in
                        self?.handleRelayState(
                            state,
                            deviceID: record.deviceID,
                            generation: generation
                        )
                    },
                    envelopeHandler: { [weak self] envelope in
                        self?.receiveRelayEnvelope(
                            envelope,
                            deviceID: record.deviceID,
                            generation: generation
                        )
                    },
                    failureHandler: { [weak self] reason in
                        self?.handleRelayFailure(
                            reason,
                            deviceID: record.deviceID,
                            generation: generation
                        )
                    }
                )
                relayEndpointsByDeviceID[record.deviceID] = RelayEndpoint(
                    generation: generation,
                    url: configuredURL,
                    record: record,
                    connection: connection
                )
                started.append(connection)
            }
        }

        for connection in stopped {
            Task { await connection.stop() }
        }
        for connection in started {
            Task { await connection.start() }
        }
    }

    private func stopRelayConnections() {
        let connections = relayLock.withLock {
            let result = relayEndpointsByDeviceID.values.map(\.connection)
            relayEndpointsByDeviceID.removeAll()
            relayReplayWindowsByDeviceID.removeAll()
            return result
        }
        for connection in connections {
            Task { await connection.stop() }
        }
    }

    private func handleRelayState(
        _ state: CompanionRelayConnection.State,
        deviceID: String,
        generation: UUID
    ) {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        let isCurrent = relayLock.withLock {
            relayEndpointsByDeviceID[deviceID]?.generation == generation
        }
        guard isCurrent else { return }
        CodexSendLog.append("mobile relay device=\(deviceID) state=\(state)")
    }

    private func handleRelayFailure(
        _ reason: String,
        deviceID: String,
        generation: UUID
    ) {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        let isCurrent = relayLock.withLock {
            relayEndpointsByDeviceID[deviceID]?.generation == generation
        }
        guard isCurrent else { return }
        CodexSendLog.append(
            "mobile relay transport failed device=\(deviceID) reason="
                + CompanionRelayAudit.sanitizedFailure(reason)
        )
    }

    private func receiveRelayEnvelope(
        _ envelope: CompanionBridgeEncryptedEnvelope,
        deviceID: String,
        generation: UUID
    ) {
        requestQueue.async { [weak self] in
            self?.processRelayEnvelope(
                envelope,
                deviceID: deviceID,
                generation: generation
            )
        }
    }

    private func processRelayEnvelope(
        _ envelope: CompanionBridgeEncryptedEnvelope,
        deviceID: String,
        generation: UUID
    ) {
        guard lifecycleLock.withLock({ isRunning }) else { return }
        let endpoint = relayLock.withLock { relayEndpointsByDeviceID[deviceID] }
        guard let endpoint,
              endpoint.generation == generation,
              envelope.senderID == deviceID
        else { return }

        let request: CompanionBridgeRequest
        do {
            request = try CompanionBridgeSecurity.open(
                envelope,
                secret: endpoint.record.secret,
                as: CompanionBridgeRequest.self
            )
        } catch {
            CodexSendLog.append("mobile relay rejected invalid envelope device=\(deviceID)")
            return
        }

        let acceptsSequence = relayLock.withLock {
            var replayWindow = relayReplayWindowsByDeviceID[deviceID]
                ?? CompanionBridgeReplayWindow()
            let accepted = replayWindow.accept(
                sequence: envelope.sequence,
                from: envelope.senderID
            )
            if accepted {
                relayReplayWindowsByDeviceID[deviceID] = replayWindow
            }
            return accepted
        }
        guard acceptsSequence else {
            CodexSendLog.append("mobile relay rejected replay device=\(deviceID)")
            return
        }

        CodexSendLog.append(
            "mobile relay request operation=\(request.operation.rawValue)"
        )

        Task { [weak self] in
            guard let self else { return }
            let context = CompanionBridgeRequestContext(
                deviceID: deviceID,
                relayGeneration: generation
            )
            let response = await self.handle(
                request,
                context: context
            )
            await self.deliverResponse(response, context: context)
        }
    }

    func deliverResponse(
        _ response: CompanionBridgeResponse,
        context: CompanionBridgeRequestContext
    ) async {
        if context.nearbyPeer != nil {
            if sendNearbyResponse(response, context: context) { return }
            CodexSendLog.append(
                "mobile bridge response fallback nearby-to-relay operation="
                    + response.operation.rawValue
            )
            if await sendRelayResponse(response, context: context) { return }
        } else {
            if await sendRelayResponse(response, context: context) { return }
            CodexSendLog.append(
                "mobile bridge response fallback relay-to-nearby operation="
                    + response.operation.rawValue
            )
            if sendNearbyResponse(response, context: context) { return }
        }

        CodexSendLog.append(
            "mobile bridge response unavailable operation=" + response.operation.rawValue
        )
    }

    private func sendNearbyResponse(
        _ response: CompanionBridgeResponse,
        context: CompanionBridgeRequestContext
    ) -> Bool {
        if let nearbyResponseSenderOverride {
            return nearbyResponseSenderOverride(response, context)
        }

        let peer = context.nearbyPeer ?? context.deviceID.flatMap { deviceID in
            session.connectedPeers.first(where: {
                authorizedDeviceID(for: $0) == deviceID
            })
        }
        guard let peer, session.connectedPeers.contains(peer) else { return false }
        return send(response, to: peer)
    }

    private func sendRelayResponse(
        _ response: CompanionBridgeResponse,
        context: CompanionBridgeRequestContext
    ) async -> Bool {
        if let relayResponseSenderOverride {
            return await relayResponseSenderOverride(response, context)
        }
        guard let deviceID = context.deviceID else { return false }
        return await sendRelay(
            response,
            deviceID: deviceID,
            generation: context.relayGeneration
        )
    }

    private func sendRelay(
        _ response: CompanionBridgeResponse,
        deviceID: String,
        generation: UUID?
    ) async -> Bool {
        let endpoint = relayLock.withLock { relayEndpointsByDeviceID[deviceID] }
        guard let endpoint,
              generation == nil || endpoint.generation == generation
        else { return false }
        do {
            let channelID = CompanionBridgeSecurity.channelID(secret: endpoint.record.secret)
            let sequence = relaySequenceStore.next(
                channelID: channelID,
                senderID: macDeviceID
            )
            let envelope = try CompanionBridgeSecurity.seal(
                response,
                secret: endpoint.record.secret,
                senderID: macDeviceID,
                sequence: sequence
            )
            try await endpoint.connection.send(envelope)
            CodexSendLog.append(
                "mobile relay response operation=\(response.operation.rawValue) "
                    + "succeeded=\(response.succeeded)"
            )
            return true
        } catch {
            CodexSendLog.append(
                "mobile relay response failed device=\(deviceID) error=\(error.localizedDescription)"
            )
            return false
        }
    }

    private func isAuthorizedOrPairing(_ peer: MCPeerID) -> Bool {
        authorizationLock.withLock {
            authorizedDeviceIDByPeerName[peer.displayName] != nil
                || pendingPairingByPeerName[peer.displayName] != nil
        }
    }

    private func isPairing(_ peer: MCPeerID) -> Bool {
        authorizationLock.withLock { pendingPairingByPeerName[peer.displayName] != nil }
    }

    private func pendingPairing(_ peer: MCPeerID) -> CompanionBridgeInvitation? {
        authorizationLock.withLock { pendingPairingByPeerName[peer.displayName] }
    }

    func authorize(_ peer: MCPeerID, deviceID: String) {
        authorizationLock.withLock {
            pendingPairingByPeerName.removeValue(forKey: peer.displayName)
            authorizedDeviceIDByPeerName[peer.displayName] = deviceID
        }
    }

    func requestContext(for peer: MCPeerID) -> CompanionBridgeRequestContext {
        CompanionBridgeRequestContext(
            deviceID: authorizedDeviceID(for: peer),
            nearbyPeer: peer
        )
    }

    private func authorizedDeviceID(for peer: MCPeerID) -> String? {
        authorizationLock.withLock {
            authorizedDeviceIDByPeerName[peer.displayName]
        }
    }

    private func clearAuthorization(_ peer: MCPeerID) {
        authorizationLock.withLock {
            pendingPairingByPeerName.removeValue(forKey: peer.displayName)
            authorizedDeviceIDByPeerName.removeValue(forKey: peer.displayName)
        }
    }

    private static func makeInstallationID() -> String {
        let defaults = UserDefaults.standard
        if let existing = defaults.string(forKey: installationIDKey), !existing.isEmpty {
            return existing
        }
        let created = UUID().uuidString
        defaults.set(created, forKey: installationIDKey)
        return created
    }
}

extension CodexCompanionMobileBridgeServer: MCNearbyServiceAdvertiserDelegate {
    func advertiser(
        _ advertiser: MCNearbyServiceAdvertiser,
        didReceiveInvitationFromPeer peerID: MCPeerID,
        withContext context: Data?,
        invitationHandler: @escaping (Bool, MCSession?) -> Void
    ) {
        guard let context,
              let invitation = try? decoder.decode(CompanionBridgeInvitation.self, from: context)
        else {
            invitationHandler(false, nil)
            CodexSendLog.append(
                "mobile bridge rejected invitation reason=malformed contextBytes=\(context?.count ?? 0)"
            )
            return
        }

        let decision = pairingCoordinator.invitationDecision(invitation)
        switch decision {
        case .acceptTrusted:
            authorize(peerID, deviceID: invitation.deviceID)
            invitationHandler(true, session)
            CodexSendLog.append("mobile bridge accepted trusted device=\(invitation.deviceID)")
        case .acceptPairing:
            authorizationLock.withLock {
                pendingPairingByPeerName[peerID.displayName] = invitation
            }
            invitationHandler(true, session)
            CodexSendLog.append("mobile bridge accepted pairing device=\(invitation.deviceID)")
        case .rejectVersion, .rejectExpired, .rejectAuthentication, .rejectUnpaired:
            invitationHandler(false, nil)
            if relayAuditLogThrottle.shouldRecord(
                key: "rejected-invitation:\(invitation.deviceID)"
            ) {
                let auditSummary = CompanionBridgeInvitationAudit.rejectionSummary(
                    decision: decision,
                    invitation: invitation,
                    trustedRecordFound: pairingCoordinator.trustedRecord(
                        for: invitation.deviceID
                    ) != nil,
                    now: Date()
                )
                CodexSendLog.append(
                    "mobile bridge rejected invitation device=\(invitation.deviceID) \(auditSummary)"
                )
            }
        }
    }

    func advertiser(
        _ advertiser: MCNearbyServiceAdvertiser,
        didNotStartAdvertisingPeer error: Error
    ) {
        CodexSendLog.append("mobile bridge advertising failed error=\(error.localizedDescription)")
    }
}

extension CodexCompanionMobileBridgeServer: MCSessionDelegate {
    func session(_ session: MCSession, peer peerID: MCPeerID, didChange state: MCSessionState) {
        CodexSendLog.append("mobile bridge peer=\(peerID.displayName) state=\(state.rawValue)")
        if state == .notConnected {
            clearAuthorization(peerID)
        }
    }

    func session(_ session: MCSession, didReceive data: Data, fromPeer peerID: MCPeerID) {
        receive(data, from: peerID)
    }

    func session(
        _ session: MCSession,
        didReceive stream: InputStream,
        withName streamName: String,
        fromPeer peerID: MCPeerID
    ) {}

    func session(
        _ session: MCSession,
        didStartReceivingResourceWithName resourceName: String,
        fromPeer peerID: MCPeerID,
        with progress: Progress
    ) {}

    func session(
        _ session: MCSession,
        didFinishReceivingResourceWithName resourceName: String,
        fromPeer peerID: MCPeerID,
        at localURL: URL?,
        withError error: Error?
    ) {}

    #if os(iOS)
    func session(
        _ session: MCSession,
        didReceiveCertificate certificate: [Any]?,
        fromPeer peerID: MCPeerID,
        certificateHandler: @escaping (Bool) -> Void
    ) {
        certificateHandler(true)
    }
    #endif
}

private extension NSLock {
    func withLock<Value>(_ operation: () throws -> Value) rethrows -> Value {
        lock()
        defer { unlock() }
        return try operation()
    }
}

private extension CompanionBridgeGoal {
    init(_ goal: CodexGoalSnapshot) {
        threadID = goal.threadID
        objective = goal.objective
        status = switch goal.status {
        case .active: .active
        case .paused: .paused
        case .blocked: .blocked
        case .usageLimited: .usageLimited
        case .budgetLimited: .budgetLimited
        case .complete: .complete
        }
        tokenBudget = goal.tokenBudget
        tokensUsed = goal.tokensUsed
        elapsedSeconds = goal.timeUsedSeconds
        createdAt = goal.createdAt
        updatedAt = goal.updatedAt
    }
}
