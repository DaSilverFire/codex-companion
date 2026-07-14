import AppKit
import ApplicationServices
import Foundation

enum CodexVisibleApprovalButtonMatcher {
    static func matches(_ label: String, decision: CodexApprovalDecision) -> Bool {
        let normalized = label
            .folding(options: [.caseInsensitive, .diacriticInsensitive], locale: .current)
            .lowercased()
            .replacingOccurrences(of: "-", with: " ")
            .split(whereSeparator: { !$0.isLetter && !$0.isNumber })
            .joined(separator: " ")

        switch decision {
        case .approveOnce:
            return normalized == "approve"
                || normalized.contains("approve once")
                || normalized.contains("allow once")
        case .approveSimilarCommands:
            return normalized.contains("approve similar")
                || normalized.contains("approve for this session")
                || normalized.contains("allow for this session")
                || normalized.contains("always allow")
                || normalized.contains("commands like this")
        case .decline:
            return normalized.contains("tell codex something else")
                || normalized == "decline"
                || normalized == "deny"
        }
    }
}

enum CodexVisibleApprovalRoutePolicy {
    static func canPress(
        requestThreadID: String,
        currentVisibleThreadID: String?
    ) -> Bool {
        currentVisibleThreadID == requestThreadID
    }
}

struct CodexVisibleApprovalSender: Sendable {
    private static let maximumTraversalDepth = 36
    private static let maximumVisitedElements = 15_000

    func respond(
        to request: CodexPendingApproval,
        decision: CodexApprovalDecision
    ) async -> CodexAppServerApprovalOutcome {
        guard AXIsProcessTrusted() else {
            CodexSendLog.append(
                "visible approval unavailable missing accessibility thread=\(request.threadID)"
            )
            return .sharedDaemonUnavailable
        }

        let logReader = CodexDesktopApprovalLogReader()
        let previousThreadID = logReader.currentVisibleThreadID()
        if CodexVisibleApprovalRoutePolicy.canPress(
            requestThreadID: request.threadID,
            currentVisibleThreadID: previousThreadID
        ),
           await pressMatchingControl(decision: decision)
        {
            return await confirmedOutcome(for: request, decision: decision)
        }

        let didRequestRoute = await MainActor.run {
            openThreadInBackground(request.threadID)
        }
        guard didRequestRoute else {
            CodexSendLog.append(
                "visible approval could not open background route thread=\(request.threadID)"
            )
            return .failed
        }

        for delay in [150, 250, 400, 650, 900] {
            try? await Task.sleep(for: .milliseconds(delay))
            guard CodexVisibleApprovalRoutePolicy.canPress(
                requestThreadID: request.threadID,
                currentVisibleThreadID: logReader.currentVisibleThreadID()
            ) else {
                continue
            }
            if await pressMatchingControl(decision: decision) {
                let outcome = await confirmedOutcome(for: request, decision: decision)
                await restoreThreadIfNeeded(
                    previousThreadID,
                    targetThreadID: request.threadID
                )
                return outcome
            }
        }

        await restoreThreadIfNeeded(previousThreadID, targetThreadID: request.threadID)
        CodexSendLog.append(
            "visible approval control not found thread=\(request.threadID) decision=\(String(describing: decision))"
        )
        return .failed
    }

    private func restoreThreadIfNeeded(
        _ previousThreadID: String?,
        targetThreadID: String
    ) async {
        guard
            let previousThreadID,
            previousThreadID != targetThreadID
        else {
            return
        }

        try? await Task.sleep(for: .milliseconds(120))
        let restored = await MainActor.run {
            openThreadInBackground(previousThreadID)
        }
        CodexSendLog.append(
            "visible approval restored background route thread=\(previousThreadID) requested=\(restored)"
        )
    }

    private func confirmedOutcome(
        for request: CodexPendingApproval,
        decision: CodexApprovalDecision
    ) async -> CodexAppServerApprovalOutcome {
        for delay in [150, 300, 600, 1_000] {
            try? await Task.sleep(for: .milliseconds(delay))
            let pending = CodexDesktopApprovalLogReader().pendingApproval(for: request.threadID)
            if pending?.requestID != request.requestID {
                CodexSendLog.append(
                    "visible approval confirmed thread=\(request.threadID) request=\(request.requestID)"
                )
                return decision == .decline ? .declined : .approved
            }
        }

        CodexSendLog.append(
            "visible approval confirmation timed out thread=\(request.threadID) request=\(request.requestID)"
        )
        return .timedOut
    }

    @MainActor
    private func pressMatchingControl(decision: CodexApprovalDecision) async -> Bool {
        guard let app = runningChatGPTApp() else { return false }
        let appElement = AXUIElementCreateApplication(app.processIdentifier)

        if let control = matchingControl(in: appElement, decision: decision) {
            return press(control)
        }

        guard decision == .approveSimilarCommands,
              let menuTrigger = matchingControl(in: appElement, decision: .approveOnce),
              actionNames(for: menuTrigger).contains(kAXShowMenuAction)
        else {
            return false
        }

        let triggerLabel = searchableLabel(for: menuTrigger)
        let menuResult = AXUIElementPerformAction(
            menuTrigger,
            kAXShowMenuAction as CFString
        )
        CodexSendLog.append(
            "visible approval showed menu result=\(menuResult.rawValue) label=\(triggerLabel)"
        )
        guard menuResult == .success else { return false }

        for delay in [100, 180, 300] {
            try? await Task.sleep(for: .milliseconds(delay))
            if let menuItem = matchingControl(in: appElement, decision: decision) {
                return press(menuItem)
            }
        }
        return false
    }

    @MainActor
    private func matchingControl(
        in root: AXUIElement,
        decision: CodexApprovalDecision
    ) -> AXUIElement? {
        var visited = 0
        return matchingControl(
            in: root,
            decision: decision,
            depth: 0,
            visited: &visited
        )
    }

    @MainActor
    private func matchingControl(
        in element: AXUIElement,
        decision: CodexApprovalDecision,
        depth: Int,
        visited: inout Int
    ) -> AXUIElement? {
        guard
            depth <= Self.maximumTraversalDepth,
            visited < Self.maximumVisitedElements
        else {
            return nil
        }
        visited += 1

        let role = stringAttribute(element, kAXRoleAttribute)
        if (role == kAXButtonRole || role == kAXMenuItemRole),
           CodexVisibleApprovalButtonMatcher.matches(
               searchableLabel(for: element),
               decision: decision
           )
        {
            return element
        }

        for child in elementArrayAttribute(element, kAXChildrenAttribute) {
            if let match = matchingControl(
                in: child,
                decision: decision,
                depth: depth + 1,
                visited: &visited
            ) {
                return match
            }
        }
        return nil
    }

    @MainActor
    private func press(_ element: AXUIElement) -> Bool {
        let label = searchableLabel(for: element)
        let result = AXUIElementPerformAction(element, kAXPressAction as CFString)
        CodexSendLog.append(
            "visible approval pressed result=\(result.rawValue) label=\(label)"
        )
        return result == .success
    }

    @MainActor
    private func actionNames(for element: AXUIElement) -> [String] {
        var names: CFArray?
        guard AXUIElementCopyActionNames(element, &names) == .success else {
            return []
        }
        return names as? [String] ?? []
    }

    @MainActor
    private func searchableLabel(for element: AXUIElement) -> String {
        [
            stringAttribute(element, kAXTitleAttribute),
            stringAttribute(element, kAXDescriptionAttribute),
            stringAttribute(element, kAXHelpAttribute),
            stringAttribute(element, kAXValueAttribute),
        ]
        .compactMap { $0 }
        .joined(separator: " ")
    }

    @MainActor
    private func runningChatGPTApp() -> NSRunningApplication? {
        NSRunningApplication
            .runningApplications(withBundleIdentifier: "com.openai.codex")
            .first
    }

    @MainActor
    private func openThreadInBackground(_ threadID: String) -> Bool {
        guard
            let encodedThreadID = threadID.addingPercentEncoding(
                withAllowedCharacters: .urlPathAllowed
            ),
            let url = URL(string: "codex://threads/\(encodedThreadID)"),
            let appURL = runningChatGPTApp()?.bundleURL
                ?? WorkspacePaths.codexAppURLs.first(where: {
                    FileManager.default.fileExists(atPath: $0.path)
                })
        else {
            return false
        }

        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = false
        CodexSendLog.append("open background \(url.absoluteString)")
        NSWorkspace.shared.open(
            [url],
            withApplicationAt: appURL,
            configuration: configuration
        ) { _, error in
            if let error {
                CodexSendLog.append(
                    "background route open failed thread=\(threadID) error=\(error.localizedDescription)"
                )
            }
        }
        return true
    }

    @MainActor
    private func elementArrayAttribute(
        _ element: AXUIElement,
        _ attribute: String
    ) -> [AXUIElement] {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(
            element,
            attribute as CFString,
            &value
        ) == .success else {
            return []
        }
        return value as? [AXUIElement] ?? []
    }

    @MainActor
    private func stringAttribute(
        _ element: AXUIElement,
        _ attribute: String
    ) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(
            element,
            attribute as CFString,
            &value
        ) == .success else {
            return nil
        }
        return value as? String
    }
}
