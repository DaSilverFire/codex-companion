import Foundation

struct CompanionChatStreamCompletion: Equatable, Sendable {
    var text: String
    var inputTokens: Int?
    var outputTokens: Int?

    init(
        text: String,
        inputTokens: Int? = nil,
        outputTokens: Int? = nil
    ) {
        self.text = text
        self.inputTokens = inputTokens
        self.outputTokens = outputTokens
    }
}

enum CompanionChatStreamEvent: Equatable, Sendable {
    case started
    case assistantDelta(String)
    case completed(CompanionChatStreamCompletion)
}

struct CompanionCumulativeTextAccumulator: Equatable, Sendable {
    private(set) var text = ""

    mutating func delta(for cumulativeText: String) -> String {
        guard cumulativeText != text else { return "" }
        guard cumulativeText.hasPrefix(text) else {
            text = cumulativeText
            return ""
        }
        let delta = String(cumulativeText.dropFirst(text.count))
        text = cumulativeText
        return delta
    }
}

enum CompanionChatStreamError: LocalizedError {
    case emptyResponse
    case missingCompletion

    var errorDescription: String? {
        switch self {
        case .emptyResponse:
            return "The selected chat model returned an empty response."
        case .missingCompletion:
            return "The selected chat model ended without a final response."
        }
    }
}

enum CompanionChatFinalResponseStream {
    static func make(
        operation: @escaping @Sendable () async throws -> CompanionChatStreamCompletion
    ) -> AsyncThrowingStream<CompanionChatStreamEvent, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                continuation.yield(.started)
                do {
                    var completion = try await operation()
                    completion.text = completion.text
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    guard !completion.text.isEmpty else {
                        throw CompanionChatStreamError.emptyResponse
                    }
                    continuation.yield(.assistantDelta(completion.text))
                    continuation.yield(.completed(completion))
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}

enum CompanionChatStreamCollector {
    static func collect(
        _ stream: AsyncThrowingStream<CompanionChatStreamEvent, Error>
    ) async throws -> CompanionChatStreamCompletion {
        for try await event in stream {
            if case .completed(let completion) = event {
                return completion
            }
        }
        throw CompanionChatStreamError.missingCompletion
    }
}

protocol CompanionChatStreaming: Sendable {
    func stream(
        prompt: String,
        attachments: [CompanionBridgeAttachment]
    ) -> AsyncThrowingStream<CompanionChatStreamEvent, Error>
}
