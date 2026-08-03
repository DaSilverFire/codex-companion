import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexThreadSourceProfileResolverTests {
    @Test
    func sharedRuntimeIdentityReadRequestsARefreshWithoutPersistingCredentials() throws {
        let rpc = SourceProfileRPCClient(
            response: Self.accountResponse(email: "main@example.com")
        )
        let identity = try CodexSharedAccountRuntimeIdentityReader(client: rpc).identity()

        #expect(identity == CodexAccountProfileIdentity(
            accountType: "chatgpt",
            email: "main@example.com",
            planType: "pro"
        ))
        let request = try #require(rpc.requests.first)
        #expect(request.id == 2)
        #expect(request.method == "account/read")
        #expect(request.params["refreshToken"] as? Bool == true)
    }

    @Test
    func staleSharedSocketUsesTheTemporarySharedRuntimeClient() throws {
        let daemonRPC = SourceProfileRPCClient(
            response: Self.accountResponse(email: "daemon@example.com")
        )
        let temporaryRPC = SourceProfileRPCClient(
            response: Self.accountResponse(email: "temporary@example.com")
        )
        let socketURL = URL(fileURLWithPath: "/tmp/stale-codex-app-server.sock")
        let client = CodexSharedAccountRuntimeClientFactory.make(
            socketURL: socketURL,
            socketProbe: { _ in false },
            daemonClient: daemonRPC,
            temporaryClient: temporaryRPC
        )

        let identity = try CodexSharedAccountRuntimeIdentityReader(client: client).identity()

        #expect(identity.email == "temporary@example.com")
        #expect(daemonRPC.requests.isEmpty)
        #expect(temporaryRPC.requests.count == 1)
    }

    @Test
    func uniqueSharedRuntimeIdentityBindsTheExactProfile() throws {
        let defaultsName = "CodexThreadSourceProfileResolverTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let main = CodexAccountProfile(id: UUID(), label: "Main")
        let backup = CodexAccountProfile(id: UUID(), label: "Backup")
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "main@example.com",
                planType: "pro"
            ),
            for: main.id
        )
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: backup.id
        )
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [main, backup] },
            bindingStore: bindings,
            identityStore: identities,
            sharedIdentityReader: {
                CodexAccountProfileIdentity(
                    accountType: "CHATGPT",
                    email: " MAIN@example.com ",
                    planType: "PRO"
                )
            }
        )

        let profileID = try resolver.resolveProfileID(for: " thread-1 ")

        #expect(profileID == main.id)
        #expect(bindings.profileID(for: "thread-1") == main.id)
    }

    @Test
    func missingSharedRuntimeIdentityMatchLeavesTheThreadUnbound() throws {
        let defaultsName = "CodexThreadSourceProfileResolverMissingTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "different@example.com",
                planType: "pro"
            ),
            for: profile.id
        )
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [profile] },
            bindingStore: bindings,
            identityStore: identities,
            sharedIdentityReader: {
                CodexAccountProfileIdentity(
                    accountType: "chatgpt",
                    email: "main@example.com",
                    planType: "pro"
                )
            }
        )

        #expect(throws: CodexThreadSourceProfileResolutionError.noMatchingProfile) {
            try resolver.resolveProfileID(for: "thread-1")
        }
        #expect(bindings.profileID(for: "thread-1") == nil)
    }

    @Test
    func duplicateIdentityMatchesLeaveTheThreadUnbound() throws {
        let defaultsName = "CodexThreadSourceProfileResolverAmbiguousTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let first = CodexAccountProfile(id: UUID(), label: "First")
        let second = CodexAccountProfile(id: UUID(), label: "Second")
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let sharedIdentity = CodexAccountProfileIdentity(
            accountType: "chatgpt",
            email: "same@example.com",
            planType: "pro"
        )
        identities.save(sharedIdentity, for: first.id)
        identities.save(sharedIdentity, for: second.id)
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [first, second] },
            bindingStore: bindings,
            identityStore: identities,
            sharedIdentityReader: { sharedIdentity }
        )

        #expect(throws: CodexThreadSourceProfileResolutionError.ambiguousProfiles) {
            try resolver.resolveProfileID(for: "thread-1")
        }
        #expect(bindings.profileID(for: "thread-1") == nil)
    }

    @Test
    func bindingCreatedDuringIdentityReadIsNotOverwritten() throws {
        let defaultsName = "CodexThreadSourceProfileResolverRaceTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let main = CodexAccountProfile(id: UUID(), label: "Main")
        let backup = CodexAccountProfile(id: UUID(), label: "Backup")
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let mainIdentity = CodexAccountProfileIdentity(
            accountType: "chatgpt",
            email: "main@example.com",
            planType: "pro"
        )
        identities.save(mainIdentity, for: main.id)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: backup.id
        )
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [main, backup] },
            bindingStore: bindings,
            identityStore: identities,
            sharedIdentityReader: {
                bindings.bind(threadID: "thread-1", to: backup.id)
                return mainIdentity
            }
        )

        #expect(throws: CodexThreadSourceProfileResolutionError.bindingChanged) {
            try resolver.resolveProfileID(for: "thread-1")
        }
        #expect(bindings.profileID(for: "thread-1") == backup.id)
    }

    @Test
    func taskStreamEndpointUsesTheBoundProfilesExistingDaemonSocket() throws {
        let defaultsName = "CodexThreadSourceProfileResolverEndpointTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-1", to: profile.id)
        let daemonBaseURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let sqliteHomeURL = daemonBaseURL.appendingPathComponent("shared-sqlite", isDirectory: true)
        let executableURL = URL(fileURLWithPath: "/usr/bin/true")
        let expectedSocketURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: daemonBaseURL
        )
        .appendingPathComponent("app-server-control", isDirectory: true)
        .appendingPathComponent("app-server-control.sock")
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [profile] },
            bindingStore: bindings,
            sharedIdentityReader: {
                Issue.record("A bound task must not re-read the shared account identity")
                throw CodexThreadSourceProfileResolutionError.unverifiableAccountIdentity
            },
            executableURLsProvider: { [executableURL] },
            socketProbe: { $0 == expectedSocketURL },
            daemonBaseURL: daemonBaseURL,
            sharedSQLiteHomeURL: sqliteHomeURL
        )

        let endpoint = try resolver.resolveTaskStreamEndpoint(for: "thread-1")

        #expect(endpoint.profileID == profile.id)
        #expect(endpoint.executableURL == executableURL)
        #expect(endpoint.socketURL == expectedSocketURL)
        #expect(endpoint.environmentOverrides["CODEX_HOME"] == expectedSocketURL
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .path)
        #expect(endpoint.environmentOverrides["CODEX_SQLITE_HOME"] == sqliteHomeURL.path)
    }

    @Test
    func taskStreamEndpointRefusesToStartOrRepairAnUnreachableProfileRuntime() throws {
        let defaultsName = "CodexThreadSourceProfileResolverUnavailableTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-1", to: profile.id)
        let resolver = CodexThreadSourceProfileResolver(
            profilesProvider: { [profile] },
            bindingStore: bindings,
            executableURLsProvider: { [URL(fileURLWithPath: "/usr/bin/true")] },
            socketProbe: { _ in false }
        )

        #expect(throws: CodexThreadSourceProfileResolutionError.profileRuntimeUnavailable) {
            try resolver.resolveTaskStreamEndpoint(for: "thread-1")
        }
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
}

private final class SourceProfileRPCClient: CodexAppServerRPCPerforming, @unchecked Sendable {
    private let lock = NSLock()
    private let response: CodexRPCResponse
    private var recordedRequests: [CodexRPCRequest] = []

    init(response: CodexRPCResponse) {
        self.response = response
    }

    var requests: [CodexRPCRequest] {
        lock.withLock { recordedRequests }
    }

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        lock.withLock { recordedRequests.append(contentsOf: requests) }
        return Dictionary(uniqueKeysWithValues: requests.map { ($0.id, response) })
    }
}
