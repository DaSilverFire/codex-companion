import Foundation

struct CodexResetConfirmation: Equatable, Sendable {
    var creditID: String
    var displayTitle: String
    var idempotencyKey: UUID
}

@MainActor
final class CodexRateLimitStore: ObservableObject {
    typealias SnapshotReader = @Sendable () throws -> CodexUsageSnapshot
    typealias ResetConsumer = @Sendable (String, UUID) throws -> CodexResetConsumeOutcome

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
        readSnapshot: @escaping SnapshotReader = {
            try CodexAppServerControlService.shared.readRateLimits(as: CodexUsageSnapshot.self)
        },
        consumeReset: @escaping ResetConsumer = { creditID, idempotencyKey in
            try CodexAppServerControlService.shared.consumeResetCredit(
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
            return errorMessage ?? "Rate limits unavailable"
        }

        let short = main.shortWindow.map { "\(Int($0.remainingPercent.rounded()))% short left" }
        let weekly = main.weeklyWindow.map { "\(Int($0.remainingPercent.rounded()))% weekly left" }
        let resets = snapshot.availableResetCount > 0
            ? "\(snapshot.availableResetCount) reset\(snapshot.availableResetCount == 1 ? "" : "s")"
            : nil
        return [short, weekly, resets].compactMap { $0 }.joined(separator: " · ")
    }

    var hoverSummary: String {
        guard let snapshot, let main = snapshot.allGroups.first else {
            return isLoading ? "Checking Codex usage..." : (errorMessage ?? "Rate limits unavailable")
        }

        let hourly = main.shortWindow.map { "Hourly remaining: \(Int($0.remainingPercent.rounded()))%" }
        let weekly = main.weeklyWindow.map { "Weekly remaining: \(Int($0.remainingPercent.rounded()))%" }
        let resets = snapshot.availableResetCount > 0
            ? "Banked resets: \(snapshot.availableResetCount)"
            : "No banked resets"
        return [hourly, weekly, resets].compactMap { $0 }.joined(separator: "\n")
    }

    var availableResetCredits: [CodexRateLimitResetCredit] {
        snapshot?.availableResetCredits ?? []
    }

    var availableResetCount: Int {
        snapshot?.availableResetCount ?? 0
    }

    func refreshIfNeeded(maxAge: TimeInterval = 60) {
        guard !isLoading else { return }
        if let lastUpdated, Date().timeIntervalSince(lastUpdated) < maxAge {
            return
        }
        refresh()
    }

    func refresh() {
        guard !isLoading else { return }
        isLoading = true
        errorMessage = nil
        let readSnapshot = self.readSnapshot

        Task.detached(priority: .utility) {
            let result = Result { try readSnapshot() }
            await MainActor.run {
                switch result {
                case .success(let snapshot):
                    self.snapshot = snapshot
                    self.lastUpdated = Date()
                    self.errorMessage = nil
                case .failure(let error):
                    self.errorMessage = (error as? LocalizedError)?.errorDescription
                        ?? error.localizedDescription
                }
                self.isLoading = false
            }
        }
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

    func confirmResetRedemption(_ confirmation: CodexResetConfirmation) {
        guard confirmation == pendingResetConfirmation, !isRedeemingReset else { return }
        pendingResetConfirmation = nil
        isRedeemingReset = true
        resetStatusMessage = "Applying \(confirmation.displayTitle)..."
        let consumeReset = self.consumeReset

        Task.detached(priority: .userInitiated) {
            let result = Result {
                try consumeReset(confirmation.creditID, confirmation.idempotencyKey)
            }
            await MainActor.run {
                self.isRedeemingReset = false
                switch result {
                case .success(.reset):
                    self.resetStatusMessage = "Codex usage reset applied."
                    self.refresh()
                case .success(.nothingToReset):
                    self.resetStatusMessage = "There is currently no Codex limit to reset."
                case .success(.noCredit):
                    self.resetStatusMessage = "That Codex reset is no longer available."
                    self.refresh()
                case .success(.alreadyRedeemed):
                    self.resetStatusMessage = "That Codex reset was already used."
                    self.refresh()
                case .failure(let error):
                    self.resetStatusMessage = error.localizedDescription
                }
            }
        }
    }
}
