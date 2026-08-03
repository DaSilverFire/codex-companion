import Foundation

enum CodexRolloutTaskEventParser {
    private static let maximumLineBytes = 2 * 1_024 * 1_024
    private static let maximumTextCharacters = 128 * 1_024

    static func event(
        from data: Data,
        threadID: String,
        fallbackID: String
    ) -> CodexTaskStreamEvent? {
        guard !data.isEmpty, data.count <= maximumLineBytes else { return nil }

        if let lifecycle = CodexTaskLifecycleParser.state(from: data) {
            switch lifecycle.status {
            case .active:
                return CodexTaskStreamEvent(
                    threadID: threadID,
                    turnID: lifecycle.turnID,
                    kind: .turnStarted,
                    taskStatus: .running
                )
            case .completed:
                return CodexTaskStreamEvent(
                    threadID: threadID,
                    turnID: lifecycle.turnID,
                    kind: .turnCompleted,
                    taskStatus: .completed
                )
            case .failed:
                return CodexTaskStreamEvent(
                    threadID: threadID,
                    turnID: lifecycle.turnID,
                    kind: .failed,
                    taskStatus: .failed,
                    errorCode: "rollout_failure"
                )
            }
        }

        guard let item = CodexMobileTaskArchive().timelineItem(
            from: data,
            fallbackID: fallbackID
        ) else { return nil }

        switch item.kind {
        case .message:
            guard item.role == .assistant,
                  let text = bounded(item.text),
                  !text.isEmpty
            else { return nil }
            return CodexTaskStreamEvent(
                threadID: threadID,
                turnID: item.turnID,
                itemID: item.id,
                kind: .assistantDelta,
                text: text,
                taskStatus: .running
            )

        case .reasoning:
            guard let summary = bounded(item.title ?? item.text),
                  !summary.isEmpty
            else { return nil }
            return CodexTaskStreamEvent(
                threadID: threadID,
                turnID: item.turnID,
                itemID: item.id,
                kind: .reasoningSummaryDelta,
                text: summary,
                taskStatus: .running
            )

        case .tool, .status, .compaction:
            // Raw tool output is deliberately omitted from the live path. The
            // canonical timeline refresh merges safe output and media later.
            guard item.title != "Tool result" else { return nil }
            let kind: CompanionBridgeLiveEventKind = item.status == .inProgress
                ? .itemStarted
                : .itemCompleted
            return CodexTaskStreamEvent(
                threadID: threadID,
                turnID: item.turnID,
                itemID: item.id,
                kind: kind,
                item: item,
                taskStatus: .running
            )
        }
    }

    private static func bounded(_ value: String?) -> String? {
        guard let value, !value.isEmpty else { return nil }
        guard value.count > maximumTextCharacters else { return value }
        return String(value.prefix(maximumTextCharacters))
    }
}

struct CodexRolloutTailRecord: Equatable, Sendable {
    var offset: UInt64
    var data: Data
}

struct CodexRolloutTailBuffer: Sendable {
    private let maximumLineBytes: Int
    private var buffered = Data()
    private var bufferedStartOffset: UInt64?
    private var isDiscardingOversizedLine = false

    init(maximumLineBytes: Int = 2 * 1_024 * 1_024) {
        self.maximumLineBytes = max(1, maximumLineBytes)
    }

    mutating func append(
        _ data: Data,
        startingAt offset: UInt64
    ) -> [CodexRolloutTailRecord] {
        guard !data.isEmpty else { return [] }

        if let bufferedStartOffset,
           bufferedStartOffset + UInt64(buffered.count) != offset {
            buffered.removeAll(keepingCapacity: false)
            self.bufferedStartOffset = nil
            isDiscardingOversizedLine = false
        }
        if bufferedStartOffset == nil {
            bufferedStartOffset = offset
        }
        buffered.append(data)

        var records: [CodexRolloutTailRecord] = []
        while let newline = buffered.firstIndex(of: 0x0A) {
            let lineOffset = bufferedStartOffset ?? offset
            let line = buffered.subdata(in: buffered.startIndex..<newline)
            let consumed = buffered.distance(from: buffered.startIndex, to: newline) + 1

            if !isDiscardingOversizedLine,
               !line.isEmpty,
               line.count <= maximumLineBytes {
                records.append(CodexRolloutTailRecord(offset: lineOffset, data: line))
            }

            buffered.removeSubrange(buffered.startIndex...newline)
            bufferedStartOffset = lineOffset + UInt64(consumed)
            isDiscardingOversizedLine = false
        }

        if buffered.count > maximumLineBytes {
            buffered.removeAll(keepingCapacity: false)
            bufferedStartOffset = offset + UInt64(data.count)
            isDiscardingOversizedLine = true
        }
        return records
    }
}

struct CodexRolloutTaskEventClient: CodexTaskEventClient, Sendable {
    var rolloutURL: URL
    var hasActiveTurn: Bool

    func subscribe(
        threadID: String,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) throws -> any CodexTaskEventStream {
        let session = try CodexRolloutTaskEventSession(
            rolloutURL: rolloutURL,
            threadID: threadID,
            hasActiveTurn: hasActiveTurn,
            onEvent: onEvent,
            onTermination: onTermination
        )
        session.start()
        return session
    }
}

private final class CodexRolloutTaskEventSession: CodexTaskEventStream, @unchecked Sendable {
    private static let readChunkBytes = 512 * 1_024

    private let rolloutURL: URL
    private let threadID: String
    private let hasActiveTurn: Bool
    private let onEvent: @Sendable (CodexTaskStreamEvent) -> Void
    private let onTermination: @Sendable (String?) -> Void
    private let queue = DispatchQueue(label: "com.silverfire.codexcompanion.rollout-task-stream")
    private let handle: FileHandle
    private var source: DispatchSourceFileSystemObject?
    private var buffer = CodexRolloutTailBuffer()
    private var nextReadOffset: UInt64
    private var isCancelled = false
    private var didTerminate = false

    init(
        rolloutURL: URL,
        threadID: String,
        hasActiveTurn: Bool,
        onEvent: @escaping @Sendable (CodexTaskStreamEvent) -> Void,
        onTermination: @escaping @Sendable (String?) -> Void
    ) throws {
        self.rolloutURL = rolloutURL
        self.threadID = threadID
        self.hasActiveTurn = hasActiveTurn
        self.onEvent = onEvent
        self.onTermination = onTermination
        handle = try FileHandle(forReadingFrom: rolloutURL)
        nextReadOffset = try handle.seekToEnd()
    }

    func start() {
        let source = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: handle.fileDescriptor,
            eventMask: [.write, .extend, .delete, .rename],
            queue: queue
        )
        source.setEventHandler { [weak self, weak source] in
            guard let self, !isCancelled else { return }
            let events = source?.data ?? []
            if events.contains(.delete) || events.contains(.rename) {
                finish(reason: "The Codex task history moved while it was being followed.")
                cancelOnQueue()
                return
            }
            readAvailableRecords()
        }
        source.setCancelHandler { [handle] in
            try? handle.close()
        }
        self.source = source
        source.resume()

        queue.async { [weak self] in
            guard let self, !isCancelled else { return }
            if hasActiveTurn {
                onEvent(CodexTaskStreamEvent(
                    threadID: threadID,
                    kind: .turnStarted,
                    taskStatus: .running
                ))
            }
            readAvailableRecords()
        }
    }

    func cancel() {
        queue.async { [weak self] in
            self?.cancelOnQueue()
        }
    }

    private func readAvailableRecords() {
        guard !isCancelled else { return }
        do {
            while true {
                let startOffset = nextReadOffset
                guard let data = try handle.read(upToCount: Self.readChunkBytes),
                      !data.isEmpty
                else { return }
                nextReadOffset += UInt64(data.count)
                for record in buffer.append(data, startingAt: startOffset) {
                    guard let event = CodexRolloutTaskEventParser.event(
                        from: record.data,
                        threadID: threadID,
                        fallbackID: String(record.offset)
                    ) else { continue }
                    onEvent(event)
                }
            }
        } catch {
            finish(reason: "The Codex task history could not be followed.")
            cancelOnQueue()
        }
    }

    private func finish(reason: String?) {
        guard !isCancelled, !didTerminate else { return }
        didTerminate = true
        onTermination(reason)
    }

    private func cancelOnQueue() {
        guard !isCancelled else { return }
        isCancelled = true
        source?.cancel()
        source = nil
    }
}
