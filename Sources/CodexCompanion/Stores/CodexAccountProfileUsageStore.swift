import Foundation

@MainActor
final class CodexAccountProfileUsageStore: ObservableObject {
    typealias SnapshotReader = @Sendable (CodexAccountProfile) throws -> CodexUsageSnapshot
    typealias ResetConsumer = @Sendable (
        CodexAccountProfile,
        String,
        UUID
    ) throws -> CodexResetConsumeOutcome

    @Published private(set) var profileID: UUID?
    @Published private(set) var snapshot: CodexUsageSnapshot?
    @Published private(set) var isLoading = false
    @Published private(set) var lastUpdated: Date?
    @Published private(set) var errorMessage: String?
    @Published private(set) var pendingResetConfirmation: CodexResetConfirmation?
    @Published private(set) var isRedeemingReset = false
    @Published private(set) var resetStatusMessage: String?

    private let readSnapshot: SnapshotReader
    private let consumeReset: ResetConsumer

    init(
        readSnapshot: @escaping SnapshotReader = { profile in
            try CodexAccountProfileUsageService().readUsage(for: profile)
        },
        consumeReset: @escaping ResetConsumer = { profile, creditID, idempotencyKey in
            try CodexAccountProfileUsageService().consumeReset(
                for: profile,
                creditID: creditID,
                idempotencyKey: idempotencyKey
            )
        }
    ) {
        self.readSnapshot = readSnapshot
        self.consumeReset = consumeReset
    }

    var menuSummary: String {
        if isLoading && snapshot == nil {
            return "Checking usage..."
        }
        guard let snapshot, let main = snapshot.allGroups.first else {
            return errorMessage ?? "Usage unavailable"
        }
        let short = main.shortWindow.map { "\(Int($0.remainingPercent.rounded()))% short left" }
        let weekly = main.weeklyWindow.map { "\(Int($0.remainingPercent.rounded()))% weekly left" }
        let resets = snapshot.availableResetCount > 0
            ? "\(snapshot.availableResetCount) reset\(snapshot.availableResetCount == 1 ? "" : "s")"
            : nil
        return [short, weekly, resets].compactMap { $0 }.joined(separator: " · ")
    }

    var availableResetCredits: [CodexRateLimitResetCredit] {
        snapshot?.availableResetCredits ?? []
    }

    var availableResetCount: Int {
        snapshot?.availableResetCount ?? 0
    }

    func select(_ profile: CodexAccountProfile?) {
        guard profileID != profile?.id else { return }
        profileID = profile?.id
        snapshot = nil
        isLoading = false
        lastUpdated = nil
        errorMessage = nil
        pendingResetConfirmation = nil
        isRedeemingReset = false
        resetStatusMessage = nil
    }

    func refresh(for profile: CodexAccountProfile) async {
        select(profile)
        guard !isLoading else { return }
        isLoading = true
        errorMessage = nil
        let readSnapshot = self.readSnapshot
        let result = await Task.detached(priority: .utility) {
            Result { try readSnapshot(profile) }
        }.value
        guard profileID == profile.id else { return }
        switch result {
        case .success(let snapshot):
            self.snapshot = snapshot
            lastUpdated = Date()
            errorMessage = nil
        case .failure(let error):
            errorMessage = Self.errorText(error)
        }
        isLoading = false
    }

    func prepareResetRedemption(for credit: CodexRateLimitResetCredit) {
        guard credit.isAvailable else {
            resetStatusMessage = "That Codex reset is not available."
            return
        }
        pendingResetConfirmation = CodexResetConfirmation(
            creditID: credit.id,
            displayTitle: credit.displayTitle,
            idempotencyKey: UUID()
        )
        resetStatusMessage = nil
    }

    func cancelResetRedemption() {
        pendingResetConfirmation = nil
    }

    func confirmResetRedemption(
        _ confirmation: CodexResetConfirmation,
        for profile: CodexAccountProfile
    ) async {
        guard
            profileID == profile.id,
            confirmation == pendingResetConfirmation,
            !isRedeemingReset
        else { return }

        pendingResetConfirmation = nil
        isRedeemingReset = true
        resetStatusMessage = "Applying \(confirmation.displayTitle)..."
        let consumeReset = self.consumeReset
        let result = await Task.detached(priority: .userInitiated) {
            Result {
                try consumeReset(
                    profile,
                    confirmation.creditID,
                    confirmation.idempotencyKey
                )
            }
        }.value
        guard profileID == profile.id else { return }
        isRedeemingReset = false

        switch result {
        case .success(.reset):
            resetStatusMessage = "Codex usage reset applied for \(profile.label)."
        case .success(.nothingToReset):
            resetStatusMessage = "There is currently no Codex limit to reset for \(profile.label)."
        case .success(.noCredit):
            resetStatusMessage = "That Codex reset is no longer available."
        case .success(.alreadyRedeemed):
            resetStatusMessage = "That Codex reset was already used."
        case .failure(let error):
            resetStatusMessage = Self.errorText(error)
        }
    }

    private static func errorText(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    }
}
