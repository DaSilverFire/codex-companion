import Foundation
import MultipeerConnectivity

final class CodexCompanionMobileBridgeServer: NSObject {
    private let peerID: MCPeerID
    private lazy var session = MCSession(
        peer: peerID,
        securityIdentity: nil,
        encryptionPreference: .required
    )
    private lazy var advertiser = MCNearbyServiceAdvertiser(
        peer: peerID,
        discoveryInfo: ["protocol": String(CompanionBridgeProtocol.version)],
        serviceType: CompanionBridgeProtocol.serviceType
    )
    private let archive: CodexMobileTaskArchive
    private let capabilityService: CodexAppServerCapabilityService
    private let onDeviceChatService: any OnDeviceChatServing
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let requestQueue = DispatchQueue(
        label: "com.silverfire.codexcompanion.mobile-bridge",
        qos: .userInitiated
    )

    init(
        archive: CodexMobileTaskArchive = CodexMobileTaskArchive(),
        capabilityService: CodexAppServerCapabilityService = CodexAppServerCapabilityService(),
        onDeviceChatService: any OnDeviceChatServing = OnDeviceChatServiceFactory.make()
    ) {
        let computerName = Host.current().localizedName?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        peerID = MCPeerID(displayName: computerName?.isEmpty == false ? computerName! : "Codex Companion Mac")
        self.archive = archive
        self.capabilityService = capabilityService
        self.onDeviceChatService = onDeviceChatService
        encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .millisecondsSince1970
        decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .millisecondsSince1970
        super.init()
        session.delegate = self
        advertiser.delegate = self
    }

    func start() {
        advertiser.startAdvertisingPeer()
        CodexSendLog.append("mobile bridge advertising peer=\(peerID.displayName)")
    }

    func stop() {
        advertiser.stopAdvertisingPeer()
        session.disconnect()
        CodexSendLog.append("mobile bridge stopped")
    }

    private func receive(_ data: Data, from peer: MCPeerID) {
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
                let response = await self.handle(request)
                self.send(response, to: peer)
            }
        }
    }

    private func handle(_ request: CompanionBridgeRequest) async -> CompanionBridgeResponse {
        guard request.protocolVersion == CompanionBridgeProtocol.version else {
            return .failure(
                for: request,
                code: "protocol_mismatch",
                message: "Update Codex Companion on the Mac and iPhone."
            )
        }

        do {
            switch request.operation {
            case .handshake:
                return .success(for: request, macName: peerID.displayName)
            case .listTasks:
                let page = try archive.tasks(cursor: request.cursor, limit: request.limit)
                return .success(
                    for: request,
                    tasks: page.tasks,
                    nextCursor: page.nextCursor
                )
            case .loadMessages:
                guard let threadID = request.threadID else {
                    return .failure(for: request, code: "missing_thread", message: "Choose a task first.")
                }
                let page = try archive.messages(
                    threadID: threadID,
                    cursor: request.cursor,
                    limit: request.limit
                )
                let timeline = try archive.timeline(
                    threadID: threadID,
                    cursor: request.cursor,
                    limit: request.limit
                )
                let subagents = try archive.subagents(parentThreadID: threadID, limit: 8)
                return .success(
                    for: request,
                    messages: page.messages,
                    nextCursor: page.nextCursor,
                    threadID: threadID,
                    timelineItems: timeline.items,
                    revision: timeline.revision,
                    timelineNextCursor: timeline.nextCursor,
                    subagents: subagents,
                    contextUsage: timeline.contextUsage
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
        guard let text = request.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty
        else {
            return .failure(for: request, code: "invalid_message", message: "Enter a message first.")
        }

        let agent = CompanionBridgeChatAgent.builtIns.first {
            $0.id == request.chatAgentID
        } ?? CompanionBridgeChatAgent.builtIns[0]
        let prompt = """
        Mode: \(agent.name)
        \(agent.promptInstruction)

        User request:
        \(text)
        """

        do {
            let answer = try await onDeviceChatService.send(prompt: prompt)
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
            return .failure(
                for: request,
                code: "on_device_chat_unavailable",
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
        let outcome = await CodexAppServerSender().submit(
            prompt: text,
            threadID: threadID,
            cwd: request.cwd ?? task?.cwd,
            action: action,
            expectedTurnID: task?.activeTurnID,
            clientMessageID: UUID().uuidString,
            onQueued: {}
        )
        switch outcome {
        case .sent:
            return .success(for: request, message: action == .steer ? "Steered task." : "Reply sent.")
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
        let outcome = await CodexAppServerTaskCreator().create(
            prompt: prompt,
            cwd: request.cwd,
            model: request.model,
            reasoningEffort: request.reasoningEffort,
            skillName: request.skillName,
            skillPath: request.skillPath
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
}

extension CodexCompanionMobileBridgeServer: MCNearbyServiceAdvertiserDelegate {
    func advertiser(
        _ advertiser: MCNearbyServiceAdvertiser,
        didReceiveInvitationFromPeer peerID: MCPeerID,
        withContext context: Data?,
        invitationHandler: @escaping (Bool, MCSession?) -> Void
    ) {
        invitationHandler(true, session)
        CodexSendLog.append("mobile bridge accepted encrypted peer=\(peerID.displayName)")
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
