#include "ui/pet/PetDirectionalLook.h"

#include <cmath>
#include <numbers>

namespace companion {

std::optional<PetDirectionalLookFrame>
PetDirectionalLook::resolve(
    QPointF pointer,
    QRectF petFrame,
    int startRow) noexcept
{
    const QPointF center = petFrame.center();
    const double dx =
        pointer.x() - center.x();
    const double screenDeltaY =
        pointer.y() - center.y();
    const double distance =
        std::hypot(dx, screenDeltaY);
    const double proximityRadius =
        std::hypot(
            petFrame.width(),
            petFrame.height());
    if (distance <= 1.0
        || distance > proximityRadius) {
        return std::nullopt;
    }

    const double upwardDeltaY =
        -screenDeltaY;
    double angle =
        std::atan2(dx, upwardDeltaY)
        * 180.0
        / std::numbers::pi;
    if (angle < 0.0) {
        angle += 360.0;
    }
    const int directionIndex =
        static_cast<int>(
            std::floor(
                (angle + 11.25)
                / 22.5))
        % 16;
    return PetDirectionalLookFrame {
        startRow + directionIndex / 8,
        directionIndex % 8,
    };
}

} // namespace companion
