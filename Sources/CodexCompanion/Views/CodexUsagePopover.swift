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

private enum CodexUsageAnimation {
    static let phase = Animation.spring(
        response: 0.34,
        dampingFraction: 0.86,
        blendDuration: 0.08
    )
    static let refresh = Animation.smooth(duration: 0.18, extraBounce: 0.03)
}

struct CodexUsagePopover: View {
    @ObservedObject var store: CodexRateLimitStore

    var body: some View {
        content
            .companionLiquidGlassMenuSurface(
                cornerRadius: CodexUsagePresentationMetrics.cornerRadius
            )
            .onAppear {
                store.refreshIfNeeded(maxAge: 10)
            }
            .onDisappear {
                store.cancelResetRedemption()
            }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            header

            phaseContent
                .id(phase)
                .transition(
                    .opacity.combined(
                        with: .scale(scale: 0.985, anchor: .top)
                    )
                )

            if let statusMessage = store.resetStatusMessage {
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
        .animation(CodexUsageAnimation.phase, value: store.resetStatusMessage)
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
                store.refresh()
            }
        } label: {
            ZStack {
                Image(systemName: "arrow.clockwise")
                    .font(.system(size: 11, weight: .bold))
                    .opacity(store.isLoading ? 0 : 1)

                ProgressView()
                    .controlSize(.mini)
                    .opacity(store.isLoading ? 1 : 0)
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
        .disabled(store.isLoading)
        .accessibilityLabel(store.isLoading ? "Refreshing Codex usage" : "Refresh Codex usage")
        .help("Refresh Codex usage")
    }

    @ViewBuilder
    private var phaseContent: some View {
        switch phase {
        case .loading:
            loadingContent
        case .usage:
            if let snapshot = store.snapshot {
                usageContent(snapshot)
            }
        case .confirmation:
            if let confirmation = store.pendingResetConfirmation {
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

            Text(store.errorMessage ?? "Codex usage is unavailable.")
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, minHeight: 58, alignment: .topLeading)
    }

    private var phase: CodexUsagePresentationPhase {
        CodexUsagePresentationPolicy.phase(
            isLoading: store.isLoading,
            hasSnapshot: store.snapshot != nil,
            hasConfirmation: store.pendingResetConfirmation != nil
        )
    }

    private var headerDetail: String {
        if let lastUpdated = store.lastUpdated {
            return "Updated \(lastUpdated.formatted(date: .omitted, time: .shortened))"
        }
        return store.menuSummary
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
            } else if store.availableResetCredits.isEmpty {
                Text("Available resets were reported without selectable details.")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(store.availableResetCredits) { credit in
                    resetChoice(credit)
                }
            }
        }
    }

    private func resetChoice(_ credit: CodexRateLimitResetCredit) -> some View {
        Button {
            withAnimation(CodexUsageAnimation.phase) {
                store.prepareResetRedemption(for: credit)
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
                        store.cancelResetRedemption()
                    }
                }

                confirmationButton(title: "Apply Reset", isProminent: true) {
                    withAnimation(CodexUsageAnimation.phase) {
                        store.confirmResetRedemption(confirmation)
                    }
                }
                .disabled(store.isRedeemingReset)
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
            Image(systemName: store.isRedeemingReset ? "hourglass" : "checkmark.circle")
                .font(.system(size: 9, weight: .semibold))
                .frame(width: 14)
            Text(message)
                .font(.system(size: 9, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
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
