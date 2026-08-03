#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>

namespace companion {

class PetRoamingPlanner final {
public:
    static constexpr double speedDipsPerSecond = 54.0;
    static constexpr double tickIntervalSeconds =
        1.0 / 12.0;
    static constexpr double targetInsetDips = 12.0;
    static constexpr double clampMarginDips = 6.0;
    static constexpr double arrivalThresholdDips = 4.0;
    static constexpr int postDragPauseMilliseconds =
        1500;

    static QPointF clamp(
        QPointF origin,
        QSizeF windowSize,
        QRectF workArea,
        double margin = clampMarginDips) noexcept;
    static QPointF stepToward(
        QPointF origin,
        QPointF target,
        double elapsedSeconds,
        double pointsPerSecond =
            speedDipsPerSecond) noexcept;
    static bool hasArrived(
        QPointF origin,
        QPointF target) noexcept;
    static QPointF targetFromUnit(
        QRectF workArea,
        QSizeF windowSize,
        double unitX,
        double unitY) noexcept;
    static int idlePauseMilliseconds(
        double unit) noexcept;
};

} // namespace companion
