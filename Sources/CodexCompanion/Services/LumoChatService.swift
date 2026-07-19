import Foundation

protocol LumoChatServing: Sendable {
    func send(prompt: String, model: LumoModel, apiKey: String) async throws -> LumoChatResult
}

struct LumoChatService: LumoChatServing {
    static let defaultEndpoint = URL(string: "https://lumo.proton.me/api/ai/v1/chat/completions")!

    private let session: URLSession
    let endpoint: URL
    private let maxOutputTokens = 700

    init(session: URLSession = .shared, endpoint: URL = LumoChatService.defaultEndpoint) {
        self.session = session
        self.endpoint = endpoint
    }

    func send(prompt: String, model: LumoModel, apiKey: String) async throws -> LumoChatResult {
        let request = try makeRequest(prompt: prompt, model: model, apiKey: apiKey)
        let (data, response) = try await session.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse else {
            throw LumoChatError.transport("No HTTP response from Lumo.")
        }
        return try decodeResponse(data: data, response: httpResponse)
    }

    func makeRequest(prompt: String, model: LumoModel, apiKey: String) throws -> URLRequest {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(
            LumoChatRequest(
                model: model.apiModelID,
                messages: [LumoChatMessage(role: "user", content: prompt)],
                stream: false,
                maxTokens: maxOutputTokens
            )
        )
        return request
    }

    func decodeResponse(data: Data, response: HTTPURLResponse) throws -> LumoChatResult {
        guard (200..<300).contains(response.statusCode) else {
            if let apiError = try? JSONDecoder().decode(LumoErrorEnvelope.self, from: data) {
                throw LumoChatError.api(apiError.error.message)
            }
            let body = String(data: data, encoding: .utf8) ?? "Unknown Lumo error."
            throw LumoChatError.api("Lumo returned \(response.statusCode): \(body)")
        }

        let decoded: LumoChatEnvelope
        do {
            decoded = try JSONDecoder().decode(LumoChatEnvelope.self, from: data)
        } catch {
            let body = String(data: data, encoding: .utf8) ?? "Unreadable response body."
            throw LumoChatError.api("Could not read Lumo response: \(error.localizedDescription). \(body.clipped(360))")
        }

        let text = decoded.choices
            .compactMap { $0.message.content }
            .joined(separator: "\n")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            throw LumoChatError.api("Lumo returned no response text.")
        }

        return LumoChatResult(
            text: text,
            inputTokens: decoded.usage?.promptTokens,
            outputTokens: decoded.usage?.completionTokens
        )
    }
}

struct LumoChatResult {
    var text: String
    var inputTokens: Int?
    var outputTokens: Int?
}

enum LumoChatError: LocalizedError {
    case transport(String)
    case api(String)

    var errorDescription: String? {
        switch self {
        case .transport(let message), .api(let message):
            return message
        }
    }
}

private struct LumoChatRequest: Encodable {
    var model: String
    var messages: [LumoChatMessage]
    var stream: Bool
    var maxTokens: Int

    enum CodingKeys: String, CodingKey {
        case model
        case messages
        case stream
        case maxTokens = "max_tokens"
    }
}

private struct LumoChatMessage: Codable {
    var role: String
    var content: String?

    init(role: String, content: String) {
        self.role = role
        self.content = content
    }
}

private struct LumoChatEnvelope: Decodable {
    var choices: [LumoChatChoice]
    var usage: LumoChatUsage?
}

private struct LumoChatChoice: Decodable {
    var message: LumoChatMessage
}

private struct LumoChatUsage: Decodable {
    var promptTokens: Int?
    var completionTokens: Int?

    enum CodingKeys: String, CodingKey {
        case promptTokens = "prompt_tokens"
        case completionTokens = "completion_tokens"
    }
}

private struct LumoErrorEnvelope: Decodable {
    var error: LumoErrorBody
}

private struct LumoErrorBody: Decodable {
    var message: String
}
