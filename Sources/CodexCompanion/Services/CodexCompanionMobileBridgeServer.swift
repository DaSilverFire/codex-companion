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

final class CodexCompanionMobileBridgeServer: NSObject {
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
    private let historyLoadCoordinator = CompanionHistoryLoadCoordinator()
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
        }
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
                let response = await self.handle(request, pairingPeer: peer)
                self.send(response, to: peer)
            }
        }
    }

    func handle(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        await handle(request, pairingPeer: nil)
    }

    private func handle(
        _ request: CompanionBridgeRequest,
        pairingPeer: MCPeerID?
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
               let pairingPeer,
               isPairing(pairingPeer) {
                return .failure(
                    for: request,
                    code: "pairing_incomplete",
                    message: "Finish pairing before using this Mac."
                )
            }
            switch request.operation {
            case .handshake:
                if let pairingPeer,
                   let invitation = pendingPairing(pairingPeer) {
                    let record = try pairingCoordinator.completePairing(invitation)
                    markAuthorized(pairingPeer, deviceID: record.deviceID)
                    synchronizeRelayConnections()
                    return .success(
                        for: request,
                        macName: peerID.displayName,
                        macDeviceID: macDeviceID,
                        pairingSecret: record.secret,
                        relayURLString: CompanionRelaySettings.configuredURL()?.absoluteString
                    )
                }
                return .success(
                    for: request,
                    macName: peerID.displayName,
                    macDeviceID: macDeviceID,
                    relayURLString: CompanionRelaySettings.configuredURL()?.absoluteString
                )
            case .listTasks:
                let page = try archive.tasks(cursor: request.cursor, limit: request.limit)
                let goals = (try? goalControlService.readGoals(
                    threadIDs: page.tasks.map(\.id)
                )) ?? [:]
                return .success(
                    for: request,
                    tasks: Self.attachingGoals(goals, to: page.tasks),
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
                    return CompanionHistorySnapshot(
                        messages: page.messages,
                        nextCursor: page.nextCursor,
                        timelineItems: timeline.items,
                        revision: timeline.revision,
                        timelineNextCursor: timeline.nextCursor,
                        subagents: try archive.subagents(parentThreadID: threadID, limit: 8),
                        contextUsage: timeline.contextUsage
                    )
                }
                return .success(
                    for: request,
                    messages: snapshot.messages,
                    nextCursor: snapshot.nextCursor,
                    threadID: threadID,
                    timelineItems: snapshot.timelineItems,
                    revision: snapshot.revision,
                    timelineNextCursor: snapshot.timelineNextCursor,
                    subagents: snapshot.subagents,
                    contextUsage: snapshot.contextUsage
                )
            case .sendMessage:
                return await sendMessage(request)
            case .respondToApproval:
                return await respondToApproval(request)
            case .createTask:
                return await createTask(request)
            case .loadCapabilities:
                let capabilities = try capabilityService.load(cwd: request.cwd)
                return .success(for: request, capabilities: capabilities)
            case .sendCasualChat:
                return await sendCasualChat(request)
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
            }
        } catch {
            return .failure(
                for: request,
                code: "archive_error",
                message: error.localizedDescription
            )
        }
    }

    private func sendCasualChat(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        let text = request.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let attachments = request.attachments ?? []
        guard !text.isEmpty || !attachments.isEmpty else {
            return .failure(for: request, code: "invalid_message", message: "Enter a message first.")
        }

        let provider = request.chatProvider ?? .onDevice
        do {
            try CompanionIncomingAttachmentStore.validate(attachments)
            let agent = CompanionBridgeChatAgent.builtIns.first {
                $0.id == request.chatAgentID
            } ?? CompanionBridgeChatAgent.builtIns[0]
            let prompt = """
            Mode: \(agent.name)
            \(agent.promptInstruction)

            User request:
            \(text)
            """

            let answer: String
            switch provider {
            case .onDevice:
                answer = try await onDeviceChatService.send(
                    prompt: prompt,
                    attachments: attachments
                )
            case .openAIAPI:
                guard attachments.isEmpty else {
                    return .failure(
                        for: request,
                        code: "chat_attachments_unsupported",
                        message: "OpenAI API chat does not support Companion attachments yet. Choose On-device or remove the attachment."
                    )
                }
                guard let apiKey = openAIAPIKeyProvider() else {
                    return .failure(
                        for: request,
                        code: "missing_openai_api_key",
                        message: "Add an OpenAI API key in Codex Companion Settings on the Mac. ChatGPT subscriptions and API billing are separate."
                    )
                }
                let model = request.chatModelID.flatMap(ChatGPTModel.init(rawValue:)) ?? .gpt56Luna
                answer = try await openAIChatService.send(
                    prompt: prompt,
                    model: model,
                    apiKey: apiKey
                ).text
            case .lumoAPI:
                guard attachments.isEmpty else {
                    return .failure(
                        for: request,
                        code: "chat_attachments_unsupported",
                        message: "Lumo API chat does not support Companion attachments yet. Choose On-device or remove the attachment."
                    )
                }
                guard let apiKey = lumoAPIKeyProvider() else {
                    return .failure(
                        for: request,
                        code: "missing_lumo_api_key",
                        message: "Add a Lumo API key in Codex Companion Settings on the Mac."
                    )
                }
                let model = request.chatModelID.flatMap(LumoModel.init(rawValue:)) ?? .automatic
                answer = try await lumoChatService.send(
                    prompt: prompt,
                    model: model,
                    apiKey: apiKey
                ).text
            }
            return .success(
                for: request,
                chatMessage: CompanionBridgeMessage(
                    id: UUID().uuidString,
                    role: .assistant,
                    text: answer,
                    createdAt: Date()
                )
            )
        } catch {
            let errorCode: String
            switch provider {
            case .onDevice: errorCode = "on_device_chat_unavailable"
            case .openAIAPI: errorCode = "openai_chat_unavailable"
            case .lumoAPI: errorCode = "lumo_chat_unavailable"
            }
            return .failure(
                for: request,
                code: errorCode,
                message: error.localizedDescription
            )
        }
    }

    private func loadUsage(_ request: CompanionBridgeRequest) -> CompanionBridgeResponse {
        do {
            let snapshot = try CodexAppServerControlService.shared.readRateLimits(
                as: CodexUsageSnapshot.self
            )
            return .success(
                for: request,
                usageSnapshot: CompanionBridgeUsageSnapshot(snapshot: snapshot)
            )
        } catch {
            return .failure(
                for: request,
                code: "usage_unavailable",
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
            let outcome = try CodexAppServerControlService.shared.consumeResetCredit(
                creditID: creditID,
                idempotencyKey: idempotencyKey
            )
            let message: String
            switch outcome {
            case .reset:
                message = "Codex usage reset applied."
            case .nothingToReset:
                message = "There is currently no Codex limit to reset."
            case .noCredit:
                message = "That Codex reset is no longer available."
            case .alreadyRedeemed:
                message = "That Codex reset was already used."
            }
            let refreshed = try? CodexAppServerControlService.shared.readRateLimits(
                as: CodexUsageSnapshot.self
            )
            let bridgeSnapshot = refreshed.map {
                CompanionBridgeUsageSnapshot(snapshot: $0)
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
                threadID: threadID,
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
            let goal = try goalControlService.resumeGoal(threadID: threadID)
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
                threadID: threadID,
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

    private func sendMessage(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        guard let threadID = request.threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
              !threadID.isEmpty,
              let text = request.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty
        else {
            return .failure(for: request, code: "invalid_message", message: "Enter a message first.")
        }
        let action: CodexSendAction = request.sendAction == .steer ? .steer : .reply
        let task = try? archive.tasks(cursor: nil, limit: CompanionBridgeProtocol.maximumPageSize)
            .tasks.first(where: { $0.id == threadID })
        let model = request.model?.trimmingCharacters(in: .whitespacesAndNewlines)
        let reasoningEffort = request.reasoningEffort?.trimmingCharacters(in: .whitespacesAndNewlines)
        var retainedCurrentSettings = false
        if model?.isEmpty == false || reasoningEffort?.isEmpty == false {
            let settingsOutcome = await threadSettingsUpdater(threadID, model, reasoningEffort)
            if settingsOutcome != .sent {
                retainedCurrentSettings = true
                CodexSendLog.append(
                    "mobile bridge retained current task settings thread=\(threadID) "
                        + "outcome=\(String(describing: settingsOutcome))"
                )
            }
        }
        let stagedAttachments: [CodexFollowerAttachment]
        do {
            stagedAttachments = try CompanionIncomingAttachmentStore().stage(
                request.attachments ?? [],
                requestID: request.id
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
            threadID,
            request.cwd ?? task?.cwd,
            action,
            task?.activeTurnID,
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
            threadID: threadID,
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

    private func createTask(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        guard let prompt = request.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !prompt.isEmpty
        else {
            return .failure(for: request, code: "invalid_message", message: "Describe the new task first.")
        }
        let stagedAttachments: [CodexFollowerAttachment]
        do {
            stagedAttachments = try CompanionIncomingAttachmentStore().stage(
                request.attachments ?? [],
                requestID: request.id
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
            attachments: stagedAttachments
        )
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
                code: "native_transport_setup_required",
                message: "Restart ChatGPT once after native Companion transport is enabled. The task was not started."
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

    private func send(_ response: CompanionBridgeResponse, to peer: MCPeerID) {
        do {
            let data = try encoder.encode(response)
            try session.send(data, toPeers: [peer], with: .reliable)
        } catch {
            CodexSendLog.append("mobile bridge response failed peer=\(peer.displayName) error=\(error.localizedDescription)")
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
            let response = await self.handle(request, pairingPeer: nil)
            await self.sendRelay(response, deviceID: deviceID, generation: generation)
        }
    }

    private func sendRelay(
        _ response: CompanionBridgeResponse,
        deviceID: String,
        generation: UUID
    ) async {
        let endpoint = relayLock.withLock { relayEndpointsByDeviceID[deviceID] }
        guard let endpoint, endpoint.generation == generation else { return }
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
        } catch {
            CodexSendLog.append(
                "mobile relay response failed device=\(deviceID) error=\(error.localizedDescription)"
            )
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

    private func markAuthorized(_ peer: MCPeerID, deviceID: String) {
        authorizationLock.withLock {
            pendingPairingByPeerName.removeValue(forKey: peer.displayName)
            authorizedDeviceIDByPeerName[peer.displayName] = deviceID
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
            CodexSendLog.append("mobile bridge rejected unpaired peer=\(peerID.displayName)")
            return
        }

        switch pairingCoordinator.invitationDecision(invitation) {
        case .acceptTrusted:
            markAuthorized(peerID, deviceID: invitation.deviceID)
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
                CodexSendLog.append(
                    "mobile bridge rejected invitation device=\(invitation.deviceID)"
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
