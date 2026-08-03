#include "ui/PetWindowController.h"

#include "ui/PetViewModel.h"
#include "ui/pet/PetRoamingPlanner.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScreen>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

QRect productionWorkArea(QPoint referencePoint)
{
    QScreen* screen =
        QGuiApplication::screenAt(
            referencePoint);
    if (screen == nullptr) {
        screen =
            QGuiApplication::primaryScreen();
    }
    return screen == nullptr
        ? QRect()
        : screen->availableGeometry();
}

double productionRandomUnit()
{
    return QRandomGenerator::global()
        ->generateDouble();
}

QPoint productionCursorPosition()
{
    return QCursor::pos();
}

constexpr double dragMarginDips = 6.0;
constexpr double dragDirectionThresholdDips = 0.12;
constexpr double dragDirectionReversalHysteresisDips =
    3.0;
constexpr qint64 windowHoverAuthorityMilliseconds = 120;
constexpr int windowsPresentationIntervalMilliseconds = 17;
constexpr double windowsPresentationIntervalSeconds =
    static_cast<double>(
        windowsPresentationIntervalMilliseconds)
    / 1000.0;
constexpr qint64 sourceMotionPrimerMilliseconds = 83;

bool validPixelsPerDip(QPointF pixelsPerDip)
{
    return qIsFinite(pixelsPerDip.x())
        && qIsFinite(pixelsPerDip.y())
        && pixelsPerDip.x() > 0.0
        && pixelsPerDip.y() > 0.0;
}

int horizontalDirectionSign(double dxDips)
{
    if (dxDips < -dragDirectionThresholdDips) {
        return -1;
    }
    if (dxDips > dragDirectionThresholdDips) {
        return 1;
    }
    return 0;
}

bool validDragMonitor(
    const companion::PetWindowController::
        DragMonitor& monitor)
{
    return monitor.availableGeometry.isValid()
        && validPixelsPerDip(
            monitor.pixelsPerDip);
}

QRectF dragOriginBounds(
    const companion::PetWindowController::
        DragMonitor& monitor,
    QSizeF windowSize)
{
    const QRectF workArea =
        monitor.availableGeometry;
    const QPointF margin(
        dragMarginDips
            * monitor.pixelsPerDip.x(),
        dragMarginDips
            * monitor.pixelsPerDip.y());
    const double minimumX =
        workArea.left() + margin.x();
    const double maximumX = std::max(
        minimumX,
        workArea.left()
            + workArea.width()
            - windowSize.width()
            - margin.x());
    const double minimumY =
        workArea.top() + margin.y();
    const double maximumY = std::max(
        minimumY,
        workArea.top()
            + workArea.height()
            - windowSize.height()
            - margin.y());
    return {
        minimumX,
        minimumY,
        maximumX - minimumX,
        maximumY - minimumY,
    };
}

QPointF clampDragOrigin(
    QPointF origin,
    const QRectF& bounds)
{
    return {
        std::clamp(
            origin.x(),
            bounds.left(),
            bounds.left()
                + bounds.width()),
        std::clamp(
            origin.y(),
            bounds.top(),
            bounds.top()
                + bounds.height()),
    };
}

companion::PetWindowController::
    DragCoordinateSpace
logicalDragCoordinateSpace(
    companion::PetWindowController::
        WorkAreaResolver workAreaResolver,
    companion::PetWindowController::
        CursorPositionSource cursorPositionSource)
{
    return {
        [cursorPositionSource =
             std::move(cursorPositionSource)] {
            const QPoint pointer =
                cursorPositionSource
                ? cursorPositionSource()
                : QCursor::pos();
            return QPointF(pointer);
        },
        [](QQuickWindow& window) {
            return QRectF(
                window.position(),
                window.size());
        },
        [workAreaResolver =
             std::move(workAreaResolver)](
            QQuickWindow& window,
            QPointF pointer) {
            const QRect resolved =
                workAreaResolver
                ? workAreaResolver(
                      pointer.toPoint())
                : QRect();
            const QRect fallback =
                window.screen() == nullptr
                ? QRect(
                      0,
                      0,
                      window.width(),
                      window.height())
                : window.screen()
                      ->availableGeometry();
            return companion::
                PetWindowController::
                    DragMonitor {
                        resolved.isValid()
                            ? QRectF(resolved)
                            : QRectF(fallback),
                        {1.0, 1.0},
                    };
        },
        [](QQuickWindow& window,
           QPointF origin) {
            window.setPosition({
                qRound(origin.x()),
                qRound(origin.y()),
            });
        },
        [] {
            return QGuiApplication::mouseButtons()
                .testFlag(Qt::LeftButton);
        },
    };
}

#ifdef Q_OS_WIN

using GetDpiForMonitorFunction =
    HRESULT(WINAPI*)(
        HMONITOR,
        int,
        UINT*,
        UINT*);

GetDpiForMonitorFunction
getDpiForMonitorFunction()
{
    static const auto function = [] {
        HMODULE module =
            GetModuleHandleW(L"Shcore.dll");
        if (module == nullptr) {
            module =
                LoadLibraryW(L"Shcore.dll");
        }
        return module == nullptr
            ? nullptr
            : reinterpret_cast<
                  GetDpiForMonitorFunction>(
                  GetProcAddress(
                      module,
                      "GetDpiForMonitor"));
    }();
    return function;
}

QPointF monitorPixelsPerDip(
    HMONITOR monitor,
    QQuickWindow& window)
{
    UINT dpiX = 96;
    UINT dpiY = 96;
    const auto getDpi =
        getDpiForMonitorFunction();
    if (getDpi != nullptr
        && SUCCEEDED(
            getDpi(
                monitor,
                0,
                &dpiX,
                &dpiY))
        && dpiX > 0
        && dpiY > 0) {
        return {
            static_cast<double>(dpiX)
                / 96.0,
            static_cast<double>(dpiY)
                / 96.0,
        };
    }
    const double fallback =
        std::max(
            1.0,
            static_cast<double>(
                window.devicePixelRatio()));
    return {fallback, fallback};
}

companion::PetWindowController::
    DragCoordinateSpace
productionDragCoordinateSpace()
{
    return {
        [] {
            POINT pointer {};
            if (GetCursorPos(&pointer) == FALSE) {
                return QPointF(
                    QCursor::pos());
            }
            return QPointF(
                pointer.x,
                pointer.y);
        },
        [](QQuickWindow& window) {
            const auto hwnd =
                reinterpret_cast<HWND>(
                    window.winId());
            RECT frame {};
            if (hwnd == nullptr
                || GetWindowRect(
                       hwnd,
                       &frame)
                    == FALSE) {
                return QRectF(
                    window.position(),
                    window.size());
            }
            return QRectF(
                frame.left,
                frame.top,
                frame.right - frame.left,
                frame.bottom - frame.top);
        },
        [](QQuickWindow& window,
           QPointF pointer) {
            const POINT nativePoint {
                qRound(pointer.x()),
                qRound(pointer.y()),
            };
            const HMONITOR monitor =
                MonitorFromPoint(
                    nativePoint,
                    MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo {};
            monitorInfo.cbSize =
                sizeof(monitorInfo);
            if (monitor == nullptr
                || GetMonitorInfoW(
                       monitor,
                       &monitorInfo)
                    == FALSE) {
                const QRect fallback =
                    window.screen() == nullptr
                    ? QRect(
                          0,
                          0,
                          window.width(),
                          window.height())
                    : window.screen()
                          ->availableGeometry();
                const double scale =
                    std::max(
                        1.0,
                        static_cast<double>(
                            window
                                .devicePixelRatio()));
                return companion::
                    PetWindowController::
                        DragMonitor {
                            QRectF(fallback),
                            {scale, scale},
                        };
            }
            const RECT work =
                monitorInfo.rcWork;
            return companion::
                PetWindowController::
                    DragMonitor {
                        QRectF(
                            work.left,
                            work.top,
                            work.right
                                - work.left,
                            work.bottom
                                - work.top),
                        monitorPixelsPerDip(
                            monitor,
                            window),
                    };
        },
        [](QQuickWindow& window,
           QPointF origin) {
            const auto hwnd =
                reinterpret_cast<HWND>(
                    window.winId());
            if (hwnd != nullptr
                && SetWindowPos(
                       hwnd,
                       nullptr,
                       qRound(origin.x()),
                       qRound(origin.y()),
                       0,
                       0,
                       SWP_NOSIZE
                           | SWP_NOZORDER
                           | SWP_NOACTIVATE)
                    != FALSE) {
                return;
            }
            window.setPosition({
                qRound(origin.x()),
                qRound(origin.y()),
            });
        },
        [] {
            return (
                GetAsyncKeyState(VK_LBUTTON)
                & 0x8000)
                != 0;
        },
    };
}

#else

companion::PetWindowController::
    DragCoordinateSpace
productionDragCoordinateSpace()
{
    return logicalDragCoordinateSpace(
        productionWorkArea,
        productionCursorPosition);
}

#endif

} // namespace

namespace companion {

PetWindowController::PetWindowController(
    PetViewModel& model,
    std::optional<QPoint> restoredPosition,
    PositionPersistCommand persistPosition,
    QObject* parent)
    : PetWindowController(
          model,
          restoredPosition,
          std::move(persistPosition),
          productionWorkArea,
          productionRandomUnit,
          productionCursorPosition,
          productionDragCoordinateSpace(),
          parent)
{
}

PetWindowController::PetWindowController(
    PetViewModel& model,
    std::optional<QPoint> restoredPosition,
    PositionPersistCommand persistPosition,
    WorkAreaResolver workAreaResolver,
    UnitRandomSource randomSource,
    CursorPositionSource cursorPositionSource,
    QObject* parent)
    : PetWindowController(
          model,
          restoredPosition,
          std::move(persistPosition),
          workAreaResolver,
          randomSource,
          cursorPositionSource,
          logicalDragCoordinateSpace(
              workAreaResolver,
              cursorPositionSource),
          parent)
{
}

PetWindowController::PetWindowController(
    PetViewModel& model,
    std::optional<QPoint> restoredPosition,
    PositionPersistCommand persistPosition,
    WorkAreaResolver workAreaResolver,
    UnitRandomSource randomSource,
    CursorPositionSource cursorPositionSource,
    DragCoordinateSpace dragCoordinateSpace,
    QObject* parent)
    : QObject(parent),
      model_(model),
      restoredPosition_(restoredPosition),
      persistPosition_(
          std::move(persistPosition)),
      workAreaResolver_(
          std::move(workAreaResolver)),
      randomSource_(std::move(randomSource)),
      cursorPositionSource_(
          std::move(cursorPositionSource)),
      dragCoordinateSpace_(
          std::move(dragCoordinateSpace))
{
    timer_.setInterval(
        windowsPresentationIntervalMilliseconds);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(
        &timer_,
        &QTimer::timeout,
        this,
        [this] {
            advance(
                currentClockMilliseconds());
        });
    dragContinuationTimer_.setInterval(16);
    dragContinuationTimer_.setTimerType(
        Qt::PreciseTimer);
    connect(
        &dragContinuationTimer_,
        &QTimer::timeout,
        this,
        [this] {
            if (!model_.dragging()
                || !dragStartPointer_.has_value()
                || !dragStartOrigin_.has_value()) {
                dragContinuationTimer_.stop();
                return;
            }
            if (!dragPrimaryButtonPressed()) {
                endDrag();
                return;
            }
            dragTo();
        });
}

PetWindowController::~PetWindowController()
{
    stop();
}

void PetWindowController::attachWindow(
    QQuickWindow& window)
{
    window_ = &window;
    const QSizeF windowSize(
        window.width(),
        window.height());
    const QPoint reference =
        restoredPosition_.value_or(
            cursorPositionSource_
            ? cursorPositionSource_()
            : QPoint());
    const QRect workArea =
        workAreaFor(
            reference
            + QPoint(
                window.width() / 2,
                window.height() / 2));
    const QPoint requested =
        restoredPosition_.value_or(
            defaultOrigin(workArea));
    const QPointF clamped =
        PetRoamingPlanner::clamp(
            requested,
            windowSize,
            workArea);
    const QPoint integerOrigin(
        qRound(clamped.x()),
        qRound(clamped.y()));
    window.setPosition(integerOrigin);
    preciseOrigin_ = clamped;
    emit windowPositionChanged(
        integerOrigin);
    if (!restoredPosition_.has_value()
        || integerOrigin != requested) {
        persistCurrentPosition();
    }
}

void PetWindowController::start()
{
    if (timer_.isActive()) {
        return;
    }
    elapsedClock_.start();
    currentNowMilliseconds_ = 0;
    lastTickMilliseconds_ = 0;
    timer_.start();
}

void PetWindowController::stop()
{
    if (timer_.isActive()) {
        timer_.stop();
        persistCurrentPosition();
    }
    clearDragState();
    model_.endDrag();
    clearMovementTarget();
    model_.clearDirectionalLook();
}

void PetWindowController::advance(
    qint64 nowMilliseconds)
{
    currentNowMilliseconds_ =
        nowMilliseconds;
    if (window_.isNull()) {
        return;
    }

    reconcilePointerHover();

    const QPointF actualOrigin =
        window_->position();
    if (preciseOrigin_.has_value()
        && std::hypot(
               preciseOrigin_->x()
                   - actualOrigin.x(),
               preciseOrigin_->y()
                   - actualOrigin.y())
            > 1.5) {
        preciseOrigin_ = actualOrigin;
    }
    const QPointF origin =
        preciseOrigin_.value_or(
            actualOrigin);

    const qint64 previousTick =
        lastTickMilliseconds_;
    lastTickMilliseconds_ =
        nowMilliseconds;

    const bool presentationPaused =
        !sessionActive_
        || !model_.visible()
        || model_.menuOpen()
        || roamingSuspended_;
    if (presentationPaused) {
        clearMovementTarget();
        preciseOrigin_ = actualOrigin;
        model_.setRoamingIdle();
        model_.clearDirectionalLook();
        return;
    }
    if (model_.attentionActive()) {
        clearMovementTarget();
        preciseOrigin_ = actualOrigin;
        model_.setRoamingIdle();
        model_.clearDirectionalLook();
        return;
    }

    if (!model_.allowAutonomousMovement()) {
        clearMovementTarget();
        preciseOrigin_ = actualOrigin;
        model_.setRoamingIdle();
        updateDirectionalLook();
        return;
    }

    if (model_.pointerHovered()
        || model_.controlsHovered()
        || model_.dragging()) {
        clearMovementTarget();
        preciseOrigin_ = actualOrigin;
        model_.setRoamingIdle();
        model_.clearDirectionalLook();
        return;
    }

    if (nowMilliseconds
        < pausedUntilMilliseconds_) {
        preciseOrigin_ = actualOrigin;
        model_.setRoamingIdle();
        updateDirectionalLook();
        return;
    }

    const QRect workArea =
        workAreaFor(
            window_->geometry().center());
    const QSizeF windowSize(
        window_->width(),
        window_->height());
    if (!targetOrigin_.has_value()
        || PetRoamingPlanner::hasArrived(
            origin,
            *targetOrigin_)) {
        model_.setRoamingIdle();
        updateDirectionalLook();
        const double pauseUnit =
            randomSource_
            ? randomSource_()
            : 0.5;
        pausedUntilMilliseconds_ =
            nowMilliseconds
            + PetRoamingPlanner::
                idlePauseMilliseconds(
                    pauseUnit);
        const double targetXUnit =
            randomSource_
            ? randomSource_()
            : 0.5;
        const double targetYUnit =
            randomSource_
            ? randomSource_()
            : 0.5;
        targetOrigin_ =
            PetRoamingPlanner::
                targetFromUnit(
                    workArea,
                    windowSize,
                    targetXUnit,
                    targetYUnit);
        preciseOrigin_ = origin;
        resetMotionPrimer();
        return;
    }

    model_.clearDirectionalLook();
    const double elapsedSeconds =
        previousTick < 0
        ? PetRoamingPlanner::
              tickIntervalSeconds
        : std::min(
              static_cast<double>(
                  nowMilliseconds
                  - previousTick)
                  / 1000.0,
              windowsPresentationIntervalSeconds);
    const QPointF next =
        PetRoamingPlanner::stepToward(
            origin,
            *targetOrigin_,
            elapsedSeconds);
    const QPointF clamped =
        PetRoamingPlanner::clamp(
            next,
            windowSize,
            workArea);
    const bool animationAccepted =
        model_.setRoamingMotion(
            clamped.x() - origin.x(),
            clamped.y() - origin.y());
    if (!shouldMoveAfterAnimation(
            animationAccepted,
            nowMilliseconds)) {
        return;
    }

    preciseOrigin_ = clamped;
    const bool shouldPersist =
        lastPersistMilliseconds_ < 0
        || nowMilliseconds
                - lastPersistMilliseconds_
            >= 1000;
    setWindowOrigin(
        clamped,
        shouldPersist);
    if (shouldPersist) {
        lastPersistMilliseconds_ =
            nowMilliseconds;
    }
    if (std::abs(clamped.x() - next.x())
            > 0.5
        || std::abs(
               clamped.y() - next.y())
            > 0.5) {
        clearMovementTarget();
        preciseOrigin_.reset();
    }
}

void PetWindowController::setSessionActive(
    bool active)
{
    if (sessionActive_ == active) {
        return;
    }
    sessionActive_ = active;
    clearMovementTarget();
    lastTickMilliseconds_ =
        currentNowMilliseconds_;
    if (!sessionActive_) {
        model_.setHoverState(false, false);
        model_.setRoamingIdle();
        model_.clearDirectionalLook();
        return;
    }
    if (!window_.isNull()) {
        const QRect workArea =
            workAreaFor(
                window_->geometry().center());
        setWindowOrigin(
            PetRoamingPlanner::clamp(
                window_->position(),
                window_->size(),
                workArea),
            true);
    }
}

void PetWindowController::setRoamingSuspended(
    bool suspended)
{
    if (roamingSuspended_ == suspended) {
        return;
    }

    roamingSuspended_ = suspended;
    clearMovementTarget();
    lastTickMilliseconds_ =
        currentNowMilliseconds_;
    if (roamingSuspended_) {
        if (!window_.isNull()) {
            preciseOrigin_ =
                window_->position();
        }
        model_.setRoamingIdle();
        model_.clearDirectionalLook();
    }
}

bool PetWindowController::roamingSuspended() const noexcept
{
    return roamingSuspended_;
}

int PetWindowController::
movementTimerIntervalMilliseconds() const noexcept
{
    return timer_.interval();
}

void PetWindowController::beginDrag()
{
    if (window_.isNull()) {
        return;
    }
    pauseBriefly();
    const QPointF pointer =
        dragPointerPosition();
    const QRectF frame =
        dragWindowFrame();
    const DragMonitor monitor =
        dragMonitorAt(pointer);
    dragStartPointer_ = pointer;
    dragStartOrigin_ =
        frame.topLeft();
    const QPointF pixelsPerDip =
        validPixelsPerDip(
            monitor.pixelsPerDip)
        ? monitor.pixelsPerDip
        : QPointF(1.0, 1.0);
    dragGrabOffsetDips_ = {
        (pointer.x() - frame.x())
            / pixelsPerDip.x(),
        (pointer.y() - frame.y())
            / pixelsPerDip.y(),
    };
    dragLastPointer_ = pointer;
    dragMonitor_ =
        validDragMonitor(monitor)
        ? std::optional<DragMonitor>(
              monitor)
        : std::nullopt;
    dragDirectionSign_ = 0;
    dragDirectionReversalDistanceDips_ = 0.0;
    preciseOrigin_ =
        window_->position();
    model_.beginDrag();
    if (dragCoordinateSpace_
            .primaryButtonPressed) {
        dragContinuationTimer_.start();
    }
}

void PetWindowController::dragTo()
{
    if (window_.isNull()
        || !dragStartPointer_.has_value()
        || !dragStartOrigin_.has_value()) {
        return;
    }
    const QPointF pointer =
        dragPointerPosition();
    const QRectF frame =
        dragWindowFrame();
    const DragMonitor pointerMonitor =
        dragMonitorAt(pointer);
    const QPointF previousPointer =
        dragLastPointer_.value_or(
            *dragStartPointer_);
    const QPointF frameDelta =
        pointer - previousPointer;
    const QPointF totalDelta =
        pointer - *dragStartPointer_;
    const QPointF pixelsPerDip =
        validPixelsPerDip(
            pointerMonitor.pixelsPerDip)
        ? pointerMonitor.pixelsPerDip
        : QPointF(1.0, 1.0);
    const double frameDxDips =
        frameDelta.x()
        / pixelsPerDip.x();
    const double totalDxDips =
        totalDelta.x()
        / pixelsPerDip.x();
    const int frameDirectionSign =
        horizontalDirectionSign(frameDxDips);
    double directionDxDips = 0.0;
    if (dragDirectionSign_ == 0) {
        directionDxDips =
            frameDirectionSign != 0
            ? frameDxDips
            : totalDxDips;
        dragDirectionSign_ =
            horizontalDirectionSign(
                directionDxDips);
    } else if (frameDirectionSign == 0) {
        directionDxDips = 0.0;
    } else if (frameDirectionSign
               == dragDirectionSign_) {
        dragDirectionReversalDistanceDips_ =
            0.0;
        directionDxDips = frameDxDips;
    } else {
        dragDirectionReversalDistanceDips_ +=
            std::abs(frameDxDips);
        if (dragDirectionReversalDistanceDips_
                >= dragDirectionReversalHysteresisDips) {
            dragDirectionSign_ =
                frameDirectionSign;
            dragDirectionReversalDistanceDips_ =
                0.0;
            directionDxDips = frameDxDips;
        }
    }
    model_.updateDrag(
        directionDxDips,
        frameDelta.y()
            / pixelsPerDip.y());
    dragLastPointer_ = pointer;

    const QPointF next =
        nextDragOrigin(
            pointer,
            frame,
            pointerMonitor);
    moveDragWindow(next);
}

void PetWindowController::endDrag()
{
    if (window_.isNull()
        || (!dragStartPointer_.has_value()
            && !dragStartOrigin_.has_value()
            && !model_.dragging())) {
        return;
    }
    dragContinuationTimer_.stop();
    clearDragState();
    model_.endDrag();
    preciseOrigin_ =
        window_->position();
    pauseBriefly();
    persistCurrentPosition();
}

QRect PetWindowController::availableWorkAreaAt(
    QPoint referencePoint) const
{
    return workAreaFor(referencePoint);
}

void PetWindowController::pauseBriefly()
{
    pausedUntilMilliseconds_ =
        currentClockMilliseconds()
        + PetRoamingPlanner::
            postDragPauseMilliseconds;
    clearMovementTarget();
}

void PetWindowController::publishHoverState(
    bool pointerHovered,
    bool controlsHovered)
{
    windowHoverAuthorityUntilMilliseconds_ =
        currentClockMilliseconds()
        + windowHoverAuthorityMilliseconds;
    model_.setHoverState(
        pointerHovered,
        controlsHovered);
}

QRect PetWindowController::workAreaFor(
    QPoint referencePoint) const
{
    const QRect resolved =
        workAreaResolver_
        ? workAreaResolver_(
              referencePoint)
        : QRect();
    return resolved.isValid()
        ? resolved
        : QRect(
              0,
              0,
              qMax(
                  124,
                  window_.isNull()
                      ? 124
                      : window_->width()),
              qMax(
                  164,
                  window_.isNull()
                      ? 164
                      : window_->height()));
}

QPoint PetWindowController::defaultOrigin(
    const QRect& workArea) const
{
    const int width =
        window_.isNull()
        ? 124
        : window_->width();
    const int height =
        window_.isNull()
        ? 164
        : window_->height();
    return {
        workArea.left()
            + workArea.width()
            - width
            - 12,
        workArea.top()
            + workArea.height()
            - height
            - 12,
    };
}

void PetWindowController::
reconcilePointerHover()
{
    if (window_.isNull()
        || model_.dragging()
        || currentClockMilliseconds()
            < windowHoverAuthorityUntilMilliseconds_) {
        return;
    }
    const QPoint pointer =
        cursorPositionSource_
        ? cursorPositionSource_()
        : QPoint();
    const QPoint localPointer =
        window_->mapFromGlobal(pointer);
    const QRect petHitRegion(
        12,
        48,
        100,
        108);
    const int controlsWidth =
        model_.menuOpen() ? 116 : 36;
    const QRect controlsHitRegion(
        window_->width()
            - 4
            - controlsWidth,
        8,
        controlsWidth,
        48);
    const bool tracksControls =
        model_.controlsVisible();
    const bool petHovered =
        petHitRegion.contains(localPointer);
    const bool controlsHovered =
        tracksControls
        && controlsHitRegion.contains(localPointer);
    model_.setHoverState(
        petHovered,
        controlsHovered);
}

void PetWindowController::
updateDirectionalLook()
{
    if (window_.isNull()) {
        model_.clearDirectionalLook();
        return;
    }
    const QPoint pointer =
        cursorPositionSource_
        ? cursorPositionSource_()
        : QPoint();
    model_.updateDirectionalLook(
        pointer,
        window_->geometry());
}

void PetWindowController::
clearMovementTarget()
{
    targetOrigin_.reset();
    resetMotionPrimer();
}

void PetWindowController::resetMotionPrimer()
{
    motionPrimedAtMilliseconds_.reset();
}

bool PetWindowController::
shouldMoveAfterAnimation(
    bool animationAccepted,
    qint64 nowMilliseconds)
{
    if (!animationAccepted) {
        resetMotionPrimer();
        return false;
    }
    if (!motionPrimedAtMilliseconds_) {
        motionPrimedAtMilliseconds_ =
            nowMilliseconds;
        return false;
    }
    return nowMilliseconds
            - *motionPrimedAtMilliseconds_
        >= sourceMotionPrimerMilliseconds;
}

QPointF PetWindowController::
dragPointerPosition() const
{
    if (dragCoordinateSpace_
            .pointerPosition) {
        const QPointF pointer =
            dragCoordinateSpace_
                .pointerPosition();
        if (qIsFinite(pointer.x())
            && qIsFinite(pointer.y())) {
            return pointer;
        }
    }
    const QPoint fallback =
        cursorPositionSource_
        ? cursorPositionSource_()
        : QCursor::pos();
    return QPointF(fallback);
}

bool PetWindowController::
dragPrimaryButtonPressed() const
{
    return dragCoordinateSpace_
               .primaryButtonPressed
        && dragCoordinateSpace_
               .primaryButtonPressed();
}

QRectF PetWindowController::
dragWindowFrame() const
{
    if (window_.isNull()) {
        return {};
    }
    if (dragCoordinateSpace_
            .windowFrame) {
        const QRectF frame =
            dragCoordinateSpace_
                .windowFrame(*window_);
        if (frame.isValid()) {
            return frame;
        }
    }
    return QRectF(
        window_->position(),
        window_->size());
}

PetWindowController::DragMonitor
PetWindowController::dragMonitorAt(
    QPointF pointer) const
{
    if (!window_.isNull()
        && dragCoordinateSpace_.monitorAt) {
        const DragMonitor monitor =
            dragCoordinateSpace_.monitorAt(
                *window_,
                pointer);
        if (validDragMonitor(monitor)) {
            return monitor;
        }
    }
    return {
        QRectF(
            workAreaFor(
                pointer.toPoint())),
        {1.0, 1.0},
    };
}

QSizeF PetWindowController::
dragWindowSizeFor(
    const DragMonitor& monitor) const
{
    if (window_.isNull()) {
        return {};
    }
    const QPointF pixelsPerDip =
        validPixelsPerDip(
            monitor.pixelsPerDip)
        ? monitor.pixelsPerDip
        : QPointF(1.0, 1.0);
    return {
        std::max(
            1.0,
            static_cast<double>(
                window_->width())
                * pixelsPerDip.x()),
        std::max(
            1.0,
            static_cast<double>(
                window_->height())
                * pixelsPerDip.y()),
    };
}

QPointF PetWindowController::
nextDragOrigin(
    QPointF pointer,
    QRectF currentFrame,
    const DragMonitor& pointerMonitor)
{
    const DragMonitor resolvedMonitor =
        validDragMonitor(pointerMonitor)
        ? pointerMonitor
        : dragMonitor_.value_or(
              DragMonitor {
                  QRectF(
                      workAreaFor(
                          pointer.toPoint())),
                   {1.0, 1.0},
               });
    const QPointF proposed =
        dragGrabOffsetDips_.has_value()
        ? QPointF(
              pointer.x()
                  - dragGrabOffsetDips_->x()
                      * resolvedMonitor
                            .pixelsPerDip.x(),
              pointer.y()
                  - dragGrabOffsetDips_->y()
                      * resolvedMonitor
                            .pixelsPerDip.y())
        : *dragStartOrigin_
              + pointer
              - *dragStartPointer_;
    const bool staysOnCurrentMonitor =
        dragMonitor_.has_value()
        && dragMonitor_->availableGeometry
            == resolvedMonitor
                   .availableGeometry;
    const QSizeF windowSize =
        staysOnCurrentMonitor
            && currentFrame.size().isValid()
        ? currentFrame.size()
        : dragWindowSizeFor(
              resolvedMonitor);
    dragMonitor_ = resolvedMonitor;
    return clampDragOrigin(
        proposed,
        dragOriginBounds(
            resolvedMonitor,
            windowSize));
}

void PetWindowController::moveDragWindow(
    QPointF origin)
{
    if (window_.isNull()) {
        return;
    }
    const QRectF currentFrame =
        dragWindowFrame();
    if (std::hypot(
            currentFrame.x() - origin.x(),
            currentFrame.y() - origin.y())
        <= 0.25) {
        return;
    }
    if (dragCoordinateSpace_
            .moveWindow) {
        dragCoordinateSpace_.moveWindow(
            *window_,
            origin);
    } else {
        window_->setPosition({
            qRound(origin.x()),
            qRound(origin.y()),
        });
    }
    preciseOrigin_ =
        window_->position();
    emit windowPositionChanged(
        window_->position());
}

void PetWindowController::clearDragState()
{
    dragContinuationTimer_.stop();
    dragStartPointer_.reset();
    dragStartOrigin_.reset();
    dragGrabOffsetDips_.reset();
    dragLastPointer_.reset();
    dragMonitor_.reset();
    dragDirectionSign_ = 0;
    dragDirectionReversalDistanceDips_ = 0.0;
}

void PetWindowController::setWindowOrigin(
    QPointF origin,
    bool persist)
{
    if (window_.isNull()) {
        return;
    }
    const QPoint position(
        qRound(origin.x()),
        qRound(origin.y()));
    if (window_->position() != position) {
        window_->setPosition(position);
        emit windowPositionChanged(
            position);
    }
    if (persist) {
        persistCurrentPosition();
    }
}

void PetWindowController::
persistCurrentPosition()
{
    if (window_.isNull()
        || !persistPosition_) {
        return;
    }
    const auto persisted =
        persistPosition_(
            window_->position());
    if (!persisted.hasValue()) {
        emit runtimeErrorOccurred(
            persisted.error());
    }
}

qint64 PetWindowController::
currentClockMilliseconds() const
{
    return elapsedClock_.isValid()
        ? elapsedClock_.elapsed()
        : currentNowMilliseconds_;
}

} // namespace companion
