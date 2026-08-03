#include "ui/CompanionPresentationPolicy.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {

int clampedCoordinate(
    int candidate,
    int availableStart,
    int availableExtent,
    int windowExtent,
    int margin) noexcept
{
    const int minimum = availableStart + margin;
    const int maximum =
        availableStart + availableExtent
        - margin - windowExtent;
    if (maximum < minimum) {
        return availableStart
            + (availableExtent - windowExtent) / 2;
    }
    return std::clamp(candidate, minimum, maximum);
}

QPoint clampedOrigin(
    QPoint candidate,
    const QRect& availableWorkArea,
    const QSize& windowSize,
    int margin) noexcept
{
    return {
        clampedCoordinate(
            candidate.x(),
            availableWorkArea.x(),
            availableWorkArea.width(),
            windowSize.width(),
            margin),
        clampedCoordinate(
            candidate.y(),
            availableWorkArea.y(),
            availableWorkArea.height(),
            windowSize.height(),
            margin),
    };
}

qint64 squaredDistance(QPoint first, QPoint second) noexcept
{
    const qint64 deltaX =
        static_cast<qint64>(first.x())
        - second.x();
    const qint64 deltaY =
        static_cast<qint64>(first.y())
        - second.y();
    return deltaX * deltaX + deltaY * deltaY;
}

} // namespace

namespace companion {

bool CompanionPresentationPolicy::showsPetMenuControls(
    bool menuOpen,
    bool hidesUntilHover,
    bool petHovered,
    bool menuControlHovered) noexcept
{
    if (menuOpen) {
        return petHovered
            || menuControlHovered;
    }
    return !hidesUntilHover
        || petHovered
        || menuControlHovered;
}

bool CompanionPresentationPolicy::acceptsRoamingMotion(
    bool petHovered,
    bool petDragging,
    bool hasInteractionAnimation) noexcept
{
    return !petHovered
        && !petDragging
        && !hasInteractionAnimation;
}

QPoint CompanionPresentationPolicy::settingsWindowOrigin(
    const QRect& availableWorkArea,
    const QSize& windowSize,
    const QRect& petFrame,
    int margin,
    int gap) noexcept
{
    if (!availableWorkArea.isValid()
        || !windowSize.isValid()
        || windowSize.isEmpty()) {
        return availableWorkArea.topLeft();
    }

    margin = std::max(0, margin);
    gap = std::max(0, gap);
    const QPoint centered = clampedOrigin(
        {
            availableWorkArea.x()
                + (availableWorkArea.width()
                   - windowSize.width()) / 2,
            availableWorkArea.y()
                + (availableWorkArea.height()
                   - windowSize.height()) / 2,
        },
        availableWorkArea,
        windowSize,
        margin);
    if (!petFrame.isValid()
        || !QRect(centered, windowSize)
                .intersects(petFrame)) {
        return centered;
    }

    const std::array candidates = {
        QPoint(
            petFrame.x() - gap - windowSize.width(),
            centered.y()),
        QPoint(
            petFrame.x() + petFrame.width() + gap,
            centered.y()),
        QPoint(
            centered.x(),
            petFrame.y() - gap - windowSize.height()),
        QPoint(
            centered.x(),
            petFrame.y() + petFrame.height() + gap),
    };

    QPoint best = centered;
    qint64 bestDistance =
        std::numeric_limits<qint64>::max();
    for (const QPoint candidate : candidates) {
        const QPoint clamped = clampedOrigin(
            candidate,
            availableWorkArea,
            windowSize,
            margin);
        if (QRect(clamped, windowSize)
                .intersects(petFrame)) {
            continue;
        }

        const qint64 distance =
            squaredDistance(clamped, centered);
        if (distance < bestDistance) {
            best = clamped;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace companion
