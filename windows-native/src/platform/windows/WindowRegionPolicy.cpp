#include "platform/windows/WindowRegionPolicy.h"

#include <QQuickWindow>
#include <QVariant>
#include <QtMath>
#include <algorithm>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

companion::CompanionError regionError(
    QString code,
    QString message,
    DWORD nativeError = ERROR_SUCCESS)
{
    QVariantMap context;
    if (nativeError != ERROR_SUCCESS) {
        context.insert(
            QStringLiteral("win32Error"),
            QVariant::fromValue<qulonglong>(
                nativeError));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

int scaledPixel(qreal logicalValue, qreal devicePixelRatio)
{
    return std::max(
        0,
        qRound(
            logicalValue
            * devicePixelRatio));
}

qreal numericProperty(
    const QQuickWindow& window,
    const char* name)
{
    return window.property(name).toDouble();
}

} // namespace

namespace companion {

WindowRegionGeometry WindowRegionPolicy::geometryFor(
    QSize logicalWindowSize,
    QMarginsF logicalInsets,
    qreal logicalRadius,
    qreal devicePixelRatio)
{
    const qreal scale =
        std::max<qreal>(
            0.01,
            devicePixelRatio);
    const int physicalWidth =
        scaledPixel(
            logicalWindowSize.width(),
            scale);
    const int physicalHeight =
        scaledPixel(
            logicalWindowSize.height(),
            scale);
    const int left =
        scaledPixel(
            logicalInsets.left(),
            scale);
    const int top =
        scaledPixel(
            logicalInsets.top(),
            scale);
    const int right =
        scaledPixel(
            logicalInsets.right(),
            scale);
    const int bottom =
        scaledPixel(
            logicalInsets.bottom(),
            scale);
    const int ellipse =
        scaledPixel(
            logicalRadius * 2,
            scale);

    return {
        QRect(
            left,
            top,
            std::max(
                0,
                physicalWidth
                    - left
                    - right),
            std::max(
                0,
                physicalHeight
                    - top
                    - bottom)),
        QSize(
            ellipse,
            ellipse),
    };
}

Result<void> WindowRegionPolicy::apply(
    QQuickWindow& window)
{
    const auto hwnd =
        reinterpret_cast<HWND>(
            window.winId());
    if (hwnd == nullptr) {
        return Result<void>::failure(
            regionError(
                QStringLiteral(
                    "window.region-handle-unavailable"),
                QStringLiteral(
                    "Could not apply the Companion window shape before its native surface was created.")));
    }

    const bool enabled =
        window.property(
            "nativeBackdropRegionEnabled")
            .toBool();
    if (!enabled) {
        if (SetWindowRgn(
                hwnd,
                nullptr,
                TRUE)
            == 0) {
            return Result<void>::failure(
                regionError(
                    QStringLiteral(
                        "window.region-clear-failed"),
                    QStringLiteral(
                        "Could not clear the Companion window shape."),
                    GetLastError()));
        }
        return Result<void>::success();
    }

    const WindowRegionGeometry geometry =
        geometryFor(
            window.size(),
            QMarginsF(
                numericProperty(
                    window,
                    "nativeBackdropRegionInsetLeft"),
                numericProperty(
                    window,
                    "nativeBackdropRegionInsetTop"),
                numericProperty(
                    window,
                    "nativeBackdropRegionInsetRight"),
                numericProperty(
                    window,
                    "nativeBackdropRegionInsetBottom")),
            numericProperty(
                window,
                "nativeBackdropRegionRadius"),
            window.devicePixelRatio());
    if (geometry.bounds.isEmpty()) {
        return Result<void>::failure(
            regionError(
                QStringLiteral(
                    "window.region-empty"),
                QStringLiteral(
                    "The Companion window shape has no visible area.")));
    }

    const int right =
        geometry.bounds.x()
        + geometry.bounds.width()
        + 1;
    const int bottom =
        geometry.bounds.y()
        + geometry.bounds.height()
        + 1;
    HRGN region =
        geometry.ellipse.isEmpty()
        ? CreateRectRgn(
              geometry.bounds.x(),
              geometry.bounds.y(),
              right,
              bottom)
        : CreateRoundRectRgn(
              geometry.bounds.x(),
              geometry.bounds.y(),
              right,
              bottom,
              geometry.ellipse.width(),
              geometry.ellipse.height());
    if (region == nullptr) {
        return Result<void>::failure(
            regionError(
                QStringLiteral(
                    "window.region-create-failed"),
                QStringLiteral(
                    "Could not create the Companion window shape."),
                GetLastError()));
    }

    if (SetWindowRgn(
            hwnd,
            region,
            TRUE)
        == 0) {
        const DWORD nativeError =
            GetLastError();
        DeleteObject(region);
        return Result<void>::failure(
            regionError(
                QStringLiteral(
                    "window.region-apply-failed"),
                QStringLiteral(
                    "Could not apply the Companion window shape."),
                nativeError));
    }

    return Result<void>::success();
}

} // namespace companion
