import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexThreadAccountHandoffTests {
    @Test
    func accountRuntimeDiagnosticsRedactCredentialValues() {
        let redacted = CodexAccountRuntimeDiagnostics.redact(
            #"access_token=secret-access refresh_token:secret-refresh api_key=secret-key sk-proj-secret"#
        )

        #expect(!redacted.contains("secret-access"))
        #expect(!redacted.contains("secret-refresh"))
        #expect(!redacted.contains("secret-key"))
        #expect(!redacted.contains("sk-proj-secret"))
        #expect(redacted.contains("<redacted>"))
    }

    @Test
    func forksTheExactPersistedThreadIntoTheDestinationProfile() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let rolloutURL = URL(
            fileURLWithPath: "/tmp/sessions/2026/07/26/rollout-thread-main.jsonl"
        )
        let forkedRolloutURL = URL(
            fileURLWithPath: "/tmp/sessions/2026/07/26/rollout-thread-fork.jsonl"
        )
        let rpc = RecordingProfileRPCProvider(
            response: CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-fork",
                        "forkedFromId": "thread-main",
                        "path": forkedRolloutURL.path,
                    ],
                ],
                error: nil
            )
        )
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings
        )

        let result = try service.handoff(
            threadID: " thread-main ",
            rolloutURL: rolloutURL,
            hasActiveTurn: false,
            to: profile
        )

        #expect(result == CodexThreadAccountHandoffResult(
            threadID: "thread-main",
            runtimeThreadID: "thread-fork",
            rolloutURL: forkedRolloutURL.standardizedFileURL,
            profileID: profile.id
        ))
        #expect(rpc.requestedProfileIDs == [profile.id])
        let request = try #require(rpc.recordedRequests.first {
            $0.method == "thread/fork"
        })
        #expect(request.method == "thread/fork")
        #expect(request.params["threadId"] as? String == "thread-main")
        #expect(request.params["excludeTurns"] as? Bool == true)
        #expect(request.params["deferGoalContinuation"] as? Bool == true)
        #expect(bindings.profileID(for: "thread-main") == profile.id)
        #expect(bindings.profileID(forPhysicalThreadID: "thread-main") == nil)
        #expect(bindings.profileID(for: "thread-fork") == profile.id)
    }

    @Test
    func verifiesTheDestinationRuntimeAccountBeforeForkingTheThread() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(responsesByMethod: [
            "account/read": CodexRPCResponse(
                result: [
                    "account": [
                        "type": "chatgpt",
                        "email": "backup@example.com",
                        "planType": "pro",
                    ],
                    "requiresOpenaiAuth": true,
                ],
                error: nil
            ),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-fork",
                        "forkedFromId": "thread-main",
                        "path": "/tmp/rollout-thread-fork.jsonl",
                    ],
                ],
                error: nil
            ),
        ])
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings
        )

        _ = try service.handoff(
            threadID: "thread-main",
            rolloutURL: rolloutURL,
            hasActiveTurn: false,
            to: profile
        )

        #expect(rpc.recordedRequests.map(\.method) == ["account/read", "thread/fork"])
        let accountRequest = try #require(rpc.recordedRequests.first)
        #expect(accountRequest.params["refreshToken"] as? Bool == true)
        #expect(bindings.profileID(for: "thread-main") == profile.id)
        #expect(bindings.profileID(forPhysicalThreadID: "thread-main") == nil)
        #expect(bindings.profileID(for: "thread-fork") == profile.id)
    }

    @Test
    func accountVerificationFailureKeepsThePreviousBinding() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let originalProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(responsesByMethod: [
            "account/read": CodexRPCResponse(
                result: nil,
                error: "The destination daemon is signed out."
            ),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-fork",
                        "forkedFromId": "thread-main",
                        "path": "/tmp/rollout-thread-fork.jsonl",
                    ],
                ],
                error: nil
            ),
        ])
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-main", to: originalProfile.id)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings
        )

        #expect(throws: CodexThreadAccountHandoffError.server(
            "The destination daemon is signed out."
        )) {
            try service.handoff(
                threadID: "thread-main",
                rolloutURL: rolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }
        #expect(bindings.profileID(for: "thread-main") == originalProfile.id)
    }

    @Test
    func destinationDaemonForkFailureKeepsThePreviousBindingAndIdentity() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let originalProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(responsesByMethod: [
            "account/read": CodexRPCResponse(
                result: [
                    "account": [
                        "type": "chatgpt",
                        "email": "backup@example.com",
                        "planType": "pro",
                    ],
                    "requiresOpenaiAuth": true,
                ],
                error: nil
            ),
            "thread/fork": CodexRPCResponse(
                result: nil,
                error: "The destination daemon could not fork this task."
            ),
        ])
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-main", to: originalProfile.id)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings,
            identityStore: identities
        )

        #expect(throws: CodexThreadAccountHandoffError.server(
            "The destination daemon could not fork this task."
        )) {
            try service.handoff(
                threadID: "thread-main",
                rolloutURL: rolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }
        #expect(rpc.recordedRequests.map(\.method) == ["account/read", "thread/fork"])
        #expect(bindings.profileID(for: "thread-main") == originalProfile.id)
        #expect(identities.identity(for: destinationProfile.id) == nil)
    }

    @Test
    func mismatchedDaemonIdentityKeepsThePreviousBinding() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let originalProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(responsesByMethod: [
            "account/read": CodexRPCResponse(
                result: [
                    "account": [
                        "type": "chatgpt",
                        "email": "wrong@example.com",
                        "planType": "pro",
                    ],
                    "requiresOpenaiAuth": true,
                ],
                error: nil
            ),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-fork",
                        "forkedFromId": "thread-main",
                        "path": "/tmp/rollout-thread-fork.jsonl",
                    ],
                ],
                error: nil
            ),
        ])
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-main", to: originalProfile.id)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: destinationProfile.id
        )
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings,
            identityStore: identities
        )

        #expect(throws: CodexThreadAccountHandoffError.accountIdentityMismatch) {
            try service.handoff(
                threadID: "thread-main",
                rolloutURL: rolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }
        #expect(bindings.profileID(for: "thread-main") == originalProfile.id)
        #expect(rpc.recordedRequests.map(\.method) == ["account/read"])
    }

    @Test
    func firstVerifiedDaemonIdentityIsPersistedAfterForkSucceeds() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(responsesByMethod: [
            "account/read": CodexRPCResponse(
                result: [
                    "account": [
                        "type": "chatgpt",
                        "email": "Backup@Example.com",
                        "planType": "pro",
                    ],
                    "requiresOpenaiAuth": true,
                ],
                error: nil
            ),
            "thread/fork": CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-fork",
                        "forkedFromId": "thread-main",
                        "path": "/tmp/rollout-thread-fork.jsonl",
                    ],
                ],
                error: nil
            ),
        ])
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings,
            identityStore: identities
        )

        _ = try service.handoff(
            threadID: "thread-main",
            rolloutURL: rolloutURL,
            hasActiveTurn: false,
            to: profile
        )

        #expect(identities.identity(for: profile.id) == CodexAccountProfileIdentity(
            accountType: "chatgpt",
            email: "backup@example.com",
            planType: "pro"
        ))
        #expect(bindings.profileID(for: "thread-main") == profile.id)
        #expect(bindings.profileID(forPhysicalThreadID: "thread-main") == nil)
        #expect(bindings.profileID(for: "thread-fork") == profile.id)
    }

    @Test
    func refusesToMoveAThreadWhileItsTurnIsStillActive() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let rpc = RecordingProfileRPCProvider(
            response: CodexRPCResponse(result: nil, error: nil)
        )
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings
        )

        #expect(throws: CodexThreadAccountHandoffError.activeTurn) {
            try service.handoff(
                threadID: "thread-main",
                rolloutURL: URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl"),
                hasActiveTurn: true,
                to: profile
            )
        }
        #expect(rpc.recordedRequests.isEmpty)
        #expect(bindings.profileID(for: "thread-main") == nil)
    }

    @Test
    func aMismatchedForkResponseDoesNotChangeTheExistingBinding() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let originalProfile = CodexAccountProfile(id: UUID(), label: "Backup")
        let destinationProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let rolloutURL = URL(fileURLWithPath: "/tmp/rollout-thread-main.jsonl")
        let rpc = RecordingProfileRPCProvider(
            response: CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "different-thread",
                        "forkedFromId": "another-thread",
                        "path": "/tmp/rollout-different-thread.jsonl",
                    ],
                ],
                error: nil
            )
        )
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-main", to: originalProfile.id)
        let service = CodexThreadAccountHandoffService(
            clientProvider: rpc,
            bindingStore: bindings
        )

        #expect(throws: CodexThreadAccountHandoffError.forkMismatch) {
            try service.handoff(
                threadID: "thread-main",
                rolloutURL: rolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }
        #expect(bindings.profileID(for: "thread-main") == originalProfile.id)
    }

    @Test
    func profileHomeIsStableAndContainsNoAccountSecret() {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let baseURL = URL(fileURLWithPath: "/tmp/Codex Profiles", isDirectory: true)

        let first = CodexAccountProfileRuntime.homeURL(for: profile, baseURL: baseURL)
        let second = CodexAccountProfileRuntime.homeURL(for: profile, baseURL: baseURL)

        #expect(first == second)
        #expect(first.deletingLastPathComponent() == baseURL.standardizedFileURL)
        #expect(first.lastPathComponent == profile.id.uuidString.lowercased())
        #expect(!first.path.localizedCaseInsensitiveContains(profile.label))
    }

    @Test
    func taskRuntimeKeepsCredentialsIsolatedButUsesTheSharedThreadDatabase() {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let baseURL = URL(fileURLWithPath: "/tmp/Codex Profiles", isDirectory: true)
        let sharedSQLiteHomeURL = URL(fileURLWithPath: "/tmp/Shared Codex", isDirectory: true)

        let environment = CodexAccountProfileRuntime.taskEnvironment(
            for: profile,
            baseURL: baseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )

        #expect(environment["CODEX_HOME"] == CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: baseURL
        ).path)
        #expect(environment["CODEX_SQLITE_HOME"] == sharedSQLiteHomeURL.path)
    }
}

private final class RecordingProfileRPCProvider:
    CodexAccountProfileRPCClientProviding,
    CodexAppServerRPCPerforming,
    @unchecked Sendable
{
    private let lock = NSLock()
    private let responsesByMethod: [String: CodexRPCResponse]
    private var requests: [CodexRPCRequest] = []
    private var profileIDs: [UUID] = []

    init(response: CodexRPCResponse) {
        responsesByMethod = [
            "account/read": CodexRPCResponse(
                result: [
                    "account": [
                        "type": "chatgpt",
                        "email": "profile@example.com",
                        "planType": "pro",
                    ],
                    "requiresOpenaiAuth": true,
                ],
                error: nil
            ),
            "thread/fork": response,
        ]
    }

    init(responsesByMethod: [String: CodexRPCResponse]) {
        self.responsesByMethod = responsesByMethod
    }

    var recordedRequests: [CodexRPCRequest] {
        lock.withLock { requests }
    }

    var requestedProfileIDs: [UUID] {
        lock.withLock { profileIDs }
    }

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        lock.withLock { profileIDs.append(profile.id) }
        return self
    }

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        lock.withLock { self.requests.append(contentsOf: requests) }
        return Dictionary(uniqueKeysWithValues: requests.compactMap { request in
            responsesByMethod[request.method].map { (request.id, $0) }
        })
    }
}
