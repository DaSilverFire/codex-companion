import SwiftUI

struct RateLimitsView: View {
    @ObservedObject var store: CodexRateLimitStore

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("Codex Rate Limits")
                        .font(.title3.weight(.semibold))
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button {
                    store.refresh()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .disabled(store.isLoading)
            }

            if store.isLoading && store.snapshot == nil {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 160)
            } else if let snapshot = store.snapshot {
                ScrollView {
                    VStack(alignment: .leading, spacing: 14) {
                        if let planType = snapshot.planType {
                            Text("Plan: \(planType.displayTitle)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        ForEach(snapshot.allGroups) { group in
                            RateLimitGroupView(group: group)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else {
                ContentUnavailableView(
                    "Rate limits unavailable",
                    systemImage: "gauge.with.dots.needle.50percent",
                    description: Text(store.errorMessage ?? "Sign in to Codex, then refresh.")
                )
                .frame(maxWidth: .infinity, minHeight: 180)
            }

            if let errorMessage = store.errorMessage, store.snapshot != nil {
                Text(errorMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(16)
    }

    private var subtitle: String {
        if let lastUpdated = store.lastUpdated {
            return "Updated \(lastUpdated.formatted(date: .omitted, time: .shortened))"
        }

        return "Uses your local Codex sign-in token."
    }
}

private struct RateLimitGroupView: View {
    var group: CodexRateLimitGroup

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(group.title)
                    .font(.headline)
                Spacer()
                Text(group.rateLimit.allowed == false || group.rateLimit.limitReached == true ? "Blocked" : "Allowed")
                    .font(.caption.weight(.medium))
                    .foregroundStyle(group.rateLimit.allowed == false || group.rateLimit.limitReached == true ? .red : .secondary)
            }

            if let shortWindow = group.shortWindow {
                RateLimitWindowRow(title: "Short Window", window: shortWindow)
            }

            if let weeklyWindow = group.weeklyWindow {
                RateLimitWindowRow(title: "Weekly", window: weeklyWindow)
            }
        }
        .padding(12)
        .rateLimitGlassSurface()
    }
}

private struct RateLimitWindowRow: View {
    var title: String
    var window: CodexRateLimitWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("\(title) · \(window.durationLabel)")
                    .font(.caption.weight(.medium))
                Spacer()
                Text("\(Int(window.remainingPercent.rounded()))% left")
                    .font(.caption)
                    .foregroundStyle(.primary)
            }

            ProgressView(value: window.clampedUsedPercent, total: 100)

            HStack {
                Text("\(Int(window.clampedUsedPercent.rounded()))% used")
                Spacer()
                if let resetDate = window.resetDate {
                    Text("Resets \(resetDate.formatted(date: .abbreviated, time: .shortened))")
                }
            }
            .font(.caption2)
            .foregroundStyle(.secondary)
        }
    }
}

private extension View {
    @ViewBuilder
    func rateLimitGlassSurface() -> some View {
        if #available(macOS 26.0, *) {
            self
                .glassEffect(.clear.interactive(), in: .rect(cornerRadius: 12))
                .overlay {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(Color.primary.opacity(0.16), lineWidth: 1)
                        .allowsHitTesting(false)
                }
        } else {
            self
                .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
        }
    }
}
