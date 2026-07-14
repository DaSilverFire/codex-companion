import SwiftUI

struct HandoffView: View {
    @ObservedObject var model: CompanionAppModel

    var body: some View {
        VStack(spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Handoff")
                    .font(.title3.weight(.semibold))
                Text(model.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            TextEditor(text: $model.prompt)
                .font(.body)
                .scrollContentBackground(.hidden)
                .padding(8)
                .frame(height: 116)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                .overlay {
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(.separator.opacity(0.45))
                }

            Picker("Route", selection: $model.routeMode) {
                ForEach(model.selectableRouteModes) { mode in
                    Text(mode.title).tag(mode)
                }
            }
            .pickerStyle(.segmented)

            if !model.isCodexOnlyMode && model.routeMode == .chatGPT {
                Picker("ChatGPT model", selection: $model.selectedChatGPTModel) {
                    ForEach(ChatGPTModel.allCases) { chatGPTModel in
                        Text(chatGPTModel.title).tag(chatGPTModel)
                    }
                }
                .pickerStyle(.menu)
            }

            HStack(spacing: 8) {
                Button {
                    model.sendPrompt()
                } label: {
                    Label("Send", systemImage: "arrow.up.forward.app")
                }
                .buttonStyle(.borderedProminent)

                if !model.isCodexOnlyMode {
                    Button {
                        model.sendPrompt(mode: .chatGPT)
                    } label: {
                        Label("ChatGPT", systemImage: "bubble.left.and.text.bubble.right")
                    }
                }

                Button {
                    model.continueCodex()
                } label: {
                    Label("Codex", systemImage: "hammer")
                }
            }

            recentHandoffs
        }
        .padding(16)
    }

    private var recentHandoffs: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Recent Handoffs")
                .font(.headline)

            if model.historyStore.items.isEmpty {
                Text("Sent prompts will appear here.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                VStack(spacing: 6) {
                    ForEach(model.historyStore.items.prefix(5)) { item in
                        Button {
                            model.prompt = item.prompt
                            model.sendPrompt(mode: item.destination == .codex ? .codex : .chatGPT)
                        } label: {
                            HStack {
                                Image(systemName: item.destination == .codex ? "hammer" : "bubble.left")
                                    .foregroundStyle(.secondary)
                                    .frame(width: 18)
                                Text(item.prompt.clipped(54))
                                    .lineLimit(1)
                                Spacer()
                                Text(item.destination.title)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .padding(10)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}
