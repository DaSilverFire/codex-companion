import ApplicationServices
import Foundation

enum KeyboardEventSender {
    static func optionSpace() {
        postKey(49, flags: .maskAlternate)
    }

    static func commandV() {
        postKey(9, flags: .maskCommand)
    }

    static func commandA() {
        postKey(0, flags: .maskCommand)
    }

    static func returnKey() {
        postKey(36)
    }

    static func commandReturnKey() {
        postKey(36, flags: .maskCommand)
    }

    static func escape() {
        postKey(53)
    }

    static func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) {
        let source = CGEventSource(stateID: .hidSystemState)
        let keyDown = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true)
        let keyUp = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false)

        keyDown?.flags = flags
        keyUp?.flags = flags
        keyDown?.post(tap: .cghidEventTap)
        keyUp?.post(tap: .cghidEventTap)
    }
}
