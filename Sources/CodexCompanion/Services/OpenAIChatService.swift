import Foundation

struct OpenAIChatService {
    private let endpoint = URL(string: "https://api.openai.com/v1/responses")!
    private let maxOutputTokens = 700

    func send(prompt: String, model: ChatGPTModel, apiKey: String) async throws -> OpenAIChatResult {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(
            ResponsesRequest(
                model: model.apiModelID,
                input: prompt,
                reasoning: ResponsesReasoning(effort: model.reasoningEffort),
                text: ResponsesText(verbosity: model.verbosity),
                maxOutputTokens: maxOutputTokens
            )
        )

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let httpResponse = response as? HTTPURLResponse else {
            throw OpenAIChatError.transport("No HTTP response from OpenAI.")
        }

        guard (200..<300).contains(httpResponse.statusCode) else {
            if let apiError = try? JSONDecoder().decode(OpenAIErrorEnvelope.self, from: data) {
                throw OpenAIChatError.api(apiError.error.message)
            }
            let body = String(data: data, encoding: .utf8) ?? "Unknown OpenAI error."
            throw OpenAIChatError.api("OpenAI returned \(httpResponse.statusCode): \(body)")
        }

        let decoded: ResponsesEnvelope
        do {
            decoded = try JSONDecoder().decode(ResponsesEnvelope.self, from: data)
        } catch {
            let body = String(data: data, encoding: .utf8) ?? "Unreadable response body."
            throw OpenAIChatError.api("Could not read OpenAI response: \(error.localizedDescription). \(body.clipped(360))")
        }
        let text = decoded.outputText ?? decoded.outputTextFromItems
        guard let text, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw OpenAIChatError.api("OpenAI returned no response text.")
        }

        return OpenAIChatResult(
            text: text.trimmingCharacters(in: .whitespacesAndNewlines),
            inputTokens: decoded.usage?.inputTokens,
            outputTokens: decoded.usage?.outputTokens
        )
    }
}

struct OpenAIChatResult {
    var text: String
    var inputTokens: Int?
    var outputTokens: Int?
}

enum OpenAIChatError: LocalizedError {
    case transport(String)
    case api(String)

    var errorDescription: String? {
        switch self {
        case .transport(let message), .api(let message):
            return message
        }
    }
}

private struct ResponsesRequest: Encodable {
    var model: String
    var input: String
    var reasoning: ResponsesReasoning
    var text: ResponsesText
    var maxOutputTokens: Int

    enum CodingKeys: String, CodingKey {
        case model
        case input
        case reasoning
        case text
        case maxOutputTokens = "max_output_tokens"
    }
}

private struct ResponsesReasoning: Encodable {
    var effort: String
}

private struct ResponsesText: Encodable {
    var verbosity: String
}

private struct ResponsesEnvelope: Decodable {
    var outputText: String?
    var output: [ResponsesOutputItem]?
    var usage: ResponsesUsage?

    enum CodingKeys: String, CodingKey {
        case outputText = "output_text"
        case output
        case usage
    }

    var outputTextFromItems: String? {
        let chunks = output?
            .flatMap { $0.content ?? [] }
            .compactMap(\.text)
        guard let chunks, !chunks.isEmpty else { return nil }
        return chunks.joined(separator: "\n")
    }
}

private struct ResponsesOutputItem: Decodable {
    var content: [ResponsesContentItem]?
}

private struct ResponsesContentItem: Decodable {
    var type: String?
    var text: String?
}

private struct ResponsesUsage: Decodable {
    var inputTokens: Int?
    var outputTokens: Int?

    enum CodingKeys: String, CodingKey {
        case inputTokens = "input_tokens"
        case outputTokens = "output_tokens"
    }
}

private struct OpenAIErrorEnvelope: Decodable {
    var error: OpenAIErrorBody
}

private struct OpenAIErrorBody: Decodable {
    var message: String
}
