#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace companion {

class TrayWindowPlacement final {
public:
    static QPoint nearAnchor(
        QPoint anchor,
        QRect availableGeometry,
        QSize windowSize,
        int margin);
};

} // namespace companion
