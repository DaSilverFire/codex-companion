import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexThreadForkHandoffTests {
    @Test
    func handoffForksIntoTheDestinationRuntimeAndPreservesSourceOwnership() throws {
        let defaultsName = "CodexThreadForkHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let sourceProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let sourceRolloutURL = URL(fileURLWithPath: "/tmp/source-thread.jsonl")
        let forkedRolloutURL = URL(fileURLWithPath: "/tmp/forked-thread.jsonl")
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let lineages = CodexThreadLineageStore(defaults: defaults)
        bindings.bind(threadID: "source-thread", to: sourceProfile.id)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: destinationProfile.id
        )
        let rpc = ForkHandoffRPCProvider(responsesByMethod: [
            "account/read": Self.accountResponse(email: "backup@example.com"),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "forked-thread",
                        "forkedFromId": "source-thread",
                        "path": forkedRolloutURL.path,
                    ],
                ],
                error: nil
            ),
        ])
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings,
            identityStore: identities,
            lineageStore: lineages,
            threadSettingsProvider: { threadID in
                #expect(threadID == "source-thread")
                return CodexThreadRuntimeSettings(
                    model: "gpt-5.6-sol",
                    reasoningEffort: "ultra"
                )
            }
        )

        let result = try service.handoff(
            threadID: "source-thread",
            rolloutURL: sourceRolloutURL,
            hasActiveTurn: false,
            to: destinationProfile
        )

        #expect(result == CodexThreadAccountHandoffResult(
            threadID: "source-thread",
            runtimeThreadID: "forked-thread",
            rolloutURL: forkedRolloutURL.standardizedFileURL,
            profileID: destinationProfile.id
        ))
        #expect(rpc.recordedRequests.map(\.method) == ["account/read", "thread/fork"])
        let forkRequest = try #require(rpc.recordedRequests.last)
        #expect(forkRequest.params["threadId"] as? String == "source-thread")
        #expect(forkRequest.params["excludeTurns"] as? Bool == true)
        #expect(forkRequest.params["deferGoalContinuation"] as? Bool == true)
        #expect(forkRequest.params["model"] as? String == "gpt-5.6-sol")
        let config = try #require(forkRequest.params["config"] as? [String: Any])
        #expect(config["model_reasoning_effort"] as? String == "ultra")
        #expect(lineages.canonicalThreadID(for: "forked-thread") == "source-thread")
        #expect(lineages.activeThreadID(for: "source-thread") == "forked-thread")
        #expect(bindings.profileID(for: "source-thread") == destinationProfile.id)
        #expect(bindings.profileID(forPhysicalThreadID: "source-thread") == sourceProfile.id)
        #expect(bindings.profileID(for: "forked-thread") == destinationProfile.id)
    }

    @Test
    func legacyCrossProfileForksRecoverLineageAndMissingReasoning() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("CodexLegacyForkLineageTests.\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let sourceRollout = directory.appendingPathComponent("source.jsonl")
        let middleRollout = directory.appendingPathComponent("middle.jsonl")
        let activeRollout = directory.appendingPathComponent("active.jsonl")
        try Self.writeSessionMeta(to: sourceRollout, threadID: "source-thread")
        try Self.writeSessionMeta(
            to: middleRollout,
            threadID: "middle-thread",
            forkedFromThreadID: "source-thread"
        )
        try Self.writeSessionMeta(
            to: activeRollout,
            threadID: "active-thread",
            forkedFromThreadID: "middle-thread"
        )

        let databaseURL = directory.appendingPathComponent("state_5.sqlite")
        let sql = """
        create table threads (
            id text primary key, rollout_path text, model text, reasoning_effort text,
            created_at integer, recency_at_ms integer, updated_at_ms integer, updated_at integer
        );
        insert into threads values
            ('source-thread', '\(sourceRollout.path)', 'gpt-5.6-sol', 'ultra', 1, 1000, 1000, 1),
            ('middle-thread', '\(middleRollout.path)', 'gpt-5.6-sol', 'ultra', 2, 2000, 2000, 2),
            ('active-thread', '\(activeRollout.path)', 'gpt-5.6-sol', null, 3, 3000, 3000, 3);
        """
        let sqlite = try CodexSQLiteProcessRunner.run(
            executableURL: URL(fileURLWithPath: "/usr/bin/sqlite3"),
            arguments: [databaseURL.path, sql]
        )
        #expect(sqlite.terminationStatus == 0)

        let sourceProfileID = UUID()
        let middleProfileID = UUID()
        let activeProfileID = UUID()
        let lineages = CodexLegacyThreadLineageReader(
            databaseURL: databaseURL
        ).lineages(bindings: [
            "source-thread": sourceProfileID.uuidString,
            "middle-thread": middleProfileID.uuidString,
            "active-thread": activeProfileID.uuidString,
        ])
        #expect(lineages == [CodexThreadLineage(
            canonicalThreadID: "source-thread",
            activeThreadID: "active-thread",
            physicalThreadIDs: ["source-thread", "middle-thread", "active-thread"]
        )])

        let settings = try CodexThreadRuntimeSettingsCatalogReader(
            databaseURL: databaseURL
        ).settings(for: "active-thread")
        #expect(settings == CodexThreadRuntimeSettings(
            model: "gpt-5.6-sol",
            reasoningEffort: "ultra"
        ))
    }

    @Test
    func legacyLineageRoutesToTheMostRecentlyActivePhysicalThread() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("CodexLegacyForkActivityTests.\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let canonicalRollout = directory.appendingPathComponent("canonical.jsonl")
        let staleForkRollout = directory.appendingPathComponent("stale-fork.jsonl")
        try Self.writeSessionMeta(to: canonicalRollout, threadID: "canonical-thread")
        try Self.writeSessionMeta(
            to: staleForkRollout,
            threadID: "stale-fork",
            forkedFromThreadID: "canonical-thread"
        )

        let databaseURL = directory.appendingPathComponent("state_5.sqlite")
        let sql = """
        create table threads (
            id text primary key, rollout_path text, model text, reasoning_effort text,
            created_at integer, recency_at_ms integer, updated_at_ms integer, updated_at integer
        );
        insert into threads values
            ('canonical-thread', '\(canonicalRollout.path)', 'gpt-5.6-sol', 'ultra', 1, 9000, 9000, 9),
            ('stale-fork', '\(staleForkRollout.path)', 'gpt-5.6-sol', 'ultra', 2, 3000, 3000, 3);
        """
        let sqlite = try CodexSQLiteProcessRunner.run(
            executableURL: URL(fileURLWithPath: "/usr/bin/sqlite3"),
            arguments: [databaseURL.path, sql]
        )
        #expect(sqlite.terminationStatus == 0)

        let canonicalProfileID = UUID()
        let forkProfileID = UUID()
        let lineages = CodexLegacyThreadLineageReader(
            databaseURL: databaseURL
        ).lineages(bindings: [
            "canonical-thread": canonicalProfileID.uuidString,
            "stale-fork": forkProfileID.uuidString,
        ])

        #expect(lineages == [CodexThreadLineage(
            canonicalThreadID: "canonical-thread",
            activeThreadID: "canonical-thread",
            physicalThreadIDs: ["canonical-thread", "stale-fork"]
        )])
    }

    @Test
    func forkLineageMismatchDoesNotCommitDestinationRouting() throws {
        let defaultsName = "CodexThreadForkHandoffMismatchTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let sourceProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let sourceRolloutURL = URL(fileURLWithPath: "/tmp/source-thread.jsonl")
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "source-thread", to: sourceProfile.id)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: destinationProfile.id
        )
        let rpc = ForkHandoffRPCProvider(responsesByMethod: [
            "account/read": Self.accountResponse(email: "backup@example.com"),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "forked-thread",
                        "forkedFromId": "another-thread",
                        "path": "/tmp/forked-thread.jsonl",
                    ],
                ],
                error: nil
            ),
        ])
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings,
            identityStore: identities
        )

        #expect(throws: CodexThreadAccountHandoffError.forkMismatch) {
            try service.handoff(
                threadID: "source-thread",
                rolloutURL: sourceRolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }
        #expect(bindings.profileID(for: "source-thread") == sourceProfile.id)
        #expect(bindings.profileID(for: "forked-thread") == nil)
    }

    @Test
    func repeatedPhysicalForksKeepOneDurableCanonicalIdentity() throws {
        let defaultsName = "CodexThreadForkLineageChainTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let lineages = CodexThreadLineageStore(defaults: defaults)

        #expect(lineages.registerFork(
            sourceThreadID: "source-thread",
            destinationThreadID: "first-fork"
        ) == "source-thread")
        #expect(lineages.registerFork(
            sourceThreadID: "first-fork",
            destinationThreadID: "second-fork"
        ) == "source-thread")

        let reloaded = CodexThreadLineageStore(defaults: defaults)
        #expect(reloaded.canonicalThreadID(for: "second-fork") == "source-thread")
        #expect(reloaded.activeThreadID(for: "source-thread") == "second-fork")
        #expect(!reloaded.canRegisterFork(
            sourceThreadID: "source-thread",
            destinationThreadID: "third-fork"
        ))
    }

    private static func accountResponse(email: String) -> CodexRPCResponse {
        CodexRPCResponse(
            result: [
                "account": [
                    "type": "chatgpt",
                    "email": email,
                    "planType": "pro",
                ],
            ],
            error: nil
        )
    }

    private static func writeSessionMeta(
        to url: URL,
        threadID: String,
        forkedFromThreadID: String? = nil
    ) throws {
        var payload: [String: Any] = ["id": threadID]
        if let forkedFromThreadID {
            payload["forked_from_id"] = forkedFromThreadID
        }
        let data = try JSONSerialization.data(withJSONObject: [
            "type": "session_meta",
            "payload": payload,
        ])
        try (data + Data([0x0A])).write(to: url)
    }
}

private final class ForkHandoffRPCProvider:
    CodexAccountProfileRPCClientProviding,
    CodexAppServerRPCPerforming,
    @unchecked Sendable
{
    private let lock = NSLock()
    private let responsesByMethod: [String: CodexRPCResponse]
    private var requests: [CodexRPCRequest] = []

    init(responsesByMethod: [String: CodexRPCResponse]) {
        self.responsesByMethod = responsesByMethod
    }

    var recordedRequests: [CodexRPCRequest] {
        lock.withLock { requests }
    }

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        self
    }

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        lock.withLock { self.requests.append(contentsOf: requests) }
        return Dictionary(uniqueKeysWithValues: requests.compactMap { request in
            responsesByMethod[request.method].map { (request.id, $0) }
        })
    }
}
