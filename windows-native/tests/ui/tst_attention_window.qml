import QtQuick
import QtTest
import CodexCompanion

TestCase {
    id: testCase

    name: "AttentionWindow"
    when: windowShown

    QtObject {
        id: fakeAttentionModel

        property bool hasAttention: true
        property var attentionMessage: ({
            "kind": "attention",
            "title": "Psst, I need your approval.",
            "detail": "Approve the build command",
            "processTitle": "Windows parity",
            "processId": "task-1"
        })
    }

    Component {
        id: attentionWindowComponent

        AttentionWindow {
            attentionModel: fakeAttentionModel
        }
    }

    SignalSpy {
        id: openSpy
        signalName: "openRequested"
    }

    SignalSpy {
        id: dismissSpy
        signalName: "dismissRequested"
    }

    function init() {
        fakeAttentionModel.hasAttention = true
        fakeAttentionModel.attentionMessage = {
            "kind": "attention",
            "title": "Psst, I need your approval.",
            "detail": "Approve the build command",
            "processTitle": "Windows parity",
            "processId": "task-1"
        }
        openSpy.target = null
        dismissSpy.target = null
    }

    function createAttentionWindow() {
        const attentionWindow = createTemporaryObject(
            attentionWindowComponent,
            null)
        verify(attentionWindow)
        attentionWindow.visible = true
        tryCompare(attentionWindow, "visible", true)
        waitForRendering(attentionWindow.contentItem)
        return attentionWindow
    }

    function test_surface_matches_attention_contract() {
        const attentionWindow =
            createAttentionWindow()

        verify(attentionWindow.width >= 190)
        verify(attentionWindow.width <= 286)
        verify(attentionWindow.height >= 52)
        verify(attentionWindow.height <= 112)
        compare(
            attentionWindow.minimumWidth,
            attentionWindow.width)
        compare(
            attentionWindow.maximumWidth,
            attentionWindow.width)
        compare(
            attentionWindow.minimumHeight,
            attentionWindow.height)
        compare(
            attentionWindow.maximumHeight,
            attentionWindow.height)
        verify(
            (attentionWindow.flags
                & Qt.Tool) !== 0)
        verify(
            (attentionWindow.flags
                & Qt.FramelessWindowHint) !== 0)
        verify(
            (attentionWindow.flags
                & Qt.WindowStaysOnTopHint) !== 0)
        verify(
            (attentionWindow.flags
                & Qt.WindowDoesNotAcceptFocus) !== 0)
        verify(
            attentionWindow.nativeBackdropRegionEnabled
                !== undefined)
        compare(
            attentionWindow.nativeBackdropRegionEnabled,
            true)
        compare(
            attentionWindow.nativeBackdropRegionInsetLeft,
            0)
        compare(
            attentionWindow.nativeBackdropRegionInsetTop,
            0)
        compare(
            attentionWindow.nativeBackdropRegionInsetRight,
            0)
        compare(
            attentionWindow.nativeBackdropRegionInsetBottom,
            0)
        compare(
            attentionWindow.nativeBackdropRegionRadius,
            16)

        compare(
            attentionWindow.titleLabel.text,
            "Psst, I need your approval.")
        compare(
            attentionWindow.processLabel.text,
            "Windows parity")
        compare(
            attentionWindow.detailLabel.text,
            "Approve the build command")
        verify(
            attentionWindow.kindGlyph.text.length > 0)
        compare(
            attentionWindow.colorForKind("response"),
            CompanionTheme.textSecondary)
        compare(
            attentionWindow.colorForKind("goal"),
            CompanionTheme.goal)
    }

    function test_click_and_close_emit_reusable_actions() {
        const attentionWindow =
            createAttentionWindow()
        openSpy.target = attentionWindow
        dismissSpy.target = attentionWindow
        openSpy.clear()
        dismissSpy.clear()
        compare(
            attentionWindow.actionSurface.interactionId,
            "attention.open")

        mouseClick(attentionWindow.actionSurface)

        compare(openSpy.count, 1)
        compare(dismissSpy.count, 0)

        attentionWindow.close()

        tryCompare(dismissSpy, "count", 1)
        compare(attentionWindow.visible, true)
    }

    function test_message_updates_without_recreating_window() {
        const attentionWindow =
            createAttentionWindow()

        fakeAttentionModel.attentionMessage = {
            "kind": "completion",
            "title": "All done!",
            "detail": "The build passed",
            "processTitle": "Windows parity",
            "processId": "task-1"
        }

        tryCompare(
            attentionWindow.titleLabel,
            "text",
            "All done!")
        compare(
            attentionWindow.detailLabel.text,
            "The build passed")
        compare(
            attentionWindow.processLabel.text,
            "Windows parity")
    }
}
