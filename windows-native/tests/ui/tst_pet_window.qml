import QtQuick
import QtTest
import QtQuick.Controls.Basic
import CodexCompanion

TestCase {
    id: testCase

    name: "PetWindow"
    when: windowShown

    QtObject {
        id: fakeModel

        property bool controlsVisible: true
        property bool controlsFollowHover: false
        property bool pointerHovered: false
        property bool controlsHovered: false
        property bool dragging: false
        property bool menuOpen: false
        property bool animationPlaybackEnabled: false
        property url spriteSheetSource: ""
        property int spriteColumns: 16
        property int spriteRows: 12
        property int sourceFrameWidth: 192
        property int sourceFrameHeight: 208
        property int frameRow: 0
        property int frameColumn: 0
        property int frameDurationMilliseconds: 160
        property int advanceCount: 0
        property int pointerHoverChangeCount: 0
        property int hoverStateChangeCount: 0
        property string hoverStateHistory: ""
        property int animationRequestCount: 0
        property string lastAnimation: ""

        signal animationFrameChanged()
        signal hoverChanged()

        function setPointerHovered(value) {
            pointerHoverChangeCount += 1
            pointerHovered = value
            hoverChanged()
        }

        function setControlsHovered(value) {
            controlsHovered = value
            hoverChanged()
        }

        function setHoverState(
            pointerValue,
            controlsValue) {
            hoverStateChangeCount += 1
            hoverStateHistory += String(pointerValue)
                + ","
                + String(controlsValue)
                + ";"
            if (pointerHovered !== pointerValue) {
                pointerHoverChangeCount += 1
            }
            pointerHovered = pointerValue
            controlsHovered = controlsValue
            if (controlsFollowHover) {
                controlsVisible =
                    pointerHovered || controlsHovered
            }
            hoverChanged()
        }

        function beginDrag() {
            dragging = true
        }

        function updateDrag(dx, dy) {
        }

        function endDrag() {
            dragging = false
        }

        function toggleMenu() {
            menuOpen = !menuOpen
        }

        function advanceAnimationFrame() {
            advanceCount += 1
            frameColumn = (frameColumn + 1) % 6
            frameDurationMilliseconds =
                frameColumn % 2 === 0 ? 40 : 90
            animationFrameChanged()
        }

        function setAnimation(animation) {
            animationRequestCount += 1
            lastAnimation = animation
        }
    }

    QtObject {
        id: fakeShellModel

        property string routeMode: "processes"
        property bool usageLoading: false
        property int activeProcessCount: 0
        property string chatAccentColor: "#ff6e14"
    }

    QtObject {
        id: fakeReactionModel

        property int goalConfettiTrigger: 0
    }

    QtObject {
        id: fakeMovementController

        property int beginCount: 0
        property int dragCount: 0
        property int endCount: 0
        property int publishHoverStateCount: 0
        property bool lastPublishedPointerHovered: false
        property bool lastPublishedControlsHovered: false

        function beginDrag() {
            beginCount += 1
            fakeModel.beginDrag()
        }

        function dragTo() {
            dragCount += 1
            fakeModel.updateDrag(1, 0)
        }

        function endDrag() {
            endCount += 1
            fakeModel.endDrag()
        }

        function publishHoverState(
            pointerHovered,
            controlsHovered) {
            publishHoverStateCount += 1
            lastPublishedPointerHovered =
                pointerHovered
            lastPublishedControlsHovered =
                controlsHovered
            fakeModel.setHoverState(
                pointerHovered,
                controlsHovered)
        }
    }

    Component {
        id: petWindowComponent

        PetWindow {
            petModel: fakeModel
            shellModel: fakeShellModel
            movementController:
                fakeMovementController
            liveAnimation: false
        }
    }

    SignalSpy {
        id: usageSpy
        signalName: "usageRequested"
    }

    SignalSpy {
        id: processesSpy
        signalName: "processesRequested"
    }

    SignalSpy {
        id: modelPickerSpy
        signalName: "modelPickerRequested"
    }

    SignalSpy {
        id: chatSpy
        signalName: "chatRequested"
    }

    SignalSpy {
        id: hideSpy
        signalName: "hideRequested"
    }

    SignalSpy {
        id: settingsSpy
        signalName: "settingsRequested"
    }

    function init() {
        fakeModel.controlsVisible = true
        fakeModel.controlsFollowHover = false
        fakeModel.pointerHovered = false
        fakeModel.controlsHovered = false
        fakeModel.dragging = false
        fakeModel.menuOpen = false
        fakeModel.animationPlaybackEnabled = false
        fakeModel.spriteColumns = 16
        fakeModel.spriteRows = 12
        fakeModel.sourceFrameWidth = 192
        fakeModel.sourceFrameHeight = 208
        fakeModel.frameRow = 0
        fakeModel.frameColumn = 0
        fakeModel.frameDurationMilliseconds = 160
        fakeModel.advanceCount = 0
        fakeModel.pointerHoverChangeCount = 0
        fakeModel.hoverStateChangeCount = 0
        fakeModel.hoverStateHistory = ""
        fakeModel.animationRequestCount = 0
        fakeModel.lastAnimation = ""
        fakeShellModel.routeMode = "processes"
        fakeShellModel.usageLoading = false
        fakeShellModel.activeProcessCount = 0
        fakeReactionModel.goalConfettiTrigger = 0
        fakeMovementController.beginCount = 0
        fakeMovementController.dragCount = 0
        fakeMovementController.endCount = 0
        fakeMovementController.publishHoverStateCount = 0
        fakeMovementController.lastPublishedPointerHovered = false
        fakeMovementController.lastPublishedControlsHovered = false
        usageSpy.target = null
        processesSpy.target = null
        modelPickerSpy.target = null
        chatSpy.target = null
        hideSpy.target = null
    }

    function createPetWindow() {
        const petWindow = createTemporaryObject(
            petWindowComponent,
            null)
        verify(petWindow)
        petWindow.visible = true
        tryCompare(petWindow, "visible", true)
        waitForRendering(petWindow.contentItem)
        return petWindow
    }

    function test_window_and_hit_regions_match_macos_contract() {
        const petWindow = createPetWindow()

        compare(petWindow.width, 124)
        compare(petWindow.height, 164)
        compare(petWindow.minimumWidth, 124)
        compare(petWindow.maximumWidth, 124)
        compare(petWindow.minimumHeight, 164)
        compare(petWindow.maximumHeight, 164)
        verify((petWindow.flags & Qt.Tool) !== 0)
        verify((petWindow.flags & Qt.FramelessWindowHint) !== 0)
        verify((petWindow.flags & Qt.WindowStaysOnTopHint) !== 0)

        compare(petWindow.artwork.x, 12)
        compare(petWindow.artwork.y, 48)
        compare(petWindow.artwork.width, 100)
        compare(petWindow.artwork.height, 108)

        compare(petWindow.dragSurface.x, 12)
        compare(petWindow.dragSurface.y, 48)
        compare(petWindow.dragSurface.width, 100)
        compare(petWindow.dragSurface.height, 108)
        compare(
            petWindow.dragSurface.cursorShape,
            Qt.ArrowCursor)

        compare(petWindow.usageButton.width, 36)
        compare(petWindow.usageButton.height, 48)
        compare(petWindow.processesButton.width, 36)
        compare(petWindow.processesButton.height, 48)
        compare(petWindow.menuButton.width, 36)
        compare(petWindow.menuButton.height, 48)
        compare(petWindow.controlCluster.x, 4)
        compare(petWindow.controlCluster.y, 8)
        compare(petWindow.controlCluster.width, 116)
        compare(petWindow.controlCluster.height, 48)
        compare(petWindow.controlHoverSurface.x, 84)
        compare(petWindow.controlHoverSurface.y, 8)
        compare(petWindow.controlHoverSurface.width, 36)
        compare(petWindow.controlHoverSurface.height, 48)
        compare(petWindow.usageButton.visible, false)
        compare(petWindow.processesButton.visible, false)
        compare(petWindow.menuButton.x, 80)
        compare(
            petWindow.contextMenu.popupType,
            Popup.Window)

        fakeModel.menuOpen = true
        tryCompare(petWindow.controlCluster, "x", 4)
        tryCompare(petWindow.controlCluster, "width", 116)
        compare(petWindow.controlHoverSurface.x, 4)
        compare(petWindow.controlHoverSurface.width, 116)
        tryCompare(petWindow.usageButton, "opacity", 1)
        tryCompare(petWindow.processesButton, "opacity", 1)
        compare(petWindow.usageButton.visible, true)
        compare(petWindow.processesButton.visible, true)
    }

    function test_hidden_controls_do_not_shift_pet_geometry() {
        const petWindow = createPetWindow()
        const artworkX = petWindow.artwork.x
        const artworkY = petWindow.artwork.y

        fakeModel.controlsVisible = false

        tryCompare(petWindow.controlCluster, "opacity", 0)
        tryCompare(petWindow.controlCluster, "visible", false)
        compare(petWindow.artwork.x, artworkX)
        compare(petWindow.artwork.y, artworkY)

        fakeModel.controlsVisible = true
        tryCompare(petWindow.controlCluster, "opacity", 1)
        compare(petWindow.controlCluster.visible, true)
        compare(petWindow.artwork.x, artworkX)
        compare(petWindow.artwork.y, artworkY)
    }

    function test_spring_overshoot_does_not_escape_control_bounds() {
        const petWindow = createPetWindow()

        petWindow.controlsMotionStart = 0
        petWindow.controlsMotionTarget = 1
        petWindow.controlsMotionClock = 0.71
        petWindow.expansionMotionStart = 0
        petWindow.expansionMotionTarget = 1
        petWindow.expansionMotionClock = 0.71
        petWindow.badgeMotionStart = 0
        petWindow.badgeMotionTarget = 1
        petWindow.badgeMotionClock = 0.71

        verify(petWindow.controlsMotionProgress > 1)
        verify(petWindow.expansionMotionProgress > 1)
        verify(petWindow.badgeMotionProgress > 1)
        compare(petWindow.controlCluster.y, 8)
        compare(petWindow.controlCluster.scale, 1)
        compare(petWindow.usageButton.x, 0)
        compare(petWindow.usageButton.scale, 1)
        compare(petWindow.processesButton.x, 40)
        compare(petWindow.processesButton.scale, 1)
        compare(petWindow.menuButton.glyphRotation, 180)
        compare(petWindow.menuGlyphItem.scale, 0.72)
        compare(petWindow.menuBadgeItem.scale, 1)
        compare(petWindow.menuBadgeItem.rotation, 0)
    }

    function test_hidden_control_row_does_not_capture_hover() {
        const petWindow = createPetWindow()
        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        fakeModel.controlsHovered = false
        fakeModel.controlsVisible = false
        tryCompare(
            petWindow.controlCluster,
            "visible",
            false)
        compare(
            petWindow.controlHoverSurface.visible,
            true)

        mouseMove(
            petWindow.controlHoverSurface,
            petWindow.controlHoverSurface.width / 2,
            petWindow.controlHoverSurface.height / 2)
        tryCompare(
            fakeModel,
            "controlsHovered",
            false)

        fakeModel.controlsVisible = true
        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        mouseMove(
            petWindow.controlHoverSurface,
            petWindow.controlHoverSurface.width / 2,
            petWindow.controlHoverSurface.height / 2)
        tryCompare(
            fakeModel,
            "controlsHovered",
            true)

        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        tryCompare(
            fakeModel,
            "controlsHovered",
            false)
    }

    function test_pet_to_control_hover_is_reconciled_atomically() {
        const petWindow = createPetWindow()

        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        fakeModel.hoverStateChangeCount = 0
        fakeModel.hoverStateHistory = ""

        mouseMove(
            petWindow.dragSurface,
            petWindow.dragSurface.width / 2,
            petWindow.dragSurface.height / 2)
        tryCompare(
            fakeModel,
            "hoverStateChangeCount",
            1)
        compare(
            fakeModel.hoverStateHistory,
            "true,false;")

        mouseMove(
            petWindow.contentItem,
            102,
            50)
        tryCompare(
            fakeModel,
            "hoverStateChangeCount",
            2)
        compare(
            fakeModel.hoverStateHistory,
            "true,false;true,true;")

        mouseMove(
            petWindow.contentItem,
            102,
            32)
        tryCompare(
            fakeModel,
            "hoverStateChangeCount",
            3)
        compare(
            fakeModel.hoverStateHistory,
            "true,false;true,true;false,true;")
        verify(
            fakeModel.hoverStateHistory.indexOf(
                "false,false;") === -1)
    }

    function test_fast_hidden_pet_to_control_hover_is_not_dropped() {
        const petWindow = createPetWindow()
        fakeModel.controlsFollowHover = true
        fakeModel.controlsVisible = false

        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        wait(petWindow.hoverSettleDuration + 10)
        fakeModel.hoverStateChangeCount = 0
        fakeModel.hoverStateHistory = ""
        fakeMovementController.publishHoverStateCount = 0

        mouseMove(
            petWindow.contentItem,
            102,
            60)
        mouseMove(
            petWindow.contentItem,
            102,
            32)
        wait(petWindow.hoverSettleDuration + 10)

        compare(
            fakeModel.hoverStateHistory,
            "true,false;false,true;")
        compare(
            fakeMovementController.publishHoverStateCount,
            2)
        compare(fakeModel.pointerHovered, false)
        compare(fakeModel.controlsHovered, true)
        compare(fakeModel.controlsVisible, true)
    }

    function test_control_hover_remains_stable_past_tooltip_delay() {
        const petWindow = createPetWindow()
        fakeModel.controlsFollowHover = true
        fakeModel.controlsVisible = false

        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        wait(petWindow.hoverSettleDuration + 10)

        mouseMove(
            petWindow.dragSurface,
            petWindow.dragSurface.width / 2,
            petWindow.dragSurface.height / 2)
        tryCompare(fakeModel, "pointerHovered", true)
        tryCompare(fakeModel, "controlsVisible", true)

        mouseMove(
            petWindow.contentItem,
            102,
            32)
        tryCompare(fakeModel, "pointerHovered", false)
        tryCompare(fakeModel, "controlsHovered", true)
        tryCompare(fakeModel, "controlsVisible", true)

        wait(700)

        compare(fakeModel.pointerHovered, false)
        compare(fakeModel.controlsHovered, true)
        compare(fakeModel.controlsVisible, true)
        compare(petWindow.controlCluster.visible, true)
        compare(petWindow.menuButton.hovered, true)
    }

    function test_pet_control_uses_macos_hover_deformation() {
        const petWindow = createPetWindow()

        compare(
            petWindow.menuButton.transform.length,
            0,
            "Pet control hit bounds must not deform")
        mouseMove(
            petWindow.menuButton,
            petWindow.menuButton.width / 2,
            petWindow.menuButton.height / 2)

        tryCompare(petWindow.menuButton, "hovered", true)
        tryCompare(
            petWindow.menuButton,
            "horizontalScale",
            1.014)
        tryCompare(
            petWindow.menuButton,
            "verticalScale",
            0.992)
    }

    function test_control_buttons_emit_their_actions() {
        const petWindow = createPetWindow()
        usageSpy.target = petWindow
        processesSpy.target = petWindow
        modelPickerSpy.target = petWindow
        chatSpy.target = petWindow
        usageSpy.clear()
        processesSpy.clear()
        modelPickerSpy.clear()
        chatSpy.clear()
        fakeModel.menuOpen = true
        tryCompare(petWindow.usageButton, "visible", true)
        tryCompare(petWindow.processesButton, "visible", true)
        tryCompare(petWindow.usageButton, "opacity", 1)
        tryCompare(petWindow.processesButton, "opacity", 1)
        tryCompare(petWindow.usageButton, "x", 0)
        tryCompare(petWindow.processesButton, "x", 40)
        compare(
            petWindow.usageButton.interactionId,
            "pet.usage.toggle")
        compare(
            petWindow.processesButton.interactionId,
            "pet.route.toggle")
        compare(
            petWindow.menuButton.interactionId,
            "pet.menu.toggle")
        waitForRendering(petWindow.contentItem)

        mouseClick(petWindow.usageButton)
        mouseClick(petWindow.processesButton)

        compare(usageSpy.count, 1)
        compare(chatSpy.count, 1)
        compare(modelPickerSpy.count, 0)
        compare(processesSpy.count, 0)
        compare(
            petWindow.usageButton.Accessible.name,
            "Codex usage and resets")
        compare(
            petWindow.processesButton.Accessible.name,
            "Open local chat")

        fakeShellModel.routeMode = "local-chat"
        compare(
            petWindow.usageButton.interactionId,
            "pet.model.toggle")
        mouseClick(petWindow.usageButton)
        mouseClick(petWindow.processesButton)

        compare(usageSpy.count, 1)
        compare(chatSpy.count, 1)
        compare(modelPickerSpy.count, 1)
        compare(processesSpy.count, 1)
        compare(
            petWindow.usageButton.Accessible.name,
            "Chat service and model")
        compare(
            petWindow.processesButton.Accessible.name,
            "Show Codex processes")

        mouseClick(petWindow.menuButton)
        compare(fakeModel.menuOpen, false)
        mouseClick(petWindow.menuButton)
        compare(processesSpy.count, 2)
        verify(petWindow.usageButton.Accessible.name.length > 0)
        verify(petWindow.processesButton.Accessible.name.length > 0)
        verify(petWindow.menuButton.Accessible.name.length > 0)
    }

    function test_closed_menu_shows_the_active_process_badge() {
        const petWindow = createPetWindow()

        fakeShellModel.activeProcessCount = 2
        compare(petWindow.menuButton.text, "2")
        compare(
            petWindow.activeProcessAccent,
            "#ff6e14")
        tryVerify(function() {
            return (
            Math.abs(
                petWindow.menuButton.background.color.r
                - 1) < 0.001)
        })
        tryVerify(function() {
            return (
            Math.abs(
                petWindow.menuButton.background.color.g
                - 0.431372549) < 0.001)
        })
        tryVerify(function() {
            return (
            Math.abs(
                petWindow.menuButton.background.color.b
                - 0.078431373) < 0.001)
        })
        compare(
            petWindow.menuButton.toolTipText,
            "Show menu - 2 active Codex processes")
        compare(
            petWindow.menuButton.Accessible.name,
            "Show menu - 2 active Codex processes")

        fakeShellModel.activeProcessCount = 12
        compare(petWindow.menuButton.text, "9+")

        fakeModel.menuOpen = true
        tryCompare(petWindow.menuGlyphItem, "rotation", 180)
        compare(petWindow.menuButton.toolTipText, "Hide menu")
        compare(petWindow.menuButton.Accessible.name, "Hide menu")

        fakeModel.menuOpen = false
        fakeShellModel.activeProcessCount = 0
        tryCompare(petWindow.menuGlyphItem, "rotation", 0)
        compare(petWindow.menuButton.toolTipText, "Show menu")
        compare(petWindow.menuButton.Accessible.name, "Show menu")
    }

    function test_goal_completion_confetti_matches_macos_pet_overlay() {
        const petWindow = createPetWindow()

        verify(petWindow.goalConfettiOverlay !== undefined)
        petWindow.reactionModel = fakeReactionModel
        compare(petWindow.reactionModel, fakeReactionModel)

        const overlay = petWindow.goalConfettiOverlay
        overlay.liveAnimation = false
        compare(overlay.x, 0)
        compare(overlay.y, 40)
        compare(overlay.width, 124)
        compare(overlay.height, 124)
        compare(overlay.enabled, false)
        compare(overlay.particleRepeater.count, 0)

        fakeReactionModel.goalConfettiTrigger = 7

        tryCompare(overlay, "activeTrigger", 7)
        compare(overlay.particleRepeater.count, 30)
        compare(overlay.burstProgress, 0)

        const first = overlay.particleRepeater.itemAt(0)
        const second = overlay.particleRepeater.itemAt(1)
        verify(first)
        verify(second)
        compare(first.width, 5)
        compare(first.height, 7.5)
        compare(first.color, "#ffd60a")
        compare(second.width, 6)
        compare(second.height, 9)
        compare(second.color, "#30d158")

        overlay.burstProgress = 1
        const vector = overlay.particleVector(0, 7)
        const expectedCenterX =
            overlay.width / 2 + vector.x * 34
        const expectedCenterY =
            overlay.height * 0.58 + vector.y * 34
        verify(Math.abs(
            first.x + first.width / 2
                - expectedCenterX) < 0.01)
        verify(Math.abs(
            first.y + first.height / 2
                - expectedCenterY) < 0.01)
        compare(first.opacity, 0)
        compare(first.scale, 0.88)
        compare(second.rotation, 31)
    }

    function test_menu_controls_use_macos_style_transitions() {
        const petWindow = createPetWindow()
        compare(petWindow.menuMotionDuration, 620)
        petWindow.reduceMotion = true
        compare(petWindow.menuMotionDuration, 120)

        fakeModel.controlsVisible = false
        wait(35)
        verify(petWindow.controlCluster.opacity < 1)
        verify(petWindow.controlCluster.opacity > 0)
        verify(petWindow.controlCluster.y > 8)
        verify(petWindow.controlCluster.scale < 1)
        tryCompare(petWindow.controlCluster, "opacity", 0)
        tryCompare(petWindow.controlCluster, "y", 12)
        tryCompare(petWindow.controlCluster, "scale", 0.94)

        fakeModel.controlsVisible = true
        wait(35)
        verify(petWindow.controlCluster.opacity > 0)
        verify(petWindow.controlCluster.opacity < 1)
        tryCompare(petWindow.controlCluster, "opacity", 1)
        tryCompare(petWindow.controlCluster, "y", 8)
        tryCompare(petWindow.controlCluster, "scale", 1)

        fakeModel.menuOpen = true
        wait(35)
        verify(petWindow.usageButton.opacity > 0)
        verify(petWindow.usageButton.opacity < 1)
        verify(petWindow.usageButton.scale > 0.72)
        verify(petWindow.usageButton.scale < 1)
        tryCompare(petWindow.usageButton, "opacity", 1)
        tryCompare(petWindow.usageButton, "scale", 1)
        tryCompare(petWindow.usageButton, "x", 0)
        tryCompare(petWindow.processesButton, "x", 40)
        tryCompare(petWindow.menuGlyphItem, "rotation", 180)

        fakeModel.menuOpen = false
        fakeShellModel.activeProcessCount = 2
        wait(35)
        verify(petWindow.menuBadgeItem.opacity > 0)
        verify(petWindow.menuBadgeItem.opacity < 1)
        verify(petWindow.menuGlyphItem.opacity > 0)
        verify(petWindow.menuGlyphItem.opacity < 1)
        tryCompare(petWindow.menuBadgeItem, "opacity", 1)
        tryCompare(petWindow.menuBadgeItem, "scale", 1)
        tryCompare(petWindow.menuBadgeItem, "rotation", 0)
        tryCompare(petWindow.menuGlyphItem, "opacity", 0)
        tryCompare(petWindow.menuGlyphItem, "scale", 0.72)
    }

    function test_menu_toggle_routes_newly_opened_quick_bar_to_processes() {
        const petWindow = createPetWindow()
        processesSpy.target = petWindow
        processesSpy.clear()
        fakeShellModel.routeMode = "local-chat"
        fakeModel.menuOpen = false

        petWindow.menuButton.clicked()

        compare(processesSpy.count, 1)
        compare(fakeModel.menuOpen, false)

        fakeModel.menuOpen = true
        petWindow.menuButton.clicked()

        compare(processesSpy.count, 1)
        compare(fakeModel.menuOpen, false)
    }

    function test_right_click_menu_contains_only_hide_and_settings() {
        const petWindow = createPetWindow()
        hideSpy.target = petWindow
        settingsSpy.target = petWindow
        hideSpy.clear()
        settingsSpy.clear()
        compare(petWindow.contextMenu.count, 2)
        compare(
            petWindow.contextHideItem.interactionId,
            "pet.context.hide")
        compare(
            petWindow.contextHideItem.text,
            "Hide Pet")
        compare(
            petWindow.contextSettingsItem.interactionId,
            "pet.context.settings")
        compare(
            petWindow.contextSettingsItem.text,
            "Open Settings")

        mouseClick(
            petWindow.dragSurface,
            petWindow.dragSurface.width / 2,
            petWindow.dragSurface.height / 2,
            Qt.RightButton)
        tryCompare(petWindow.contextMenu, "visible", true)
        mouseClick(petWindow.contextHideItem)
        tryCompare(petWindow.contextMenu, "visible", false)
        compare(hideSpy.count, 1)

        petWindow.contextMenu.popup()
        tryCompare(petWindow.contextMenu, "visible", true)
        mouseClick(petWindow.contextSettingsItem)
        tryCompare(petWindow.contextMenu, "visible", false)
        compare(settingsSpy.count, 1)
    }

    function test_sprite_crops_exact_atlas_cell_before_scaling() {
        const petWindow = createPetWindow()
        tryCompare(
            petWindow.spriteImage,
            "status",
            Image.Ready)
        verify(
            petWindow.spriteImage.source.toString().endsWith(
                "spritesheet.webp"))

        compare(petWindow.spriteImage.smooth, false)
        compare(petWindow.spriteImage.mipmap, false)
        compare(petWindow.spriteImage.fillMode, Image.PreserveAspectFit)
        compare(petWindow.spriteImage.sourceClipRect.x, 0)
        compare(petWindow.spriteImage.sourceClipRect.y, 0)
        compare(petWindow.spriteImage.sourceClipRect.width, 192)
        compare(petWindow.spriteImage.sourceClipRect.height, 208)
        verify(petWindow.artwork.clip)
        compare(petWindow.spriteImage.x, 0)
        compare(petWindow.spriteImage.y, 0)
        compare(petWindow.spriteImage.width, petWindow.artwork.width)
        compare(petWindow.spriteImage.height, petWindow.artwork.height)

        fakeModel.frameRow = 2
        fakeModel.frameColumn = 3
        tryCompare(petWindow.spriteImage.sourceClipRect, "x", 576)
        tryCompare(petWindow.spriteImage.sourceClipRect, "y", 416)
        compare(petWindow.spriteImage.sourceClipRect.width, 192)
        compare(petWindow.spriteImage.sourceClipRect.height, 208)

        fakeModel.frameRow = 11
        fakeModel.frameColumn = 15
        tryCompare(petWindow.spriteImage.sourceClipRect, "x", 2880)
        tryCompare(petWindow.spriteImage.sourceClipRect, "y", 2288)
        compare(petWindow.spriteImage.sourceClipRect.width, 192)
        compare(petWindow.spriteImage.sourceClipRect.height, 208)
        waitForRendering(petWindow.artwork)
        const finalFrame = grabImage(petWindow.artwork)
        let finalFrameVisiblePixels = 0
        for (let finalY = 0; finalY < finalFrame.height; ++finalY) {
            for (let finalX = 0; finalX < finalFrame.width; ++finalX) {
                if (finalFrame.alpha(finalX, finalY) > 0) {
                    finalFrameVisiblePixels += 1
                }
            }
        }
        verify(finalFrameVisiblePixels > 500)

        fakeModel.frameRow = 0
        fakeModel.frameColumn = 0
        waitForRendering(petWindow.contentItem)
        const image = grabImage(petWindow.contentItem)

        let transparentPixels = 0
        let visiblePixels = 0
        let darkPixels = 0
        let goldenPixels = 0
        for (let y = 0; y < image.height; ++y) {
            for (let x = 0; x < image.width; ++x) {
                const alpha = image.alpha(x, y)
                const red = image.red(x, y)
                const green = image.green(x, y)
                const blue = image.blue(x, y)
                if (alpha === 0) {
                    transparentPixels += 1
                } else {
                    visiblePixels += 1
                }
                if (alpha > 180
                        && red < 70
                        && green < 70
                        && blue < 70) {
                    darkPixels += 1
                }
                if (alpha > 180
                        && red > 180
                        && green > 120
                        && blue < 100) {
                    goldenPixels += 1
                }
            }
        }

        verify(transparentPixels > 12000)
        verify(visiblePixels > 900)
        verify(darkPixels > 500)
        verify(goldenPixels > 4)
    }

    function test_sprite_geometry_follows_the_selected_pet_model() {
        const petWindow = createPetWindow()

        compare(petWindow.artwork.atlasColumns, 16)
        compare(petWindow.artwork.atlasRows, 12)
        compare(petWindow.artwork.sourceFrameWidth, 192)
        compare(petWindow.artwork.sourceFrameHeight, 208)

        fakeModel.spriteColumns = 8
        fakeModel.spriteRows = 9
        fakeModel.sourceFrameWidth = 384
        fakeModel.sourceFrameHeight = 277
        fakeModel.frameRow = 8
        fakeModel.frameColumn = 7

        tryCompare(petWindow.artwork, "atlasColumns", 8)
        tryCompare(petWindow.artwork, "atlasRows", 9)
        tryCompare(
            petWindow.artwork,
            "sourceFrameWidth",
            384)
        tryCompare(
            petWindow.artwork,
            "sourceFrameHeight",
            277)
        tryCompare(
            petWindow.spriteImage.sourceClipRect,
            "x",
            2688)
        tryCompare(
            petWindow.spriteImage.sourceClipRect,
            "y",
            2216)
    }

    function test_drag_window_motion_does_not_clear_pet_hover() {
        const petWindow = createPetWindow()

        mouseMove(
            petWindow.dragSurface,
            petWindow.dragSurface.width / 2,
            petWindow.dragSurface.height / 2)
        tryCompare(
            fakeModel,
            "pointerHovered",
            true)

        fakeModel.dragging = true
        tryCompare(
            petWindow,
            "dragGestureActive",
            true)
        fakeModel.pointerHoverChangeCount = 0

        mouseMove(
            petWindow.contentItem,
            0,
            petWindow.height - 1)
        wait(20)

        compare(fakeModel.pointerHovered, true)
        compare(fakeModel.pointerHoverChangeCount, 0)

        fakeModel.dragging = false

        tryCompare(fakeModel, "pointerHovered", false)
        tryCompare(
            fakeModel,
            "pointerHoverChangeCount",
            1)
    }

    function test_hover_jitter_publishes_only_the_settled_state() {
        const petWindow = createPetWindow()

        mouseMove(
            petWindow.dragSurface,
            petWindow.dragSurface.width / 2,
            petWindow.dragSurface.height / 2)
        tryCompare(
            fakeModel,
            "pointerHovered",
            true)
        fakeModel.pointerHoverChangeCount = 0
        fakeModel.hoverStateChangeCount = 0
        fakeModel.hoverStateHistory = ""
        fakeMovementController.publishHoverStateCount = 0

        petWindow.queueHoverState(false, false)
        wait(1)
        petWindow.queueHoverState(true, false)
        wait(1)
        petWindow.queueHoverState(false, false)

        tryCompare(
            fakeModel,
            "pointerHovered",
            false)
        compare(
            fakeModel.pointerHoverChangeCount,
            1)
        compare(
            fakeModel.hoverStateChangeCount,
            1)
        compare(
            fakeModel.hoverStateHistory,
            "false,false;")
        compare(
            fakeMovementController.publishHoverStateCount,
            1)
        compare(
            fakeMovementController.lastPublishedPointerHovered,
            false)
        compare(
            fakeMovementController.lastPublishedControlsHovered,
            false)
    }

    function test_local_drag_cancellation_defers_to_native_controller() {
        const petWindow = createPetWindow()
        petWindow.dragGestureActive = true
        fakeModel.beginDrag()

        petWindow.dragSurface.canceled()

        compare(fakeMovementController.endCount, 0)
        compare(fakeModel.dragging, true)
        compare(petWindow.dragGestureActive, true)

        fakeModel.endDrag()

        compare(petWindow.dragGestureActive, false)
    }

    function test_animation_playback_is_delegated_to_model() {
        const petWindow = createPetWindow()
        compare(
            fakeModel.animationPlaybackEnabled,
            false)

        petWindow.liveAnimation = true
        tryCompare(
            fakeModel,
            "animationPlaybackEnabled",
            true)

        petWindow.liveAnimation = false
        tryCompare(
            fakeModel,
            "animationPlaybackEnabled",
            false)
    }
}
