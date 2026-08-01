import Foundation

struct CodexAccountProfileUsageService: Sendable {
    var clientProvider: any CodexAccountProfileRPCClientProviding

    init(
        clientProvider: any CodexAccountProfileRPCClientProviding =
            CodexAccountProfileRPCClientProvider()
    ) {
        self.clientProvider = clientProvider
    }

    func readUsage(for profile: CodexAccountProfile) throws -> CodexUsageSnapshot {
        let service = try controlService(for: profile)
        return try service.readRateLimits(as: CodexUsageSnapshot.self)
    }

    func consumeReset(
        for profile: CodexAccountProfile,
        creditID: String,
        idempotencyKey: UUID
    ) throws -> CodexResetConsumeOutcome {
        let service = try controlService(for: profile)
        return try service.consumeResetCredit(
            creditID: creditID,
            idempotencyKey: idempotencyKey
        )
    }

    private func controlService(
        for profile: CodexAccountProfile
    ) throws -> CodexAppServerControlService {
        CodexAppServerControlService(client: try clientProvider.client(for: profile))
    }
}
