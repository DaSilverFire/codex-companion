import CoreGraphics
import Foundation

struct PetDirectionalLookFrame: Equatable, Hashable {
    var row: Int
    var column: Int

    static func resolve(
        pointer: CGPoint,
        petFrame: CGRect,
        startRow: Int
    ) -> PetDirectionalLookFrame? {
        let dx = pointer.x - petFrame.midX
        let dy = pointer.y - petFrame.midY
        let distance = hypot(dx, dy)
        let proximityRadius = hypot(petFrame.width, petFrame.height)
        guard distance > 1, distance <= proximityRadius else { return nil }

        var angle = atan2(dx, dy) * 180 / .pi
        if angle < 0 {
            angle += 360
        }
        let directionIndex = Int((angle / 22.5).rounded()) % 16
        return PetDirectionalLookFrame(
            row: startRow + directionIndex / 8,
            column: directionIndex % 8
        )
    }
}
