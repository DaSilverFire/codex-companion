import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionBridgeRequestTests {
    @Test
    func retryingARequestKeepsTheSameNativeClientMessageID() {
        let id = UUID()
        let first = CompanionBridgeRequest(
            id: id,
            operation: .sendMessage,
            threadID: "thread-1",
            text: "send exactly once",
            sendAction: .reply
        )
        let retry = CompanionBridgeRequest(
            id: id,
            operation: .sendMessage,
            threadID: "thread-1",
            text: "send exactly once",
            sendAction: .reply
        )

        #expect(first.clientMessageID == id.uuidString)
        #expect(retry.clientMessageID == first.clientMessageID)
    }

    @Test
    func goalControlPayloadsRoundTripAcrossTheSharedBridgeContract() throws {
        let request = CompanionBridgeRequest(
            operation: .createGoal,
            threadID: "thread-goal",
            goalObjective: "Finish end-to-end goal controls",
            goalTokenBudget: 120_000
        )
        let decodedRequest = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decodedRequest.operation == .createGoal)
        #expect(decodedRequest.goalObjective == "Finish end-to-end goal controls")
        #expect(decodedRequest.goalTokenBudget == 120_000)

        let goal = CompanionBridgeGoal(
            threadID: "thread-goal",
            objective: "Finish end-to-end goal controls",
            status: .blocked,
            tokenBudget: 120_000,
            tokensUsed: 4_200,
            elapsedSeconds: 90,
            createdAt: 100,
            updatedAt: 200
        )
        let response = CompanionBridgeResponse.success(for: request, goal: goal)
        let decodedResponse = try JSONDecoder().decode(
            CompanionBridgeResponse.self,
            from: JSONEncoder().encode(response)
        )

        #expect(decodedResponse.goal == goal)
        #expect(decodedResponse.operation == .createGoal)
    }

    @Test(arguments: [
        CompanionBridgeOperation.createGoal,
        .resumeGoal,
        .updateGoal,
    ])
    func goalControlOperationsRemainCodable(_ operation: CompanionBridgeOperation) throws {
        let request = CompanionBridgeRequest(operation: operation, threadID: "thread-goal")
        let decoded = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decoded.operation == operation)
    }

    @Test
    func bridgeMessagesDecodeWhenOlderPeersOmitAttachments() throws {
        let data = Data(
            #"{"id":"message-1","role":"assistant","text":"Ready","createdAt":null}"#.utf8
        )

        let message = try JSONDecoder().decode(CompanionBridgeMessage.self, from: data)

        #expect(message.text == "Ready")
        #expect(message.attachments == nil)
    }

    @Test
    func attachmentUploadRequestsPreserveBoundedChunkMetadata() throws {
        let requestID = UUID()
        let uploadID = UUID()
        let attachmentID = UUID()
        let payload = Data(
            """
            {
              "id": "\(requestID.uuidString)",
              "protocolVersion": 1,
              "operation": "uploadAttachmentChunk",
              "attachmentUploadID": "\(uploadID.uuidString)",
              "attachmentID": "\(attachmentID.uuidString)",
              "attachmentChunkOffset": 196608,
              "attachmentChunkData": "YWJj"
            }
            """.utf8
        )

        let request = try JSONDecoder().decode(CompanionBridgeRequest.self, from: payload)
        let encoded = try JSONEncoder().encode(request)
        let object = try #require(
            JSONSerialization.jsonObject(with: encoded) as? [String: Any]
        )

        #expect(request.operation.rawValue == "uploadAttachmentChunk")
        #expect(object["attachmentUploadID"] as? String == uploadID.uuidString)
        #expect(object["attachmentID"] as? String == attachmentID.uuidString)
        #expect((object["attachmentChunkOffset"] as? NSNumber)?.int64Value == 196_608)
        #expect(object["attachmentChunkData"] as? String == "YWJj")
    }

    @Test
    func attachmentUploadResponsesPreserveAcknowledgedOffset() throws {
        let responseID = UUID()
        let payload = Data(
            """
            {
              "id": "\(responseID.uuidString)",
              "protocolVersion": 1,
              "operation": "uploadAttachmentChunk",
              "succeeded": true,
              "attachmentUploadProgress": {
                "nextOffset": 393216,
                "isComplete": false
              }
            }
            """.utf8
        )

        let response = try JSONDecoder().decode(CompanionBridgeResponse.self, from: payload)
        let encoded = try JSONEncoder().encode(response)
        let object = try #require(
            JSONSerialization.jsonObject(with: encoded) as? [String: Any]
        )
        let progress = try #require(
            object["attachmentUploadProgress"] as? [String: Any]
        )

        #expect(response.operation.rawValue == "uploadAttachmentChunk")
        #expect((progress["nextOffset"] as? NSNumber)?.int64Value == 393_216)
        #expect(progress["isComplete"] as? Bool == false)
    }

    @Test
    func accountProfileSelectionSurvivesBridgeRoundTrip() throws {
        let profileID = UUID()
        let request = CompanionBridgeRequest(
            operation: .createTask,
            text: "Create this task on the selected account",
            accountProfileID: profileID
        )

        let decoded = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decoded.accountProfileID == profileID)
    }

    @Test
    func existingTaskAccountHandoffSurvivesBridgeRoundTrip() throws {
        let profileID = UUID()
        let request = CompanionBridgeRequest(
            operation: .switchTaskAccount,
            threadID: "thread-account-handoff",
            accountProfileID: profileID
        )

        let decoded = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decoded.operation == .switchTaskAccount)
        #expect(decoded.threadID == "thread-account-handoff")
        #expect(decoded.accountProfileID == profileID)
    }

    @Test
    func olderCapabilityPayloadDecodesWithoutAccountProfiles() throws {
        let payload = Data(
            #"{"models":[],"skills":[],"plugins":[],"chatAgents":[]}"#.utf8
        )

        let capabilities = try JSONDecoder().decode(
            CompanionBridgeCapabilities.self,
            from: payload
        )

        #expect(capabilities.accountProfiles == nil)
        #expect(capabilities.selectedAccountProfileID == nil)
    }

    @Test
    func unavailableTaskCreationReportsTheAccountServiceInsteadOfRequestingARestart() {
        let request = CompanionBridgeRequest(
            operation: .createTask,
            text: "Create a mobile task"
        )

        let response = CodexCompanionMobileBridgeServer.createTaskResponse(
            for: request,
            outcome: .sharedDaemonUnavailable
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "native_transport_unavailable")
        #expect(response.message?.contains("account service") == true)
        #expect(response.message?.contains("Restart ChatGPT") == false)
    }

    @Test
    func liveSubscriptionPayloadRoundTrips() throws {
        let subscriptionID = UUID()
        let request = CompanionBridgeRequest(
            operation: .subscribeTaskStream,
            threadID: "thread-live",
            subscriptionID: subscriptionID
        )

        let decoded = try JSONDecoder().decode(
            CompanionBridgeRequest.self,
            from: JSONEncoder().encode(request)
        )

        #expect(decoded.operation == .subscribeTaskStream)
        #expect(decoded.threadID == "thread-live")
        #expect(decoded.subscriptionID == subscriptionID)
    }

    @Test
    func unsolicitedLiveEventRoundTripsWithoutARequestResponseID() throws {
        let streamID = UUID()
        let event = CompanionBridgeServerEvent(
            eventID: UUID(),
            liveEvent: CompanionBridgeLiveEvent(
                channel: .task,
                streamID: streamID,
                sequence: 7,
                threadID: "thread-live",
                turnID: "turn-1",
                itemID: "item-1",
                kind: .assistantDelta,
                text: "Hel"
            )
        )

        let decoded = try JSONDecoder().decode(
            CompanionBridgeServerEvent.self,
            from: JSONEncoder().encode(event)
        )

        #expect(decoded == event)
        #expect(decoded.messageType == "server_event")
        #expect(decoded.liveEvent.sequence == 7)
        #expect(decoded.liveEvent.text == "Hel")
    }

    @Test
    func presenceManifestAndChunkRoundTrip() throws {
        let atlasFile = CompanionPresencePetFile(
            name: "atlas.png",
            sha256: String(repeating: "a", count: 64),
            byteCount: 4_096
        )
        let thumbnailFile = CompanionPresencePetFile(
            name: "thumbnail.png",
            sha256: String(repeating: "b", count: 64),
            byteCount: 512
        )
        let manifest = CompanionPresencePetManifest(
            schemaVersion: 1,
            packageID: "shadow-compact-v1",
            petID: "shadow-16",
            displayName: "Shadow",
            assetVersion: "1",
            atlas: CompanionPresencePetAtlas(
                file: atlasFile,
                cellWidth: 144,
                cellHeight: 144,
                columns: 12,
                rows: 3
            ),
            thumbnail: thumbnailFile,
            animations: [
                CompanionPresencePetAnimation(
                    state: .idle,
                    row: 0,
                    frameCount: 12,
                    frameDurationsMilliseconds: Array(repeating: 100, count: 12),
                    posterFrame: 0
                ),
            ],
            contentHash: String(repeating: "c", count: 64)
        )
        let chunk = CompanionPresencePetChunk(
            packageID: manifest.packageID,
            contentHash: manifest.contentHash,
            fileName: atlasFile.name,
            offset: 0,
            data: Data([0, 1, 2, 3]),
            nextOffset: 4,
            isComplete: false
        )
        let request = CompanionBridgeRequest(
            operation: .loadPresencePetChunk,
            presencePetPackageID: manifest.packageID,
            presencePetContentHash: manifest.contentHash,
            presencePetFileName: atlasFile.name,
            presencePetOffset: 0,
            presencePetLength: 196_608
        )
        let response = CompanionBridgeResponse.success(
            for: request,
            features: [.taskStreamV1, .presencePetPackageV1],
            selectedDesktopPetID: "custom:shadow-16",
            presencePetManifest: manifest,
            presencePetChunk: chunk
        )

        let decoded = try JSONDecoder().decode(
            CompanionBridgeResponse.self,
            from: JSONEncoder().encode(response)
        )

        #expect(decoded.features == [.taskStreamV1, .presencePetPackageV1])
        #expect(decoded.selectedDesktopPetID == "custom:shadow-16")
        #expect(decoded.presencePetManifest == manifest)
        #expect(decoded.presencePetChunk == chunk)
    }

    @Test
    func olderHandshakeDecodesWithoutFeatureOrPresenceFields() throws {
        let data = Data(
            #"{"id":"00000000-0000-0000-0000-000000000001","protocolVersion":1,"operation":"handshake","succeeded":true}"#.utf8
        )

        let response = try JSONDecoder().decode(CompanionBridgeResponse.self, from: data)

        #expect(response.features == nil)
        #expect(response.selectedDesktopPetID == nil)
        #expect(response.presencePetCatalog == nil)
        #expect(response.presencePetManifest == nil)
        #expect(response.presencePetChunk == nil)
    }

    @Test
    func stagedAttachmentPathsNeverCrossTheBridge() throws {
        let localURL = URL(fileURLWithPath: "/private/companion/staged/secret.txt")
        let attachment = CompanionBridgeAttachment(
            kind: .file,
            filename: "secret.txt",
            mimeType: "text/plain",
            data: Data(),
            byteCount: 42,
            uploadID: UUID(),
            localFileURL: localURL
        )

        let encoded = try JSONEncoder().encode(attachment)
        let decoded = try JSONDecoder().decode(
            CompanionBridgeAttachment.self,
            from: encoded
        )

        #expect(!String(decoding: encoded, as: UTF8.self).contains(localURL.path))
        #expect(decoded.localFileURL == nil)
    }
}
