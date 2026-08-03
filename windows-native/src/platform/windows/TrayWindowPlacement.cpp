#include "platform/windows/TrayWindowPlacement.h"

#include <QtGlobal>

namespace {

int boundedCoordinate(int value, int minimum, int maximum)
{
    if (maximum < minimum) {
        return minimum;
    }
    return qBound(minimum, value, maximum);
}

} // namespace

namespace companion {

QPoint TrayWindowPlacement::nearAnchor(
    QPoint anchor,
    QRect availableGeometry,
    QSize windowSize,
    int margin)
{
    if (!availableGeometry.isValid()
        || windowSize.width() <= 0
        || windowSize.height() <= 0) {
        return availableGeometry.topLeft();
    }

    const int requestedX = anchor.x() - windowSize.width();
    const bool anchorIsAboveCenter =
        anchor.y() <= availableGeometry.center().y();
    const int requestedY = anchorIsAboveCenter
        ? anchor.y() + margin
        : anchor.y() - windowSize.height() - margin;

    const int maximumX =
        availableGeometry.right() - windowSize.width() + 1;
    const int maximumY =
        availableGeometry.bottom() - windowSize.height() + 1;
    return {
        boundedCoordinate(
            requestedX,
            availableGeometry.left(),
            maximumX),
        boundedCoordinate(
            requestedY,
            availableGeometry.top(),
            maximumY),
    };
}

} // namespace companion
