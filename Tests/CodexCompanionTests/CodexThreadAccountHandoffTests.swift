import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexThreadAccountHandoffTests {
    @Test
    func resumesTheExactPersistedThreadBeforeChangingItsProfileBinding() throws {
        let defaultsName = "CodexThreadAccountHandoffTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let rolloutURL = URL(
            fileURLWithPath: "/tmp/sessions/2026/07/26/rollout-thread-main.jsonl"
        )
        let rpc = RecordingProfileRPCProvider(
            response: CodexRPCResponse(
                result: [
                    "thread": [
                        "id": "thread-main",
                        "path": rolloutURL.path,
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
            rolloutURL: rolloutURL.standardizedFileURL,
            profileID: profile.id
        ))
        #expect(rpc.requestedProfileIDs == [profile.id])
        let request = try #require(rpc.recordedRequests.first)
        #expect(request.method == "thread/resume")
        #expect(request.params["threadId"] as? String == "thread-main")
        #expect(request.params["path"] as? String == rolloutURL.standardizedFileURL.path)
        #expect(bindings.profileID(for: "thread-main") == profile.id)
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
    func aMismatchedResumeResponseDoesNotChangeTheExistingBinding() throws {
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
                        "path": rolloutURL.path,
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

        #expect(throws: CodexThreadAccountHandoffError.resumeMismatch) {
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
    private let response: CodexRPCResponse
    private var requests: [CodexRPCRequest] = []
    private var profileIDs: [UUID] = []

    init(response: CodexRPCResponse) {
        self.response = response
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
        return Dictionary(uniqueKeysWithValues: requests.map { ($0.id, response) })
    }
}
