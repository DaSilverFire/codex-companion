#include "ui/pet/PetMenuPlacement.h"

#include <QtGlobal>

#include <algorithm>

namespace companion {

QRect PetMenuPlacement::anchorFrame(
    const QRect& petFrame,
    bool controlsVisible)
{
    const int topDrop =
        trayBaselineDrop
        + (controlsVisible
               ? 0
               : hiddenControlsDrop);
    return {
        petFrame.x(),
        petFrame.y() + topDrop,
        petFrame.width(),
        std::max(0, petFrame.height() - topDrop),
    };
}

QPoint PetMenuPlacement::positionedOrigin(
    const QRect& anchorFrame,
    const QSize& traySize,
    const QRect& availableFrame)
{
    const int minimumX =
        availableFrame.x() + screenMargin;
    const int minimumY =
        availableFrame.y() + screenMargin;
    const int maximumX = std::max(
        minimumX,
        availableFrame.x()
            + availableFrame.width()
            - traySize.width()
            - screenMargin);
    const int maximumY = std::max(
        minimumY,
        availableFrame.y()
            + availableFrame.height()
            - traySize.height()
            - screenMargin);
    const int centeredX = qRound(
        anchorFrame.x()
        + anchorFrame.width() / 2.0
        - traySize.width() / 2.0);
    const int centeredY = qRound(
        anchorFrame.y()
        + anchorFrame.height() / 2.0
        - traySize.height() / 2.0);
    const int clampedX =
        qBound(minimumX, centeredX, maximumX);
    const int clampedY =
        qBound(minimumY, centeredY, maximumY);

    const int aboveY =
        anchorFrame.y()
        - traySize.height()
        - trayGap;
    if (aboveY >= minimumY) {
        return {clampedX, aboveY};
    }

    const int leftX =
        anchorFrame.x()
        - traySize.width()
        - trayGap;
    if (leftX >= minimumX) {
        return {leftX, clampedY};
    }

    const int rightX =
        anchorFrame.x()
        + anchorFrame.width()
        + trayGap;
    if (rightX + traySize.width()
        <= availableFrame.x()
            + availableFrame.width()
            - screenMargin) {
        return {rightX, clampedY};
    }

    const int belowY =
        anchorFrame.y()
        + anchorFrame.height()
        + trayGap;
    if (belowY + traySize.height()
        <= availableFrame.y()
            + availableFrame.height()
            - screenMargin) {
        return {clampedX, belowY};
    }

    return {clampedX, clampedY};
}

QPoint PetMenuPlacement::positionedAuxiliaryOrigin(
    const QRect& primaryFrame,
    const QSize& auxiliarySize,
    const QRect& availableFrame)
{
    const int minimumX =
        availableFrame.x() + screenMargin;
    const int minimumY =
        availableFrame.y() + screenMargin;
    const int maximumX = std::max(
        minimumX,
        availableFrame.right()
            - auxiliarySize.width()
            - screenMargin
            + 1);
    const int maximumY = std::max(
        minimumY,
        availableFrame.bottom()
            - auxiliarySize.height()
            - screenMargin
            + 1);
    const int centeredX = qRound(
        primaryFrame.x()
        + primaryFrame.width() / 2.0
        - auxiliarySize.width() / 2.0);
    const int centeredY = qRound(
        primaryFrame.y()
        + primaryFrame.height() / 2.0
        - auxiliarySize.height() / 2.0);
    const int clampedX =
        qBound(minimumX, centeredX, maximumX);
    const int clampedY =
        qBound(minimumY, centeredY, maximumY);

    const int leftX =
        primaryFrame.x()
        - auxiliarySize.width()
        - trayGap;
    if (leftX >= minimumX) {
        return {leftX, clampedY};
    }

    const int rightX =
        primaryFrame.right()
        + trayGap
        + 1;
    if (rightX + auxiliarySize.width()
        <= availableFrame.right()
            - screenMargin
            + 1) {
        return {rightX, clampedY};
    }

    const int aboveY =
        primaryFrame.y()
        - auxiliarySize.height()
        - trayGap;
    if (aboveY >= minimumY) {
        return {clampedX, aboveY};
    }

    const int belowY =
        primaryFrame.bottom()
        + trayGap
        + 1;
    if (belowY + auxiliarySize.height()
        <= availableFrame.bottom()
            - screenMargin
            + 1) {
        return {clampedX, belowY};
    }

    return {clampedX, clampedY};
}

QPoint PetMenuPlacement::positionedAttentionOrigin(
    const QRect& petFrame,
    const QSize& attentionSize,
    const QRect& availableFrame)
{
    constexpr int attentionGap = 5;

    const int minimumX =
        availableFrame.x() + screenMargin;
    const int minimumY =
        availableFrame.y() + screenMargin;
    const int maximumX = std::max(
        minimumX,
        availableFrame.right()
            - attentionSize.width()
            - screenMargin
            + 1);
    const int maximumY = std::max(
        minimumY,
        availableFrame.bottom()
            - attentionSize.height()
            - screenMargin
            + 1);
    const int centeredX = qRound(
        petFrame.x()
        + petFrame.width() / 2.0
        - attentionSize.width() / 2.0);
    const int clampedX =
        qBound(minimumX, centeredX, maximumX);

    const int aboveY =
        petFrame.y()
        - attentionSize.height()
        - attentionGap;
    if (aboveY >= minimumY) {
        return {clampedX, aboveY};
    }

    const int belowY =
        petFrame.bottom()
        + attentionGap
        + 1;
    if (belowY + attentionSize.height()
        <= availableFrame.bottom()
            - screenMargin
            + 1) {
        return {clampedX, belowY};
    }

    return {
        clampedX,
        qBound(minimumY, belowY, maximumY),
    };
}

} // namespace companion
