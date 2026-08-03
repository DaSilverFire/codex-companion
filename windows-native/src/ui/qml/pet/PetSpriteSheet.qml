import QtQuick

Item {
    id: root

    property url source
    property int atlasColumns: 16
    property int atlasRows: 12
    property int sourceFrameWidth: 192
    property int sourceFrameHeight: 208
    property int frameRow: 0
    property int frameColumn: 0
    property alias image: spriteImage
    readonly property int safeColumns:
        Math.max(1, atlasColumns)
    readonly property int safeRows:
        Math.max(1, atlasRows)
    readonly property int safeFrameWidth:
        Math.max(1, sourceFrameWidth)
    readonly property int safeFrameHeight:
        Math.max(1, sourceFrameHeight)
    readonly property int clampedFrameRow:
        Math.max(
            0,
            Math.min(safeRows - 1, frameRow))
    readonly property int clampedFrameColumn:
        Math.max(
            0,
            Math.min(
                safeColumns - 1,
                frameColumn))

    implicitWidth: 100
    implicitHeight: 108
    clip: true

    Image {
        id: spriteImage

        anchors.fill: parent
        source: root.source
        sourceClipRect: Qt.rect(
            root.clampedFrameColumn
                * root.safeFrameWidth,
            root.clampedFrameRow
                * root.safeFrameHeight,
            root.safeFrameWidth,
            root.safeFrameHeight)
        fillMode: Image.PreserveAspectFit
        asynchronous: false
        cache: true
        smooth: false
        mipmap: false
    }
}
