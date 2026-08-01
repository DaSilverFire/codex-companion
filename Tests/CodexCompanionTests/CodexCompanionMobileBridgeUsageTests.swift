import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeUsageTests {
    @Test
    func usageRequestUsesTheExplicitMobileAccountProfile() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let recorder = MobileBridgeUsageRecorder(snapshot: try Self.snapshot())
        let server = CodexCompanionMobileBridgeServer(
            accountProfileProvider: { profileID in
                profileID == profile.id ? profile : nil
            },
            accountProfileUsageReader: { profile in
                try recorder.readUsage(for: profile)
            },
            accountProfileResetConsumer: { profile, creditID, idempotencyKey in
                try recorder.consumeReset(
                    for: profile,
                    creditID: creditID,
                    idempotencyKey: idempotencyKey
                )
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .loadUsage,
                accountProfileID: profile.id
            )
        )

        #expect(response.succeeded)
        #expect(response.usageSnapshot?.accountProfileID == profile.id)
        #expect(response.usageSnapshot?.accountProfileLabel == "Main")
        #expect(recorder.readProfileIDs == [profile.id])
    }

    @Test
    func resetRequestAndRefreshStayOnTheExplicitMobileAccountProfile() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Account 3")
        let recorder = MobileBridgeUsageRecorder(snapshot: try Self.snapshot())
        let server = CodexCompanionMobileBridgeServer(
            accountProfileProvider: { profileID in
                profileID == profile.id ? profile : nil
            },
            accountProfileUsageReader: { profile in
                try recorder.readUsage(for: profile)
            },
            accountProfileResetConsumer: { profile, creditID, idempotencyKey in
                try recorder.consumeReset(
                    for: profile,
                    creditID: creditID,
                    idempotencyKey: idempotencyKey
                )
            }
        )
        let idempotencyKey = UUID()

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .consumeUsageReset,
                accountProfileID: profile.id,
                resetCreditID: "credit-account-3",
                idempotencyKey: idempotencyKey
            )
        )

        #expect(response.succeeded)
        #expect(response.usageSnapshot?.accountProfileID == profile.id)
        #expect(response.usageSnapshot?.accountProfileLabel == "Account 3")
        #expect(recorder.resetCalls == [
            MobileBridgeUsageRecorder.ResetCall(
                profileID: profile.id,
                creditID: "credit-account-3",
                idempotencyKey: idempotencyKey
            ),
        ])
        #expect(recorder.readProfileIDs == [profile.id])
    }

    @Test
    func unknownMobileAccountProfileDoesNotFallBackToTheMacAccount() async throws {
        let recorder = MobileBridgeUsageRecorder(snapshot: try Self.snapshot())
        let server = CodexCompanionMobileBridgeServer(
            accountProfileProvider: { _ in nil },
            accountProfileUsageReader: { profile in
                try recorder.readUsage(for: profile)
            },
            accountProfileResetConsumer: { profile, creditID, idempotencyKey in
                try recorder.consumeReset(
                    for: profile,
                    creditID: creditID,
                    idempotencyKey: idempotencyKey
                )
            }
        )

        let response = await server.handle(
            CompanionBridgeRequest(
                operation: .loadUsage,
                accountProfileID: UUID()
            )
        )

        #expect(!response.succeeded)
        #expect(response.errorCode == "unknown_account_profile")
        #expect(recorder.readProfileIDs.isEmpty)
    }

    private static func snapshot() throws -> CodexUsageSnapshot {
        try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(
                """
                {
                  "rateLimits": {
                    "limitId": "codex",
                    "primary": {"usedPercent": 19, "windowDurationMins": 300},
                    "secondary": {"usedPercent": 42, "windowDurationMins": 10080},
                    "planType": "pro"
                  },
                  "rateLimitResetCredits": {
                    "availableCount": 1,
                    "credits": [{
                      "id": "credit-account-3",
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
        )
    }
}

private final class MobileBridgeUsageRecorder: @unchecked Sendable {
    struct ResetCall: Equatable {
        var profileID: UUID
        var creditID: String
        var idempotencyKey: UUID
    }

    private let lock = NSLock()
    private let snapshot: CodexUsageSnapshot
    private var recordedReadProfileIDs: [UUID] = []
    private var recordedResetCalls: [ResetCall] = []

    init(snapshot: CodexUsageSnapshot) {
        self.snapshot = snapshot
    }

    var readProfileIDs: [UUID] {
        lock.withLock { recordedReadProfileIDs }
    }

    var resetCalls: [ResetCall] {
        lock.withLock { recordedResetCalls }
    }

    func readUsage(for profile: CodexAccountProfile) throws -> CodexUsageSnapshot {
        lock.withLock { recordedReadProfileIDs.append(profile.id) }
        return snapshot
    }

    func consumeReset(
        for profile: CodexAccountProfile,
        creditID: String,
        idempotencyKey: UUID
    ) throws -> CodexResetConsumeOutcome {
        lock.withLock {
            recordedResetCalls.append(
                ResetCall(
                    profileID: profile.id,
                    creditID: creditID,
                    idempotencyKey: idempotencyKey
                )
            )
        }
        return .reset
    }
}
