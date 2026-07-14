import SwiftUI

struct CodexUsagePopover: View {
    @ObservedObject var store: CodexRateLimitStore

    var body: some View {
        usageSurface
            .onAppear {
                store.refreshIfNeeded(maxAge: 10)
            }
            .onDisappear {
                store.cancelResetRedemption()
            }
    }

    @ViewBuilder
    private var usageSurface: some View {
        if #available(macOS 26.0, *) {
            content
                .background(
                    Color.black.opacity(0.40),
                    in: RoundedRectangle(cornerRadius: 22, style: .continuous)
                )
                .glassEffect(
                    .clear.tint(Color.black.opacity(0.18)).interactive(),
                    in: .rect(cornerRadius: 22)
                )
        } else {
            content
                .background(
                    .ultraThinMaterial,
                    in: RoundedRectangle(cornerRadius: 22, style: .continuous)
                )
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Image(systemName: "gauge.with.dots.needle.50percent")
                    .font(.system(size: 12, weight: .semibold))

                VStack(alignment: .leading, spacing: 1) {
                    Text("Codex usage")
                        .font(.system(size: 12, weight: .semibold))
                    Text(store.menuSummary)
                        .font(.system(size: 9, weight: .medium))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }

                Spacer(minLength: 4)

                Button {
                    store.refresh()
                } label: {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 10, weight: .bold))
                        .frame(width: 26, height: 26)
                        .contentShape(Circle())
                }
                .buttonStyle(.plain)
                .disabled(store.isLoading)
                .help("Refresh Codex usage")
            }

            if store.isLoading && store.snapshot == nil {
                ProgressView()
                    .controlSize(.small)
                    .frame(maxWidth: .infinity, minHeight: 54)
            } else if let confirmation = store.pendingResetConfirmation {
                resetConfirmation(confirmation)
            } else if let snapshot = store.snapshot {
                ForEach(snapshot.allGroups.prefix(2)) { group in
                    VStack(alignment: .leading, spacing: 7) {
                        Text(group.title)
                            .font(.system(size: 10, weight: .semibold))
                        if let shortWindow = group.shortWindow {
                            UsageWindowRow(title: "Hourly", window: shortWindow)
                        }
                        if let weeklyWindow = group.weeklyWindow {
                            UsageWindowRow(title: "Weekly", window: weeklyWindow)
                        }
                    }
                }

                Divider()
                    .opacity(0.35)

                resetChoices(snapshot: snapshot)
            } else {
                Text(store.errorMessage ?? "Codex usage is unavailable.")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if let statusMessage = store.resetStatusMessage {
                Text(statusMessage)
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(12)
        .frame(width: 286, alignment: .leading)
    }

    @ViewBuilder
    private func resetChoices(snapshot: CodexUsageSnapshot) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("Banked resets", systemImage: "arrow.counterclockwise.circle")
                    .font(.system(size: 10, weight: .semibold))
                Spacer()
                Text("\(snapshot.availableResetCount)")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.secondary)
            }

            if snapshot.availableResetCount == 0 {
                Text("No Codex usage resets are available.")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
            } else if store.availableResetCredits.isEmpty {
                Text("The account reports available resets but did not provide selectable reset details.")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(store.availableResetCredits) { credit in
                    Button {
                        store.prepareResetRedemption(for: credit)
                    } label: {
                        HStack(spacing: 7) {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(credit.displayTitle)
                                    .font(.system(size: 10, weight: .semibold))
                                if let expirationDate = credit.expirationDate {
                                    Text("Expires \(expirationDate.formatted(date: .abbreviated, time: .shortened))")
                                        .font(.system(size: 8, weight: .medium))
                                        .foregroundStyle(.secondary)
                                }
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .font(.system(size: 8, weight: .bold))
                                .foregroundStyle(.secondary)
                        }
                        .padding(.horizontal, 9)
                        .frame(minHeight: 34)
                        .background(Color.primary.opacity(0.07), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                    }
                    .buttonStyle(.plain)
                    .help("Review this reset before applying it")
                }
            }
        }
    }

    private func resetConfirmation(_ confirmation: CodexResetConfirmation) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            Label("Apply usage reset?", systemImage: "exclamationmark.circle")
                .font(.system(size: 11, weight: .semibold))

            Text("This will consume \(confirmation.displayTitle) and reset the eligible Codex limit. This cannot be undone.")
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(spacing: 7) {
                Button("Cancel") {
                    store.cancelResetRedemption()
                }
                .buttonStyle(.bordered)

                Button("Apply Reset") {
                    store.confirmResetRedemption(confirmation)
                }
                .buttonStyle(.borderedProminent)
                .disabled(store.isRedeemingReset)
            }
            .controlSize(.small)
        }
    }
}

private struct UsageWindowRow: View {
    var title: String
    var window: CodexRateLimitWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(.secondary)
                Spacer()
                Text("\(Int(window.remainingPercent.rounded()))% left")
                    .font(.system(size: 10, weight: .semibold))
            }

            ProgressView(value: window.remainingPercent, total: 100)
                .tint(window.remainingPercent < 20 ? .orange : .accentColor)

            if let resetDate = window.resetDate {
                Text("Resets \(resetDate.formatted(date: .abbreviated, time: .shortened))")
                    .font(.system(size: 8, weight: .medium))
                    .foregroundStyle(.secondary)
            }
        }
    }
}
