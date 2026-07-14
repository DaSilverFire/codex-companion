import AppKit
import ApplicationServices
import Darwin
import Foundation

enum CodexSendAction: Equatable, Sendable {
    case reply
    case steer

    var logName: String {
        switch self {
        case .reply:
            return "reply"
        case .steer:
            return "steer"
        }
    }
}

struct CodexVisibleReplySender {
    private static let lastAccessibilityPromptKey = "CodexVisibleReplySender.lastAccessibilityPrompt"

    static var isAccessibilityTrusted: Bool {
        AXIsProcessTrusted()
    }

    static func requestAccessibilityTrustNow() {
        guard !isAccessibilityTrusted else { return }
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
        AXIsProcessTrustedWithOptions(options as CFDictionary)
        UserDefaults.standard.set(Date(), forKey: lastAccessibilityPromptKey)
        logSend("requested accessibility trust")
    }

    static func openAccessibilitySettings() {
        let urls = [
            "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_Accessibility",
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility",
            "x-apple.systempreferences:com.apple.preference.security",
        ]

        for rawURL in urls {
            guard let url = URL(string: rawURL) else { continue }
            if NSWorkspace.shared.open(url) {
                logSend("opened accessibility settings")
                return
            }
        }
    }

    func submit(prompt: String, threadID: String, cwd: String? = nil, action: CodexSendAction = .reply) -> Bool {
        let trimmedPrompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        let trimmedThreadID = threadID.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedPrompt.isEmpty, !trimmedThreadID.isEmpty else { return false }

        openCodexThread(trimmedThreadID)
        let isTrusted = Self.isAccessibilityTrusted
        Self.logSend("submit thread=\(trimmedThreadID) action=\(action.logName) trusted=\(isTrusted)")
        copyToPasteboard(trimmedPrompt)
        if !isTrusted {
            Self.requestAccessibilityTrustIfNeeded()
            Self.logSend("blocked missing accessibility permission action=\(action.logName); prompt copied and trust requested")
            return false
        }
        scheduleFocusedSubmit(prompt: trimmedPrompt, action: action)
        return true
    }

    func openThread(_ threadID: String?) {
        guard
            let threadID = threadID?.trimmingCharacters(in: .whitespacesAndNewlines),
            !threadID.isEmpty
        else {
            openCodexApp()
            return
        }

        openCodexThread(threadID)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            Self.activateCodex()
        }
    }

    private func openCodexThread(_ threadID: String) {
        if
            let encodedThreadID = threadID.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
            let url = URL(string: "codex://threads/\(encodedThreadID)")
        {
            Self.logSend("open \(url.absoluteString)")
            if let appURL = Self.runningCodexApp()?.bundleURL
                ?? WorkspacePaths.codexAppURLs.first(where: { FileManager.default.fileExists(atPath: $0.path) }) {
                let configuration = NSWorkspace.OpenConfiguration()
                configuration.activates = true
                NSWorkspace.shared.open([url], withApplicationAt: appURL, configuration: configuration) { _, _ in }
            } else {
                NSWorkspace.shared.open(url)
            }
            return
        }

        openCodexApp()
    }

    private func openCodexApp() {
        if let appURL = WorkspacePaths.codexAppURLs.first(where: { FileManager.default.fileExists(atPath: $0.path) }) {
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = true
            NSWorkspace.shared.openApplication(at: appURL, configuration: configuration) { _, _ in }
            return
        }

        NSWorkspace.shared.open(URL(string: "https://chatgpt.com/codex")!)
    }

    private func scheduleFocusedSubmit(prompt: String, action: CodexSendAction) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.90) {
            Self.logSend("focus pass 1 activated=\(Self.activateCodex())")
            Self.raiseFrontCodexWindow()
            Self.focusLikelyComposer()
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 1.80) {
            Self.logSend("focus pass 2 activated=\(Self.activateCodex())")
            Self.raiseFrontCodexWindow()
            Self.focusLikelyComposer()
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 2.35) {
            Self.logSend("click pass activated=\(Self.activateCodex())")
            Self.raiseFrontCodexWindow()
            Self.clickLikelyComposer()
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 2.85) {
            Self.logSend("paste pass activated=\(Self.activateCodex())")
            Self.raiseFrontCodexWindow()
            Self.focusLikelyComposer()
            if Self.fillLikelyComposer(with: prompt) {
                Self.logSend("filled composer with accessibility")
            } else {
                KeyboardEventSender.commandA()
                usleep(45_000)
                KeyboardEventSender.commandV()
                Self.logSend("pasted")
            }
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 3.25) {
            Self.raiseFrontCodexWindow()
            switch action {
            case .reply:
                Self.clickLikelySubmitButton()
                Self.logSend("clicked reply submit")
            case .steer:
                KeyboardEventSender.commandReturnKey()
                Self.logSend("command-returned steer")
            }
        }
    }

    private func copyToPasteboard(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }

    @discardableResult
    private static func activateCodex() -> Bool {
        guard let app = runningCodexApp() else { return false }
        return app.activate(options: [.activateAllWindows])
    }

    private static func runningCodexApp() -> NSRunningApplication? {
        let apps = NSRunningApplication.runningApplications(withBundleIdentifier: "com.openai.codex")
        return apps.sorted { lhs, rhs in
            appPreferenceRank(lhs.bundleURL) < appPreferenceRank(rhs.bundleURL)
        }
        .first
    }

    private static func appPreferenceRank(_ bundleURL: URL?) -> Int {
        guard let path = bundleURL?.standardizedFileURL.path else { return Int.max }
        return WorkspacePaths.codexAppURLs
            .map { $0.standardizedFileURL.path }
            .firstIndex(of: path) ?? Int.max
    }

    @discardableResult
    private static func focusLikelyComposer() -> Bool {
        guard AXIsProcessTrusted(), let app = runningCodexApp() else { return false }

        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        guard let candidate = findLikelyComposerCandidate(in: appElement) else {
            if let focused = axElementAttribute(appElement, kAXFocusedUIElementAttribute),
               isTextInput(focused),
               composerScore(for: focused) >= 55 {
                AXUIElementSetAttributeValue(focused, kAXFocusedAttribute as CFString, kCFBooleanTrue)
                Self.logSend("focused existing composer score=\(composerScore(for: focused))")
                return true
            }
            return false
        }
        AXUIElementSetAttributeValue(candidate.element, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        Self.logSend("focused composer score=\(candidate.score) frame=\(Self.describe(candidate.frame))")
        return true
    }

    private static func raiseFrontCodexWindow() {
        guard AXIsProcessTrusted(), let app = runningCodexApp() else { return }
        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        if let focusedWindow = axElementAttribute(appElement, kAXFocusedWindowAttribute) {
            AXUIElementPerformAction(focusedWindow, kAXRaiseAction as CFString)
            return
        }
        if let window = axElementArrayAttribute(appElement, kAXWindowsAttribute).first {
            AXUIElementPerformAction(window, kAXRaiseAction as CFString)
        }
    }

    private static func clickLikelyComposer() {
        if AXIsProcessTrusted(), let app = runningCodexApp() {
            let appElement = AXUIElementCreateApplication(app.processIdentifier)
            if let candidate = findLikelyComposerCandidate(in: appElement), candidate.frame.width > 10, candidate.frame.height > 10 {
                let point = CGPoint(x: candidate.frame.midX, y: candidate.frame.midY)
                Self.logSend("click composer point=\(Int(point.x)),\(Int(point.y)) score=\(candidate.score) frame=\(Self.describe(candidate.frame))")
                PointerEventSender.leftClick(at: point)
                return
            }
        }

        if AXIsProcessTrusted(), let frame = frontWindowFrameFromAccessibility() {
            let point = CGPoint(
                x: frame.midX,
                y: frame.maxY - min(78, max(54, frame.height * 0.08))
            )
            Self.logSend("click ax point=\(Int(point.x)),\(Int(point.y)) frame=\(Int(frame.width))x\(Int(frame.height))")
            PointerEventSender.leftClick(at: point)
            return
        }

        guard let bounds = frontWindowBoundsFromCoreGraphics() else { return }
        let point = CGPoint(
            x: bounds.midX,
            y: bounds.maxY - min(78, max(54, bounds.height * 0.08))
        )
        Self.logSend("click cg point=\(Int(point.x)),\(Int(point.y)) frame=\(Int(bounds.width))x\(Int(bounds.height))")
        PointerEventSender.leftClick(at: point)
    }

    private static func clickLikelySubmitButton() {
        if AXIsProcessTrusted(), let frame = frontWindowFrameFromAccessibility() {
            clickSubmitButton(in: frame, source: "ax")
            return
        }

        guard let frame = frontWindowBoundsFromCoreGraphics() else { return }
        clickSubmitButton(in: frame, source: "cg")
    }

    private static func clickSubmitButton(in frame: CGRect, source: String) {
        let visibleComposerWidth = min(frame.width, 1_180)
        let x = min(
            frame.maxX - 64,
            frame.minX + max(280, visibleComposerWidth - 86)
        )
        let point = CGPoint(
            x: x,
            y: frame.maxY - min(38, max(28, frame.height * 0.03))
        )
        Self.logSend("click submit \(source) point=\(Int(point.x)),\(Int(point.y)) frame=\(Int(frame.width))x\(Int(frame.height))")
        PointerEventSender.leftClick(at: point)
    }

    private static func findLikelyComposer(in appElement: AXUIElement) -> AXUIElement? {
        findLikelyComposerCandidate(in: appElement)?.element
    }

    private static func findLikelyComposerCandidate(in appElement: AXUIElement) -> ComposerCandidate? {
        let roots = axElementArrayAttribute(appElement, kAXWindowsAttribute)
        var candidates: [ComposerCandidate] = []
        var visited = 0

        for root in roots {
            collectTextInputs(from: root, depth: 0, visited: &visited, candidates: &candidates)
        }

        return candidates.sorted { lhs, rhs in
            if lhs.score != rhs.score {
                return lhs.score > rhs.score
            }
            return lhs.frame.minY > rhs.frame.minY
        }.first
    }

    @discardableResult
    private static func fillLikelyComposer(with text: String) -> Bool {
        guard AXIsProcessTrusted(), let app = runningCodexApp() else { return false }
        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        guard let candidate = findLikelyComposerCandidate(in: appElement) else {
            Self.logSend("no composer candidate for fill")
            return false
        }

        AXUIElementSetAttributeValue(candidate.element, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        let result = AXUIElementSetAttributeValue(candidate.element, kAXValueAttribute as CFString, text as CFTypeRef)
        Self.logSend("fill composer result=\(result.rawValue) score=\(candidate.score) frame=\(Self.describe(candidate.frame))")
        return result == .success
    }

    private static func frontWindowFrameFromAccessibility() -> CGRect? {
        guard let app = runningCodexApp() else { return nil }
        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        if let focusedWindow = axElementAttribute(appElement, kAXFocusedWindowAttribute) {
            let focusedFrame = frame(of: focusedWindow)
            if focusedFrame.width > 500, focusedFrame.height > 300 {
                return focusedFrame
            }
        }

        return axElementArrayAttribute(appElement, kAXWindowsAttribute)
            .map(frame(of:))
            .filter { $0.width > 500 && $0.height > 300 }
            .max { lhs, rhs in
                lhs.width * lhs.height < rhs.width * rhs.height
            }
    }

    private static func frontWindowBoundsFromCoreGraphics() -> CGRect? {
        guard
            let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
                as? [[String: Any]]
        else {
            return nil
        }

        return windows.compactMap { info -> CGRect? in
            guard
                info[kCGWindowOwnerName as String] as? String == "Codex",
                (info[kCGWindowLayer as String] as? Int) == 0,
                let alpha = info[kCGWindowAlpha as String] as? Double,
                alpha > 0.05,
                let bounds = info[kCGWindowBounds as String] as? [String: Any],
                let x = bounds["X"] as? CGFloat,
                let y = bounds["Y"] as? CGFloat,
                let width = bounds["Width"] as? CGFloat,
                let height = bounds["Height"] as? CGFloat,
                width > 500,
                height > 300
            else {
                return nil
            }

            return CGRect(x: x, y: y, width: width, height: height)
        }
        .max { lhs, rhs in
            lhs.width * lhs.height < rhs.width * rhs.height
        }
    }

    private static func collectTextInputs(
        from element: AXUIElement,
        depth: Int,
        visited: inout Int,
        candidates: inout [ComposerCandidate]
    ) {
        guard depth <= 14, visited < 900 else { return }
        visited += 1

        if isTextInput(element) {
            candidates.append(ComposerCandidate(
                element: element,
                frame: frame(of: element),
                score: composerScore(for: element)
            ))
        }

        for child in axElementArrayAttribute(element, kAXChildrenAttribute) {
            collectTextInputs(from: child, depth: depth + 1, visited: &visited, candidates: &candidates)
        }
    }

    private static func isTextInput(_ element: AXUIElement) -> Bool {
        guard let role = stringAttribute(element, kAXRoleAttribute) else { return false }
        return role == kAXTextAreaRole || role == kAXTextFieldRole || role == "AXComboBox"
    }

    private static func composerScore(for element: AXUIElement) -> Int {
        let labels = [
            stringAttribute(element, kAXTitleAttribute),
            stringAttribute(element, kAXDescriptionAttribute),
            stringAttribute(element, kAXPlaceholderValueAttribute),
            stringAttribute(element, kAXHelpAttribute),
            stringAttribute(element, kAXValueAttribute),
        ]
        .compactMap { $0?.lowercased() }
        .joined(separator: " ")

        var score = 0
        if labels.contains("follow-up") || labels.contains("follow up") { score += 80 }
        if labels.contains("ask for follow") { score += 70 }
        if labels.contains("message") { score += 40 }
        if labels.contains("ask") { score += 25 }
        if stringAttribute(element, kAXRoleAttribute) == kAXTextAreaRole { score += 20 }

        let elementFrame = frame(of: element)
        if elementFrame.width > 250 { score += 10 }
        if elementFrame.height > 20 { score += 5 }
        return score
    }

    private static func axElementAttribute(_ element: AXUIElement, _ attribute: String) -> AXUIElement? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success else { return nil }
        guard let value else { return nil }
        return (value as! AXUIElement)
    }

    private static func axElementArrayAttribute(_ element: AXUIElement, _ attribute: String) -> [AXUIElement] {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success else { return [] }
        return value as? [AXUIElement] ?? []
    }

    private static func stringAttribute(_ element: AXUIElement, _ attribute: String) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, attribute as CFString, &value) == .success else { return nil }
        return value as? String
    }

    private static func frame(of element: AXUIElement) -> CGRect {
        var positionValue: CFTypeRef?
        var sizeValue: CFTypeRef?
        var position = CGPoint.zero
        var size = CGSize.zero

        if AXUIElementCopyAttributeValue(element, kAXPositionAttribute as CFString, &positionValue) == .success,
           let positionValue,
           CFGetTypeID(positionValue) == AXValueGetTypeID() {
            AXValueGetValue((positionValue as! AXValue), .cgPoint, &position)
        }

        if AXUIElementCopyAttributeValue(element, kAXSizeAttribute as CFString, &sizeValue) == .success,
           let sizeValue,
           CFGetTypeID(sizeValue) == AXValueGetTypeID() {
            AXValueGetValue((sizeValue as! AXValue), .cgSize, &size)
        }

        return CGRect(origin: position, size: size)
    }

    private static func describe(_ frame: CGRect) -> String {
        "\(Int(frame.origin.x)),\(Int(frame.origin.y)),\(Int(frame.width))x\(Int(frame.height))"
    }

    private static func logSend(_ message: String) {
        let directory = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Logs", isDirectory: true)
            .appendingPathComponent("CodexCompanion", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent("send.log")
        let line = "\(ISO8601DateFormatter().string(from: Date())) \(message)\n"
        guard let data = line.data(using: .utf8) else { return }

        if FileManager.default.fileExists(atPath: url.path),
           let handle = try? FileHandle(forWritingTo: url) {
            defer {
                try? handle.close()
            }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
            return
        }

        try? data.write(to: url, options: .atomic)
    }

    private static func requestAccessibilityTrustIfNeeded() {
        guard !isAccessibilityTrusted else { return }
        guard shouldPromptForAccessibilityPermission else { return }
        requestAccessibilityTrustNow()
    }

    private static var shouldPromptForAccessibilityPermission: Bool {
        guard let lastPrompt = UserDefaults.standard.object(forKey: lastAccessibilityPromptKey) as? Date else {
            return true
        }
        return Date().timeIntervalSince(lastPrompt) > 30 * 60
    }
}

private struct ComposerCandidate {
    var element: AXUIElement
    var frame: CGRect
    var score: Int
}
