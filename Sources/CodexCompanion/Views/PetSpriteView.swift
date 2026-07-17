import AppKit
import ImageIO
import QuartzCore
import SwiftUI

struct PetSpriteView: NSViewRepresentable {
    var pet: PetDefinition
    var state: PetAnimationState
    var speedScale: Double = 1
    var directionalLookFrame: PetDirectionalLookFrame?

    func makeNSView(context: Context) -> SpriteSheetPreviewView {
        let view = SpriteSheetPreviewView()
        view.configure(
            pet: pet,
            state: state,
            speedScale: speedScale,
            directionalLookFrame: directionalLookFrame
        )
        return view
    }

    func updateNSView(_ nsView: SpriteSheetPreviewView, context: Context) {
        nsView.configure(
            pet: pet,
            state: state,
            speedScale: speedScale,
            directionalLookFrame: directionalLookFrame
        )
    }
}

final class SpriteSheetPreviewView: NSView {
    private struct SpriteFrame {
        var row: Int
        var column: Int
        var duration: TimeInterval
    }

    private let spriteLayer = CALayer()
    private var activeSheet: CGImage?
    private var activeSheetKey: String?
    private var activeAnimationKey: String?
    private var sheetCache: [String: CGImage] = [:]
    private var frameCache: [String: CGImage] = [:]
    private var sequence: [SpriteFrame] = []
    private var state: PetAnimationState = .idle
    private var columns = 8
    private var rows = 9
    private var frameCount = 1
    private var frameIndex = 0
    private var loopStartIndex = 0
    private var timer: Timer?

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        configureTransparentLayer()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configureTransparentLayer()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        configureTransparentLayer()
    }

    override func layout() {
        super.layout()
        spriteLayer.frame = bounds
    }

    func configure(
        pet: PetDefinition,
        state: PetAnimationState,
        speedScale: Double,
        directionalLookFrame: PetDirectionalLookFrame? = nil
    ) {
        let lookSource = directionalLookFrame == nil ? nil : pet.directionalLookFrames
        let activeURL = lookSource?.spritesheetURL ?? pet.spritesheetURL
        let resolvedColumns = min(32, max(1, lookSource?.spriteColumns ?? pet.spriteColumns))
        let resolvedRows = max(1, lookSource?.spriteRows ?? pet.spriteRows)
        let resolvedFrameCount = max(1, pet.frameCount(for: state))
        let durationScale = max(0.4, min(3.0, speedScale))
        let sheetKey = [
            pet.id,
            activeURL.path,
            fileFingerprint(for: activeURL),
            "\(resolvedColumns)",
            "\(resolvedRows)",
        ].joined(separator: "|")
        let animationKey = [
            sheetKey,
            state.rawValue,
            "\(resolvedFrameCount)",
            "\(durationScale)",
            directionalLookFrame.map { "\($0.row):\($0.column)" } ?? "animated",
        ].joined(separator: "|")
        let sheetChanged = activeSheetKey != sheetKey
        let animationChanged = activeAnimationKey != animationKey

        self.state = state
        columns = resolvedColumns
        rows = resolvedRows
        frameCount = resolvedFrameCount

        if sheetChanged {
            timer?.invalidate()
            timer = nil
            if let cachedSheet = sheetCache[sheetKey] {
                activeSheet = cachedSheet
            } else {
                activeSheet = loadSheet(from: activeURL)
                if let activeSheet {
                    if sheetCache.count >= 4 {
                        sheetCache.removeAll(keepingCapacity: true)
                    }
                    sheetCache[sheetKey] = activeSheet
                }
            }
            activeSheetKey = activeSheet == nil ? nil : sheetKey
            activeAnimationKey = nil
            spriteLayer.contents = nil
        }

        if sheetChanged || animationChanged {
            timer?.invalidate()
            timer = nil
            frameIndex = 0

            guard activeSheet != nil else {
                activeAnimationKey = nil
                return
            }

            activeAnimationKey = animationKey
            let nextSequence: (frames: [SpriteFrame], loopStartIndex: Int)
            if let directionalLookFrame {
                nextSequence = (
                    [SpriteFrame(
                        row: directionalLookFrame.row,
                        column: directionalLookFrame.column,
                        duration: 1
                    )],
                    0
                )
            } else {
                nextSequence = makeSequence(
                    pet: pet,
                    state: state,
                    columns: columns,
                    rows: rows,
                    frameCount: frameCount,
                    idleFrameCount: pet.frameCount(for: .idle),
                    durationScale: durationScale
                )
            }
            sequence = nextSequence.frames
            loopStartIndex = min(nextSequence.loopStartIndex, max(0, sequence.count - 1))
            applyFrame()
            prewarmFrameCacheAsync(sheetKey: activeSheetKey)
        } else {
            frameIndex %= max(1, sequence.count)
        }

        configureTransparentLayer()
        if sheetChanged || animationChanged || timer == nil {
            scheduleNextFrame()
        }
    }

    private func configureTransparentLayer() {
        wantsLayer = true
        layer?.backgroundColor = NSColor.clear.cgColor
        layer?.isOpaque = false
        layer?.masksToBounds = true
        layer?.actions = [
            "backgroundColor": NSNull(),
            "bounds": NSNull(),
            "contents": NSNull(),
            "opacity": NSNull(),
            "position": NSNull(),
        ]
        spriteLayer.backgroundColor = NSColor.clear.cgColor
        spriteLayer.isOpaque = false
        spriteLayer.masksToBounds = true
        spriteLayer.contentsGravity = .resizeAspect
        spriteLayer.magnificationFilter = .nearest
        spriteLayer.minificationFilter = .nearest
        spriteLayer.actions = [
            "bounds": NSNull(),
            "contents": NSNull(),
            "opacity": NSNull(),
            "position": NSNull(),
        ]
        spriteLayer.frame = bounds
        if spriteLayer.superlayer == nil {
            layer?.addSublayer(spriteLayer)
        }
    }

    private func loadSheet(from url: URL) -> CGImage? {
        guard
            let data = try? Data(contentsOf: url, options: [.mappedIfSafe]),
            let source = CGImageSourceCreateWithData(data as CFData, nil)
        else {
            return nil
        }
        guard let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
            return nil
        }

        return Self.renderFrameImage(image, width: image.width, height: image.height) ?? image
    }

    private func makeSequence(
        pet: PetDefinition,
        state: PetAnimationState,
        columns: Int,
        rows: Int,
        frameCount: Int,
        idleFrameCount: Int,
        durationScale: Double
    ) -> (frames: [SpriteFrame], loopStartIndex: Int) {
        if state == .goalComplete {
            return makeGoalCompleteSequence(
                pet: pet,
                columns: columns,
                rows: rows,
                durationScale: durationScale
            )
        }

        let actionState = pet.resolvedAnimationState(for: state)
        let actionRow = rowIndex(for: actionState, rows: rows)
        let actionFrameCount = min(columns, frameCount)
        let actionTiming = state.frameTiming(frameCount: actionFrameCount)
        let actionFrames = makeFrames(
            row: actionRow,
            count: actionFrameCount,
            baseDuration: actionTiming.base,
            finalDuration: actionTiming.final,
            durationScale: durationScale
        )
        let idleCount = max(1, min(columns, idleFrameCount))
        let idleTiming = PetAnimationState.idle.frameTiming(frameCount: idleCount)
        let idleFrames = makeFrames(
            row: PetAnimationState.idle.rowIndex,
            count: idleCount,
            baseDuration: idleTiming.base,
            finalDuration: idleTiming.final,
            durationScale: durationScale
        )

        if state == .idle {
            return (idleFrames, 0)
        }

        if state.loopsContinuously {
            return (actionFrames, 0)
        }

        let actionRepeatCount = actionFrameCount > 16 ? 1 : 3
        let prelude = (0..<actionRepeatCount).flatMap { _ in actionFrames }
        return (prelude + idleFrames, prelude.count)
    }

    private func makeGoalCompleteSequence(
        pet: PetDefinition,
        columns: Int,
        rows: Int,
        durationScale: Double
    ) -> (frames: [SpriteFrame], loopStartIndex: Int) {
        let idleTiming = PetAnimationState.idle.frameTiming(frameCount: max(1, min(columns, pet.frameCount(for: .idle))))
        let idleFrames = makeFrames(
            row: rowIndex(for: .idle, rows: rows),
            count: max(1, min(columns, pet.frameCount(for: .idle))),
            baseDuration: idleTiming.base,
            finalDuration: idleTiming.final,
            durationScale: durationScale
        )

        if pet.hasNativeRow(for: .goalComplete) {
            let nativeCount = max(1, min(columns, pet.frameCount(for: .goalComplete)))
            let nativeTiming = PetAnimationState.goalComplete.frameTiming(frameCount: nativeCount)
            let nativeFrames = makeFrames(
                row: PetAnimationState.goalComplete.rowIndex,
                count: nativeCount,
                baseDuration: nativeTiming.base,
                finalDuration: nativeTiming.final,
                durationScale: durationScale
            )
            let prelude = nativeCount > 16 ? nativeFrames : nativeFrames + nativeFrames
            return (prelude + idleFrames, prelude.count)
        }

        let jumpCount = max(1, min(columns, pet.frameCount(for: .jumping)))
        let waveCount = max(1, min(columns, pet.frameCount(for: .waving)))
        let reviewCount = max(1, min(columns, pet.frameCount(for: .review)))
        let shadowStyle = pet.usesShadowStyle

        let jumpFrames = makeFrames(
            row: rowIndex(for: .jumping, rows: rows),
            count: min(shadowStyle ? 12 : 5, jumpCount),
            baseDuration: shadowStyle ? 0.10 : 0.12,
            finalDuration: shadowStyle ? 0.14 : 0.16,
            durationScale: durationScale
        )
        let waveFrames = makeFrames(
            row: rowIndex(for: .waving, rows: rows),
            count: min(shadowStyle ? 18 : 4, waveCount),
            baseDuration: shadowStyle ? 0.12 : 0.14,
            finalDuration: shadowStyle ? 0.18 : 0.20,
            durationScale: durationScale
        )
        let reviewFrames = makeFrames(
            row: rowIndex(for: .review, rows: rows),
            count: min(shadowStyle ? 10 : 6, reviewCount),
            baseDuration: shadowStyle ? 0.12 : 0.15,
            finalDuration: shadowStyle ? 0.20 : 0.22,
            durationScale: durationScale
        )

        if shadowStyle {
            let bounceDownFrames = Array(jumpFrames.dropLast().reversed())
            let waveReturnFrames = Array(waveFrames.dropLast().reversed())
            let settleFrames = Array(reviewFrames.prefix(max(4, min(10, reviewFrames.count))))
            let prelude = jumpFrames + bounceDownFrames + waveFrames + waveReturnFrames + settleFrames
            return (prelude + idleFrames, prelude.count)
        }

        let prelude = jumpFrames + waveFrames + reviewFrames
        return (prelude + idleFrames, prelude.count)
    }

    private func rowIndex(for state: PetAnimationState, rows: Int) -> Int {
        if state.rowIndex < rows {
            return state.rowIndex
        }

        switch state {
        case .goalComplete:
            if PetAnimationState.review.rowIndex < rows {
                return PetAnimationState.review.rowIndex
            }
            return PetAnimationState.idle.rowIndex
        default:
            return min(rows - 1, max(0, state.rowIndex))
        }
    }

    private func makeFrames(
        row: Int,
        count: Int,
        baseDuration: TimeInterval,
        finalDuration: TimeInterval,
        durationScale: Double
    ) -> [SpriteFrame] {
        (0..<max(1, count)).map { column in
            SpriteFrame(
                row: row,
                column: column,
                duration: (column == count - 1 ? finalDuration : baseDuration) * durationScale
            )
        }
    }

    private func applyFrame() {
        guard let activeSheet, !sequence.isEmpty else {
            spriteLayer.contents = nil
            return
        }

        frameIndex %= sequence.count
        let frame = sequence[frameIndex]
        let cellWidth = max(1, activeSheet.width / columns)
        let cellHeight = max(1, activeSheet.height / rows)
        let row = min(rows - 1, max(0, frame.row))
        let column = min(columns - 1, max(0, frame.column))
        let cropRect = CGRect(
            x: CGFloat(column * cellWidth),
            y: CGFloat(row * cellHeight),
            width: CGFloat(cellWidth),
            height: CGFloat(cellHeight)
        )
        guard let frameImage = cachedFrameImage(
            sheet: activeSheet,
            row: row,
            column: column,
            cellWidth: cellWidth,
            cellHeight: cellHeight,
            cropRect: cropRect
        ) else {
            spriteLayer.contents = nil
            return
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        spriteLayer.contents = frameImage
        CATransaction.commit()
    }

    private func cachedFrameImage(
        sheet: CGImage,
        row: Int,
        column: Int,
        cellWidth: Int,
        cellHeight: Int,
        cropRect: CGRect
    ) -> CGImage? {
        let cacheKey = frameCacheKey(row: row, column: column)
        if let cached = frameCache[cacheKey] {
            return cached
        }
        guard let cropped = sheet.cropping(to: cropRect) else { return nil }

        let rendered = Self.renderFrameImage(cropped, width: cellWidth, height: cellHeight) ?? cropped
        frameCache[cacheKey] = rendered
        return rendered
    }

    private func prewarmFrameCacheAsync(sheetKey: String?) {
        guard let activeSheet, !sequence.isEmpty else { return }
        let cellWidth = max(1, activeSheet.width / columns)
        let cellHeight = max(1, activeSheet.height / rows)
        var seen = Set<String>()
        var jobs: [(key: String, cropRect: CGRect)] = []

        for frame in sequence {
            let row = min(rows - 1, max(0, frame.row))
            let column = min(columns - 1, max(0, frame.column))
            let cacheKey = frameCacheKey(row: row, column: column, sheetKey: sheetKey)
            guard seen.insert(cacheKey).inserted, frameCache[cacheKey] == nil else { continue }
            let cropRect = CGRect(
                x: CGFloat(column * cellWidth),
                y: CGFloat(row * cellHeight),
                width: CGFloat(cellWidth),
                height: CGFloat(cellHeight)
            )
            jobs.append((cacheKey, cropRect))
        }

        guard !jobs.isEmpty else { return }

        DispatchQueue.global(qos: .utility).async {
            let renderedFrames: [(String, CGImage)] = jobs.compactMap { job in
                guard let cropped = activeSheet.cropping(to: job.cropRect) else { return nil }
                let rendered = Self.renderFrameImage(cropped, width: cellWidth, height: cellHeight) ?? cropped
                return (job.key, rendered)
            }

            DispatchQueue.main.async { [weak self] in
                guard
                    let self,
                    self.activeSheetKey == sheetKey
                else {
                    return
                }

                for (key, image) in renderedFrames where self.frameCache[key] == nil {
                    self.frameCache[key] = image
                }
            }
        }
    }

    private static func renderFrameImage(_ image: CGImage, width: Int, height: Int) -> CGImage? {
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard
            let context = CGContext(
                data: nil,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: 0,
                space: colorSpace,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            )
        else {
            return nil
        }

        context.interpolationQuality = .none
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return context.makeImage()
    }

    private func frameCacheKey(
        row: Int,
        column: Int,
        sheetKey: String? = nil
    ) -> String {
        "\(sheetKey ?? activeSheetKey ?? "unknown")|\(row):\(column)"
    }

    private func scheduleNextFrame() {
        timer?.invalidate()
        guard activeSheet != nil, sequence.count > 1 else { return }
        let duration = max(0.04, sequence[frameIndex].duration)
        timer = Timer.scheduledTimer(withTimeInterval: duration, repeats: false) { [weak self] _ in
            guard let self else { return }
            let nextIndex = frameIndex + 1
            frameIndex = nextIndex >= sequence.count ? loopStartIndex : nextIndex
            applyFrame()
            scheduleNextFrame()
        }
        RunLoop.main.add(timer!, forMode: .common)
    }

    private func fileFingerprint(for url: URL) -> String {
        guard
            let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
        else {
            return "unknown"
        }
        let modified = values.contentModificationDate?.timeIntervalSince1970 ?? 0
        let size = values.fileSize ?? 0
        return "\(modified)-\(size)"
    }

    deinit {
        timer?.invalidate()
    }
}
