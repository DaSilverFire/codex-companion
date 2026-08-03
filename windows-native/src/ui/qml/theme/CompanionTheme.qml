pragma Singleton

import QtQuick

QtObject {
    readonly property color window: "#101214"
    readonly property color micaChrome: "#b8101214"
    readonly property color windowsGlassChrome: "#a0080a0c"
    readonly property color materialChrome: micaChrome
    readonly property color surface: "#181b1e"
    readonly property color surfaceRaised: "#22262a"
    readonly property color border: "#383e43"
    readonly property color separator: "#2b3034"
    readonly property color textPrimary: "#f3f5f4"
    readonly property color textSecondary: "#bcc3c0"
    readonly property color textMuted: "#858e8a"
    readonly property color control: "#2a2f33"
    readonly property color controlHover: "#363c41"
    readonly property color controlPressed: "#454c52"
    readonly property color controlSelected: "#3a3f43"
    readonly property color glassControlFill:
        Qt.rgba(0, 0, 0, 0.18)
    readonly property color glassSelectedFill:
        Qt.rgba(1, 1, 1, 0.11)
    readonly property color glassBorder:
        Qt.rgba(1, 1, 1, 0.16)
    readonly property color glassSelectedBorder:
        Qt.rgba(1, 1, 1, 0.30)
    readonly property color glassStatusBorder:
        Qt.rgba(1, 1, 1, 0.18)
    readonly property color accent: "#e2e6e4"
    readonly property color accentMuted: "#747b78"
    readonly property color accentHover: "#f1f3f2"
    readonly property color accentPressed: "#bfc5c2"
    readonly property color accentText: "#101214"
    readonly property color info: "#297af5"
    readonly property color goal: "#635bdb"
    readonly property color success: "#65c88a"
    readonly property color warning: "#e2b45b"
    readonly property color danger: "#e27676"
    readonly property int radius: 8
    readonly property int gap: 8

    function chromeForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return windowsGlassChrome
        case "solid-black":
            return window
        default:
            return micaChrome
        }
    }

    function traySurfaceForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(0.48, 0.52, 0.56, 0.26)
        case "solid-black":
            return window
        default:
            return Qt.rgba(0.16, 0.20, 0.24, 0.42)
        }
    }

    function traySurfaceBorderForBackdrop(mode) {
        return mode === "solid-black"
            ? border
            : Qt.rgba(1, 1, 1, mode === "windows-glass"
                ? 0.22
                : 0.18)
    }

    function trayControlFillForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(0, 0, 0, 0.12)
        case "solid-black":
            return surface
        default:
            return glassControlFill
        }
    }

    function traySelectedControlFillForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(1, 1, 1, 0.08)
        case "solid-black":
            return surfaceRaised
        default:
            return glassSelectedFill
        }
    }

    function trayControlBorderForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(1, 1, 1, 0.22)
        case "solid-black":
            return border
        default:
            return glassBorder
        }
    }

    function traySelectedControlBorderForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(1, 1, 1, 0.34)
        case "solid-black":
            return accentMuted
        default:
            return glassSelectedBorder
        }
    }

    function trayStatusBorderForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(1, 1, 1, 0.26)
        case "solid-black":
            return border
        default:
            return glassStatusBorder
        }
    }

    function popoverSurfaceForBackdrop(mode) {
        switch (mode) {
        case "windows-glass":
            return Qt.rgba(0.02, 0.025, 0.03, 0.78)
        case "solid-black":
            return surfaceRaised
        default:
            return Qt.rgba(0.04, 0.045, 0.05, 0.88)
        }
    }
}
