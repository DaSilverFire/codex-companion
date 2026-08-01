import SwiftUI

enum CodexUsagePresentationPhase: Equatable {
    case loading
    case usage
    case confirmation
    case unavailable
}

enum CodexUsagePresentationPolicy {
    static func phase(
        isLoading: Bool,
        hasSnapshot: Bool,
        hasConfirmation: Bool
    ) -> CodexUsagePresentationPhase {
        if hasConfirmation {
            return .confirmation
        }
        if hasSnapshot {
            return .usage
        }
        return isLoading ? .loading : .unavailable
    }
}

enum CodexUsagePresentationMetrics {
    static let popoverWidth: CGFloat = 292
    static let iconButtonSize: CGFloat = 32
    static let resetRowHeight: CGFloat = 40
    static let confirmationButtonHeight: CGFloat = 32
    static let confirmationButtonWidth: CGFloat = 112
    static let cornerRadius: CGFloat = 20
}

enum CodexUsageProfileDefaults {
    static func applicationDefaults(
        bundleIdentifier: String? = Bundle.main.bundleIdentifier
    ) -> UserDefaults {
        guard
            let bundleIdentifier,
            !bundleIdentifier.isEmpty,
            let defaults = UserDefaults(suiteName: bundleIdentifier)
        else {
            return .standard
        }
        return defaults
    }
}

private enum CodexUsageAnimation {
    static let phase = Animation.spring(
        response: 0.34,
        dampingFraction: 0.86,
        blendDuration: 0.08
    )
    static let refresh = Animation.smooth(duration: 0.18, extraBounce: 0.03)
}

struct CodexUsagePopover: View {
    @ObservedObject private var store: CodexRateLimitStore
    @StateObject private var profileSwitcher: CodexAccountProfileSwitcher
    @StateObject private var profileUsageStore: CodexAccountProfileUsageStore

    init(
        store: CodexRateLimitStore,
        defaults: UserDefaults = CodexUsageProfileDefaults.applicationDefaults(),
        selectionChanged: @escaping @MainActor () -> Void = {}
    ) {
        self.store = store
        _profileSwitcher = StateObject(
            wrappedValue: CodexAccountProfileSwitcher(
                defaults: defaults,
                selectionChanged: selectionChanged
            )
        )
        _profileUsageStore = StateObject(wrappedValue: CodexAccountProfileUsageStore())
    }

    var body: some View {
        content
            .companionLiquidGlassMenuSurface(
                cornerRadius: CodexUsagePresentationMetrics.cornerRadius
            )
            .task(id: profileSwitcher.selectedProfileID) {
                guard let profile = profileSwitcher.selectedProfile else {
                    profileUsageStore.select(nil)
                    store.refreshIfNeeded(maxAge: 10)
                    return
                }
                profileUsageStore.select(profile)
                await profileUsageStore.refresh(for: profile)
            }
            .onDisappear {
                store.cancelResetRedemption()
                profileUsageStore.cancelResetRedemption()
            }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            header

            if !profileSwitcher.profiles.isEmpty {
                accountSelector
            }

            phaseContent
                .id(phase)
                .transition(
                    .opacity.combined(
                        with: .scale(scale: 0.985, anchor: .top)
                    )
                )

            if let statusMessage = activeResetStatusMessage {
                statusRow(statusMessage)
                    .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .padding(14)
        .frame(
            width: CodexUsagePresentationMetrics.popoverWidth,
            alignment: .leading
        )
        .animation(CodexUsageAnimation.phase, value: phase)
        .animation(CodexUsageAnimation.phase, value: activeResetStatusMessage)
    }

    private var accountSelector: some View {
        VStack(alignment: .leading, spacing: 4) {
            Picker("New work account", selection: selectedProfileBinding) {
                Text("Current app account")
                    .tag(UUID?.none)
                ForEach(profileSwitcher.profiles) { profile in
                    Text(profile.label)
                        .tag(Optional(profile.id))
                }
            }
            .pickerStyle(.menu)
            .controlSize(.small)
            .frame(maxWidth: .infinity, alignment: .leading)

            Text("New Codex work uses this account. Running work stays with its original account.")
                .font(.system(size: 8, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var selectedProfileBinding: Binding<UUID?> {
        Binding(
            get: { profileSwitcher.selectedProfileID },
            set: { profileID in
                profileSwitcher.selectProfile(id: profileID)
            }
        )
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: "gauge.with.dots.needle.50percent")
                .font(.system(size: 14, weight: .semibold))
                .frame(width: 20, height: 20)
                .foregroundStyle(.primary)

            VStack(alignment: .leading, spacing: 2) {
                Text("Codex usage")
                    .font(.system(size: 13, weight: .semibold))

                Text(headerDetail)
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 6)

            refreshButton
        }
    }

    private var refreshButton: some View {
        Button {
            withAnimation(CodexUsageAnimation.refresh) {
                refreshUsage()
            }
        } label: {
            ZStack {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 11, weight: .bold))
                    .opacity(activeIsLoading ? 0 : 1)

                ProgressView()
                    .controlSize(.mini)
                    .opacity(activeIsLoading ? 1 : 0)
            }
            .frame(
                width: CodexUsagePresentationMetrics.iconButtonSize,
                height: CodexUsagePresentationMetrics.iconButtonSize
            )
            .contentShape(Circle())
            .modifier(UsageCircleControl())
        }
        .buttonStyle(.plain)
        .frame(
            width: CodexUsagePresentationMetrics.iconButtonSize,
            height: CodexUsagePresentationMetrics.iconButtonSize
        )
        .disabled(activeIsLoading)
        .accessibilityLabel(activeIsLoading ? "Refreshing Codex usage" : "Refresh Codex usage")
        .help("Refresh Codex usage")
    }

    private func refreshUsage() {
        if let profile = profileSwitcher.selectedProfile {
            Task {
                await profileUsageStore.refresh(for: profile)
            }
        } else {
            store.refresh()
        }
    }

    @ViewBuilder
    private var phaseContent: some View {
        switch phase {
        case .loading:
            loadingContent
        case .usage:
            if let snapshot = activeSnapshot {
                usageContent(snapshot)
            }
        case .confirmation:
            if let confirmation = activePendingResetConfirmation {
                resetConfirmation(confirmation)
            }
        case .unavailable:
            unavailableContent
        }
    }

    private var loadingContent: some View {
        VStack(spacing: 8) {
            ProgressView()
                .controlSize(.small)
            Text("Checking your current limits...")
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, minHeight: 82)
    }

    private func usageContent(_ snapshot: CodexUsageSnapshot) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            ForEach(snapshot.allGroups.prefix(2)) { group in
                VStack(alignment: .leading, spacing: 9) {
                    if snapshot.allGroups.count > 1 {
                        Text(group.title)
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundStyle(.secondary)
                    }

                    if let shortWindow = group.shortWindow {
                        UsageWindowRow(
                            title: "Hourly",
                            systemImage: "clock",
                            window: shortWindow
                        )
                    }
                    if let weeklyWindow = group.weeklyWindow {
                        UsageWindowRow(
                            title: "Weekly",
                            systemImage: "calendar",
                            window: weeklyWindow
                        )
                    }
                }
            }

            Divider()
                .opacity(0.32)

            resetChoices(snapshot: snapshot)
        }
    }

    private var unavailableContent: some View {
        HStack(alignment: .top, spacing: 9) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.orange)
                .frame(width: 18)

            Text(activeErrorMessage ?? "Codex usage is unavailable.")
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, minHeight: 58, alignment: .topLeading)
    }

    private var phase: CodexUsagePresentationPhase {
        CodexUsagePresentationPolicy.phase(
            isLoading: activeIsLoading,
            hasSnapshot: activeSnapshot != nil,
            hasConfirmation: activePendingResetConfirmation != nil
        )
    }

    private var headerDetail: String {
        if let lastUpdated = activeLastUpdated {
            return "Updated \(lastUpdated.formatted(date: .omitted, time: .shortened))"
        }
        return activeMenuSummary
    }

    @ViewBuilder
    private func resetChoices(snapshot: CodexUsageSnapshot) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack(spacing: 7) {
                Label("Banked resets", systemImage: "arrow.counterclockwise.circle")
                    .font(.system(size: 10, weight: .semibold))
                Spacer()
                Text("\(snapshot.availableResetCount)")
                    .font(.system(size: 10, weight: .bold))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            if snapshot.availableResetCount == 0 {
                Text("No Codex usage resets are available.")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
            } else if activeAvailableResetCredits.isEmpty {
                Text("Available resets were reported without selectable details.")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(activeAvailableResetCredits) { credit in
                    resetChoice(credit)
                }
            }
        }
    }

    private func resetChoice(_ credit: CodexRateLimitResetCredit) -> some View {
        Button {
            withAnimation(CodexUsageAnimation.phase) {
                prepareResetRedemption(for: credit)
            }
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "arrow.counterclockwise")
                    .font(.system(size: 10, weight: .semibold))
                    .frame(width: 18)

                VStack(alignment: .leading, spacing: 1) {
                    Text(credit.displayTitle)
                        .font(.system(size: 10, weight: .semibold))
                        .lineLimit(1)
                    if let expirationDate = credit.expirationDate {
                        Text("Expires \(expirationDate.formatted(date: .abbreviated, time: .shortened))")
                            .font(.system(size: 8, weight: .medium))
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }

                Spacer(minLength: 4)

                Image(systemName: "chevron.right")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundStyle(.secondary)
                    .frame(width: 12)
            }
            .padding(.horizontal, 10)
            .frame(
                maxWidth: .infinity,
                minHeight: CodexUsagePresentationMetrics.resetRowHeight,
                maxHeight: CodexUsagePresentationMetrics.resetRowHeight,
                alignment: .leading
            )
            .contentShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
            .modifier(UsageResetControl())
        }
        .buttonStyle(.plain)
        .help("Review this reset before applying it")
    }

    private func resetConfirmation(_ confirmation: CodexResetConfirmation) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Apply usage reset?", systemImage: "exclamationmark.circle")
                .font(.system(size: 11, weight: .semibold))

            Text("This consumes \(confirmation.displayTitle) and resets the eligible Codex limit. It cannot be undone.")
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 8) {
                confirmationButton(title: "Cancel", isProminent: false) {
                    withAnimation(CodexUsageAnimation.phase) {
                        cancelResetRedemption()
                    }
                }

                confirmationButton(title: "Apply Reset", isProminent: true) {
                    withAnimation(CodexUsageAnimation.phase) {
                        confirmResetRedemption(confirmation)
                    }
                }
                .disabled(activeIsRedeemingReset)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 104, alignment: .topLeading)
    }

    private func confirmationButton(
        title: String,
        isProminent: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 10, weight: .semibold))
                .frame(
                    width: CodexUsagePresentationMetrics.confirmationButtonWidth,
                    height: CodexUsagePresentationMetrics.confirmationButtonHeight
                )
                .contentShape(Capsule())
                .modifier(UsageActionControl(isProminent: isProminent))
        }
        .buttonStyle(.plain)
        .frame(
            width: CodexUsagePresentationMetrics.confirmationButtonWidth,
            height: CodexUsagePresentationMetrics.confirmationButtonHeight
        )
    }

    private func statusRow(_ message: String) -> some View {
        HStack(alignment: .top, spacing: 7) {
            Image(systemName: activeIsRedeemingReset ? "hourglass" : "checkmark.circle")
                .font(.system(size: 9, weight: .semibold))
                .frame(width: 14)
            Text(message)
                .font(.system(size: 9, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func prepareResetRedemption(for credit: CodexRateLimitResetCredit) {
        if profileSwitcher.selectedProfile != nil {
            profileUsageStore.prepareResetRedemption(for: credit)
        } else {
            store.prepareResetRedemption(for: credit)
        }
    }

    private func cancelResetRedemption() {
        if profileSwitcher.selectedProfile != nil {
            profileUsageStore.cancelResetRedemption()
        } else {
            store.cancelResetRedemption()
        }
    }

    private func confirmResetRedemption(_ confirmation: CodexResetConfirmation) {
        guard let profile = profileSwitcher.selectedProfile else {
            store.confirmResetRedemption(confirmation)
            return
        }
        Task {
            await profileUsageStore.confirmResetRedemption(confirmation, for: profile)
            guard profileSwitcher.selectedProfileID == profile.id else { return }
            await profileUsageStore.refresh(for: profile)
        }
    }

    private var activeSnapshot: CodexUsageSnapshot? {
        profileSwitcher.selectedProfile == nil ? store.snapshot : profileUsageStore.snapshot
    }

    private var activeIsLoading: Bool {
        profileSwitcher.selectedProfile == nil ? store.isLoading : profileUsageStore.isLoading
    }

    private var activeLastUpdated: Date? {
        profileSwitcher.selectedProfile == nil ? store.lastUpdated : profileUsageStore.lastUpdated
    }

    private var activeErrorMessage: String? {
        profileSwitcher.selectedProfile == nil ? store.errorMessage : profileUsageStore.errorMessage
    }

    private var activePendingResetConfirmation: CodexResetConfirmation? {
        profileSwitcher.selectedProfile == nil
            ? store.pendingResetConfirmation
            : profileUsageStore.pendingResetConfirmation
    }

    private var activeIsRedeemingReset: Bool {
        profileSwitcher.selectedProfile == nil
            ? store.isRedeemingReset
            : profileUsageStore.isRedeemingReset
    }

    private var activeResetStatusMessage: String? {
        profileSwitcher.selectedProfile == nil
            ? store.resetStatusMessage
            : profileUsageStore.resetStatusMessage
    }

    private var activeAvailableResetCredits: [CodexRateLimitResetCredit] {
        profileSwitcher.selectedProfile == nil
            ? store.availableResetCredits
            : profileUsageStore.availableResetCredits
    }

    private var activeMenuSummary: String {
        profileSwitcher.selectedProfile == nil ? store.menuSummary : profileUsageStore.menuSummary
    }
}

private struct UsageWindowRow: View {
    var title: String
    var systemImage: String
    var window: CodexRateLimitWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 7) {
                Image(systemName: systemImage)
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 14)

                Text(title)
                    .font(.system(size: 10, weight: .semibold))

                Spacer()

                Text("\(Int(window.remainingPercent.rounded()))% left")
                    .font(.system(size: 11, weight: .semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText())
            }

            ProgressView(value: window.remainingPercent, total: 100)
                .tint(window.remainingPercent < 20 ? .orange : .accentColor)
                .frame(height: 4)

            if let resetDate = window.resetDate {
                Text("Resets \(resetDate.formatted(date: .abbreviated, time: .shortened))")
                    .font(.system(size: 8, weight: .medium))
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct UsageCircleControl: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content
                .glassEffect(.regular.interactive(), in: .circle)
                .glassEffectTransition(.materialize)
        } else {
            content
                .background(.regularMaterial, in: Circle())
        }
    }
}

private struct UsageResetControl: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content
                .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 14))
                .glassEffectTransition(.materialize)
        } else {
            content
                .background(
                    .thinMaterial,
                    in: RoundedRectangle(cornerRadius: 14, style: .continuous)
                )
        }
    }
}

private struct UsageActionControl: ViewModifier {
    var isProminent: Bool

    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content
                .glassEffect(
                    isProminent
                        ? .regular.tint(Color.accentColor.opacity(0.34)).interactive()
                        : .regular.interactive(),
                    in: .capsule
                )
                .glassEffectTransition(.materialize)
        } else {
            content
                .background(
                    isProminent ? Color.accentColor.opacity(0.72) : Color.clear,
                    in: Capsule()
                )
                .background(.regularMaterial, in: Capsule())
        }
    }
}
