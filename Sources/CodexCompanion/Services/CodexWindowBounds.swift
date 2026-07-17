import AppKit
import CoreGraphics

enum CodexWindowBounds {
    static func preferredFrame(fallbackScreen: NSScreen?) -> NSRect? {
        codexMainWindowFrame() ?? fallbackScreen?.visibleFrame
    }

    private static func codexMainWindowFrame() -> NSRect? {
        guard
            let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
                as? [[String: Any]]
        else {
            return nil
        }

        let candidates = windows.compactMap { info -> NSRect? in
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
                width >= 500,
                height >= 400
            else {
                return nil
            }

            return appKitFrameFromCGWindowBounds(x: x, y: y, width: width, height: height)
        }

        return candidates.max { left, right in
            left.width * left.height < right.width * right.height
        }?.insetBy(dx: 10, dy: 10)
    }

    private static func appKitFrameFromCGWindowBounds(
        x: CGFloat,
        y: CGFloat,
        width: CGFloat,
        height: CGFloat
    ) -> NSRect {
        let cgRect = NSRect(x: x, y: y, width: width, height: height)
        let screen = NSScreen.screens.first { screen in
            flippedFrame(for: screen).intersects(cgRect)
        } ?? NSScreen.main

        guard let screen else {
            return NSRect(x: x, y: y, width: width, height: height)
        }

        return NSRect(
            x: x,
            y: screen.frame.maxY - y - height,
            width: width,
            height: height
        )
    }

    private static func flippedFrame(for screen: NSScreen) -> NSRect {
        guard let main = NSScreen.main else { return screen.frame }
        return NSRect(
            x: screen.frame.minX,
            y: main.frame.maxY - screen.frame.maxY,
            width: screen.frame.width,
            height: screen.frame.height
        )
    }
}
