import Foundation

struct CodexMobileTaskPage: Equatable, Sendable {
    var tasks: [CompanionBridgeTask]
    var nextCursor: String?
}

struct CodexMobileMessagePage: Equatable, Sendable {
    var messages: [CompanionBridgeMessage]
    var nextCursor: String?
}

struct CodexMobileTimelinePage: Equatable, Sendable {
    var items: [CompanionBridgeTimelineItem]
    var nextCursor: String?
    var revision: String
    var contextUsage: CompanionBridgeContextUsage?
}

struct CodexMobileTaskArchive: Sendable {
    private static let chunkSize = 512 * 1024
    private static let maximumLineSize = 2 * 1024 * 1024
    private static let maximumInlineMediaSize = 1024 * 1024
    private static let maximumToolDetailSize = 2_000

    private struct RawTimelineRecord {
        var offset: UInt64
        var item: CompanionBridgeTimelineItem
    }

    private struct SemanticTimelineRecord {
        var offset: UInt64
        var item: CompanionBridgeTimelineItem
    }

    private struct TaskLifecycleState {
        var isActive: Bool
        var turnID: String?
    }

    private struct DelegationSummary {
        var targetID: String?
        var targetLabel: String
        var message: String?
    }

    var homeDirectory: URL = FileManager.default.homeDirectoryForCurrentUser
    var sqliteExecutableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")
    var approvalPromotionTracker: CodexApprovalPromotionTracker = .shared
    var readPendingApprovalThreadIDs: @Sendable () -> Set<String> = {
        Set(CodexDesktopApprovalLogReader().pendingApprovals().keys)
    }

    func tasks(cursor: String?, limit requestedLimit: Int?) throws -> CodexMobileTaskPage {
        let limit = boundedLimit(requestedLimit, fallback: CompanionBridgeProtocol.defaultTaskPageSize)
        let offset = max(0, Int(cursor ?? "") ?? 0)
        let pendingApprovalThreadIDs = readPendingApprovalThreadIDs()
        let promotedThreadIDs = approvalPromotionTracker.promotedThreadIDs(
            pendingThreadIDs: pendingApprovalThreadIDs
        )
        let sidebarOrdering = CodexSidebarOrderingSnapshot.load(homeDirectory: homeDirectory)
        let rows = try sqliteRows(query: """
        select id, title, cwd, coalesce(updated_at_ms, updated_at * 1000),
               first_user_message, rollout_path, preview, coalesce(model, ''),
               coalesce(reasoning_effort, ''),
               coalesce(nullif(recency_at_ms, 0), updated_at_ms, updated_at * 1000)
        from threads
        where archived = 0
          and source not like '{"subagent":%';
        """)

        let sortedRows = rows.sorted { lhs, rhs in
            sidebarOrdering.orders(
                orderingEntry(
                    for: lhs,
                    promotedThreadIDs: promotedThreadIDs,
                    pendingApprovalThreadIDs: pendingApprovalThreadIDs
                ),
                before: orderingEntry(
                    for: rhs,
                    promotedThreadIDs: promotedThreadIDs,
                    pendingApprovalThreadIDs: pendingApprovalThreadIDs
                )
            )
        }
        let pageRows = Array(sortedRows.dropFirst(offset).prefix(limit))
        let now = Date()
        let sessionNames = sessionThreadNames()
        let tasks = pageRows.compactMap { columns -> CompanionBridgeTask? in
            guard columns.count >= 9 else { return nil }
            let threadID = columns[0]
            let cwd = columns[2].trimmingCharacters(in: .whitespacesAndNewlines)
            let firstMessage = columns[4].trimmingCharacters(in: .whitespacesAndNewlines)
            let storedPreview = columns[6].trimmingCharacters(in: .whitespacesAndNewlines)
            let updatedAt = date(fromMilliseconds: columns[3]) ?? .distantPast
            let latest = latestVisibleMessage(in: rolloutURL(from: columns[5]))
            let needsApproval = pendingApprovalThreadIDs.contains(threadID)
            let elapsed = now.timeIntervalSince(updatedAt)
            let status: CompanionBridgeTaskStatus = needsApproval
                ? .waiting
                : elapsed < 3 * 60 ? .running : .completed
            return CompanionBridgeTask(
                id: threadID,
                title: displayTitle(
                    threadID: threadID,
                    storedTitle: columns[1],
                    cwd: cwd,
                    firstMessage: firstMessage,
                    sessionNames: sessionNames
                ),
                preview: latest?.text ?? nonempty(storedPreview) ?? nonempty(firstMessage) ?? "No messages yet",
                updatedAt: updatedAt,
                cwd: nonempty(cwd),
                status: status,
                needsApproval: needsApproval,
                activeTurnID: latest?.turnID,
                model: nonempty(columns[7]),
                reasoningEffort: nonempty(columns[8]),
                taskGroup: sidebarOrdering.taskGroup(threadID: threadID, cwd: nonempty(cwd))
            )
        }

        return CodexMobileTaskPage(
            tasks: tasks,
            nextCursor: sortedRows.count > offset + limit ? String(offset + limit) : nil
        )
    }

    private func orderingEntry(
        for columns: [String],
        promotedThreadIDs: Set<String>,
        pendingApprovalThreadIDs: Set<String>
    ) -> CodexSidebarOrderingSnapshot.Entry {
        guard columns.count >= 10 else {
            return .init(
                threadID: nil,
                cwd: nil,
                statusRank: 4,
                updatedAt: nil,
                isAttentionPromoted: false
            )
        }
        let threadID = columns[0]
        let updatedAt = date(fromMilliseconds: columns[9])
        let activityDate = date(fromMilliseconds: columns[3])
        let isRunning = activityDate.map { Date().timeIntervalSince($0) < 3 * 60 } ?? false
        let statusRank = pendingApprovalThreadIDs.contains(threadID) ? 0 : isRunning ? 1 : 3
        return .init(
            threadID: threadID,
            cwd: nonempty(columns[2]),
            statusRank: statusRank,
            updatedAt: updatedAt,
            isAttentionPromoted: promotedThreadIDs.contains(threadID)
        )
    }

    func messages(
        threadID: String,
        cursor: String?,
        limit requestedLimit: Int?
    ) throws -> CodexMobileMessagePage {
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedThreadID.isEmpty else { throw CodexMobileArchiveError.invalidThreadID }
        guard let rolloutPath = try rolloutPath(for: trimmedThreadID) else {
            throw CodexMobileArchiveError.threadNotFound
        }

        let url = rolloutURL(from: rolloutPath)
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw CodexMobileArchiveError.historyMissing
        }
        let limit = boundedLimit(requestedLimit, fallback: CompanionBridgeProtocol.defaultMessagePageSize)
        return try reverseMessagePage(url: url, cursor: cursor, limit: limit)
    }

    func timeline(
        threadID: String,
        cursor: String?,
        limit requestedLimit: Int?
    ) throws -> CodexMobileTimelinePage {
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedThreadID.isEmpty else { throw CodexMobileArchiveError.invalidThreadID }
        guard let rolloutPath = try rolloutPath(for: trimmedThreadID) else {
            throw CodexMobileArchiveError.threadNotFound
        }

        let url = rolloutURL(from: rolloutPath)
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw CodexMobileArchiveError.historyMissing
        }
        let limit = boundedLimit(requestedLimit, fallback: CompanionBridgeProtocol.defaultMessagePageSize)
        return try reverseTimelinePage(url: url, cursor: cursor, limit: limit)
    }

    func subagents(parentThreadID: String, limit requestedLimit: Int?) throws -> [CompanionBridgeSubagent] {
        let parentThreadID = parentThreadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !parentThreadID.isEmpty else { throw CodexMobileArchiveError.invalidThreadID }
        let limit = boundedLimit(requestedLimit, fallback: 8)
        let pendingApprovalThreadIDs = readPendingApprovalThreadIDs()
        let rows = try sqliteRows(query: """
        select id, title, source, coalesce(updated_at_ms, updated_at * 1000),
               coalesce(nullif(recency_at_ms, 0), updated_at_ms, updated_at * 1000)
        from threads
        where archived = 0
          and source like '{"subagent":%';
        """)

        return rows.compactMap { columns -> (CompanionBridgeSubagent, Double)? in
            guard columns.count >= 5,
                  let sourceData = columns[2].data(using: .utf8),
                  let source = try? JSONSerialization.jsonObject(with: sourceData) as? [String: Any],
                  let subagent = source["subagent"] as? [String: Any],
                  let spawn = subagent["thread_spawn"] as? [String: Any],
                  spawn["parent_thread_id"] as? String == parentThreadID
            else { return nil }
            let updatedAt = date(fromMilliseconds: columns[3]) ?? .distantPast
            let needsApproval = pendingApprovalThreadIDs.contains(columns[0])
            let status: CompanionBridgeTaskStatus = needsApproval
                ? .waiting
                : Date().timeIntervalSince(updatedAt) < 3 * 60 ? .running : .completed
            let nickname = nonempty(spawn["agent_nickname"] as? String ?? "") ?? "Subagent"
            let agent = CompanionBridgeSubagent(
                id: columns[0],
                name: nickname,
                title: nonempty(columns[1]) ?? "Subagent task",
                role: nonempty(spawn["agent_role"] as? String ?? ""),
                updatedAt: updatedAt,
                status: status,
                needsApproval: needsApproval
            )
            return (agent, Double(columns[4]) ?? updatedAt.timeIntervalSince1970 * 1_000)
        }
        .sorted { $0.1 > $1.1 }
        .prefix(limit)
        .map(\.0)
    }

    private func reverseMessagePage(
        url: URL,
        cursor: String?,
        limit: Int
    ) throws -> CodexMobileMessagePage {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let fileSize = try handle.seekToEnd()
        var endOffset = min(fileSize, UInt64(cursor ?? "") ?? fileSize)
        var carry = Data()
        var isSkippingOversizedLine = false
        var collected: [(offset: UInt64, message: CompanionBridgeMessage)] = []

        while endOffset > 0, collected.count < limit {
            let startOffset = endOffset > UInt64(Self.chunkSize)
                ? endOffset - UInt64(Self.chunkSize)
                : 0
            try handle.seek(toOffset: startOffset)
            var block = handle.readData(ofLength: Int(endOffset - startOffset))

            if isSkippingOversizedLine {
                if let newline = block.lastIndex(of: 0x0A) {
                    endOffset = startOffset + UInt64(block.distance(from: block.startIndex, to: newline))
                    isSkippingOversizedLine = false
                } else {
                    endOffset = startOffset
                }
                continue
            }

            block.append(carry)

            let firstCompleteIndex: Data.Index
            if startOffset == 0 {
                firstCompleteIndex = block.startIndex
                carry.removeAll(keepingCapacity: false)
            } else if let newline = block.firstIndex(of: 0x0A) {
                carry = block.subdata(in: block.startIndex..<newline)
                firstCompleteIndex = block.index(after: newline)
            } else {
                if block.count > Self.maximumLineSize {
                    carry.removeAll(keepingCapacity: false)
                    isSkippingOversizedLine = true
                } else {
                    carry = block
                }
                endOffset = startOffset
                continue
            }

            let complete = block.subdata(in: firstCompleteIndex..<block.endIndex)
            var records: [(UInt64, Data)] = []
            var lineStart = complete.startIndex
            while lineStart < complete.endIndex {
                let lineEnd = complete[lineStart...].firstIndex(of: 0x0A) ?? complete.endIndex
                if lineEnd > lineStart {
                    let localOffset = complete.distance(from: complete.startIndex, to: lineStart)
                    let absolute = startOffset
                        + UInt64(block.distance(from: block.startIndex, to: firstCompleteIndex))
                        + UInt64(localOffset)
                    records.append((absolute, complete.subdata(in: lineStart..<lineEnd)))
                }
                guard lineEnd < complete.endIndex else { break }
                lineStart = complete.index(after: lineEnd)
            }

            for (absoluteOffset, line) in records.reversed() {
                guard let parsed = visibleMessage(from: line, fallbackID: "\(absoluteOffset)") else { continue }
                collected.append((absoluteOffset, parsed.message))
                if collected.count == limit { break }
            }
            endOffset = startOffset
        }

        let chronological = collected.reversed().map(\.message)
        let nextCursor = collected.count == limit && (collected.last?.offset ?? 0) > 0
            ? String(collected.last!.offset)
            : nil
        return CodexMobileMessagePage(messages: chronological, nextCursor: nextCursor)
    }

    private func reverseTimelinePage(
        url: URL,
        cursor: String?,
        limit: Int
    ) throws -> CodexMobileTimelinePage {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let fileSize = try handle.seekToEnd()
        var endOffset = min(fileSize, UInt64(cursor ?? "") ?? fileSize)
        var carry = Data()
        var isSkippingOversizedLine = false
        var rawRecords: [RawTimelineRecord] = []
        var semanticRecords: [SemanticTimelineRecord] = []
        var latestContextUsage: CompanionBridgeContextUsage?
        var latestTaskLifecycle: TaskLifecycleState?

        while endOffset > 0,
              semanticRecords.count < limit || !hasCompleteLeadingSemanticGroup(rawRecords) {
            let startOffset = endOffset > UInt64(Self.chunkSize)
                ? endOffset - UInt64(Self.chunkSize)
                : 0
            try handle.seek(toOffset: startOffset)
            var block = handle.readData(ofLength: Int(endOffset - startOffset))

            if isSkippingOversizedLine {
                if let newline = block.lastIndex(of: 0x0A) {
                    endOffset = startOffset + UInt64(block.distance(from: block.startIndex, to: newline))
                    isSkippingOversizedLine = false
                } else {
                    endOffset = startOffset
                }
                continue
            }

            block.append(carry)
            let firstCompleteIndex: Data.Index
            if startOffset == 0 {
                firstCompleteIndex = block.startIndex
                carry.removeAll(keepingCapacity: false)
            } else if let newline = block.firstIndex(of: 0x0A) {
                carry = block.subdata(in: block.startIndex..<newline)
                firstCompleteIndex = block.index(after: newline)
            } else {
                if block.count > Self.maximumLineSize {
                    carry.removeAll(keepingCapacity: false)
                    isSkippingOversizedLine = true
                } else {
                    carry = block
                }
                endOffset = startOffset
                continue
            }

            let complete = block.subdata(in: firstCompleteIndex..<block.endIndex)
            var records: [(UInt64, Data)] = []
            var lineStart = complete.startIndex
            while lineStart < complete.endIndex {
                let lineEnd = complete[lineStart...].firstIndex(of: 0x0A) ?? complete.endIndex
                if lineEnd > lineStart {
                    let localOffset = complete.distance(from: complete.startIndex, to: lineStart)
                    let absolute = startOffset
                        + UInt64(block.distance(from: block.startIndex, to: firstCompleteIndex))
                        + UInt64(localOffset)
                    records.append((absolute, complete.subdata(in: lineStart..<lineEnd)))
                }
                guard lineEnd < complete.endIndex else { break }
                lineStart = complete.index(after: lineEnd)
            }

            for (absoluteOffset, line) in records.reversed() {
                if cursor == nil, latestContextUsage == nil {
                    latestContextUsage = contextUsage(from: line)
                }
                if cursor == nil, latestTaskLifecycle == nil {
                    latestTaskLifecycle = taskLifecycleState(from: line)
                }
                guard let item = timelineItem(from: line, fallbackID: "\(absoluteOffset)") else { continue }
                rawRecords.append(RawTimelineRecord(offset: absoluteOffset, item: item))
            }
            endOffset = startOffset
            semanticRecords = semanticTimelineRecords(from: rawRecords.reversed())
        }

        var pageRecords = Array(semanticRecords.suffix(limit))
        if cursor == nil,
           shouldMarkLatestReasoningActive(
               in: pageRecords,
               lifecycle: latestTaskLifecycle
           ),
           let reasoningIndex = pageRecords.lastIndex(where: { record in
               guard record.item.kind == .reasoning else { return false }
               guard let activeTurnID = latestTaskLifecycle?.turnID else { return true }
               return record.item.turnID == activeTurnID
           }) {
            pageRecords[reasoningIndex].item.status = .inProgress
        }
        let hasOlderItems = semanticRecords.count > pageRecords.count || endOffset > 0
        let nextCursor = hasOlderItems
            ? pageRecords.first.map { String($0.offset) }
            : nil
        let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
        let modified = (attributes?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        return CodexMobileTimelinePage(
            items: pageRecords.map(\.item),
            nextCursor: nextCursor,
            revision: "\(fileSize):\(Int(modified * 1_000))",
            contextUsage: latestContextUsage
        )
    }

    private func hasCompleteLeadingSemanticGroup(_ reverseChronological: [RawTimelineRecord]) -> Bool {
        guard let earliest = reverseChronological.last?.item else { return true }
        if earliest.kind == .message || earliest.kind == .reasoning
            || earliest.kind == .status || earliest.kind == .compaction {
            return true
        }
        return earliest.kind == .tool && earliest.title == "Messaged an agent"
    }

    private func semanticTimelineRecords<S: Sequence>(
        from chronologicalRecords: S
    ) -> [SemanticTimelineRecord] where S.Element == RawTimelineRecord {
        let records = Array(chronologicalRecords)
        let representedCallIDs = Set(records.compactMap { record -> String? in
            guard record.item.kind == .tool,
                  record.item.title != "Tool result"
            else { return nil }
            return record.item.callID
        })
        let outputsByCallID = Dictionary(grouping: records.filter {
            $0.item.kind == .tool && $0.item.title == "Tool result" && $0.item.callID != nil
        }) { $0.item.callID! }

        var projected: [SemanticTimelineRecord] = []
        var activeReasoningIndex: Int?
        var delegationContext: (index: Int, targetID: String?)?

        for record in records {
            var item = record.item

            if item.kind == .tool, item.title == "Tool result" {
                if let callID = item.callID, representedCallIDs.contains(callID) {
                    continue
                }
                appendStandaloneOrGroupedTool(
                    record: SemanticTimelineRecord(offset: record.offset, item: item),
                    projected: &projected,
                    activeReasoningIndex: &activeReasoningIndex
                )
                delegationContext = nil
                continue
            }

            switch item.kind {
            case .message, .status, .compaction:
                projected.append(SemanticTimelineRecord(offset: record.offset, item: item))
                activeReasoningIndex = nil
                delegationContext = nil

            case .reasoning:
                projected.append(SemanticTimelineRecord(offset: record.offset, item: item))
                activeReasoningIndex = projected.index(before: projected.endIndex)
                delegationContext = nil

            case .tool:
                let outputs = item.callID.flatMap { outputsByCallID[$0] } ?? []
                if item.title == "Messaged an agent" {
                    let summary = delegationSummary(for: item, outputs: outputs)
                    let canMerge = delegationContext.map {
                        $0.targetID == nil || summary.targetID == nil || $0.targetID == summary.targetID
                    } ?? false

                    if canMerge, let context = delegationContext {
                        mergeDelegation(
                            summary,
                            outputs: outputs,
                            into: &projected[context.index].item
                        )
                        if context.targetID == nil, summary.targetID != nil {
                            delegationContext = (context.index, summary.targetID)
                        }
                    } else {
                        item.detail = delegationDetail(summary)
                        mergeOutputMediaAndFailure(outputs, into: &item, includesText: false)
                        projected.append(SemanticTimelineRecord(offset: record.offset, item: item))
                        delegationContext = (
                            projected.index(before: projected.endIndex),
                            summary.targetID
                        )
                    }
                    activeReasoningIndex = nil
                    continue
                }

                if item.title == "Wait" {
                    mergeOutputMediaAndFailure(outputs, into: &item, includesText: true)
                    guard item.status == .failed || !item.media.isEmpty else { continue }
                    if let context = delegationContext {
                        mergeTool(item, into: &projected[context.index].item)
                    } else if let activeReasoningIndex {
                        mergeTool(item, into: &projected[activeReasoningIndex].item)
                    } else {
                        projected.append(SemanticTimelineRecord(offset: record.offset, item: item))
                    }
                    continue
                }

                mergeOutputMediaAndFailure(outputs, into: &item, includesText: true)
                delegationContext = nil
                if let activeReasoningIndex {
                    mergeTool(item, into: &projected[activeReasoningIndex].item)
                } else {
                    projected.append(SemanticTimelineRecord(offset: record.offset, item: item))
                }
            }
        }
        return projected
    }

    private func appendStandaloneOrGroupedTool(
        record: SemanticTimelineRecord,
        projected: inout [SemanticTimelineRecord],
        activeReasoningIndex: inout Int?
    ) {
        if let activeReasoningIndex {
            mergeTool(record.item, into: &projected[activeReasoningIndex].item)
        } else {
            projected.append(record)
        }
    }

    private func mergeOutputMediaAndFailure(
        _ outputs: [RawTimelineRecord],
        into item: inout CompanionBridgeTimelineItem,
        includesText: Bool
    ) {
        for output in outputs {
            if includesText,
               let detail = output.item.detail,
               detail != item.detail {
                appendDetail("Result\n\(detail)", to: &item.detail)
            }
            appendMedia(output.item.media, to: &item.media)
            if output.item.status == .failed {
                item.status = .failed
            }
        }
    }

    private func mergeTool(
        _ tool: CompanionBridgeTimelineItem,
        into activity: inout CompanionBridgeTimelineItem
    ) {
        let title = tool.title ?? "Used a tool"
        let entry = tool.detail.map { "\(title)\n\($0)" } ?? title
        appendDetail(entry, to: &activity.detail)
        appendMedia(tool.media, to: &activity.media)
        if tool.status == .failed {
            activity.status = .failed
        }
    }

    private func appendDetail(_ detail: String, to existing: inout String?) {
        guard let detail = nonempty(detail) else { return }
        if let current = nonempty(existing ?? "") {
            guard current != detail, !current.hasSuffix("\n\n\(detail)") else { return }
            existing = "\(current)\n\n\(detail)"
        } else {
            existing = detail
        }
    }

    private func appendMedia(
        _ media: [CompanionBridgeMedia],
        to existing: inout [CompanionBridgeMedia]
    ) {
        let existingIDs = Set(existing.map(\.id))
        existing.append(contentsOf: media.filter { !existingIDs.contains($0.id) })
    }

    private func delegationSummary(
        for item: CompanionBridgeTimelineItem,
        outputs: [RawTimelineRecord]
    ) -> DelegationSummary {
        let arguments = jsonObject(from: item.detail)
        let output = outputs.lazy.compactMap { jsonObject(from: $0.item.detail) }.first
        let targetID = (arguments?["target"] as? String) ?? (output?["agent_id"] as? String)
        let targetLabel = (output?["nickname"] as? String)
            ?? targetID
            ?? (arguments?["agent_type"] as? String).map { $0.capitalized }
            ?? "Agent"
        let message = (arguments?["message"] as? String)
            ?? textFromInputItems(arguments?["items"])
        return DelegationSummary(
            targetID: targetID,
            targetLabel: targetLabel,
            message: nonempty(message ?? "")
        )
    }

    private func jsonObject(from text: String?) -> [String: Any]? {
        guard let data = text?.data(using: .utf8) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    private func textFromInputItems(_ rawItems: Any?) -> String? {
        guard let items = rawItems as? [[String: Any]] else { return nil }
        return nonempty(items.compactMap { item in
            guard item["type"] as? String == "text" else { return nil }
            return item["text"] as? String
        }.joined(separator: "\n"))
    }

    private func delegationDetail(_ summary: DelegationSummary) -> String {
        if let message = summary.message {
            return "Target: \(summary.targetLabel)\n\n\(message)"
        }
        return "Target: \(summary.targetLabel)"
    }

    private func mergeDelegation(
        _ summary: DelegationSummary,
        outputs: [RawTimelineRecord],
        into item: inout CompanionBridgeTimelineItem
    ) {
        if let message = summary.message {
            appendDetail(message, to: &item.detail)
        }
        mergeOutputMediaAndFailure(outputs, into: &item, includesText: false)
    }

    private func taskLifecycleState(from data: Data) -> TaskLifecycleState? {
        guard data.count <= Self.maximumLineSize,
              let raw = try? JSONSerialization.jsonObject(with: data),
              let root = raw as? [String: Any],
              root["type"] as? String == "event_msg",
              let payload = root["payload"] as? [String: Any],
              let type = payload["type"] as? String
        else { return nil }
        let turnID = payload["turn_id"] as? String
        switch type {
        case "task_started", "turn_started":
            return TaskLifecycleState(isActive: true, turnID: turnID)
        case "task_complete", "task_completed", "turn_complete", "turn_completed", "task_aborted":
            return TaskLifecycleState(isActive: false, turnID: turnID)
        default:
            return nil
        }
    }

    private func shouldMarkLatestReasoningActive(
        in records: [SemanticTimelineRecord],
        lifecycle: TaskLifecycleState?
    ) -> Bool {
        if let lifecycle {
            return lifecycle.isActive && records.contains { record in
                guard record.item.kind == .reasoning else { return false }
                guard let turnID = lifecycle.turnID else { return true }
                return record.item.turnID == turnID
            }
        }
        return records.last?.item.kind == .reasoning
    }

    private func contextUsage(from data: Data) -> CompanionBridgeContextUsage? {
        guard data.count <= Self.maximumLineSize,
              let raw = try? JSONSerialization.jsonObject(with: data),
              let root = raw as? [String: Any],
              root["type"] as? String == "event_msg",
              let payload = root["payload"] as? [String: Any],
              payload["type"] as? String == "token_count",
              let info = payload["info"] as? [String: Any],
              let lastUsage = info["last_token_usage"] as? [String: Any],
              let usedTokens = integer(lastUsage["total_tokens"]),
              let contextWindow = integer(info["model_context_window"]),
              usedTokens >= 0,
              contextWindow > 0
        else { return nil }
        return CompanionBridgeContextUsage(
            usedTokens: usedTokens,
            contextWindow: contextWindow
        )
    }

    private func integer(_ value: Any?) -> Int? {
        if let value = value as? Int { return value }
        return (value as? NSNumber)?.intValue
    }

    private func timelineItem(
        from data: Data,
        fallbackID: String
    ) -> CompanionBridgeTimelineItem? {
        guard data.count <= Self.maximumLineSize,
              let raw = try? JSONSerialization.jsonObject(with: data),
              let root = raw as? [String: Any],
              let payload = root["payload"] as? [String: Any],
              let recordType = root["type"] as? String
        else { return nil }

        let createdAt = (root["timestamp"] as? String).flatMap(Self.timestampFormatter.date(from:))
        let payloadType = payload["type"] as? String
        let id = (payload["id"] as? String) ?? fallbackID
        let turnID = (payload["turn_id"] as? String)
            ?? ((payload["internal_chat_message_metadata_passthrough"] as? [String: Any])?["turn_id"] as? String)

        if recordType == "response_item", payloadType == "message",
           let parsed = visibleMessage(from: data, fallbackID: fallbackID) {
            return CompanionBridgeTimelineItem(
                id: parsed.message.id,
                kind: .message,
                role: parsed.message.role,
                text: parsed.message.text,
                phase: (payload["phase"] as? String).flatMap(CompanionBridgeTimelineItemPhase.init(rawValue:)),
                createdAt: parsed.message.createdAt,
                turnID: parsed.turnID,
                media: inlineMedia(from: payload["content"], messageID: parsed.message.id)
            )
        }

        if recordType == "response_item",
           payloadType == "custom_tool_call" || payloadType == "function_call" {
            let name = (payload["name"] as? String) ?? "tool"
            let input = (payload["input"] as? String) ?? (payload["arguments"] as? String)
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: toolStatus(from: payload["status"] as? String),
                title: toolTitle(name: name, input: input),
                detail: toolDetail(name: name, from: input),
                createdAt: createdAt,
                turnID: turnID,
                callID: payload["call_id"] as? String
            )
        }

        if recordType == "response_item",
           payloadType == "function_call_output" || payloadType == "custom_tool_call_output" {
            let media = inlineMedia(from: payload["output"], messageID: id)
            let detail = textualToolOutput(from: payload["output"])
            guard !media.isEmpty || detail != nil else { return nil }
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: toolOutputStatus(from: payload["output"]),
                title: "Tool result",
                detail: detail,
                createdAt: createdAt,
                turnID: turnID,
                callID: payload["call_id"] as? String,
                media: media
            )
        }

        guard recordType == "event_msg" else { return nil }
        switch payloadType {
        case "agent_reasoning":
            guard let rawText = payload["text"] as? String,
                  let title = nonempty(reasoningTitle(rawText))
            else { return nil }
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .reasoning,
                title: title,
                createdAt: createdAt,
                turnID: turnID
            )
        case "context_compacted":
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .compaction,
                title: "Context compacted",
                createdAt: createdAt,
                turnID: turnID
            )
        case "patch_apply_end":
            let succeeded = (payload["success"] as? Bool) ?? true
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                status: succeeded ? .completed : .failed,
                title: succeeded ? "Edited files" : "File edit failed",
                createdAt: createdAt,
                turnID: turnID,
                callID: payload["call_id"] as? String
            )
        case "mcp_tool_call_end":
            let invocation = payload["invocation"] as? [String: Any]
            let tool = invocation?["tool"] as? String ?? "tool"
            let arguments = invocation?["arguments"]
            let detail = arguments.flatMap { try? JSONSerialization.data(withJSONObject: $0, options: [.sortedKeys]) }
                .map { String(decoding: $0, as: UTF8.self) }
            return CompanionBridgeTimelineItem(
                id: id,
                kind: .tool,
                title: toolTitle(name: tool, input: detail),
                detail: boundedDetail(detail),
                createdAt: createdAt,
                turnID: turnID,
                callID: payload["call_id"] as? String
            )
        default:
            return nil
        }
    }

    private func inlineMedia(from rawContent: Any?, messageID: String) -> [CompanionBridgeMedia] {
        guard let fragments = rawContent as? [[String: Any]] else { return [] }
        return fragments.enumerated().compactMap { index, fragment in
            guard fragment["type"] as? String == "input_image",
                  let url = fragment["image_url"] as? String,
                  url.hasPrefix("data:"),
                  let separator = url.firstIndex(of: ","),
                  url[..<separator].hasSuffix(";base64")
            else { return nil }
            let mimeStart = url.index(url.startIndex, offsetBy: 5)
            let mimeEnd = url.index(separator, offsetBy: -7)
            guard mimeStart <= mimeEnd,
                  let data = Data(base64Encoded: String(url[url.index(after: separator)...])),
                  data.count <= Self.maximumInlineMediaSize
            else { return nil }
            return CompanionBridgeMedia(
                id: "\(messageID)-media-\(index)",
                kind: .image,
                mimeType: String(url[mimeStart..<mimeEnd]),
                data: data
            )
        }
    }

    private func textualToolOutput(from rawOutput: Any?) -> String? {
        if let text = rawOutput as? String {
            guard !text.hasPrefix("data:") else { return nil }
            return boundedDetail(text)
        }

        if let fragments = rawOutput as? [[String: Any]] {
            let text = fragments.compactMap { fragment -> String? in
                for key in ["text", "output_text", "message", "result"] {
                    if let value = fragment[key] as? String, !value.hasPrefix("data:") {
                        return value
                    }
                }
                return nil
            }.joined(separator: "\n")
            return boundedDetail(text)
        }

        if let object = rawOutput as? [String: Any] {
            for key in ["text", "output_text", "message", "result"] {
                if let value = object[key] as? String, !value.hasPrefix("data:") {
                    return boundedDetail(value)
                }
            }
        }
        return nil
    }

    private func reasoningTitle(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("**"), trimmed.hasSuffix("**"), trimmed.count >= 4 {
            return String(trimmed.dropFirst(2).dropLast(2))
        }
        return trimmed
    }

    private func toolStatus(from raw: String?) -> CompanionBridgeTimelineItemStatus {
        switch raw {
        case "failed", "error": return .failed
        case "in_progress", "running": return .inProgress
        default: return .completed
        }
    }

    private func toolTitle(name: String, input: String?) -> String {
        let normalizedName = name.lowercased()
        let leafName = normalizedName.split(separator: "__").last.map(String.init)
            ?? normalizedName

        if ["exec", "exec_command", "write_stdin"].contains(leafName) {
            return "Ran a command"
        }
        if leafName == "js" && normalizedName.contains("node_repl") {
            return "Ran JavaScript"
        }
        if leafName == "view_image" || leafName.contains("screenshot") {
            return "Viewed an image"
        }
        if leafName == "apply_patch" || leafName.contains("patch") {
            return "Edited files"
        }
        if leafName == "find" || leafName == "rg" || leafName.contains("search") {
            return "Searched files"
        }
        if leafName == "spawn_agent" || leafName == "send_input" {
            return "Messaged an agent"
        }
        if leafName == "wait_agent" { return "Wait" }
        if leafName.contains("read") && leafName.contains("file") {
            return "Read a file"
        }
        if leafName == "open", normalizedName.contains("browser") {
            return "Opened a link"
        }

        return leafName
                .replacingOccurrences(of: "_", with: " ")
                .split(separator: " ")
                .map { $0.capitalized }
                .joined(separator: " ")
    }

    private func toolDetail(name: String, from rawInput: String?) -> String? {
        guard let rawInput = nonempty(rawInput ?? "") else { return nil }
        if name == "spawn_agent" || name == "send_input" || name == "wait_agent" {
            return boundedDetail(rawInput)
        }
        if let data = rawInput.data(using: .utf8),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            for key in ["cmd", "path", "q", "query", "pattern", "url", "ref_id", "code", "prompt", "title"] {
                if let value = object[key] as? String, let detail = nonempty(value) {
                    return boundedDetail(detail)
                }
            }
        }
        return boundedDetail(rawInput)
    }

    private func boundedDetail(_ detail: String?) -> String? {
        guard let detail = nonempty(detail ?? "") else { return nil }
        let maximum = Self.maximumToolDetailSize
        return detail.count > maximum ? String(detail.prefix(maximum - 3)) + "..." : detail
    }

    private func toolOutputStatus(from rawOutput: Any?) -> CompanionBridgeTimelineItemStatus {
        if let object = rawOutput as? [String: Any] {
            if object["error"] != nil || object["errored"] != nil {
                return .failed
            }
            if let status = object["status"] as? String,
               status == "failed" || status == "error" || status == "errored" {
                return .failed
            }
        }
        let text = textualToolOutput(from: rawOutput)?.lowercased() ?? ""
        if text.hasPrefix("error:") || text.hasPrefix("script failed")
            || text.contains("process exited with code 1") {
            return .failed
        }
        return .completed
    }

    private func rolloutPath(for threadID: String) throws -> String? {
        let escaped = threadID.replacingOccurrences(of: "'", with: "''")
        return try sqliteRows(query: """
        select rollout_path from threads where id = '\(escaped)' limit 1;
        """).first?.first
    }

    private func latestVisibleMessage(in url: URL) -> (text: String, turnID: String?)? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let size = try? handle.seekToEnd(), size > 0 else { return nil }
        let readLength = min(size, UInt64(1024 * 1024))
        try? handle.seek(toOffset: size - readLength)
        let data = handle.readDataToEndOfFile()
        for line in data.split(separator: 0x0A, omittingEmptySubsequences: true).reversed() {
            if let parsed = visibleMessage(from: Data(line), fallbackID: UUID().uuidString),
               parsed.message.role == .assistant {
                return (parsed.message.text, parsed.turnID)
            }
        }
        return nil
    }

    private func visibleMessage(
        from data: Data,
        fallbackID: String
    ) -> (message: CompanionBridgeMessage, turnID: String?)? {
        guard data.count <= Self.maximumLineSize,
              let raw = try? JSONSerialization.jsonObject(with: data),
              let root = raw as? [String: Any],
              root["type"] as? String == "response_item",
              let payload = root["payload"] as? [String: Any],
              payload["type"] as? String == "message",
              let rawRole = payload["role"] as? String,
              let role = CompanionBridgeMessageRole(rawValue: rawRole),
              let text = visibleText(from: payload["content"]),
              !text.hasPrefix("<codex_internal_context")
        else { return nil }

        let timestamp = (root["timestamp"] as? String).flatMap(Self.timestampFormatter.date(from:))
        let messageID = (payload["id"] as? String)
            ?? (payload["message_id"] as? String)
            ?? fallbackID
        let turnID = (payload["turn_id"] as? String)
            ?? ((payload["internal_chat_message_metadata_passthrough"] as? [String: Any])?["turn_id"] as? String)
        return (
            CompanionBridgeMessage(id: messageID, role: role, text: text, createdAt: timestamp),
            turnID
        )
    }

    private func visibleText(from rawContent: Any?) -> String? {
        if let text = rawContent as? String { return nonempty(sanitizedVisibleText(text)) }
        guard let fragments = rawContent as? [[String: Any]] else { return nil }
        let text = fragments.compactMap { fragment -> String? in
            let type = fragment["type"] as? String
            guard type == "input_text" || type == "output_text" else { return nil }
            return fragment["text"] as? String
        }.joined(separator: "\n")
        return nonempty(sanitizedVisibleText(text))
    }

    private func sanitizedVisibleText(_ rawText: String) -> String {
        var text = rawText.trimmingCharacters(in: .whitespacesAndNewlines)
        text = strippingBoundaryEnvironmentContext(from: text)
        if text.hasPrefix("<subagent_notification"),
           text.hasSuffix("</subagent_notification>") {
            return ""
        }

        let openingPrefix = "<in-app-browser-context"
        let trustedSource = "source=\"ambient-ui-state\""
        let closingTag = "</in-app-browser-context>"
        var removedAmbientContext = false

        while text.hasPrefix(openingPrefix),
              let openingEnd = text.firstIndex(of: ">"),
              text[...openingEnd].contains(trustedSource),
              let closingRange = text.range(
                of: closingTag,
                range: openingEnd..<text.endIndex
              ) {
            text = String(text[closingRange.upperBound...])
                .trimmingCharacters(in: .whitespacesAndNewlines)
            removedAmbientContext = true
        }

        let requestHeading = "## My request for Codex:"
        if let headingRange = text.range(of: requestHeading, options: .backwards) {
            let prefix = text[..<headingRange.lowerBound]
            let hasGeneratedMetadata = removedAmbientContext
                || prefix.contains("# Files mentioned by the user:")
                || prefix.contains("# Applications mentioned by the user:")
                || prefix.contains("<in-app-browser-context source=\"ambient-ui-state\">")
            if hasGeneratedMetadata {
                text = String(text[headingRange.upperBound...])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        text = text.replacingOccurrences(
            of: #"(?is)[ \t]*<image\s+name=\[[^\]]+\]\s+path=\"[^\"]*\">[ \t\r\n]*</image>[ \t]*"#,
            with: "",
            options: .regularExpression
        )
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func strippingBoundaryEnvironmentContext(from rawText: String) -> String {
        let openingTag = "<environment_context>"
        let closingTag = "</environment_context>"
        var text = rawText.trimmingCharacters(in: .whitespacesAndNewlines)

        while text.hasPrefix(openingTag),
              let closingRange = text.range(of: closingTag) {
            text = String(text[closingRange.upperBound...])
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }

        while text.hasSuffix(closingTag),
              let openingRange = text.range(of: openingTag, options: .backwards) {
            text = String(text[..<openingRange.lowerBound])
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }

        return text
    }

    private func sqliteRows(query: String) throws -> [[String]] {
        let database = homeDirectory
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("state_5.sqlite")
        guard FileManager.default.fileExists(atPath: database.path) else {
            throw CodexMobileArchiveError.databaseMissing
        }

        let columnSeparator = "\u{1f}"
        let rowSeparator = "\u{1e}"
        let result = try CodexSQLiteProcessRunner.run(
            executableURL: sqliteExecutableURL,
            arguments: [
            "-separator", columnSeparator,
            "-newline", rowSeparator,
            database.path,
            query,
            ]
        )
        guard result.terminationStatus == 0 else {
            let detail = String(decoding: result.standardError, as: UTF8.self)
            throw CodexMobileArchiveError.sqliteFailed(detail)
        }
        let text = String(decoding: result.standardOutput, as: UTF8.self)
        return text
            .split(separator: Character(rowSeparator), omittingEmptySubsequences: true)
            .map { row in
                row.split(separator: Character(columnSeparator), omittingEmptySubsequences: false).map(String.init)
            }
    }

    private func displayTitle(
        threadID: String,
        storedTitle: String,
        cwd: String,
        firstMessage: String,
        sessionNames: [String: String]
    ) -> String {
        if let indexedTitle = nonempty(sessionNames[threadID] ?? "") {
            return boundedTitle(indexedTitle)
        }
        if let title = nonempty(storedTitle), title != firstMessage {
            return boundedTitle(title)
        }
        if let folder = nonempty(cwd).map({ URL(fileURLWithPath: $0).lastPathComponent }), !folder.isEmpty {
            return folder.replacingOccurrences(of: "-", with: " ").capitalized
        }
        if let first = nonempty(firstMessage) {
            return first.count > 64 ? String(first.prefix(61)) + "..." : first
        }
        return "Codex task"
    }

    private func sessionThreadNames() -> [String: String] {
        let url = homeDirectory
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("session_index.jsonl")
        guard
            let data = try? Data(contentsOf: url),
            let text = String(data: data, encoding: .utf8)
        else { return [:] }

        var names: [String: String] = [:]
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            guard
                let data = String(line).data(using: .utf8),
                let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                let id = object["id"] as? String,
                let name = object["thread_name"] as? String,
                nonempty(id) != nil,
                let trimmedName = nonempty(name)
            else { continue }
            names[id] = trimmedName
        }
        return names
    }

    private func boundedTitle(_ title: String) -> String {
        title.count > 96 ? String(title.prefix(93)) + "..." : title
    }

    private func rolloutURL(from rawPath: String) -> URL {
        rawPath.hasPrefix("/")
            ? URL(fileURLWithPath: rawPath)
            : homeDirectory.appendingPathComponent(".codex").appendingPathComponent(rawPath)
    }

    private func boundedLimit(_ requested: Int?, fallback: Int) -> Int {
        min(max(requested ?? fallback, 1), CompanionBridgeProtocol.maximumPageSize)
    }

    private func nonempty(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    private func date(fromMilliseconds raw: String) -> Date? {
        guard let milliseconds = Double(raw), milliseconds > 0 else { return nil }
        return Date(timeIntervalSince1970: milliseconds / 1000)
    }

    private static let timestampFormatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()
}

enum CodexMobileArchiveError: LocalizedError {
    case databaseMissing
    case invalidThreadID
    case threadNotFound
    case historyMissing
    case historyLineTooLarge
    case sqliteFailed(String)

    var errorDescription: String? {
        switch self {
        case .databaseMissing: return "Codex task storage was not found on this Mac."
        case .invalidThreadID: return "Choose a Codex task first."
        case .threadNotFound: return "That Codex task is no longer available."
        case .historyMissing: return "This task's local history is unavailable."
        case .historyLineTooLarge: return "A task-history entry is too large to display safely."
        case .sqliteFailed(let detail):
            let trimmed = detail.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? "Codex task storage could not be read." : trimmed
        }
    }
}

struct CodexSQLiteProcessResult: Sendable {
    var terminationStatus: Int32
    var standardOutput: Data
    var standardError: Data
}

enum CodexSQLiteProcessRunner {
    static func run(executableURL: URL, arguments: [String]) throws -> CodexSQLiteProcessResult {
        let fileManager = FileManager.default
        let captureDirectory = fileManager.temporaryDirectory
            .appendingPathComponent("CodexSQLiteProcess-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: captureDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: captureDirectory) }

        let outputURL = captureDirectory.appendingPathComponent("stdout")
        let errorURL = captureDirectory.appendingPathComponent("stderr")
        guard
            fileManager.createFile(atPath: outputURL.path, contents: nil),
            fileManager.createFile(atPath: errorURL.path, contents: nil)
        else {
            throw CocoaError(.fileWriteUnknown)
        }
        let outputHandle = try FileHandle(forWritingTo: outputURL)
        let errorHandle = try FileHandle(forWritingTo: errorURL)
        defer {
            try? outputHandle.close()
            try? errorHandle.close()
        }

        let process = Process()
        process.executableURL = executableURL
        process.arguments = arguments
        process.standardOutput = outputHandle
        process.standardError = errorHandle

        try process.run()
        process.waitUntilExit()
        try outputHandle.synchronize()
        try errorHandle.synchronize()
        return CodexSQLiteProcessResult(
            terminationStatus: process.terminationStatus,
            standardOutput: try Data(contentsOf: outputURL),
            standardError: try Data(contentsOf: errorURL)
        )
    }
}
