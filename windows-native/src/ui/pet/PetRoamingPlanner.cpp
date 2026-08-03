#include "ui/pet/PetRoamingPlanner.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace companion {

QPointF PetRoamingPlanner::clamp(
    QPointF origin,
    QSizeF windowSize,
    QRectF workArea,
    double margin) noexcept
{
    const double minimumX =
        workArea.left() + margin;
    const double maximumX = std::max(
        minimumX,
        workArea.left() + workArea.width()
            - windowSize.width() - margin);
    const double minimumY =
        workArea.top() + margin;
    const double maximumY = std::max(
        minimumY,
        workArea.top() + workArea.height()
            - windowSize.height() - margin);
    return {
        std::clamp(
            origin.x(),
            minimumX,
            maximumX),
        std::clamp(
            origin.y(),
            minimumY,
            maximumY),
    };
}

QPointF PetRoamingPlanner::stepToward(
    QPointF origin,
    QPointF target,
    double elapsedSeconds,
    double pointsPerSecond) noexcept
{
    const double dx = target.x() - origin.x();
    const double dy = target.y() - origin.y();
    const double length =
        std::max(0.001, std::hypot(dx, dy));
    const double step =
        pointsPerSecond
        * std::clamp(
            elapsedSeconds,
            1.0 / 60.0,
            1.0 / 8.0);
    return {
        origin.x() + (dx / length * step),
        origin.y() + (dy / length * step),
    };
}

bool PetRoamingPlanner::hasArrived(
    QPointF origin,
    QPointF target) noexcept
{
    return std::hypot(
               target.x() - origin.x(),
               target.y() - origin.y())
        < arrivalThresholdDips;
}

QPointF PetRoamingPlanner::targetFromUnit(
    QRectF workArea,
    QSizeF windowSize,
    double unitX,
    double unitY) noexcept
{
    const double minimumX =
        workArea.left() + targetInsetDips;
    const double maximumX = std::max(
        minimumX,
        workArea.left() + workArea.width()
            - windowSize.width()
            - targetInsetDips);
    const double minimumY =
        workArea.top() + targetInsetDips;
    const double maximumY = std::max(
        minimumY,
        workArea.top() + workArea.height()
            - windowSize.height()
            - targetInsetDips);
    return {
        minimumX
            + (maximumX - minimumX)
                * std::clamp(unitX, 0.0, 1.0),
        minimumY
            + (maximumY - minimumY)
                * std::clamp(unitY, 0.0, 1.0),
    };
}

int PetRoamingPlanner::idlePauseMilliseconds(
    double unit) noexcept
{
    return qRound(
        450.0
        + std::clamp(unit, 0.0, 1.0)
            * 750.0);
}

} // namespace companion
