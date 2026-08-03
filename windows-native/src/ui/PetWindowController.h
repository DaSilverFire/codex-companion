#pragma once

#include "core/Result.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QTimer>
#include <functional>
#include <optional>

class QQuickWindow;

namespace companion {

class PetViewModel;

class PetWindowController final : public QObject {
    Q_OBJECT

public:
    struct DragMonitor final {
        QRectF availableGeometry;
        QPointF pixelsPerDip {1.0, 1.0};
    };

    struct DragCoordinateSpace final {
        std::function<QPointF()> pointerPosition;
        std::function<QRectF(QQuickWindow&)>
            windowFrame;
        std::function<DragMonitor(
            QQuickWindow&,
            QPointF)> monitorAt;
        std::function<void(
            QQuickWindow&,
            QPointF)> moveWindow;
        std::function<bool()>
            primaryButtonPressed;
    };

    using PositionPersistCommand =
        std::function<Result<void>(QPoint)>;
    using WorkAreaResolver =
        std::function<QRect(QPoint)>;
    using UnitRandomSource =
        std::function<double()>;
    using CursorPositionSource =
        std::function<QPoint()>;

    PetWindowController(
        PetViewModel& model,
        std::optional<QPoint> restoredPosition,
        PositionPersistCommand persistPosition,
        QObject* parent = nullptr);
    PetWindowController(
        PetViewModel& model,
        std::optional<QPoint> restoredPosition,
        PositionPersistCommand persistPosition,
        WorkAreaResolver workAreaResolver,
        UnitRandomSource randomSource,
        CursorPositionSource cursorPositionSource,
        QObject* parent = nullptr);
    PetWindowController(
        PetViewModel& model,
        std::optional<QPoint> restoredPosition,
        PositionPersistCommand persistPosition,
        WorkAreaResolver workAreaResolver,
        UnitRandomSource randomSource,
        CursorPositionSource cursorPositionSource,
        DragCoordinateSpace dragCoordinateSpace,
        QObject* parent = nullptr);
    ~PetWindowController() override;

    void attachWindow(QQuickWindow& window);
    void start();
    void stop();
    void advance(qint64 nowMilliseconds);
    void setSessionActive(bool active);
    void setRoamingSuspended(bool suspended);
    bool roamingSuspended() const noexcept;
    int movementTimerIntervalMilliseconds() const noexcept;

    Q_INVOKABLE QRect availableWorkAreaAt(
        QPoint referencePoint) const;
    Q_INVOKABLE void beginDrag();
    Q_INVOKABLE void dragTo();
    Q_INVOKABLE void endDrag();
    Q_INVOKABLE void pauseBriefly();
    Q_INVOKABLE void publishHoverState(
        bool pointerHovered,
        bool controlsHovered);

signals:
    void windowPositionChanged(QPoint position);
    void runtimeErrorOccurred(
        CompanionError error);

private:
    QRect workAreaFor(
        QPoint referencePoint) const;
    QPoint defaultOrigin(
        const QRect& workArea) const;
    void reconcilePointerHover();
    void updateDirectionalLook();
    void clearMovementTarget();
    void resetMotionPrimer();
    bool shouldMoveAfterAnimation(
        bool animationAccepted,
        qint64 nowMilliseconds);
    QPointF dragPointerPosition() const;
    bool dragPrimaryButtonPressed() const;
    QRectF dragWindowFrame() const;
    DragMonitor dragMonitorAt(
        QPointF pointer) const;
    QSizeF dragWindowSizeFor(
        const DragMonitor& monitor) const;
    QPointF nextDragOrigin(
        QPointF pointer,
        QRectF currentFrame,
        const DragMonitor& pointerMonitor);
    void moveDragWindow(
        QPointF origin);
    void clearDragState();
    void setWindowOrigin(
        QPointF origin,
        bool persist);
    void persistCurrentPosition();
    qint64 currentClockMilliseconds() const;

    PetViewModel& model_;
    std::optional<QPoint>
        restoredPosition_;
    PositionPersistCommand
        persistPosition_;
    WorkAreaResolver
        workAreaResolver_;
    UnitRandomSource randomSource_;
    CursorPositionSource
        cursorPositionSource_;
    DragCoordinateSpace
        dragCoordinateSpace_;
    QPointer<QQuickWindow> window_;
    QTimer timer_;
    QTimer dragContinuationTimer_;
    QElapsedTimer elapsedClock_;
    std::optional<QPointF> targetOrigin_;
    std::optional<QPointF> preciseOrigin_;
    std::optional<QPointF>
        dragStartPointer_;
    std::optional<QPointF>
        dragStartOrigin_;
    std::optional<QPointF>
        dragGrabOffsetDips_;
    std::optional<QPointF>
        dragLastPointer_;
    std::optional<DragMonitor>
        dragMonitor_;
    int dragDirectionSign_ = 0;
    double dragDirectionReversalDistanceDips_ = 0.0;
    qint64 currentNowMilliseconds_ = 0;
    qint64 pausedUntilMilliseconds_ = 0;
    qint64 lastTickMilliseconds_ = -1;
    qint64 lastPersistMilliseconds_ = -1;
    qint64 windowHoverAuthorityUntilMilliseconds_ = -1;
    std::optional<qint64>
        motionPrimedAtMilliseconds_;
    bool sessionActive_ = true;
    bool roamingSuspended_ = false;
};

} // namespace companion
