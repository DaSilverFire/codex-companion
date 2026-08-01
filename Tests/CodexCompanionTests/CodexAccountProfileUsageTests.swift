import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAccountProfileUsageTests {
    @Test
    func usageAndResetRequestsUseTheSelectedProfileRPCClient() throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let client = RecordingProfileUsageRPCClient()
        let provider = RecordingProfileUsageRPCProvider(client: client)
        let service = CodexAccountProfileUsageService(clientProvider: provider)

        let snapshot = try service.readUsage(for: profile)
        let outcome = try service.consumeReset(
            for: profile,
            creditID: " reset-main ",
            idempotencyKey: UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
        )

        #expect(provider.requestedProfileIDs == [profile.id, profile.id])
        #expect(snapshot.allGroups.first?.shortWindow?.remainingPercent == 82)
        #expect(snapshot.allGroups.first?.weeklyWindow?.remainingPercent == 61)
        #expect(snapshot.availableResetCount == 1)
        #expect(outcome == .reset)
        #expect(client.methods == [
            "account/rateLimits/read",
            "account/rateLimitResetCredit/consume",
        ])
        #expect(client.lastResetCreditID == "reset-main")
    }

    @Test
    @MainActor
    func usageStoreReplacesTheOldProfileSnapshotBeforeLoadingAnotherProfile() async throws {
        let first = CodexAccountProfile(id: UUID(), label: "Backup")
        let second = CodexAccountProfile(id: UUID(), label: "Main")
        let recorder = ProfileUsageStoreRecorder()
        let store = CodexAccountProfileUsageStore(
            readSnapshot: { profile in
                recorder.recordRead(profile.id)
                return try Self.snapshot(usedPercent: profile.id == first.id ? 10 : 35)
            },
            consumeReset: { profile, creditID, _ in
                recorder.recordReset(profileID: profile.id, creditID: creditID)
                return .reset
            }
        )

        await store.refresh(for: first)
        #expect(store.profileID == first.id)
        #expect(store.snapshot?.allGroups.first?.shortWindow?.remainingPercent == 90)

        store.select(second)
        #expect(store.profileID == second.id)
        #expect(store.snapshot == nil)
        await store.refresh(for: second)
        #expect(store.snapshot?.allGroups.first?.shortWindow?.remainingPercent == 65)
        #expect(recorder.readProfileIDs == [first.id, second.id])
    }

    @Test
    @MainActor
    func resetRequiresConfirmationAndStaysScopedToTheSelectedProfile() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let recorder = ProfileUsageStoreRecorder()
        let store = CodexAccountProfileUsageStore(
            readSnapshot: { _ in try Self.snapshot(usedPercent: 40) },
            consumeReset: { selectedProfile, creditID, _ in
                recorder.recordReset(profileID: selectedProfile.id, creditID: creditID)
                return .reset
            }
        )
        await store.refresh(for: profile)
        let credit = try #require(store.availableResetCredits.first)

        store.prepareResetRedemption(for: credit)
        #expect(recorder.resetCalls.isEmpty)
        let confirmation = try #require(store.pendingResetConfirmation)
        await store.confirmResetRedemption(confirmation, for: profile)

        #expect(recorder.resetCalls.count == 1)
        #expect(recorder.resetCalls.first?.0 == profile.id)
        #expect(recorder.resetCalls.first?.1 == "credit-main")
        #expect(store.resetStatusMessage == "Codex usage reset applied for Main.")
    }

    private static func snapshot(usedPercent: Double) throws -> CodexUsageSnapshot {
        let data = Data(
            """
            {
              "rateLimits": {
                "limitId": "codex",
                "primary": {"usedPercent": \(usedPercent), "windowDurationMins": 300},
                "secondary": {"usedPercent": 39, "windowDurationMins": 10080},
                "planType": "pro"
              },
              "rateLimitResetCredits": {
                "availableCount": 1,
                "credits": [{
                  "id": "credit-main",
                  "resetType": "codexRateLimits",
                  "status": "available",
                  "grantedAt": 100,
                  "expiresAt": null,
                  "title": "Banked reset",
                  "description": "Reset Codex usage"
                }]
              }
            }
            """.utf8
        )
        return try JSONDecoder().decode(CodexUsageSnapshot.self, from: data)
    }
}

private final class RecordingProfileUsageRPCProvider:
    CodexAccountProfileRPCClientProviding,
    @unchecked Sendable
{
    private let lock = NSLock()
    private let client: RecordingProfileUsageRPCClient
    private var profileIDs: [UUID] = []

    init(client: RecordingProfileUsageRPCClient) {
        self.client = client
    }

    var requestedProfileIDs: [UUID] {
        lock.withLock { profileIDs }
    }

    func client(for profile: CodexAccountProfile) throws -> any CodexAppServerRPCPerforming {
        lock.withLock { profileIDs.append(profile.id) }
        return client
    }
}

private final class RecordingProfileUsageRPCClient:
    CodexAppServerRPCPerforming,
    @unchecked Sendable
{
    private let lock = NSLock()
    private var recordedMethods: [String] = []
    private var resetCreditID: String?

    var methods: [String] {
        lock.withLock { recordedMethods }
    }

    var lastResetCreditID: String? {
        lock.withLock { resetCreditID }
    }

    func perform(_ requests: [CodexRPCRequest]) throws -> [Int: CodexRPCResponse] {
        let request = try #require(requests.first)
        lock.withLock {
            recordedMethods.append(request.method)
            resetCreditID = request.params["creditId"] as? String ?? resetCreditID
        }

        switch request.method {
        case "account/rateLimits/read":
            return [
                request.id: CodexRPCResponse(
                    result: [
                        "rateLimits": [
                            "limitId": "codex",
                            "primary": ["usedPercent": 18, "windowDurationMins": 300],
                            "secondary": ["usedPercent": 39, "windowDurationMins": 10_080],
                            "planType": "pro",
                        ],
                        "rateLimitResetCredits": [
                            "availableCount": 1,
                            "credits": [[
                                "id": "credit-main",
                                "resetType": "codexRateLimits",
                                "status": "available",
                                "grantedAt": 100,
                                "expiresAt": NSNull(),
                                "title": "Banked reset",
                                "description": "Reset Codex usage",
                            ]],
                        ],
                    ],
                    error: nil
                ),
            ]
        case "account/rateLimitResetCredit/consume":
            return [
                request.id: CodexRPCResponse(
                    result: ["outcome": "reset"],
                    error: nil
                ),
            ]
        default:
            return [:]
        }
    }
}

private final class ProfileUsageStoreRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var recordedReadProfileIDs: [UUID] = []
    private var recordedResetCalls: [(UUID, String)] = []

    var readProfileIDs: [UUID] {
        lock.withLock { recordedReadProfileIDs }
    }

    var resetCalls: [(UUID, String)] {
        lock.withLock { recordedResetCalls }
    }

    func recordRead(_ profileID: UUID) {
        lock.withLock { recordedReadProfileIDs.append(profileID) }
    }

    func recordReset(profileID: UUID, creditID: String) {
        lock.withLock { recordedResetCalls.append((profileID, creditID)) }
    }
}
