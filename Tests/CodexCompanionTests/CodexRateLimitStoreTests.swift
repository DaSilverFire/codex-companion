import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexRateLimitStoreTests {
    @Test
    func decodesAppServerRateLimitsAndSelectableResetDetails() throws {
        let snapshot = try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(Self.detailedUsageJSON.utf8)
        )

        let codex = try #require(snapshot.allGroups.first)
        #expect(codex.title == "Codex")
        #expect(codex.shortWindow?.remainingPercent == 82)
        #expect(codex.weeklyWindow?.remainingPercent == 61)
        #expect(snapshot.planType == "pro")
        #expect(snapshot.availableResetCount == 2)
        #expect(snapshot.availableResetCredits.map(\.id) == ["credit-a"])
    }

    @Test
    func countOnlyResetSummaryDoesNotInventSelectableCredits() throws {
        let json = Self.detailedUsageJSON
            .replacingOccurrences(
                of: #"{"availableCount":2,"credits":[{"id":"credit-a","resetType":"codexRateLimits","status":"available","grantedAt":100,"expiresAt":200,"title":"Referral reset","description":"Resets Codex usage"},{"id":"credit-b","resetType":"codexRateLimits","status":"redeemed","grantedAt":50,"expiresAt":null,"title":null,"description":null}]}"#,
                with: #"{"availableCount":3,"credits":null}"#
            )
        let snapshot = try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(json.utf8)
        )

        #expect(snapshot.availableResetCount == 3)
        #expect(snapshot.availableResetCredits.isEmpty)
    }

    @Test
    func bridgeUsageSnapshotKeepsRemainingWindowsAndSelectableResets() throws {
        let snapshot = try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(Self.detailedUsageJSON.utf8)
        )
        let updatedAt = Date(timeIntervalSince1970: 500)

        let bridgeSnapshot = CompanionBridgeUsageSnapshot(
            snapshot: snapshot,
            updatedAt: updatedAt
        )

        let codex = try #require(bridgeSnapshot.groups.first)
        #expect(bridgeSnapshot.planType == "pro")
        #expect(bridgeSnapshot.updatedAt == updatedAt)
        #expect(codex.title == "Codex")
        #expect(codex.shortWindow?.remainingPercent == 82)
        #expect(codex.weeklyWindow?.remainingPercent == 61)
        #expect(bridgeSnapshot.availableResetCount == 2)
        #expect(bridgeSnapshot.availableResetCredits.map(\.id) == ["credit-a"])
        #expect(bridgeSnapshot.availableResetCredits.first?.displayTitle == "Referral reset")
    }

    @MainActor
    @Test
    func preparingResetConfirmationNeverConsumesTheCredit() throws {
        let snapshot = try JSONDecoder().decode(
            CodexUsageSnapshot.self,
            from: Data(Self.detailedUsageJSON.utf8)
        )
        let credit = try #require(snapshot.availableResetCredits.first)
        let store = CodexRateLimitStore(
            readSnapshot: { snapshot },
            consumeReset: { _, _ in
                Issue.record("Preparing confirmation must not consume a reset")
                return .reset
            }
        )

        store.prepareResetRedemption(for: credit)

        #expect(store.pendingResetConfirmation?.creditID == "credit-a")
        #expect(store.pendingResetConfirmation?.displayTitle == "Referral reset")
        #expect(store.snapshot == nil)
    }

    private static let detailedUsageJSON = #"{"rateLimits":{"limitId":"codex","limitName":null,"primary":{"usedPercent":18,"windowDurationMins":300,"resetsAt":300},"secondary":{"usedPercent":39,"windowDurationMins":10080,"resetsAt":400},"credits":null,"individualLimit":null,"planType":"pro","rateLimitReachedType":null},"rateLimitsByLimitId":{"codex":{"limitId":"codex","limitName":null,"primary":{"usedPercent":18,"windowDurationMins":300,"resetsAt":300},"secondary":{"usedPercent":39,"windowDurationMins":10080,"resetsAt":400},"credits":null,"individualLimit":null,"planType":"pro","rateLimitReachedType":null}},"rateLimitResetCredits":{"availableCount":2,"credits":[{"id":"credit-a","resetType":"codexRateLimits","status":"available","grantedAt":100,"expiresAt":200,"title":"Referral reset","description":"Resets Codex usage"},{"id":"credit-b","resetType":"codexRateLimits","status":"redeemed","grantedAt":50,"expiresAt":null,"title":null,"description":null}]}}"#
}
