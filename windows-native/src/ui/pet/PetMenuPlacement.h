#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace companion {

class PetMenuPlacement final
{
public:
    static constexpr int trayGap = 10;
    static constexpr int screenMargin = 8;
    static constexpr int trayBaselineDrop = 18;
    static constexpr int hiddenControlsDrop = 28;

    static QRect anchorFrame(
        const QRect& petFrame,
        bool controlsVisible);

    static QPoint positionedOrigin(
        const QRect& anchorFrame,
        const QSize& traySize,
        const QRect& availableFrame);

    static QPoint positionedAuxiliaryOrigin(
        const QRect& primaryFrame,
        const QSize& auxiliarySize,
        const QRect& availableFrame);

    static QPoint positionedAttentionOrigin(
        const QRect& petFrame,
        const QSize& attentionSize,
        const QRect& availableFrame);
};

} // namespace companion
