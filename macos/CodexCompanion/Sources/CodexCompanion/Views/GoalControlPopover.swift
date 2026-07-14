import SwiftUI

struct GoalControlPopover: View {
    var state: CodexGoalControlState
    var isUpdating: Bool
    var errorMessage: String?
    var updateDraft: (String) -> Void
    var beginEditing: () -> Void
    var cancelEditing: () -> Void
    var save: () -> Void
    var resume: () -> Void
    var dismiss: () -> Void
    @FocusState private var objectiveFocused: Bool

    var body: some View {
        surface
            .onChange(of: state.isEditing) { _, isEditing in
                if isEditing {
                    DispatchQueue.main.async {
                        objectiveFocused = true
                    }
                }
            }
    }

    @ViewBuilder
    private var surface: some View {
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
                Image(systemName: state.status.symbolName)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(state.status.tint)

                VStack(alignment: .leading, spacing: 1) {
                    Text(state.taskTitle)
                        .font(.system(size: 12, weight: .semibold))
                        .lineLimit(1)
                    Text(state.status.displayTitle)
                        .font(.system(size: 10, weight: .medium))
                        .foregroundStyle(.secondary)
                }

                Spacer(minLength: 4)

                Button(action: dismiss) {
                    Image(systemName: "xmark")
                        .font(.system(size: 10, weight: .bold))
                        .frame(width: 26, height: 26)
                        .contentShape(Circle())
                }
                .buttonStyle(.plain)
                .help("Close goal controls")
            }

            if state.isEditing {
                TextField(
                    "Goal objective",
                    text: Binding(
                        get: { state.draftObjective },
                        set: updateDraft
                    ),
                    axis: .vertical
                )
                .textFieldStyle(.plain)
                .font(.system(size: 11, weight: .medium))
                .lineLimit(2...5)
                .padding(9)
                .background(Color.black.opacity(0.18), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(Color.primary.opacity(0.16), lineWidth: 1)
                }
                .focused($objectiveFocused)
            } else {
                Text(state.originalObjective)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(.primary.opacity(0.88))
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            if let errorMessage, !errorMessage.isEmpty {
                Text(errorMessage)
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(Color.red.opacity(0.92))
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 7) {
                if state.canResume {
                    GoalControlButton(
                        title: "Resume",
                        systemName: "play.fill",
                        isPrimary: true,
                        isDisabled: isUpdating,
                        action: resume
                    )
                }

                if state.isEditing {
                    GoalControlButton(
                        title: "Cancel",
                        systemName: "xmark",
                        isDisabled: isUpdating,
                        action: cancelEditing
                    )
                    GoalControlButton(
                        title: "Save",
                        systemName: "checkmark",
                        isPrimary: true,
                        isDisabled: isUpdating,
                        action: save
                    )
                } else if state.canEdit {
                    GoalControlButton(
                        title: "Edit",
                        systemName: "pencil",
                        isDisabled: isUpdating,
                        action: beginEditing
                    )
                }

                Spacer(minLength: 0)

                if isUpdating {
                    ProgressView()
                        .controlSize(.small)
                }
            }
        }
        .padding(12)
        .frame(width: 286, alignment: .leading)
    }
}

private struct GoalControlButton: View {
    var title: String
    var systemName: String
    var isPrimary = false
    var isDisabled = false
    var action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: systemName)
                .font(.system(size: 10, weight: .semibold))
                .padding(.horizontal, 9)
                .frame(height: 28)
                .background(
                    isPrimary ? Color.accentColor.opacity(0.28) : Color.primary.opacity(0.08),
                    in: Capsule()
                )
                .overlay {
                    Capsule()
                        .stroke(Color.primary.opacity(isPrimary ? 0.22 : 0.12), lineWidth: 1)
                }
        }
        .buttonStyle(.plain)
        .disabled(isDisabled)
        .opacity(isDisabled ? 0.55 : 1)
    }
}

private extension CodexGoalStatus {
    var displayTitle: String {
        switch self {
        case .active: return "Goal active"
        case .paused: return "Goal paused"
        case .blocked: return "Goal blocked"
        case .usageLimited: return "Waiting for usage"
        case .budgetLimited: return "Goal budget reached"
        case .complete: return "Goal complete"
        }
    }

    var symbolName: String {
        switch self {
        case .active: return "target"
        case .paused: return "pause.fill"
        case .blocked: return "exclamationmark.triangle.fill"
        case .usageLimited: return "hourglass"
        case .budgetLimited: return "gauge.with.dots.needle.50percent"
        case .complete: return "checkmark.circle.fill"
        }
    }

    var tint: Color {
        switch self {
        case .active: return .blue
        case .paused: return .yellow
        case .blocked: return .orange
        case .usageLimited, .budgetLimited: return .yellow
        case .complete: return .green
        }
    }
}
