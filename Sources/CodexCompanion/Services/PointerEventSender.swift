import ApplicationServices
import CoreGraphics
import Darwin

enum PointerEventSender {
    static func leftClick(at point: CGPoint, flags: CGEventFlags = []) {
        let source = CGEventSource(stateID: .hidSystemState)
        let moved = CGEvent(
            mouseEventSource: source,
            mouseType: .mouseMoved,
            mouseCursorPosition: point,
            mouseButton: .left
        )
        moved?.flags = flags
        moved?.post(tap: .cghidEventTap)

        usleep(20_000)

        let down = CGEvent(
            mouseEventSource: source,
            mouseType: .leftMouseDown,
            mouseCursorPosition: point,
            mouseButton: .left
        )
        down?.flags = flags
        down?.post(tap: .cghidEventTap)

        usleep(20_000)

        let up = CGEvent(
            mouseEventSource: source,
            mouseType: .leftMouseUp,
            mouseCursorPosition: point,
            mouseButton: .left
        )
        up?.flags = flags
        up?.post(tap: .cghidEventTap)
    }
}
