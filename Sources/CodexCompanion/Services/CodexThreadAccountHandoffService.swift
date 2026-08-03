import Foundation

protocol CodexAccountProfileRPCClientProviding: Sendable {
    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming
}

struct CodexThreadAccountHandoffResult: Equatable, Sendable {
    var threadID: String
    var runtimeThreadID: String
    var rolloutURL: URL
    var profileID: UUID

    init(
        threadID: String,
        runtimeThreadID: String? = nil,
        rolloutURL: URL,
        profileID: UUID
    ) {
        self.threadID = threadID
        self.runtimeThreadID = runtimeThreadID ?? threadID
        self.rolloutURL = rolloutURL
        self.profileID = profileID
    }
}

struct CodexThreadRuntimeSettings: Equatable, Sendable {
    var model: String?
    var reasoningEffort: String?
}

struct CodexThreadRuntimeSettingsCatalogReader: Sendable {
    var databaseURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
        .appendingPathComponent("state_5.sqlite")
    var sqliteExecutableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")

    func settings(for threadID: String) throws -> CodexThreadRuntimeSettings {
        var visitedThreadIDs: Set<String> = []
        return try settings(for: threadID, visitedThreadIDs: &visitedThreadIDs)
    }

    private func settings(
        for threadID: String,
        visitedThreadIDs: inout Set<String>
    ) throws -> CodexThreadRuntimeSettings {
        guard visitedThreadIDs.insert(threadID).inserted else {
            return CodexThreadRuntimeSettings(model: nil, reasoningEffort: nil)
        }
        let escapedThreadID = threadID.replacingOccurrences(of: "'", with: "''")
        let columnSeparator = "\u{1f}"
        let rowSeparator = "\u{1e}"
        let query = """
        select coalesce(model, ''), coalesce(reasoning_effort, ''), rollout_path
        from threads
        where id = '\(escapedThreadID)'
        limit 1;
        """
        let arguments = CodexSQLiteProcessRunner.readArguments(
            databaseURL: databaseURL,
            query: query,
            columnSeparator: columnSeparator,
            rowSeparator: rowSeparator
        )
        let result = try CodexSQLiteProcessRunner.retryingTransientRead {
            try CodexSQLiteProcessRunner.run(
                executableURL: sqliteExecutableURL,
                arguments: arguments
            )
        }
        guard result.terminationStatus == 0 else {
            let detail = String(decoding: result.standardError, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            throw CodexThreadRuntimeSettingsCatalogError.readFailed(detail)
        }
        let columns = String(decoding: result.standardOutput, as: UTF8.self)
            .split(separator: Character(rowSeparator), omittingEmptySubsequences: true)
            .first?
            .split(separator: Character(columnSeparator), omittingEmptySubsequences: false)
            .map(String.init)
        guard let columns, columns.count >= 3 else {
            return CodexThreadRuntimeSettings(model: nil, reasoningEffort: nil)
        }
        var resolvedSettings = CodexThreadRuntimeSettings(
            model: Self.nonempty(columns[0]),
            reasoningEffort: Self.nonempty(columns[1])
        )
        if (resolvedSettings.model == nil || resolvedSettings.reasoningEffort == nil),
           let parentThreadID = Self.forkedFromThreadID(
               rolloutURL: URL(fileURLWithPath: columns[2])
           ) {
            let parentSettings = try settings(
                for: parentThreadID,
                visitedThreadIDs: &visitedThreadIDs
            )
            resolvedSettings.model = resolvedSettings.model ?? parentSettings.model
            resolvedSettings.reasoningEffort = resolvedSettings.reasoningEffort
                ?? parentSettings.reasoningEffort
        }
        return resolvedSettings
    }

    static func forkedFromThreadID(rolloutURL: URL) -> String? {
        guard
            let handle = try? FileHandle(forReadingFrom: rolloutURL),
            let data = try? handle.read(upToCount: 8 * 1_024 * 1_024)
        else { return nil }
        try? handle.close()
        let firstLine = data.firstIndex(of: 0x0A).map { data[..<$0] } ?? data[...]
        guard
            let object = try? JSONSerialization.jsonObject(with: Data(firstLine)) as? [String: Any],
            object["type"] as? String == "session_meta",
            let payload = object["payload"] as? [String: Any],
            let parentThreadID = payload["forked_from_id"] as? String
        else { return nil }
        return nonempty(parentThreadID)
    }

    private static func nonempty(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

enum CodexThreadRuntimeSettingsCatalogError: LocalizedError {
    case readFailed(String)

    var errorDescription: String? {
        switch self {
        case .readFailed(let detail):
            return detail.isEmpty
                ? "The task's model settings could not be read, so its account was not changed."
                : detail
        }
    }
}

enum CodexThreadAccountHandoffError: Error, Equatable {
    case activeTurn
    case invalidThreadID
    case invalidRolloutPath
    case missingResponse
    case server(String)
    case invalidResponse
    case forkMismatch
    case lineageConflict
    case destinationBindingConflict
    case unverifiableAccountIdentity
    case accountIdentityMismatch
}

extension CodexThreadAccountHandoffError: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .activeTurn:
            return "Wait for the current Codex turn to stop before switching accounts."
        case .invalidThreadID:
            return "The Codex task does not have a valid thread identifier."
        case .invalidRolloutPath:
            return "The Codex task does not have a valid persisted rollout path."
        case .missingResponse:
            return "Codex did not return a response for the account handoff."
        case .server(let message):
            return message
        case .invalidResponse:
            return "Codex returned an unreadable account handoff response."
        case .forkMismatch:
            return "Codex did not create a separate continuation of this task, so the account was not changed."
        case .lineageConflict:
            return "Codex returned a continuation that belongs to another task."
        case .destinationBindingConflict:
            return "The continued task was already assigned to a different Codex account."
        case .unverifiableAccountIdentity:
            return "Codex could not verify which ChatGPT account is active for this profile."
        case .accountIdentityMismatch:
            return "The Codex account service is signed in to a different ChatGPT account than this profile."
        }
    }
}

enum CodexAccountProfileRuntime {
    static var defaultBaseURL: URL {
        FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        )[0]
        .appendingPathComponent("Codex Companion", isDirectory: true)
        .appendingPathComponent("Codex Profiles", isDirectory: true)
    }

    static var defaultSharedSQLiteHomeURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
    }

    static var defaultManagedStandaloneURL: URL {
        defaultSharedSQLiteHomeURL
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
    }

    static var defaultDaemonBaseURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".ccp", isDirectory: true)
    }

    static func homeURL(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL
    ) -> URL {
        baseURL.standardizedFileURL.appendingPathComponent(
            profile.id.uuidString.lowercased(),
            isDirectory: true
        )
    }

    static func daemonHomeURL(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultDaemonBaseURL
    ) -> URL {
        let compactID = profile.id.uuidString
            .replacingOccurrences(of: "-", with: "")
            .lowercased()
            .prefix(16)
        return baseURL.standardizedFileURL.appendingPathComponent(
            String(compactID),
            isDirectory: true
        )
    }

    static func taskEnvironment(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL,
        sharedSQLiteHomeURL: URL = defaultSharedSQLiteHomeURL
    ) -> [String: String] {
        [
            "CODEX_HOME": homeURL(for: profile, baseURL: baseURL).path,
            "CODEX_SQLITE_HOME": sharedSQLiteHomeURL.standardizedFileURL.path,
        ]
    }

    static func daemonTaskEnvironment(
        for profile: CodexAccountProfile,
        daemonBaseURL: URL = defaultDaemonBaseURL,
        sharedSQLiteHomeURL: URL = defaultSharedSQLiteHomeURL
    ) -> [String: String] {
        [
            "CODEX_HOME": daemonHomeURL(for: profile, baseURL: daemonBaseURL).path,
            "CODEX_SQLITE_HOME": sharedSQLiteHomeURL.standardizedFileURL.path,
        ]
    }

    static func prepareHome(
        for profile: CodexAccountProfile,
        baseURL: URL = defaultBaseURL,
        managedStandaloneURL: URL? = nil
    ) throws {
        let profileHomeURL = homeURL(for: profile, baseURL: baseURL)
        try FileManager.default.createDirectory(
            at: profileHomeURL,
            withIntermediateDirectories: true
        )
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: profileHomeURL.path
        )

        if let managedStandaloneURL {
            try linkManagedStandaloneRuntime(
                into: profileHomeURL,
                managedStandaloneURL: managedStandaloneURL
            )
        }
    }

    static func prepareDaemonHome(
        for profile: CodexAccountProfile,
        daemonBaseURL: URL = defaultDaemonBaseURL,
        credentialBaseURL: URL = defaultBaseURL,
        managedStandaloneURL: URL = defaultManagedStandaloneURL
    ) throws {
        try prepareHome(
            for: profile,
            baseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )

        let fileManager = FileManager.default
        try fileManager.createDirectory(
            at: daemonBaseURL,
            withIntermediateDirectories: true
        )
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: daemonBaseURL.path
        )

        let credentialHomeURL = homeURL(for: profile, baseURL: credentialBaseURL)
        let daemonHomeURL = daemonHomeURL(for: profile, baseURL: daemonBaseURL)
        if let attributes = try? fileManager.attributesOfItem(atPath: daemonHomeURL.path) {
            switch attributes[.type] as? FileAttributeType {
            case .typeDirectory:
                break
            case .typeSymbolicLink:
                let destination = try fileManager.destinationOfSymbolicLink(
                    atPath: daemonHomeURL.path
                )
                let destinationURL = URL(
                    fileURLWithPath: destination,
                    relativeTo: daemonHomeURL.deletingLastPathComponent()
                ).standardizedFileURL
                guard destinationURL == credentialHomeURL.standardizedFileURL else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        daemonHomeURL.path
                    )
                }
                try fileManager.removeItem(at: daemonHomeURL)
                try fileManager.createDirectory(
                    at: daemonHomeURL,
                    withIntermediateDirectories: true
                )
            default:
                throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                    daemonHomeURL.path
                )
            }
        } else {
            try fileManager.createDirectory(
                at: daemonHomeURL,
                withIntermediateDirectories: true
            )
        }
        try fileManager.setAttributes(
            [.posixPermissions: 0o700],
            ofItemAtPath: daemonHomeURL.path
        )

        let sharedCredentialNames: Set<String> = [
            ".personality_migration",
            ".sandbox_migration",
            "auth.json",
            "config.toml",
            "installation_id",
            "models_cache.json",
            "packages",
            "plugins",
            "skills",
        ]
        let credentialArtifacts = try fileManager.contentsOfDirectory(
            at: credentialHomeURL,
            includingPropertiesForKeys: nil
        )
        for sourceURL in credentialArtifacts
            where sharedCredentialNames.contains(sourceURL.lastPathComponent)
        {
            let destinationURL = daemonHomeURL.appendingPathComponent(
                sourceURL.lastPathComponent,
                isDirectory: sourceURL.hasDirectoryPath
            )
            if let attributes = try? fileManager.attributesOfItem(
                atPath: destinationURL.path
            ) {
                guard attributes[.type] as? FileAttributeType == .typeSymbolicLink else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        destinationURL.path
                    )
                }
                let existingDestination = try fileManager.destinationOfSymbolicLink(
                    atPath: destinationURL.path
                )
                let existingDestinationURL = URL(
                    fileURLWithPath: existingDestination,
                    relativeTo: destinationURL.deletingLastPathComponent()
                ).standardizedFileURL
                guard existingDestinationURL == sourceURL.standardizedFileURL else {
                    throw CodexAccountProfileRuntimeError.daemonAliasConflict(
                        destinationURL.path
                    )
                }
                continue
            }

            try fileManager.createSymbolicLink(
                at: destinationURL,
                withDestinationURL: sourceURL.standardizedFileURL
            )
        }
    }

    private static func linkManagedStandaloneRuntime(
        into profileHomeURL: URL,
        managedStandaloneURL: URL
    ) throws {
        let fileManager = FileManager.default
        let managedCodexURL = managedStandaloneURL
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")
        guard fileManager.isExecutableFile(atPath: managedCodexURL.path) else {
            throw CodexAccountProfileRuntimeError.managedStandaloneUnavailable(
                managedCodexURL.path
            )
        }

        let packagesURL = profileHomeURL.appendingPathComponent(
            "packages",
            isDirectory: true
        )
        try fileManager.createDirectory(
            at: packagesURL,
            withIntermediateDirectories: true
        )
        let profileStandaloneURL = packagesURL.appendingPathComponent(
            "standalone",
            isDirectory: true
        )

        if let attributes = try? fileManager.attributesOfItem(
            atPath: profileStandaloneURL.path
        ) {
            if attributes[.type] as? FileAttributeType == .typeSymbolicLink {
                let destination = try fileManager.destinationOfSymbolicLink(
                    atPath: profileStandaloneURL.path
                )
                let destinationURL = URL(
                    fileURLWithPath: destination,
                    relativeTo: profileStandaloneURL.deletingLastPathComponent()
                ).standardizedFileURL
                if destinationURL.path == managedStandaloneURL.standardizedFileURL.path {
                    return
                }
                try fileManager.removeItem(at: profileStandaloneURL)
            } else {
                let existingCodexURL = profileStandaloneURL
                    .appendingPathComponent("current", isDirectory: true)
                    .appendingPathComponent("codex")
                guard fileManager.isExecutableFile(atPath: existingCodexURL.path) else {
                    throw CodexAccountProfileRuntimeError.managedStandaloneConflict(
                        profileStandaloneURL.path
                    )
                }
                return
            }
        }

        try fileManager.createSymbolicLink(
            at: profileStandaloneURL,
            withDestinationURL: managedStandaloneURL.standardizedFileURL
        )
    }
}

enum CodexAccountProfileRuntimeError: LocalizedError {
    case managedStandaloneUnavailable(String)
    case managedStandaloneConflict(String)
    case daemonAliasConflict(String)

    var errorDescription: String? {
        switch self {
        case .managedStandaloneUnavailable(let path):
            return "The managed Codex runtime is unavailable at \(path)."
        case .managedStandaloneConflict(let path):
            return "The Codex profile runtime at \(path) is incomplete."
        case .daemonAliasConflict(let path):
            return "The Codex profile daemon path at \(path) is already in use."
        }
    }
}

struct CodexThreadLineage: Codable, Equatable, Sendable {
    var canonicalThreadID: String
    var activeThreadID: String
    var physicalThreadIDs: [String]
}

struct CodexLegacyThreadLineageReader: Sendable {
    private struct ThreadRecord {
        var rolloutURL: URL
        var createdAt: Double
        var activityAt: Double
    }

    var databaseURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
        .appendingPathComponent("state_5.sqlite")
    var sqliteExecutableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")

    func lineages(bindings: [String: String]) -> [CodexThreadLineage] {
        let normalizedBindings = bindings.reduce(into: [String: UUID]()) { result, entry in
            guard let profileID = UUID(uuidString: entry.value) else { return }
            result[entry.key] = profileID
        }
        guard !normalizedBindings.isEmpty else { return [] }
        let quotedIDs = normalizedBindings.keys
            .map { "'\($0.replacingOccurrences(of: "'", with: "''"))'" }
            .joined(separator: ",")
        let query = """
        select id, rollout_path, created_at,
               max(
                   coalesce(recency_at_ms, 0),
                   coalesce(updated_at_ms, 0),
                   coalesce(updated_at, 0) * 1000,
                   coalesce(created_at, 0) * 1000
               )
        from threads
        where id in (\(quotedIDs));
        """
        let columnSeparator = "\u{1f}"
        let rowSeparator = "\u{1e}"
        let arguments = CodexSQLiteProcessRunner.readArguments(
            databaseURL: databaseURL,
            query: query,
            columnSeparator: columnSeparator,
            rowSeparator: rowSeparator
        )
        guard
            let result = try? CodexSQLiteProcessRunner.retryingTransientRead(operation: {
                try CodexSQLiteProcessRunner.run(
                    executableURL: sqliteExecutableURL,
                    arguments: arguments
                )
            }),
            result.terminationStatus == 0
        else { return [] }
        let records = String(decoding: result.standardOutput, as: UTF8.self)
            .split(separator: Character(rowSeparator), omittingEmptySubsequences: true)
            .reduce(into: [String: ThreadRecord]()) { records, row in
                let columns = row.split(
                    separator: Character(columnSeparator),
                    omittingEmptySubsequences: false
                ).map(String.init)
                guard columns.count >= 4 else { return }
                records[columns[0]] = ThreadRecord(
                    rolloutURL: URL(fileURLWithPath: columns[1]),
                    createdAt: Double(columns[2]) ?? 0,
                    activityAt: Double(columns[3]) ?? 0
                )
            }

        var parentByChild: [String: String] = [:]
        var adjacency: [String: Set<String>] = [:]
        for (childThreadID, record) in records {
            guard
                let parentThreadID = CodexThreadRuntimeSettingsCatalogReader
                    .forkedFromThreadID(rolloutURL: record.rolloutURL),
                records[parentThreadID] != nil,
                let childProfileID = normalizedBindings[childThreadID],
                let parentProfileID = normalizedBindings[parentThreadID],
                childProfileID != parentProfileID
            else { continue }
            parentByChild[childThreadID] = parentThreadID
            adjacency[childThreadID, default: []].insert(parentThreadID)
            adjacency[parentThreadID, default: []].insert(childThreadID)
        }

        var visited: Set<String> = []
        return adjacency.keys.compactMap { startThreadID -> CodexThreadLineage? in
            guard visited.insert(startThreadID).inserted else { return nil }
            var members = [startThreadID]
            var index = 0
            while index < members.count {
                let threadID = members[index]
                index += 1
                for neighbor in adjacency[threadID] ?? [] where visited.insert(neighbor).inserted {
                    members.append(neighbor)
                }
            }
            let ordered = members.sorted {
                (records[$0]?.createdAt ?? 0) < (records[$1]?.createdAt ?? 0)
            }
            guard ordered.count > 1 else { return nil }
            let activeThreadID = ordered.max {
                let left = records[$0]
                let right = records[$1]
                if left?.activityAt == right?.activityAt {
                    return (left?.createdAt ?? 0) < (right?.createdAt ?? 0)
                }
                return (left?.activityAt ?? 0) < (right?.activityAt ?? 0)
            } ?? ordered[0]
            let roots = ordered.filter { parentByChild[$0] == nil }
            return CodexThreadLineage(
                canonicalThreadID: roots.first ?? ordered[0],
                activeThreadID: activeThreadID,
                physicalThreadIDs: ordered
            )
        }
    }
}

final class CodexThreadLineageStore: @unchecked Sendable {
    static let lineagesKey = "com.silverfire.codexcompanion.codex-thread-lineages"

    private static let lock = NSLock()
    private static let legacyInferenceLock = NSLock()
    private static var cachedLegacyLineages: [CodexThreadLineage]?
    private let defaults: UserDefaults
    private let inferredLineages: [CodexThreadLineage]

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if defaults === UserDefaults.standard {
            inferredLineages = Self.legacyInferenceLock.withLock {
                if let cachedLegacyLineages = Self.cachedLegacyLineages {
                    return cachedLegacyLineages
                }
                let bindings = defaults.dictionary(
                    forKey: CodexThreadAccountProfileBindingStore.bindingsKey
                ) as? [String: String] ?? [:]
                let lineages = CodexLegacyThreadLineageReader().lineages(bindings: bindings)
                Self.cachedLegacyLineages = lineages
                return lineages
            }
        } else {
            inferredLineages = []
        }
    }

    func canonicalThreadID(for threadID: String) -> String {
        let normalized = Self.normalize(threadID)
        guard !normalized.isEmpty else { return normalized }
        return Self.lock.withLock {
            lineage(containing: normalized, in: loadLineages())?.canonicalThreadID ?? normalized
        }
    }

    func activeThreadID(for threadID: String) -> String {
        let normalized = Self.normalize(threadID)
        guard !normalized.isEmpty else { return normalized }
        return Self.lock.withLock {
            lineage(containing: normalized, in: loadLineages())?.activeThreadID ?? normalized
        }
    }

    func lineages() -> [CodexThreadLineage] {
        Self.lock.withLock { loadLineages() }
    }

    func canRegisterFork(sourceThreadID: String, destinationThreadID: String) -> Bool {
        let source = Self.normalize(sourceThreadID)
        let destination = Self.normalize(destinationThreadID)
        guard !source.isEmpty, !destination.isEmpty, source != destination else { return false }
        return Self.lock.withLock {
            let lineages = loadLineages()
            let sourceLineage = lineage(containing: source, in: lineages)
            guard (sourceLineage?.activeThreadID ?? source) == source else { return false }
            return lineage(containing: destination, in: lineages) == nil
        }
    }

    @discardableResult
    func registerFork(sourceThreadID: String, destinationThreadID: String) -> String? {
        let source = Self.normalize(sourceThreadID)
        let destination = Self.normalize(destinationThreadID)
        guard !source.isEmpty, !destination.isEmpty, source != destination else { return nil }
        return Self.lock.withLock {
            var lineages = loadLineages()
            let sourceIndex = lineages.firstIndex { $0.physicalThreadIDs.contains(source) }
            let destinationIndex = lineages.firstIndex { $0.physicalThreadIDs.contains(destination) }
            if let sourceIndex, lineages[sourceIndex].activeThreadID != source { return nil }
            if destinationIndex != nil { return nil }

            let canonicalThreadID: String
            if let sourceIndex {
                canonicalThreadID = lineages[sourceIndex].canonicalThreadID
                if !lineages[sourceIndex].physicalThreadIDs.contains(destination) {
                    lineages[sourceIndex].physicalThreadIDs.append(destination)
                }
                lineages[sourceIndex].activeThreadID = destination
            } else {
                canonicalThreadID = source
                lineages.append(CodexThreadLineage(
                    canonicalThreadID: source,
                    activeThreadID: destination,
                    physicalThreadIDs: [source, destination]
                ))
            }
            saveLineages(lineages)
            return canonicalThreadID
        }
    }

    private func lineage(
        containing threadID: String,
        in lineages: [CodexThreadLineage]
    ) -> CodexThreadLineage? {
        lineages.first { $0.physicalThreadIDs.contains(threadID) }
    }

    private func loadLineages() -> [CodexThreadLineage] {
        let persisted: [CodexThreadLineage]
        if let data = defaults.data(forKey: Self.lineagesKey) {
            persisted = (try? JSONDecoder().decode([CodexThreadLineage].self, from: data)) ?? []
        } else {
            persisted = []
        }
        return inferredLineages.reduce(into: persisted) { lineages, inferred in
            guard let index = lineages.firstIndex(where: {
                !Set($0.physicalThreadIDs).isDisjoint(with: inferred.physicalThreadIDs)
            }) else {
                lineages.append(inferred)
                return
            }
            for threadID in inferred.physicalThreadIDs
                where !lineages[index].physicalThreadIDs.contains(threadID)
            {
                lineages[index].physicalThreadIDs.append(threadID)
            }
        }
    }

    private func saveLineages(_ lineages: [CodexThreadLineage]) {
        guard let data = try? JSONEncoder().encode(lineages) else { return }
        defaults.set(data, forKey: Self.lineagesKey)
    }

    private static func normalize(_ threadID: String) -> String {
        threadID.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

final class CodexThreadAccountProfileBindingStore: @unchecked Sendable {
    static let bindingsKey = "com.silverfire.codexcompanion.codex-thread-account-profile-bindings"

    private let defaults: UserDefaults
    fileprivate let lineageStore: CodexThreadLineageStore
    private let lock = NSLock()

    init(
        defaults: UserDefaults = .standard,
        lineageStore: CodexThreadLineageStore? = nil
    ) {
        self.defaults = defaults
        self.lineageStore = lineageStore ?? CodexThreadLineageStore(defaults: defaults)
    }

    func profileID(for threadID: String) -> UUID? {
        profileID(forPhysicalThreadID: lineageStore.activeThreadID(for: threadID))
    }

    func profileID(forPhysicalThreadID threadID: String) -> UUID? {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return nil }
        return lock.withLock {
            let bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            return bindings[normalizedThreadID].flatMap(UUID.init(uuidString:))
        }
    }

    func bind(threadID: String, to profileID: UUID) {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return }
        lock.withLock {
            var bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            bindings[normalizedThreadID] = profileID.uuidString
            defaults.set(bindings, forKey: Self.bindingsKey)
        }
    }

    @discardableResult
    func bindIfUnbound(threadID: String, to profileID: UUID) -> UUID? {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return nil }
        return lock.withLock {
            var bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            if let existingValue = bindings[normalizedThreadID] {
                return UUID(uuidString: existingValue)
            }
            bindings[normalizedThreadID] = profileID.uuidString
            defaults.set(bindings, forKey: Self.bindingsKey)
            return profileID
        }
    }

    func hasBindings(to profileID: UUID) -> Bool {
        lock.withLock {
            let bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            return bindings.values.contains { UUID(uuidString: $0) == profileID }
        }
    }

    func removeBinding(for threadID: String) {
        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else { return }
        lock.withLock {
            var bindings = defaults.dictionary(forKey: Self.bindingsKey) as? [String: String] ?? [:]
            bindings.removeValue(forKey: normalizedThreadID)
            defaults.set(bindings, forKey: Self.bindingsKey)
        }
    }
}

enum CodexThreadAccountHandoffRequestFactory {
    static func accountRead(id: Int) -> CodexRPCRequest {
        return CodexRPCRequest(
            id: id,
            method: "account/read",
            params: ["refreshToken": true]
        )
    }

    static func fork(
        id: Int,
        threadID: String,
        settings: CodexThreadRuntimeSettings
    ) -> CodexRPCRequest {
        var params: [String: Any] = [
            "threadId": threadID,
            "excludeTurns": true,
            "deferGoalContinuation": true,
        ]
        if let model = settings.model {
            params["model"] = model
        }
        if let reasoningEffort = settings.reasoningEffort {
            params["config"] = ["model_reasoning_effort": reasoningEffort]
        }
        return CodexRPCRequest(
            id: id,
            method: "thread/fork",
            params: params
        )
    }
}

enum CodexAccountProfileIdentityParser {
    static func identity(from accountReadResult: [String: Any]) -> CodexAccountProfileIdentity? {
        guard
            let account = accountReadResult["account"] as? [String: Any],
            let accountType = account["type"] as? String,
            accountType.caseInsensitiveCompare("chatgpt") == .orderedSame,
            let email = account["email"] as? String,
            !email.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        else {
            return nil
        }
        return CodexAccountProfileIdentity(
            accountType: accountType,
            email: email,
            planType: account["planType"] as? String
        )
    }
}

struct CodexAccountProfileRuntimeIdentityReader: Sendable {
    private let clientProvider: any CodexAccountProfileRPCClientProviding

    init(
        clientProvider: any CodexAccountProfileRPCClientProviding =
            CodexAccountProfileDaemonRPCClientProvider()
    ) {
        self.clientProvider = clientProvider
    }

    func identity(for profile: CodexAccountProfile) throws -> CodexAccountProfileIdentity {
        let client = try clientProvider.client(for: profile)
        let request = CodexThreadAccountHandoffRequestFactory.accountRead(id: 1)
        let responses = try client.perform([request])
        guard let response = responses[request.id] else {
            throw CodexThreadAccountHandoffError.missingResponse
        }
        if let error = response.error {
            throw CodexThreadAccountHandoffError.server(error)
        }
        guard
            let result = response.result,
            let identity = CodexAccountProfileIdentityParser.identity(from: result)
        else {
            throw CodexThreadAccountHandoffError.unverifiableAccountIdentity
        }
        return identity
    }
}

struct CodexAccountProfileRuntimeVerifier: Sendable {
    typealias IdentityReader = @Sendable (CodexAccountProfile) async throws
        -> CodexAccountProfileIdentity

    private let identityStore: CodexAccountProfileIdentityStore
    private let identityReader: IdentityReader

    init(
        identityStore: CodexAccountProfileIdentityStore = CodexAccountProfileIdentityStore(),
        identityReader: @escaping IdentityReader = { profile in
            try await Task.detached(priority: .userInitiated) {
                try CodexAccountProfileRuntimeIdentityReader().identity(for: profile)
            }.value
        }
    ) {
        self.identityStore = identityStore
        self.identityReader = identityReader
    }

    func verify(_ profile: CodexAccountProfile) async -> Bool {
        do {
            let actualIdentity = try await identityReader(profile)
            if let expectedIdentity = identityStore.identity(for: profile.id) {
                guard expectedIdentity.matchesAccount(actualIdentity) else {
                    CodexAccountRuntimeDiagnostics.append(
                        "profile=\(profile.id.uuidString) runtime account mismatch"
                    )
                    return false
                }
            } else {
                identityStore.save(actualIdentity, for: profile.id)
            }
            CodexAccountRuntimeDiagnostics.append(
                "profile=\(profile.id.uuidString) runtime account verified "
                    + "account_type=\(actualIdentity.accountType)"
            )
            return true
        } catch {
            CodexAccountRuntimeDiagnostics.append(
                "profile=\(profile.id.uuidString) runtime account verification failed "
                    + "error=\(error.localizedDescription)"
            )
            return false
        }
    }
}

final class CodexThreadAccountHandoffService: @unchecked Sendable {
    typealias ThreadSettingsProvider = @Sendable (String) throws -> CodexThreadRuntimeSettings

    private let clientProvider: any CodexAccountProfileRPCClientProviding
    private let bindingStore: CodexThreadAccountProfileBindingStore
    private let identityStore: CodexAccountProfileIdentityStore
    private let lineageStore: CodexThreadLineageStore
    private let threadSettingsProvider: ThreadSettingsProvider

    init(
        clientProvider: any CodexAccountProfileRPCClientProviding =
            CodexAccountProfileDaemonRPCClientProvider(),
        bindingStore: CodexThreadAccountProfileBindingStore =
            CodexThreadAccountProfileBindingStore(),
        identityStore: CodexAccountProfileIdentityStore =
            CodexAccountProfileIdentityStore(),
        lineageStore: CodexThreadLineageStore? = nil,
        threadSettingsProvider: @escaping ThreadSettingsProvider = { threadID in
            try CodexThreadRuntimeSettingsCatalogReader().settings(for: threadID)
        }
    ) {
        self.clientProvider = clientProvider
        self.bindingStore = bindingStore
        self.identityStore = identityStore
        self.lineageStore = lineageStore ?? bindingStore.lineageStore
        self.threadSettingsProvider = threadSettingsProvider
    }

    func handoff(
        threadID: String,
        rolloutURL: URL,
        hasActiveTurn: Bool,
        to profile: CodexAccountProfile
    ) throws -> CodexThreadAccountHandoffResult {
        guard !hasActiveTurn else {
            throw CodexThreadAccountHandoffError.activeTurn
        }

        let normalizedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedThreadID.isEmpty else {
            throw CodexThreadAccountHandoffError.invalidThreadID
        }

        let normalizedRolloutURL = rolloutURL.standardizedFileURL
        guard normalizedRolloutURL.isFileURL, !normalizedRolloutURL.path.isEmpty else {
            throw CodexThreadAccountHandoffError.invalidRolloutPath
        }

        let canonicalThreadID = lineageStore.canonicalThreadID(for: normalizedThreadID)
        let runtimeThreadID = lineageStore.activeThreadID(for: normalizedThreadID)

        CodexAccountRuntimeDiagnostics.append(
            "handoff begin profile=\(profile.id.uuidString) label=\(profile.label) "
                + "canonical_thread=\(canonicalThreadID) runtime_thread=\(runtimeThreadID) "
                + "rollout=\(normalizedRolloutURL.path)"
        )

        let client = try clientProvider.client(for: profile)
        let accountRequest = CodexThreadAccountHandoffRequestFactory.accountRead(id: 2)
        let accountResponses = try client.perform([accountRequest])
        guard let accountResponse = accountResponses[accountRequest.id] else {
            throw CodexThreadAccountHandoffError.missingResponse
        }
        if let error = accountResponse.error {
            throw CodexThreadAccountHandoffError.server(error)
        }
        guard let accountResult = accountResponse.result else {
            throw CodexThreadAccountHandoffError.invalidResponse
        }
        guard let actualIdentity = CodexAccountProfileIdentityParser.identity(
            from: accountResult
        ) else {
            throw CodexThreadAccountHandoffError.unverifiableAccountIdentity
        }
        if let expectedIdentity = identityStore.identity(for: profile.id),
           !expectedIdentity.matchesAccount(actualIdentity) {
            CodexAccountRuntimeDiagnostics.append(
                "profile=\(profile.id.uuidString) account mismatch "
                    + "expected_type=\(expectedIdentity.accountType) "
                    + "actual_type=\(actualIdentity.accountType) "
                    + "email_match=false"
            )
            throw CodexThreadAccountHandoffError.accountIdentityMismatch
        }
        CodexAccountRuntimeDiagnostics.append(
            "handoff account verified profile=\(profile.id.uuidString) "
                + "account_type=\(actualIdentity.accountType) "
                + "plan=\(actualIdentity.planType ?? "unknown")"
        )

        let sourceSettings = try threadSettingsProvider(runtimeThreadID)
        let request = CodexThreadAccountHandoffRequestFactory.fork(
            id: 3,
            threadID: runtimeThreadID,
            settings: sourceSettings
        )
        let responses = try client.perform([request])
        guard let response = responses[request.id] else {
            throw CodexThreadAccountHandoffError.missingResponse
        }
        if let error = response.error {
            throw CodexThreadAccountHandoffError.server(error)
        }
        guard
            let result = response.result,
            let thread = result["thread"] as? [String: Any],
            let forkedThreadID = thread["id"] as? String,
            let forkedFromThreadID = thread["forkedFromId"] as? String,
            let forkedPath = thread["path"] as? String
        else {
            throw CodexThreadAccountHandoffError.invalidResponse
        }

        let normalizedForkedThreadID = forkedThreadID.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        let normalizedForkedFromThreadID = forkedFromThreadID.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        let normalizedForkedURL = URL(fileURLWithPath: forkedPath).standardizedFileURL
        guard
            !normalizedForkedThreadID.isEmpty,
            normalizedForkedThreadID != runtimeThreadID,
            normalizedForkedFromThreadID == runtimeThreadID,
            normalizedForkedURL.isFileURL,
            !normalizedForkedURL.path.isEmpty,
            normalizedForkedURL != normalizedRolloutURL
        else {
            throw CodexThreadAccountHandoffError.forkMismatch
        }
        guard lineageStore.canRegisterFork(
            sourceThreadID: runtimeThreadID,
            destinationThreadID: normalizedForkedThreadID
        ) else {
            throw CodexThreadAccountHandoffError.lineageConflict
        }

        let committedProfileID = bindingStore.bindIfUnbound(
            threadID: normalizedForkedThreadID,
            to: profile.id
        )
        guard committedProfileID == profile.id else {
            throw CodexThreadAccountHandoffError.destinationBindingConflict
        }
        guard lineageStore.registerFork(
            sourceThreadID: runtimeThreadID,
            destinationThreadID: normalizedForkedThreadID
        ) == canonicalThreadID else {
            throw CodexThreadAccountHandoffError.lineageConflict
        }
        identityStore.save(actualIdentity, for: profile.id)
        CodexAccountRuntimeDiagnostics.append(
            "handoff committed profile=\(profile.id.uuidString) "
                + "canonical_thread=\(canonicalThreadID) "
                + "source_thread=\(runtimeThreadID) "
                + "destination_thread=\(normalizedForkedThreadID) "
                + "destination_rollout=\(normalizedForkedURL.path)"
        )
        return CodexThreadAccountHandoffResult(
            threadID: canonicalThreadID,
            runtimeThreadID: normalizedForkedThreadID,
            rolloutURL: normalizedForkedURL,
            profileID: profile.id
        )
    }

}

struct CodexAccountProfileDaemonRPCClientProvider: CodexAccountProfileRPCClientProviding {
    typealias CommandRunner = @Sendable (CodexAccountProfileDaemonCommand) throws -> Int32
    typealias SocketProbe = @Sendable (URL) -> Bool

    var credentialBaseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    var daemonBaseURL: URL = CodexAccountProfileRuntime.defaultDaemonBaseURL
    var sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    var managedStandaloneURL: URL = CodexAccountProfileRuntime.defaultManagedStandaloneURL
    var executableURLProvider: @Sendable () -> URL? = {
        WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        })
    }
    var commandRunner: CommandRunner = { command in
        let process = Process()
        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        process.executableURL = command.executableURL
        process.arguments = command.arguments
        var environment = ProcessInfo.processInfo.environment
        command.environmentOverrides.forEach { environment[$0.key] = $0.value }
        process.environment = environment
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe
        do {
            try process.run()
        } catch {
            throw CodexAccountProfileDaemonError.launchFailed(error.localizedDescription)
        }
        CodexAccountRuntimeDiagnostics.append(
            "daemon launcher pid=\(process.processIdentifier) executable=\(command.executableURL.path) "
                + "arguments=\(command.arguments.joined(separator: " "))"
        )
        process.waitUntilExit()

        let stdout = stdoutPipe.fileHandleForReading.readDataToEndOfFile()
        if let text = String(data: stdout, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
           !text.isEmpty {
            CodexAccountRuntimeDiagnostics.append("daemon stdout \(text)")
        }
        let stderr = stderrPipe.fileHandleForReading.readDataToEndOfFile()
        if let text = String(data: stderr, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
           !text.isEmpty {
            CodexAccountRuntimeDiagnostics.append("daemon stderr \(text)")
        }
        return process.terminationStatus
    }
    var socketProbe: SocketProbe = {
        CodexAccountProfileDaemonCoordinator.socketIsReachable(at: $0)
    }
    var startupTimeout: TimeInterval = 5
    var requestTimeout: TimeInterval = 30

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        guard let executableURL = executableURLProvider() else {
            throw CodexAccountProfileDaemonError.missingExecutable
        }
        try CodexAccountProfileRuntime.prepareDaemonHome(
            for: profile,
            daemonBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )
        let command = CodexAccountProfileDaemonCommandFactory.start(
            profile: profile,
            executableURL: executableURL,
            profileBaseURL: daemonBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )
        CodexAccountRuntimeDiagnostics.append(
            "profile=\(profile.id.uuidString) label=\(profile.label) "
                + "credential_home=\(CodexAccountProfileRuntime.homeURL(for: profile, baseURL: credentialBaseURL).path) "
                + "daemon_home=\(command.environmentOverrides["CODEX_HOME"] ?? "missing") "
                + "sqlite_home=\(command.environmentOverrides["CODEX_SQLITE_HOME"] ?? "missing") "
                + "socket=\(command.socketURL.path) executable=\(command.executableURL.path)"
        )

        if !socketProbe(command.socketURL) {
            let status = try commandRunner(command)
            guard status == 0 || socketProbe(command.socketURL) else {
                throw CodexAccountProfileDaemonError.startFailed(status)
            }
            let deadline = Date().addingTimeInterval(startupTimeout)
            while !socketProbe(command.socketURL), Date() < deadline {
                Thread.sleep(forTimeInterval: 0.1)
            }
            guard socketProbe(command.socketURL) else {
                throw CodexAccountProfileDaemonError.socketUnavailable
            }
        }

        CodexAccountRuntimeDiagnostics.append(
            "profile=\(profile.id.uuidString) daemon ready socket=\(command.socketURL.path)"
        )
        return CodexAppServerProxyRPCClient(
            executableURL: command.executableURL,
            environmentOverrides: command.environmentOverrides,
            socketURL: command.socketURL,
            timeout: requestTimeout,
            logContext: "profile=\(profile.id.uuidString) socket=\(command.socketURL.path)"
        )
    }
}

struct CodexAccountProfileRPCClientProvider: CodexAccountProfileRPCClientProviding {
    var baseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    var sharedSQLiteHomeURL: URL = CodexAccountProfileRuntime.defaultSharedSQLiteHomeURL
    var executableURLProvider: @Sendable () -> URL? = {
        WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        })
    }
    var timeout: TimeInterval = 20

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        guard let executableURL = executableURLProvider() else {
            throw CodexAppServerControlError.missingExecutable
        }

        let profileHomeURL = CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: baseURL
        )
        try CodexAccountProfileRuntime.prepareHome(for: profile, baseURL: baseURL)
        return CodexAccountProfileRPCClient(
            executableURL: executableURL,
            profileHomeURL: profileHomeURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            timeout: timeout
        )
    }
}

private struct CodexAccountProfileRPCClient: CodexAppServerRPCPerforming, Sendable {
    var executableURL: URL
    var profileHomeURL: URL
    var sharedSQLiteHomeURL: URL
    var timeout: TimeInterval

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        try CodexAccountProfileRPCSession(
            executableURL: executableURL,
            profileHomeURL: profileHomeURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            requests: requests,
            timeout: timeout
        ).run()
    }
}

private final class CodexAccountProfileRPCSession {
    private let executableURL: URL
    private let profileHomeURL: URL
    private let sharedSQLiteHomeURL: URL
    private let requests: [CodexRPCRequest]
    private let timeout: TimeInterval
    private let process = Process()
    private let stdinPipe = Pipe()
    private let stdoutPipe = Pipe()
    private let stderrPipe = Pipe()
    private let lock = NSLock()
    private let completion = DispatchSemaphore(value: 0)
    private var stdoutBuffer = Data()
    private var responses: [Int: CodexRPCResponse] = [:]
    private var failure: Error?
    private var didInitialize = false
    private var didFinish = false

    init(
        executableURL: URL,
        profileHomeURL: URL,
        sharedSQLiteHomeURL: URL,
        requests: [CodexRPCRequest],
        timeout: TimeInterval
    ) {
        self.executableURL = executableURL
        self.profileHomeURL = profileHomeURL
        self.sharedSQLiteHomeURL = sharedSQLiteHomeURL
        self.requests = requests
        self.timeout = timeout
    }

    func run() throws -> [Int: CodexRPCResponse] {
        process.executableURL = executableURL
        process.arguments = ["app-server", "--listen", "stdio://"]
        var environment = ProcessInfo.processInfo.environment
        environment["CODEX_HOME"] = profileHomeURL.path
        environment["CODEX_SQLITE_HOME"] = sharedSQLiteHomeURL.standardizedFileURL.path
        process.environment = environment
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = stderrPipe

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            self?.read(handle.availableData)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { handle in
            _ = handle.availableData
        }
        process.terminationHandler = { [weak self] process in
            self?.finishIfNeeded(
                error: CodexAppServerControlError.processExited(process.terminationStatus)
            )
        }

        do {
            try process.run()
        } catch {
            cleanup()
            throw CodexAppServerControlError.launchFailed(error.localizedDescription)
        }

        send([
            "id": 1,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "codex-companion-profile",
                    "title": "Codex Companion Profile",
                    "version": Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
                        ?? "0",
                ],
                "capabilities": [
                    "experimentalApi": true,
                    "optOutNotificationMethods": [],
                ],
            ],
        ])

        if completion.wait(timeout: .now() + timeout) == .timedOut {
            finishIfNeeded(error: CodexAppServerControlError.timedOut)
        }
        cleanup()
        if process.isRunning {
            process.terminate()
        }

        if let failure {
            throw failure
        }
        return responses
    }

    private func read(_ data: Data) {
        guard !data.isEmpty else { return }
        let lines: [Data] = lock.withLock {
            stdoutBuffer.append(data)
            let newline = Data([0x0A])
            var lines: [Data] = []
            while let range = stdoutBuffer.firstRange(of: newline) {
                lines.append(
                    stdoutBuffer.subdata(in: stdoutBuffer.startIndex..<range.lowerBound)
                )
                stdoutBuffer.removeSubrange(stdoutBuffer.startIndex..<range.upperBound)
            }
            return lines
        }

        for line in lines where !line.isEmpty {
            handle(line)
        }
    }

    private func handle(_ data: Data) {
        guard
            let object = try? JSONSerialization.jsonObject(with: data),
            let message = object as? [String: Any],
            let id = Self.responseID(from: message)
        else { return }

        if id == 1 {
            guard message["error"] == nil else {
                finishIfNeeded(
                    error: CodexAppServerControlError.server(Self.errorText(from: message))
                )
                return
            }
            send(["method": "initialized"])
            let shouldSend = lock.withLock {
                guard !didInitialize else { return false }
                didInitialize = true
                return true
            }
            if shouldSend {
                requests.forEach { send($0.jsonObject) }
                if requests.isEmpty {
                    finishIfNeeded(error: nil)
                }
            }
            return
        }

        guard requests.contains(where: { $0.id == id }) else { return }
        let response: CodexRPCResponse
        if message["error"] != nil {
            response = CodexRPCResponse(
                result: nil,
                error: Self.errorText(from: message)
            )
        } else {
            response = CodexRPCResponse(
                result: message["result"] as? [String: Any],
                error: nil
            )
        }

        let complete = lock.withLock {
            responses[id] = response
            return responses.count == requests.count
        }
        if complete {
            finishIfNeeded(error: nil)
        }
    }

    private func send(_ object: [String: Any]) {
        guard
            JSONSerialization.isValidJSONObject(object),
            let data = try? JSONSerialization.data(withJSONObject: object)
        else {
            finishIfNeeded(error: CodexAppServerControlError.invalidResponse)
            return
        }
        stdinPipe.fileHandleForWriting.write(data)
        stdinPipe.fileHandleForWriting.write(Data([0x0A]))
    }

    private func finishIfNeeded(error: Error?) {
        lock.withLock {
            guard !didFinish else { return }
            didFinish = true
            failure = error
            completion.signal()
        }
    }

    private func cleanup() {
        stdoutPipe.fileHandleForReading.readabilityHandler = nil
        stderrPipe.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe.fileHandleForWriting.close()
    }

    private static func responseID(from message: [String: Any]) -> Int? {
        if let id = message["id"] as? Int { return id }
        if let id = message["id"] as? String { return Int(id) }
        return nil
    }

    private static func errorText(from message: [String: Any]) -> String {
        guard let error = message["error"] as? [String: Any] else {
            return "Codex app-server request failed."
        }
        return error["message"] as? String ?? String(describing: error)
    }
}
