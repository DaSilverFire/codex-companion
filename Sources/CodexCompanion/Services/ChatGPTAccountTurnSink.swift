import Foundation

protocol ChatGPTAccountTurnSink: Sendable {
    func receive(_ event: ChatGPTAccountMirroredTurnEvent) async
}

struct NoopChatGPTAccountTurnSink: ChatGPTAccountTurnSink {
    func receive(_ event: ChatGPTAccountMirroredTurnEvent) async {}
}
