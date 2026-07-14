import Foundation

struct CodexSidebarOrderingSnapshot: Sendable {
    struct Entry: Sendable {
        var threadID: String?
        var cwd: String?
        var statusRank: Int
        var updatedAt: Date?
        var isAttentionPromoted: Bool
    }

    private let pinnedRanks: [String: Int]
    private let projectRanks: [String: Int]
    private let projectRoots: [String]
    private let projectlessThreadIDs: Set<String>
    private let threadWorkspaceRoots: [String: String]
    private let workspaceRootLabels: [String: String]

    init(
        pinnedThreadIDs: [String] = [],
        projectOrder: [String] = [],
        projectlessThreadIDs: [String] = [],
        threadWorkspaceRoots: [String: String] = [:],
        workspaceRootLabels: [String: String] = [:]
    ) {
        pinnedRanks = pinnedThreadIDs.enumerated().reduce(into: [:]) { result, pair in
            result[pair.element, default: pair.offset] = min(result[pair.element] ?? pair.offset, pair.offset)
        }
        projectRoots = projectOrder.map(Self.normalizedPath)
        projectRanks = projectRoots.enumerated().reduce(into: [:]) { result, pair in
            let path = pair.element
            result[path, default: pair.offset] = min(result[path] ?? pair.offset, pair.offset)
        }
        self.projectlessThreadIDs = Set(projectlessThreadIDs)
        self.threadWorkspaceRoots = threadWorkspaceRoots.reduce(into: [:]) { result, pair in
            result[pair.key] = Self.normalizedPath(pair.value)
        }
        self.workspaceRootLabels = workspaceRootLabels.reduce(into: [:]) { result, pair in
            let label = pair.value.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !label.isEmpty else { return }
            result[Self.normalizedPath(pair.key)] = label
        }
    }

    static func load(
        homeDirectory: URL = FileManager.default.homeDirectoryForCurrentUser
    ) -> CodexSidebarOrderingSnapshot {
        let stateURL = homeDirectory
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent(".codex-global-state.json")
        guard
            let data = try? Data(contentsOf: stateURL),
            let raw = try? JSONSerialization.jsonObject(with: data),
            let object = raw as? [String: Any]
        else { return CodexSidebarOrderingSnapshot() }

        return CodexSidebarOrderingSnapshot(
            pinnedThreadIDs: object["pinned-thread-ids"] as? [String] ?? [],
            projectOrder: object["project-order"] as? [String] ?? [],
            projectlessThreadIDs: object["projectless-thread-ids"] as? [String] ?? [],
            threadWorkspaceRoots: object["thread-workspace-root-hints"] as? [String: String] ?? [:],
            workspaceRootLabels: object["electron-workspace-root-labels"] as? [String: String] ?? [:]
        )
    }

    func taskGroup(threadID: String, cwd: String?) -> CompanionBridgeTaskGroup {
        if projectlessThreadIDs.contains(threadID) || Self.isCodexDocumentsPath(cwd) {
            return CompanionBridgeTaskGroup(kind: .chats, title: "Chats", path: nil)
        }

        let normalizedCWD = cwd.map(Self.normalizedPath)
        let hintedRoot = threadWorkspaceRoots[threadID]
        let knownRoot = hintedRoot.flatMap { hint in
            projectRoots.contains(hint) || workspaceRootLabels[hint] != nil ? hint : nil
        } ?? projectRoots
            .filter { root in
                guard let normalizedCWD else { return false }
                return normalizedCWD == root || normalizedCWD.hasPrefix(root + "/")
            }
            .max(by: { $0.count < $1.count })

        if let root = knownRoot {
            return CompanionBridgeTaskGroup(
                kind: .project,
                title: workspaceRootLabels[root] ?? Self.fallbackProjectTitle(root),
                path: root
            )
        }

        guard let normalizedCWD else {
            return CompanionBridgeTaskGroup(kind: .chats, title: "Chats", path: nil)
        }
        return CompanionBridgeTaskGroup(
            kind: .project,
            title: workspaceRootLabels[normalizedCWD] ?? Self.fallbackProjectTitle(normalizedCWD),
            path: normalizedCWD
        )
    }

    func orders(_ lhs: Entry, before rhs: Entry) -> Bool {
        let lhsAttention = lhs.isAttentionPromoted ? 0 : 1
        let rhsAttention = rhs.isAttentionPromoted ? 0 : 1
        if lhsAttention != rhsAttention { return lhsAttention < rhsAttention }

        let lhsPin = pinRank(for: lhs.threadID)
        let rhsPin = pinRank(for: rhs.threadID)
        if lhsPin != rhsPin { return lhsPin < rhsPin }

        let lhsProject = projectRank(threadID: lhs.threadID, cwd: lhs.cwd)
        let rhsProject = projectRank(threadID: rhs.threadID, cwd: rhs.cwd)
        if lhsProject != rhsProject { return lhsProject < rhsProject }

        if lhs.statusRank != rhs.statusRank { return lhs.statusRank < rhs.statusRank }
        if lhs.updatedAt != rhs.updatedAt {
            return (lhs.updatedAt ?? .distantPast) > (rhs.updatedAt ?? .distantPast)
        }
        return (lhs.threadID ?? "") < (rhs.threadID ?? "")
    }

    func isPinned(_ threadID: String?) -> Bool {
        guard let threadID else { return false }
        return pinnedRanks[threadID] != nil
    }

    private func pinRank(for threadID: String?) -> Int {
        guard let threadID, let rank = pinnedRanks[threadID] else { return Int.max }
        return rank
    }

    private func projectRank(threadID: String?, cwd: String?) -> Int {
        let projectCount = projectRanks.count
        if let threadID,
           let hintedRoot = threadWorkspaceRoots[threadID],
           let rank = projectRanks[hintedRoot]
        {
            return rank
        }

        let normalizedCWD = cwd.map(Self.normalizedPath) ?? ""
        if !normalizedCWD.isEmpty {
            let matchingRank = projectRanks.compactMap { root, rank -> Int? in
                normalizedCWD == root || normalizedCWD.hasPrefix(root + "/") ? rank : nil
            }.min()
            if let matchingRank { return matchingRank }
        }

        if let threadID, projectlessThreadIDs.contains(threadID) {
            return projectCount + 1
        }
        return projectCount
    }

    private static func normalizedPath(_ path: String) -> String {
        URL(fileURLWithPath: path).standardizedFileURL.path
    }

    private static func fallbackProjectTitle(_ path: String) -> String {
        let title = URL(fileURLWithPath: path).lastPathComponent
        return title.isEmpty ? path : title
    }

    private static func isCodexDocumentsPath(_ path: String?) -> Bool {
        guard let path else { return false }
        let components = URL(fileURLWithPath: path).standardizedFileURL.pathComponents
        return components.indices.dropLast().contains { index in
            components[index] == "Documents" && components[index + 1] == "Codex"
        }
    }
}

final class CodexApprovalPromotionTracker: @unchecked Sendable {
    static let shared = CodexApprovalPromotionTracker()

    private let lock = NSLock()
    private let holdDuration: TimeInterval
    private var previousPendingThreadIDs: Set<String>?
    private var holdUntilByThreadID: [String: Date] = [:]

    init(holdDuration: TimeInterval = 10) {
        self.holdDuration = holdDuration
    }

    func promotedThreadIDs(
        pendingThreadIDs: Set<String>,
        now: Date = Date()
    ) -> Set<String> {
        lock.lock()
        defer { lock.unlock() }

        if let previousPendingThreadIDs {
            for resolvedThreadID in previousPendingThreadIDs.subtracting(pendingThreadIDs) {
                holdUntilByThreadID[resolvedThreadID] = now.addingTimeInterval(holdDuration)
            }
        }
        previousPendingThreadIDs = pendingThreadIDs
        holdUntilByThreadID = holdUntilByThreadID.filter { $0.value > now }
        return pendingThreadIDs.union(holdUntilByThreadID.keys)
    }
}
