import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeChatTests {
    @Test
    func casualChatForwardsAttachmentsToTheOnDeviceModel() async throws {
        let service = RecordingOnDeviceChatService()
        let server = CodexCompanionMobileBridgeServer(onDeviceChatService: service)
        let attachment = CompanionBridgeAttachment(
            kind: .image,
            filename: "shadow.png",
            mimeType: "image/png",
            data: Data([0x89, 0x50, 0x4E, 0x47])
        )
        let request = CompanionBridgeRequest(
            operation: .sendCasualChat,
            text: "What is in this image?",
            chatAgentID: "explain",
            attachments: [attachment]
        )

        let response = await server.handle(request)
        let recorded = await service.recordedRequest()

        #expect(response.succeeded)
        #expect(response.chatMessage?.text == "Attachment received")
        #expect(recorded?.prompt.contains("Mode: Explain") == true)
        #expect(recorded?.attachments == [attachment])
    }

    @Test
    func casualChatAllowsAnAttachmentWithoutTypedText() async {
        let service = RecordingOnDeviceChatService()
        let server = CodexCompanionMobileBridgeServer(onDeviceChatService: service)
        let attachment = CompanionBridgeAttachment(
            kind: .file,
            filename: "notes.txt",
            mimeType: "text/plain",
            data: Data("notes".utf8)
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendCasualChat,
                text: "",
                chatAgentID: "general",
                attachments: [attachment]
            )
        )

        #expect(response.succeeded)
        #expect(await service.recordedRequest()?.attachments == [attachment])
    }

    @Test
    func casualChatRoutesOpenAISelectionWithoutUsingTheOnDeviceModel() async {
        let onDevice = RecordingOnDeviceChatService()
        let openAI = RecordingOpenAIChatService()
        let server = CodexCompanionMobileBridgeServer(
            onDeviceChatService: onDevice,
            openAIChatService: openAI,
            openAIAPIKeyProvider: { "test-openai-key" }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendCasualChat,
                text: "Explain this result",
                chatAgentID: "explain",
                chatProvider: .openAIAPI,
                chatModelID: ChatGPTModel.gpt56Terra.rawValue
            )
        )

        #expect(response.succeeded)
        #expect(response.chatMessage?.text == "OpenAI response")
        #expect(await openAI.recordedRequest()?.model == .gpt56Terra)
        #expect(await openAI.recordedRequest()?.prompt.contains("Mode: Explain") == true)
        #expect(await onDevice.recordedRequest() == nil)
    }

    @Test
    func casualChatRoutesLumoSelectionWithoutUsingTheOnDeviceModel() async {
        let onDevice = RecordingOnDeviceChatService()
        let lumo = RecordingLumoChatService()
        let server = CodexCompanionMobileBridgeServer(
            onDeviceChatService: onDevice,
            lumoChatService: lumo,
            lumoAPIKeyProvider: { "test-lumo-key" }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendCasualChat,
                text: "Plan the work",
                chatAgentID: "plan",
                chatProvider: .lumoAPI,
                chatModelID: LumoModel.thinking.rawValue
            )
        )

        #expect(response.succeeded)
        #expect(response.chatMessage?.text == "Lumo response")
        #expect(await lumo.recordedRequest()?.model == .thinking)
        #expect(await lumo.recordedRequest()?.prompt.contains("Mode: Plan") == true)
        #expect(await onDevice.recordedRequest() == nil)
    }

    @Test
    func casualChatReportsMissingProviderCredentialInsteadOfFallingBack() async {
        let onDevice = RecordingOnDeviceChatService()
        let server = CodexCompanionMobileBridgeServer(
            onDeviceChatService: onDevice,
            openAIAPIKeyProvider: { nil }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .sendCasualChat,
                text: "Do not reroute this",
                chatProvider: .openAIAPI,
                chatModelID: ChatGPTModel.gpt56Luna.rawValue
            )
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "missing_openai_api_key")
        #expect(await onDevice.recordedRequest() == nil)
    }

    @Test
    func capabilityCatalogSeparatesChatModelsFromChatStyles() {
        let models = CodexAppServerCapabilityService.chatModels(
            hasOpenAIKey: true,
            hasLumoKey: false
        )

        #expect(models.first?.provider == .onDevice)
        #expect(models.filter { $0.provider == .openAIAPI }.count == ChatGPTModel.allCases.count)
        #expect(models.filter { $0.provider == .lumoAPI }.count == LumoModel.allCases.count)
        #expect(models.filter { $0.provider == .openAIAPI }.allSatisfy { $0.isAvailable })
        #expect(models.filter { $0.provider == .lumoAPI }.allSatisfy { !$0.isAvailable })
        #expect(!CompanionBridgeChatAgent.builtIns.contains { style in
            models.contains { $0.id == style.id }
        })
    }

    @Test
    func chatProviderAndModelSurviveBridgeRequestRoundTrip() throws {
        let request = CompanionBridgeRequest(
            operation: .sendCasualChat,
            text: "Use the selected provider",
            chatProvider: .openAIAPI,
            chatModelID: ChatGPTModel.gpt56Sol.rawValue
        )

        let encoded = try JSONEncoder().encode(request)
        let decoded = try JSONDecoder().decode(CompanionBridgeRequest.self, from: encoded)

        #expect(decoded.chatProvider == .openAIAPI)
        #expect(decoded.chatModelID == ChatGPTModel.gpt56Sol.rawValue)
    }

    @Test
    func capabilitiesFromAnOlderMacDecodeWithoutChatModels() throws {
        let legacyPayload = Data(
            #"{"models":[],"skills":[],"plugins":[],"chatAgents":[]}"#.utf8
        )

        let capabilities = try JSONDecoder().decode(
            CompanionBridgeCapabilities.self,
            from: legacyPayload
        )

        #expect(capabilities.chatModels == nil)
    }
}

private actor RecordingOnDeviceChatService: OnDeviceChatServing {
    struct Request: Equatable {
        var prompt: String
        var attachments: [CompanionBridgeAttachment]
    }

    private var request: Request?

    func prewarm() async {}

    func send(prompt: String) async throws -> String {
        try await send(prompt: prompt, attachments: [])
    }

    func send(
        prompt: String,
        attachments: [CompanionBridgeAttachment]
    ) async throws -> String {
        request = Request(prompt: prompt, attachments: attachments)
        return "Attachment received"
    }

    func recordedRequest() -> Request? {
        request
    }
}

private actor RecordingOpenAIChatService: OpenAIChatServing {
    struct Request: Equatable {
        var prompt: String
        var model: ChatGPTModel
        var apiKey: String
    }

    private var request: Request?

    func send(prompt: String, model: ChatGPTModel, apiKey: String) async throws -> OpenAIChatResult {
        request = Request(prompt: prompt, model: model, apiKey: apiKey)
        return OpenAIChatResult(text: "OpenAI response", inputTokens: 4, outputTokens: 3)
    }

    func recordedRequest() -> Request? {
        request
    }
}

private actor RecordingLumoChatService: LumoChatServing {
    struct Request: Equatable {
        var prompt: String
        var model: LumoModel
        var apiKey: String
    }

    private var request: Request?

    func send(prompt: String, model: LumoModel, apiKey: String) async throws -> LumoChatResult {
        request = Request(prompt: prompt, model: model, apiKey: apiKey)
        return LumoChatResult(text: "Lumo response", inputTokens: 4, outputTokens: 3)
    }

    func recordedRequest() -> Request? {
        request
    }
}
