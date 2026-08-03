import Foundation

enum CompanionBridgeFeature: String, Codable, Sendable {
    case taskStreamV1 = "task_stream_v1"
    case presencePetPackageV1 = "presence_pet_package_v1"
    case attachmentUploadV1 = "attachment_upload_v1"
}

enum CompanionBridgeLiveChannel: String, Codable, Sendable {
    case task
    case casualChat
}

enum CompanionBridgeLiveEventKind: String, Codable, Sendable {
    case turnStarted
    case reasoningSummaryDelta
    case itemStarted
    case itemCompleted
    case assistantDelta
    case statusChanged
    case turnCompleted
    case failed
}

struct CompanionBridgeLiveEvent: Codable, Equatable, Sendable {
    var channel: CompanionBridgeLiveChannel
    var streamID: UUID
    var sequence: UInt64
    var threadID: String?
    var turnID: String?
    var itemID: String?
    var kind: CompanionBridgeLiveEventKind
    var text: String?
    var item: CompanionBridgeTimelineItem?
    var taskStatus: CompanionBridgeTaskStatus?
    var errorCode: String?

    init(
        channel: CompanionBridgeLiveChannel,
        streamID: UUID,
        sequence: UInt64,
        threadID: String? = nil,
        turnID: String? = nil,
        itemID: String? = nil,
        kind: CompanionBridgeLiveEventKind,
        text: String? = nil,
        item: CompanionBridgeTimelineItem? = nil,
        taskStatus: CompanionBridgeTaskStatus? = nil,
        errorCode: String? = nil
    ) {
        self.channel = channel
        self.streamID = streamID
        self.sequence = sequence
        self.threadID = threadID
        self.turnID = turnID
        self.itemID = itemID
        self.kind = kind
        self.text = text
        self.item = item
        self.taskStatus = taskStatus
        self.errorCode = errorCode
    }
}

struct CompanionBridgeServerEvent: Codable, Equatable, Sendable {
    var messageType: String = "server_event"
    var protocolVersion: Int = CompanionBridgeProtocol.version
    var eventID: UUID
    var liveEvent: CompanionBridgeLiveEvent

    init(
        eventID: UUID = UUID(),
        liveEvent: CompanionBridgeLiveEvent
    ) {
        self.eventID = eventID
        self.liveEvent = liveEvent
    }
}
