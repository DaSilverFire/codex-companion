import Foundation

protocol ChatGPTAccountRelaySocket: Sendable {
    func connect(to url: URL) async throws
    func send(_ data: Data) async throws
    func receive() async throws -> Data
    func close() async
}

actor URLSessionChatGPTAccountRelaySocket: ChatGPTAccountRelaySocket {
    private let session: URLSession
    private var task: URLSessionWebSocketTask?

    init(session: URLSession = .shared) {
        self.session = session
    }

    func connect(to url: URL) async throws {
        let task = session.webSocketTask(with: url)
        self.task = task
        task.resume()
    }

    func send(_ data: Data) async throws {
        guard let task else { throw ChatGPTAccountRelayError.relayUnavailable }
        try await task.send(.data(data))
    }

    func receive() async throws -> Data {
        guard let task else { throw ChatGPTAccountRelayError.relayUnavailable }
        switch try await task.receive() {
        case .data(let data):
            return data
        case .string(let string):
            return Data(string.utf8)
        @unknown default:
            throw ChatGPTAccountRelayError.invalidMessage
        }
    }

    func close() async {
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
    }
}
