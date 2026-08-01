import SwiftUI

struct CodexAccountProfileSettingsSection: View {
    @StateObject private var switcher: CodexAccountProfileSwitcher
    @StateObject private var usageStore = CodexAccountProfileUsageStore()
    @Binding private var automaticallyContinuesGoalsAcrossAccounts: Bool
    @Binding private var automaticallyContinuesQuotaInterruptedTasksAcrossAccounts: Bool
    private let automaticGoalContinuationStatus: String?
    private let automaticTaskContinuationStatus: String?
    @State private var newProfileLabel = ""

    init(
        automaticallyContinuesGoalsAcrossAccounts: Binding<Bool>,
        automaticGoalContinuationStatus: String?,
        automaticallyContinuesQuotaInterruptedTasksAcrossAccounts: Binding<Bool>,
        automaticTaskContinuationStatus: String?,
        onReconnect: @escaping @MainActor () -> Void
    ) {
        _switcher = StateObject(
            wrappedValue: CodexAccountProfileSwitcher(
                selectionChanged: onReconnect
            )
        )
        _automaticallyContinuesGoalsAcrossAccounts =
            automaticallyContinuesGoalsAcrossAccounts
        _automaticallyContinuesQuotaInterruptedTasksAcrossAccounts =
            automaticallyContinuesQuotaInterruptedTasksAcrossAccounts
        self.automaticGoalContinuationStatus = automaticGoalContinuationStatus
        self.automaticTaskContinuationStatus = automaticTaskContinuationStatus
    }

    var body: some View {
        Section("Codex Profiles") {
            Picker("Selected profile", selection: selectedProfileBinding) {
                Text("No profile").tag(Optional<UUID>.none)
                ForEach(switcher.profiles) { profile in
                    Text(profile.label).tag(Optional(profile.id))
                }
            }

            HStack {
                TextField("Profile label, such as Main", text: $newProfileLabel)
                    .onSubmit(addProfile)

                Button("Add") {
                    addProfile()
                }
                .disabled(newProfileLabel.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }

            Toggle(
                "Continue quota-limited goals with another account",
                isOn: $automaticallyContinuesGoalsAcrossAccounts
            )

            Text("Companion waits for the turn to stop, moves only that task to the next signed-in profile with available usage, and sends \"continue\" once. Banked resets stay untouched.")
                .font(.caption)
                .foregroundStyle(.secondary)

            if let automaticGoalContinuationStatus,
               !automaticGoalContinuationStatus.isEmpty {
                Text(automaticGoalContinuationStatus)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Toggle(
                "Continue quota-interrupted tasks without goals",
                isOn: $automaticallyContinuesQuotaInterruptedTasksAcrossAccounts
            )

            Text("For a stopped task without a goal, Companion requires both confirmed Codex usage exhaustion and an explicit usage-limit turn failure. It then moves that task to one available signed-in profile and sends \"continue\" once. Network and relay errors are ignored, approval-pending work stays put, and banked resets stay untouched.")
                .font(.caption)
                .foregroundStyle(.secondary)

            if let automaticTaskContinuationStatus,
               !automaticTaskContinuationStatus.isEmpty {
                Text(automaticTaskContinuationStatus)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let profile = switcher.selectedProfile {
                authenticationControls(for: profile)

                if switcher.authenticationState == .signedIn {
                    profileUsage(for: profile)
                }

                if switcher.canRemoveProfile(id: profile.id) {
                    Button("Remove \(profile.label) from Companion", role: .destructive) {
                        if switcher.removeProfile(id: profile.id) {
                            usageStore.select(switcher.selectedProfile)
                        }
                    }
                } else {
                    Text("This profile still owns bound tasks. Remove or hand off its bound tasks first.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if let profileRemovalErrorMessage = switcher.profileRemovalErrorMessage {
                Text(profileRemovalErrorMessage)
                    .font(.caption)
                    .foregroundStyle(.red)
            }

            Text("Each label maps to an isolated official Codex sign-in. Companion stores the label and profile identifier, while the Codex CLI owns the credentials and sign-in flow.")
                .font(.caption)
                .foregroundStyle(.secondary)

            Text("Active turns stay with the account that started them. Select a profile for new work, or explicitly resume a stopped task under that profile; Companion never copies sign-in secrets between accounts.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .task(id: switcher.selectedProfileID) {
            guard let profile = switcher.selectedProfile else {
                usageStore.select(nil)
                return
            }
            usageStore.select(profile)
            await switcher.refreshSelectedProfileStatus()
            if switcher.authenticationState == .signedIn {
                await usageStore.refresh(for: profile)
            }
        }
    }

    @ViewBuilder
    private func authenticationControls(for profile: CodexAccountProfile) -> some View {
        LabeledContent("Codex sign-in") {
            authenticationStatus
        }

        Button {
            Task {
                await switcher.signInSelectedProfile()
                if switcher.authenticationState == .signedIn {
                    await usageStore.refresh(for: profile)
                }
            }
        } label: {
            Label(
                switcher.authenticationState == .signedIn
                    ? "Reauthenticate with Codex"
                    : "Sign In to Codex",
                systemImage: "person.crop.circle.badge.checkmark"
            )
        }
        .disabled(
            switcher.authenticationState == .checking
                || switcher.authenticationState == .signingIn
        )
    }

    @ViewBuilder
    private var authenticationStatus: some View {
        switch switcher.authenticationState {
        case .unchecked:
            Text("Not checked")
                .foregroundStyle(.secondary)
        case .checking:
            HStack(spacing: 6) {
                ProgressView()
                    .controlSize(.small)
                Text("Checking")
            }
        case .signedOut:
            Text("Sign-in required")
                .foregroundStyle(.orange)
        case .signingIn:
            HStack(spacing: 6) {
                ProgressView()
                    .controlSize(.small)
                Text("Complete sign-in in your browser")
            }
        case .signedIn:
            Label("Ready", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .failed(let message):
            Text(message)
                .foregroundStyle(.red)
                .multilineTextAlignment(.trailing)
        }
    }

    @ViewBuilder
    private func profileUsage(for profile: CodexAccountProfile) -> some View {
        Divider()

        LabeledContent("Usage for \(profile.label)") {
            Text(usageStore.menuSummary)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }

        if let snapshot = usageStore.snapshot {
            if let planType = snapshot.planType?.trimmingCharacters(in: .whitespacesAndNewlines),
               !planType.isEmpty {
                LabeledContent("Plan") {
                    Text(planType.displayTitle)
                        .foregroundStyle(.secondary)
                }
            }

            ForEach(snapshot.allGroups) { group in
                VStack(alignment: .leading, spacing: 5) {
                    Text(group.title)
                        .font(.subheadline.weight(.semibold))
                    usageWindow("Short window", window: group.shortWindow)
                    usageWindow("Weekly window", window: group.weeklyWindow)
                }
            }

            LabeledContent("Banked resets") {
                Text("\(usageStore.availableResetCount)")
                    .foregroundStyle(.secondary)
            }

            ForEach(usageStore.availableResetCredits) { credit in
                Button("Use \(credit.displayTitle)") {
                    usageStore.prepareResetRedemption(for: credit)
                }
                .disabled(usageStore.isRedeemingReset)
            }

            if usageStore.availableResetCount > 0,
               usageStore.availableResetCredits.isEmpty {
                Text("Codex reports banked resets, but did not expose selectable reset details for this profile.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }

        if let confirmation = usageStore.pendingResetConfirmation {
            VStack(alignment: .leading, spacing: 6) {
                Text("Apply Reset")
                    .font(.subheadline.weight(.semibold))
                Text("Use \(confirmation.displayTitle) for \(profile.label)? This only affects the selected profile.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                HStack {
                    Button("Apply Reset", role: .destructive) {
                        Task {
                            await usageStore.confirmResetRedemption(
                                confirmation,
                                for: profile
                            )
                        }
                    }
                    Button("Cancel") {
                        usageStore.cancelResetRedemption()
                    }
                }
            }
        }

        if let statusMessage = usageStore.resetStatusMessage {
            Text(statusMessage)
                .font(.caption)
                .foregroundStyle(.secondary)
        }

        if let errorMessage = usageStore.errorMessage {
            Text(errorMessage)
                .font(.caption)
                .foregroundStyle(.red)
        }

        Button {
            Task { await usageStore.refresh(for: profile) }
        } label: {
            Label("Refresh Profile Usage", systemImage: "arrow.clockwise")
        }
        .disabled(usageStore.isLoading)
    }

    @ViewBuilder
    private func usageWindow(
        _ title: String,
        window: CodexRateLimitWindow?
    ) -> some View {
        if let window {
            HStack {
                Text(title)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("\(Int(window.remainingPercent.rounded()))% remaining")
                    .foregroundStyle(.secondary)
                if let resetDate = window.resetDate {
                    Text("· resets \(resetDate.formatted(date: .abbreviated, time: .shortened))")
                        .foregroundStyle(.tertiary)
                }
            }
            .font(.caption)
        }
    }

    private var selectedProfileBinding: Binding<UUID?> {
        Binding(
            get: { switcher.selectedProfileID },
            set: { switcher.selectProfile(id: $0) }
        )
    }

    private func addProfile() {
        guard switcher.addProfile(label: newProfileLabel) != nil else { return }
        newProfileLabel = ""
    }
}
