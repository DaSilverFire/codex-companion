#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace companion {

class CompanionPresentationPolicy final {
public:
    static bool showsPetMenuControls(
        bool menuOpen,
        bool hidesUntilHover,
        bool petHovered,
        bool menuControlHovered) noexcept;

    static bool acceptsRoamingMotion(
        bool petHovered,
        bool petDragging,
        bool hasInteractionAnimation) noexcept;

    static QPoint settingsWindowOrigin(
        const QRect& availableWorkArea,
        const QSize& windowSize,
        const QRect& petFrame,
        int margin = 16,
        int gap = 16) noexcept;
};

} // namespace companion
