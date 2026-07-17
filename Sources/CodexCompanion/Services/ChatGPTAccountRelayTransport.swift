import Foundation

protocol ChatGPTAccountRelayStreaming: Sendable {
    func queue(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error>
}

extension ChatGPTAccountRelayClient: ChatGPTAccountRelayStreaming {}

struct ChatGPTAccountRelayTransport: ChatGPTAccountBridgeTransport {
    let client: any ChatGPTAccountRelayStreaming

    let capabilities = ChatGPTAccountCapabilities(
        transportID: "chatgpt-apps-sdk-relay-v1",
        transportName: "ChatGPT account relay",
        supportsStreaming: false,
        supportsConversationContinuation: false,
        models: [],
        agents: []
    )

    func stream(
        _ request: ChatGPTAccountChatRequest
    ) -> AsyncThrowingStream<ChatGPTAccountStreamEvent, Error> {
        guard request.conversationID == nil,
              request.modelID == nil,
              request.agentID == nil else {
            return AsyncThrowingStream { continuation in
                continuation.finish(throwing: ChatGPTAccountRelayError.unsupportedCapability)
            }
        }
        return client.queue(request)
    }
}
