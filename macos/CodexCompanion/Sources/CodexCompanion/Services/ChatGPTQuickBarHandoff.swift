import AppKit
import ApplicationServices
import Foundation

@MainActor
final class ChatGPTQuickBarHandoff: ObservableObject {
    @Published private(set) var status = "ChatGPT app handoff ready."
    @Published private(set) var isOpen = false

    private let bundleIdentifier = "com.openai.chat"
    private let appURL = URL(fileURLWithPath: "/Applications/ChatGPT.app")
    private var hostFrame: NSRect?
    private var pendingOpenWorkItem: DispatchWorkItem?
    private var pendingPositionWorkItem: DispatchWorkItem?
    private let lastAccessibilityPromptKey = "lastAccessibilityPermissionPrompt"

    func open(prompt: String? = nil, model: ChatGPTModel, submit: Bool = false) {
        isOpen = true
        status = "Opening ChatGPT quick bar..."
        requestAccessibilityTrustIfNeeded()

        let handoffPrompt = prompt.flatMap { trimmedPrompt($0, model: model) }
        if let handoffPrompt {
            copyToPasteboard(handoffPrompt)
        }

        openChatGPTApp()
        pendingOpenWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            Task { @MainActor in
                self?.toggleQuickBar()
                self?.positionQuickBar(after: 0.35)
                if handoffPrompt != nil {
                    self?.pastePrompt(after: 0.48, submit: submit)
                }
            }
        }
        pendingOpenWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.55, execute: workItem)

        status = handoffPrompt == nil
            ? "ChatGPT quick bar is attached to the Companion menu."
            : "Sent prompt to the ChatGPT app quick bar."
    }

    func close() {
        guard isOpen else { return }
        isOpen = false
        status = "ChatGPT app handoff closed."
        pendingOpenWorkItem?.cancel()
        pendingPositionWorkItem?.cancel()
        runningChatGPTApp()?.activate(options: [.activateAllWindows])
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.06) {
            KeyboardEventSender.escape()
        }
    }

    func updateHostFrame(_ frame: NSRect?) {
        hostFrame = frame
        guard isOpen else { return }
        positionQuickBar(after: 0.02)
    }

    private func openChatGPTApp() {
        guard FileManager.default.fileExists(atPath: appURL.path) else {
            status = "Install the ChatGPT app to use account handoff."
            return
        }

        if runningChatGPTApp() == nil {
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.activates = true
            NSWorkspace.shared.openApplication(at: appURL, configuration: configuration) { [weak self] _, error in
                if let error {
                    Task { @MainActor in
                        self?.status = "Could not open ChatGPT: \(error.localizedDescription)"
                    }
                }
            }
        } else {
            runningChatGPTApp()?.activate(options: [.activateAllWindows])
        }
    }

    private func runningChatGPTApp() -> NSRunningApplication? {
        NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).first
    }

    private func toggleQuickBar() {
        KeyboardEventSender.optionSpace()
    }

    private func pastePrompt(after delay: TimeInterval, submit: Bool) {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            KeyboardEventSender.commandV()
            if submit {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) {
                    KeyboardEventSender.returnKey()
                }
            }
        }
    }

    private func positionQuickBar(after delay: TimeInterval = 0) {
        pendingPositionWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            Task { @MainActor in
                self?.positionQuickBarNow()
            }
        }
        pendingPositionWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: workItem)
    }

    private func positionQuickBarNow() {
        guard let frame = quickBarFrame else { return }
        guard AXIsProcessTrusted() else {
            requestAccessibilityTrustIfNeeded()
            return
        }
        guard let window = chatGPTWindow() else {
            status = "Waiting for the ChatGPT quick bar window. Click ChatGPT again if it did not open."
            return
        }

        var position = accessibilityPosition(for: frame)
        var size = CGSize(width: frame.width, height: frame.height)
        guard
            let positionValue = AXValueCreate(.cgPoint, &position),
            let sizeValue = AXValueCreate(.cgSize, &size)
        else {
            return
        }

        let sizeStatus = AXUIElementSetAttributeValue(window, kAXSizeAttribute as CFString, sizeValue)
        let positionStatus = AXUIElementSetAttributeValue(window, kAXPositionAttribute as CFString, positionValue)
        if sizeStatus == .success || positionStatus == .success {
            status = "ChatGPT quick bar is attached. Type in the ChatGPT window."
        } else {
            status = "ChatGPT is open, but macOS did not allow Companion to move its window."
        }
    }

    private var quickBarFrame: NSRect? {
        guard let hostFrame else { return nil }
        let inset = PetWindowMetrics.chatGPTQuickBarInset
        let size = CGSize(
            width: hostFrame.width - inset * 2,
            height: PetWindowMetrics.chatGPTQuickBarHeight
        )
        return NSRect(
            x: hostFrame.minX + inset,
            y: hostFrame.minY + PetWindowMetrics.chatGPTQuickBarBottomOffset,
            width: size.width,
            height: size.height
        )
    }

    private func accessibilityPosition(for frame: NSRect) -> CGPoint {
        let screen = NSScreen.screens.first { $0.frame.intersects(frame) } ?? NSScreen.main
        let screenMaxY = screen?.frame.maxY ?? frame.maxY
        return CGPoint(
            x: frame.minX.rounded(),
            y: (screenMaxY - frame.maxY).rounded()
        )
    }

    private func chatGPTWindow() -> AXUIElement? {
        guard let app = runningChatGPTApp() else {
            return nil
        }

        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        var focusedWindowRef: CFTypeRef?
        if AXUIElementCopyAttributeValue(appElement, kAXFocusedWindowAttribute as CFString, &focusedWindowRef) == .success,
           let focusedWindow = focusedWindowRef {
            return (focusedWindow as! AXUIElement)
        }

        var windowsRef: CFTypeRef?
        guard
            AXUIElementCopyAttributeValue(appElement, kAXWindowsAttribute as CFString, &windowsRef) == .success,
            let windows = windowsRef as? [AXUIElement]
        else {
            return nil
        }
        return windows.first
    }

    private func trimmedPrompt(_ prompt: String, model: ChatGPTModel) -> String? {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        return """
        Use \(model.title) for this response if it is available.

        \(trimmed)
        """
    }

    private func copyToPasteboard(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }

    private func requestAccessibilityTrustIfNeeded() {
        guard !AXIsProcessTrusted() else {
            status = "Accessibility is enabled. Waiting for the ChatGPT quick bar window."
            return
        }
        if shouldPromptForAccessibilityPermission {
            let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true]
            AXIsProcessTrustedWithOptions(options as CFDictionary)
            UserDefaults.standard.set(Date(), forKey: lastAccessibilityPromptKey)
        }
        status = "Allow Codex Companion in System Settings > Privacy & Security > Accessibility."
    }

    private var shouldPromptForAccessibilityPermission: Bool {
        guard let lastPrompt = UserDefaults.standard.object(forKey: lastAccessibilityPromptKey) as? Date else {
            return true
        }
        return Date().timeIntervalSince(lastPrompt) > 30 * 60
    }
}
