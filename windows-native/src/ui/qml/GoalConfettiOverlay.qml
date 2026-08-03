import QtQuick

Item {
    id: root

    property int trigger: 0
    property int activeTrigger: 0
    property real burstProgress: 0
    property bool liveAnimation: true
    property int clearingTrigger: 0
    readonly property int particleCount: 30
    readonly property alias particleRepeater: particles

    enabled: false
    visible: activeTrigger > 0

    function pieceSize(index) {
        return 5 + index % 3
    }

    function confettiColor(index) {
        switch (index % 6) {
        case 0:
            return "#ffd60a"
        case 1:
            return "#30d158"
        case 2:
            return "#64d2ff"
        case 3:
            return "#ff375f"
        case 4:
            return "#ff9f0a"
        default:
            return "#ffffff"
        }
    }

    function particleVector(index, triggerValue) {
        const baseAngle =
            index / particleCount * Math.PI * 2
        const offset =
            ((triggerValue * 23 + index * 11) % 70)
                / 100
        const angle = baseAngle + offset
        return Qt.point(
            Math.cos(angle),
            Math.sin(angle) - 0.35)
    }

    function startBurst(value) {
        if (value <= 0) {
            return
        }

        burstAnimation.stop()
        clearTimer.stop()
        activeTrigger = value
        clearingTrigger = value
        burstProgress = 0
        if (!liveAnimation) {
            return
        }

        Qt.callLater(function() {
            if (!root.liveAnimation
                    || root.activeTrigger !== value) {
                return
            }
            burstAnimation.restart()
            clearTimer.restart()
        })
    }

    onTriggerChanged: startBurst(trigger)
    onLiveAnimationChanged: {
        if (!liveAnimation) {
            burstAnimation.stop()
            clearTimer.stop()
        }
    }
    Component.onCompleted: {
        if (trigger > 0) {
            startBurst(trigger)
        }
    }

    Repeater {
        id: particles

        model: root.activeTrigger > 0
            ? root.particleCount
            : 0

        delegate: Rectangle {
            required property int index
            readonly property point vector:
                root.particleVector(
                    index,
                    root.activeTrigger)
            readonly property real travelDistance:
                34 + index % 7 * 12
            readonly property real endOffsetX:
                vector.x * travelDistance
            readonly property real endOffsetY:
                vector.y * travelDistance
                    - index % 5 * 7

            width: root.pieceSize(index)
            height: width * 1.5
            radius: 1.5
            color: root.confettiColor(index)
            x: root.width / 2 - width / 2
                + endOffsetX
                    * root.burstProgress
            y: root.height * 0.58 - height / 2
                + endOffsetY
                    * root.burstProgress
            rotation:
                (index * 31) % 360
                    * root.burstProgress
            opacity: 1 - root.burstProgress
            scale: 1 - 0.12
                * root.burstProgress
            antialiasing: true
        }
    }

    NumberAnimation {
        id: burstAnimation

        target: root
        property: "burstProgress"
        from: 0
        to: 1
        duration: 1200
        easing.type: Easing.OutCubic
    }

    Timer {
        id: clearTimer

        interval: 1250
        repeat: false
        onTriggered: {
            if (root.activeTrigger
                    !== root.clearingTrigger) {
                return
            }
            root.activeTrigger = 0
            root.burstProgress = 0
        }
    }
}
