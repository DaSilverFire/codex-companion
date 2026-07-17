import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct LumoChatServiceTests {
    @Test
    func publicModelCatalogUsesDocumentedLumoAPIIdentifiers() {
        #expect(LumoModel.allCases == [.automatic, .fast, .thinking])
        #expect(LumoModel.allCases.map(\.title) == [
            "Lumo Auto",
            "Lumo Fast",
            "Lumo Thinking",
        ])
        #expect(LumoModel.allCases.map(\.apiModelID) == [
            "auto",
            "lumo-basic-v1",
            "lumo-plus-v1",
        ])
    }

    @Test
    func persistedSelectionUsesAutomaticRoutingByDefault() {
        #expect(LumoModel.restoringPersistedSelection("thinking") == .thinking)
        #expect(LumoModel.restoringPersistedSelection(nil) == .automatic)
        #expect(LumoModel.restoringPersistedSelection("unknown") == .automatic)
    }

    @Test
    func deliveryCatalogKeepsLumoSeparateFromOpenAI() {
        #expect(ChatGPTDeliveryMode.allCases == [.onDevice, .openAIAPI, .lumoAPI])
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("lumoAPI") == .lumoAPI)
    }

    @Test
    func requestUsesOfficialOpenAICompatibleContract() throws {
        let endpoint = try #require(URL(string: "https://lumo.proton.me/api/ai/v1/chat/completions"))
        let service = LumoChatService(endpoint: endpoint)

        let request = try service.makeRequest(
            prompt: "Explain the moon.",
            model: .thinking,
            apiKey: "lumo-test-key"
        )

        #expect(request.url == endpoint)
        #expect(request.httpMethod == "POST")
        #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer lumo-test-key")
        #expect(request.value(forHTTPHeaderField: "Content-Type") == "application/json")

        let bodyData = try #require(request.httpBody)
        let body = try #require(JSONSerialization.jsonObject(with: bodyData) as? [String: Any])
        #expect(body["model"] as? String == "lumo-plus-v1")
        #expect(body["stream"] as? Bool == false)
        #expect(body["max_tokens"] as? Int == 700)

        let messages = try #require(body["messages"] as? [[String: Any]])
        #expect(messages.count == 1)
        #expect(messages[0]["role"] as? String == "user")
        #expect(messages[0]["content"] as? String == "Explain the moon.")
    }

    @Test
    func decodesChatCompletionTextAndUsage() throws {
        let endpoint = try #require(URL(string: "https://lumo.proton.me/api/ai/v1/chat/completions"))
        let service = LumoChatService(endpoint: endpoint)
        let response = try #require(HTTPURLResponse(
            url: endpoint,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        ))
        let data = Data(
            """
            {
              "choices": [{"message": {"role": "assistant", "content": "The Moon is Earth's satellite."}}],
              "usage": {"prompt_tokens": 8, "completion_tokens": 6, "total_tokens": 14}
            }
            """.utf8
        )

        let result = try service.decodeResponse(data: data, response: response)

        #expect(result.text == "The Moon is Earth's satellite.")
        #expect(result.inputTokens == 8)
        #expect(result.outputTokens == 6)
    }

    @Test
    func surfacesDocumentedErrorMessage() throws {
        let endpoint = try #require(URL(string: "https://lumo.proton.me/api/ai/v1/chat/completions"))
        let service = LumoChatService(endpoint: endpoint)
        let response = try #require(HTTPURLResponse(
            url: endpoint,
            statusCode: 401,
            httpVersion: "HTTP/1.1",
            headerFields: nil
        ))
        let data = Data("{\"error\":{\"message\":\"Invalid API key\"}}".utf8)

        #expect(throws: LumoChatError.self) {
            _ = try service.decodeResponse(data: data, response: response)
        }
    }
}
