import AppKit
import Foundation

struct PromptRouter {
    private let codexSender = CodexVisibleReplySender()

    func route(
        prompt: String,
        mode: RouteMode,
        history: RouteHistoryStore,
        codexThreadID: String? = nil,
        chatGPTModel: ChatGPTModel = .gpt55
    ) -> RouteResult {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let destination = destination(for: trimmed, mode: mode)

        if destination == .codex {
            if !trimmed.isEmpty, let codexThreadID, !codexThreadID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                let didSend = codexSender.submit(prompt: trimmed, threadID: codexThreadID, cwd: nil, action: .reply)
                history.add(prompt: trimmed, destination: destination)
                return RouteResult(
                    destination: destination,
                    message: didSend
                        ? handoffMessage(for: destination, chatGPTModel: chatGPTModel)
                        : codexAccessibilityMessage(for: .reply),
                    succeeded: didSend
                )
            } else {
                codexSender.openThread(codexThreadID)
            }
        }

        history.add(prompt: trimmed, destination: destination)
        return RouteResult(
            destination: destination,
            message: trimmed.isEmpty
                ? openMessage(for: destination, chatGPTModel: chatGPTModel)
                : handoffMessage(for: destination, chatGPTModel: chatGPTModel)
        )
    }

    func continueCodex(
        prompt: String,
        history: RouteHistoryStore,
        threadID: String? = nil,
        cwd: String? = nil,
        action: CodexSendAction = .reply
    ) -> RouteResult {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            codexSender.openThread(threadID)
            return RouteResult(
                destination: .codex,
                message: "Opened Codex to continue."
            )
        }

        guard let threadID, !threadID.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            copyToPasteboard(trimmed)
            codexSender.openThread(nil)
            return RouteResult(
                destination: .codex,
                message: "Pick Reply or Steer on a Codex process first.",
                succeeded: false
            )
        }

        let didSend = codexSender.submit(prompt: trimmed, threadID: threadID, cwd: cwd, action: action)

        if didSend {
            history.add(prompt: trimmed, destination: .codex)
            return RouteResult(
                destination: .codex,
                message: action == .steer
                    ? "Sent steer to the Codex thread."
                    : "Sent reply to the Codex thread."
            )
        }

        return RouteResult(
            destination: .codex,
            message: codexAccessibilityMessage(for: action),
            succeeded: false
        )
    }

    func openCodexApp() {
        codexSender.openThread(nil)
    }

    func openCodexThread(_ threadID: String?) {
        codexSender.openThread(threadID)
    }

    private func destination(for prompt: String, mode: RouteMode) -> RouteDestination {
        switch mode {
        case .chatGPT:
            return .chatGPT
        case .codex:
            return .codex
        case .smart:
            return looksLikeCodingTask(prompt) ? .codex : .chatGPT
        }
    }

    private func looksLikeCodingTask(_ prompt: String) -> Bool {
        let lower = prompt.lowercased()
        let keywords = [
            "repo", "repository", "code", "file", "folder", "project", "bug", "fix",
            "test", "build", "xcode", "swift", "python", "javascript", "typescript",
            "terminal", "command", "error", "stack trace", "crash", "commit", "git",
            "pr ", "pull request", "refactor", "compile", "package.swift", "npm",
            "vite", "react", "api", "database", "migration",
        ]

        if keywords.contains(where: lower.contains) {
            return true
        }

        return lower.contains("/") || lower.contains(".swift") || lower.contains(".py") || lower.contains(".ts")
    }

    private func copyToPasteboard(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }

    private func openMessage(for destination: RouteDestination, chatGPTModel: ChatGPTModel) -> String {
        switch destination {
        case .chatGPT:
            return "Ask inside the Companion menu with \(chatGPTModel.title)."
        case .codex:
            return "Opened Codex."
        }
    }

    private func handoffMessage(for destination: RouteDestination, chatGPTModel: ChatGPTModel) -> String {
        switch destination {
        case .chatGPT:
            return "Asked \(chatGPTModel.title) inside Companion."
        case .codex:
            return "Sent prompt to Codex."
        }
    }

    private func codexAccessibilityMessage(for action: CodexSendAction = .reply) -> String {
        let label = action == .steer ? "Steer" : "Reply"
        return "Codex blocked \(label) because Companion is not Accessibility-trusted. I copied the prompt and opened the right thread."
    }
}
