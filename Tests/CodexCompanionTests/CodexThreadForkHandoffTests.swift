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
            identityStore: identities
        )

        let result = try service.handoff(
            threadID: "source-thread",
            rolloutURL: sourceRolloutURL,
            hasActiveTurn: false,
            to: destinationProfile
        )

        #expect(result == CodexThreadAccountHandoffResult(
            threadID: "forked-thread",
            rolloutURL: forkedRolloutURL.standardizedFileURL,
            profileID: destinationProfile.id
        ))
        #expect(rpc.recordedRequests.map(\.method) == ["account/read", "thread/fork"])
        let forkRequest = try #require(rpc.recordedRequests.last)
        #expect(forkRequest.params["threadId"] as? String == "source-thread")
        #expect(forkRequest.params["excludeTurns"] as? Bool == true)
        #expect(forkRequest.params["deferGoalContinuation"] as? Bool == true)
        #expect(bindings.profileID(for: "source-thread") == sourceProfile.id)
        #expect(bindings.profileID(for: "forked-thread") == destinationProfile.id)
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
