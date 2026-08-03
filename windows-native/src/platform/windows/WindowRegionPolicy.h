#pragma once

#include "core/Result.h"

#include <QMarginsF>
#include <QRect>
#include <QSize>

class QQuickWindow;

namespace companion {

struct WindowRegionGeometry final {
    QRect bounds;
    QSize ellipse;

    friend bool operator==(
        const WindowRegionGeometry&,
        const WindowRegionGeometry&) = default;
};

class WindowRegionPolicy final {
public:
    static WindowRegionGeometry geometryFor(
        QSize logicalWindowSize,
        QMarginsF logicalInsets,
        qreal logicalRadius,
        qreal devicePixelRatio);

    static Result<void> apply(
        QQuickWindow& window);
};

} // namespace companion
