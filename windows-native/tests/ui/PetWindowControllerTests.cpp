#include "ui/PetViewModel.h"
#include "ui/PetWindowController.h"

#include <QQuickWindow>
#include <QtTest>

#include <deque>
#include <functional>

namespace {

class SequenceRandom final {
public:
    explicit SequenceRandom(
        std::initializer_list<double> values)
        : values_(values)
    {
    }

    double next()
    {
        if (values_.empty()) {
            return 0.5;
        }
        const double value = values_.front();
        values_.pop_front();
        return value;
    }

private:
    std::deque<double> values_;
};

companion::PetWindowController makeController(
    companion::PetViewModel& model,
    std::optional<QPoint> restoredPosition,
    QVector<QPoint>& persistedPositions,
    SequenceRandom& random,
    QPoint* cursor = nullptr)
{
    return companion::PetWindowController(
        model,
        restoredPosition,
        [&persistedPositions](QPoint position) {
            persistedPositions.append(position);
            return companion::Result<void>::success();
        },
        [](QPoint referencePoint) {
            return referencePoint.x() >= 1920
                ? QRect(1920, 0, 1920, 1080)
                : QRect(0, 0, 1920, 1080);
        },
        [&random] {
            return random.next();
        },
        [cursor] {
            return cursor == nullptr
                ? QPoint(4000, 4000)
                : *cursor;
        });
}

class DragHarness final {
public:
    using DragMonitor =
        companion::PetWindowController::
            DragMonitor;

    companion::PetWindowController::
        DragCoordinateSpace
    coordinateSpace()
    {
        return {
            [this] {
                return pointer;
            },
            [this](QQuickWindow&) {
                return frame;
            },
            [this](
                QQuickWindow&,
                QPointF referencePoint) {
                return monitorAt
                    ? monitorAt(
                          referencePoint)
                    : DragMonitor {
                          QRectF(
                              0,
                              0,
                              1920,
                              1080),
                          {1.0, 1.0},
                      };
            },
            [this](
                QQuickWindow& window,
                QPointF origin) {
                frame.moveTopLeft(origin);
                moves.append(origin);
                window.setPosition({
                    qRound(origin.x()),
                    qRound(origin.y()),
                });
            },
            [this] {
                return primaryButtonPressed;
            },
        };
    }

    QPointF pointer;
    QRectF frame;
    QVector<QPointF> moves;
    std::function<DragMonitor(QPointF)>
        monitorAt;
    bool primaryButtonPressed = false;
};

} // namespace

class PetWindowControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void autonomousMovementUsesSmoothWindowsPresentationCadence()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        auto controller = makeController(
            model,
            std::nullopt,
            persistedPositions,
            random);

        QCOMPARE(
            controller.movementTimerIntervalMilliseconds(),
            17);
    }

    void smoothCadenceRetainsOneSourceTickMotionPrimer()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            0.0986547085,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));
        QCOMPARE(window.position(), QPoint(100, 100));

        for (const qint64 now :
             {467, 484, 501, 518}) {
            controller.advance(now);
            QCOMPARE(
                window.position(),
                QPoint(100, 100));
        }

        controller.advance(535);
        QCOMPARE(window.position(), QPoint(101, 100));
        controller.advance(552);
        QCOMPARE(window.position(), QPoint(102, 100));
    }

    void delayedPresentationTickDoesNotTeleportWindow()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            0.0986547085,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        for (const qint64 now :
             {467, 484, 501, 518}) {
            controller.advance(now);
        }

        controller.advance(535);
        QCOMPARE(window.position(), QPoint(101, 100));

        controller.advance(618);
        QCOMPARE(window.position(), QPoint(102, 100));
    }

    void exposesResolvedAvailableWorkAreaForPopupPlacement()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        auto controller = makeController(
            model,
            std::nullopt,
            persistedPositions,
            random);

        QCOMPARE(
            controller.availableWorkAreaAt(
                QPoint(2400, 500)),
            QRect(1920, 0, 1920, 1080));
        QCOMPARE(
            controller.availableWorkAreaAt(
                QPoint(400, 500)),
            QRect(0, 0, 1920, 1080));
    }

    void restoredOriginIsClampedIntoItsCurrentWorkArea()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        auto controller = makeController(
            model,
            QPoint(1800, 1000),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);

        controller.attachWindow(window);

        QCOMPARE(window.position(), QPoint(1790, 910));
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {QPoint(1790, 910)});
    }

    void dragUsesPointerMonitorAndPersistsFinalOrigin()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(1800, 200);
        auto controller = makeController(
            model,
            QPoint(1750, 100),
            persistedPositions,
            random,
            &cursor);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();
        controller.advance(0);

        controller.beginDrag();
        cursor = QPoint(2100, 200);
        controller.dragTo();
        controller.endDrag();

        QCOMPARE(window.position(), QPoint(2050, 100));
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {QPoint(2050, 100)});
    }

    void dragClampsImmediatelyToPointerMonitorLikeMac()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer = QPointF(70, 160);
        drag.frame =
            QRectF(20, 100, 124, 164);
        drag.monitorAt =
            [](QPointF pointer) {
                return DragHarness::
                    DragMonitor {
                        pointer.x() < 0
                            ? QRectF(
                                  -1920,
                                  0,
                                  1920,
                                  1080)
                            : QRectF(
                                  0,
                                  0,
                                  1920,
                                  1080),
                        {1.0, 1.0},
                    };
            };
        companion::PetWindowController controller(
            model,
            QPoint(20, 100),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint point) {
                return point.x() < 0
                    ? QRect(
                          -1920,
                          0,
                          1920,
                          1080)
                    : QRect(
                          0,
                          0,
                          1920,
                          1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        QVERIFY(model.dragging());
        drag.pointer = QPointF(-10, 160);
        controller.dragTo();

        QCOMPARE(
            drag.moves,
            QVector<QPointF> {
                QPointF(-130, 100)
            });
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        controller.endDrag();

        QVERIFY(!model.dragging());
        QCOMPARE(
            drag.moves.last(),
            QPointF(-130, 100));
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {
                QPoint(-130, 100)
            });
    }

    void dragPreservesNegativeVirtualDesktopCoordinates()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer =
            QPointF(-1740, 860);
        drag.frame =
            QRectF(
                -1800,
                800,
                124,
                164);
        drag.monitorAt =
            [](QPointF) {
                return DragHarness::
                    DragMonitor {
                        QRectF(
                            -2560,
                            738,
                            2560,
                            1414),
                        {1.0, 1.0},
                    };
            };
        companion::PetWindowController controller(
            model,
            QPoint(-1800, 800),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint) {
                return QRect(
                    -2560,
                    738,
                    2560,
                    1414);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer =
            QPointF(-1600, 900);
        controller.dragTo();

        QCOMPARE(
            drag.moves,
            QVector<QPointF> {
                QPointF(-1660, 840)
            });
    }

    void dragDirectionIgnoresJitterUntilIntentionalReversal()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer = QPointF(160, 180);
        drag.frame = QRectF(100, 100, 124, 164);
        companion::PetWindowController controller(
            model,
            QPoint(100, 100),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint) {
                return QRect(
                    0,
                    0,
                    1920,
                    1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer = QPointF(140, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        drag.pointer = QPointF(141, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        drag.pointer = QPointF(142, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        drag.pointer = QPointF(143, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        drag.pointer = QPointF(142, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        drag.pointer = QPointF(140, 180);
        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        controller.dragTo();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));
    }

    void mixedDpiCrossingClampsToTargetWorkArea()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer =
            QPointF(1760, 160);
        drag.frame =
            QRectF(
                1700,
                100,
                124,
                164);
        drag.monitorAt =
            [](QPointF pointer) {
                return pointer.x() >= 1920
                    ? DragHarness::
                          DragMonitor {
                              QRectF(
                                  1920,
                                  40,
                                  1600,
                                  1000),
                              {1.5, 1.5},
                          }
                    : DragHarness::
                          DragMonitor {
                              QRectF(
                                  0,
                                  0,
                                  1920,
                                  1080),
                              {1.0, 1.0},
                          };
            };
        companion::PetWindowController controller(
            model,
            QPoint(1700, 100),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint point) {
                return point.x() >= 1920
                    ? QRect(
                          1920,
                          40,
                          1600,
                          1000)
                    : QRect(
                          0,
                          0,
                          1920,
                          1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer =
            QPointF(1980, 100);
        controller.dragTo();

        QCOMPARE(
            drag.moves,
            QVector<QPointF> {
                QPointF(1929, 49)
            });
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        controller.endDrag();

        QCOMPARE(
            drag.moves.last(),
            QPointF(1929, 49));
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {
                QPoint(1929, 49)
            });
    }

    void mixedDpiRoundTripPreservesTheGrabPointInDips()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer =
            QPointF(1760, 360);
        drag.frame =
            QRectF(
                1700,
                300,
                124,
                164);
        drag.monitorAt =
            [](QPointF pointer) {
                return pointer.x() >= 1920
                    ? DragHarness::
                          DragMonitor {
                              QRectF(
                                  1920,
                                  40,
                                  2560,
                                  1400),
                              {1.5, 1.5},
                          }
                    : DragHarness::
                          DragMonitor {
                              QRectF(
                                  0,
                                  0,
                                  1920,
                                  1080),
                              {1.0, 1.0},
                          };
            };
        companion::PetWindowController controller(
            model,
            QPoint(1700, 300),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint point) {
                return point.x() >= 1920
                    ? QRect(
                          1920,
                          40,
                          2560,
                          1400)
                    : QRect(
                          0,
                          0,
                          1920,
                          1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer =
            QPointF(2300, 600);
        controller.dragTo();

        QCOMPARE(
            drag.moves,
            QVector<QPointF> {
                QPointF(2210, 510)
            });

        drag.pointer =
            QPointF(1760, 360);
        controller.dragTo();
        controller.endDrag();

        const QVector<QPointF> expectedMoves {
            QPointF(2210, 510),
            QPointF(1700, 300),
        };
        QCOMPARE(
            drag.moves,
            expectedMoves);
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {
                QPoint(1700, 300)
            });
    }

    void releaseAfterPointerLeavesMonitorKeepsLastDraggedOrigin()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer = QPointF(2160, 180);
        drag.frame = QRectF(2100, 100, 124, 164);
        drag.monitorAt =
            [](QPointF pointer) {
                return DragHarness::DragMonitor {
                    pointer.x() >= 1920
                        ? QRectF(1920, 0, 1920, 1080)
                        : QRectF(0, 0, 1920, 1080),
                    {1.0, 1.0},
                };
            };
        companion::PetWindowController controller(
            model,
            QPoint(2100, 100),
            [&persistedPositions](QPoint position) {
                persistedPositions.append(position);
                return companion::Result<void>::success();
            },
            [](QPoint point) {
                return point.x() >= 1920
                    ? QRect(1920, 0, 1920, 1080)
                    : QRect(0, 0, 1920, 1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer = QPointF(2460, 180);
        controller.dragTo();
        QCOMPARE(drag.frame.topLeft(), QPointF(2400, 100));

        drag.pointer = QPointF(20, 20);
        controller.endDrag();

        QCOMPARE(drag.frame.topLeft(), QPointF(2400, 100));
        QCOMPARE(window.position(), QPoint(2400, 100));
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {QPoint(2400, 100)});
    }

    void globalPollingContinuesAfterLocalMouseCaptureStops()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        DragHarness drag;
        drag.pointer = QPointF(160, 180);
        drag.frame = QRectF(100, 100, 124, 164);
        drag.primaryButtonPressed = true;
        companion::PetWindowController controller(
            model,
            QPoint(100, 100),
            [&persistedPositions](
                QPoint position) {
                persistedPositions.append(
                    position);
                return companion::Result<void>::
                    success();
            },
            [](QPoint) {
                return QRect(
                    0,
                    0,
                    1920,
                    1080);
            },
            [&random] {
                return random.next();
            },
            [] {
                return QPoint(4000, 4000);
            },
            drag.coordinateSpace());
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        persistedPositions.clear();

        controller.beginDrag();
        drag.pointer = QPointF(260, 230);

        QTRY_VERIFY_WITH_TIMEOUT(
            !drag.moves.isEmpty(),
            250);
        QCOMPARE(
            drag.moves.last(),
            QPointF(200, 150));
        QVERIFY(model.dragging());

        drag.primaryButtonPressed = false;

        QTRY_VERIFY_WITH_TIMEOUT(
            !model.dragging(),
            250);
        QCOMPARE(
            persistedPositions,
            QVector<QPoint> {
                QPoint(200, 150)
            });
    }

    void movementPrimesAnimationBeforeMovingAfterSourcePrimer()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            88.0 / 892.0,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        QCOMPARE(window.position(), QPoint(100, 100));
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));

        controller.advance(450);
        QCOMPARE(window.position(), QPoint(100, 100));
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        controller.advance(533);
        QCOMPARE(window.position(), QPoint(101, 100));
    }

    void repeatedRoamingTicksPreserveAnimationFrameProgress()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            88.0 / 892.0,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));
        QCOMPARE(model.frameColumn(), 0);

        model.advanceAnimationFrame();
        QCOMPARE(model.frameColumn(), 1);

        controller.advance(533);

        QCOMPARE(window.position(), QPoint(101, 100));
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));
        QCOMPARE(model.frameColumn(), 1);
    }

    void pointerHoverPausesRoamingAndLeaveResumesFresh()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            88.0 / 892.0,
            0.0,
            0.0,
            88.0 / 892.0,
        });
        QPoint cursor(4000, 4000);
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random,
            &cursor);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        controller.advance(533);
        const QPoint movingPosition =
            window.position();
        QVERIFY(movingPosition.x() > 100);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        cursor =
            movingPosition + QPoint(62, 96);
        controller.advance(616);
        QVERIFY(model.pointerHovered());
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("jumping"));

        controller.advance(782);
        QCOMPARE(window.position(), movingPosition);

        cursor = QPoint(4000, 4000);
        controller.advance(865);
        QVERIFY(!model.pointerHovered());
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));

        controller.advance(1315);
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        controller.advance(1398);
        QVERIFY(window.x() < movingPosition.x());
        QCOMPARE(window.y(), movingPosition.y());
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));
    }

    void petHitRegionMatchesArtworkBounds()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(162, 202);
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random,
            &cursor);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        QVERIFY(model.pointerHovered());

        cursor = QPoint(108, 202);
        controller.advance(83);
        QVERIFY(!model.pointerHovered());

        cursor = QPoint(212, 202);
        controller.advance(166);
        QVERIFY(!model.pointerHovered());

        cursor = QPoint(211, 255);
        controller.advance(249);
        QVERIFY(model.pointerHovered());

        cursor = QPoint(211, 256);
        controller.advance(332);
        QVERIFY(!model.pointerHovered());
    }

    void attentionPauseDiscardsRoamingTargetAndRestartsFresh()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            88.0 / 892.0,
            0.0,
            0.0,
            88.0 / 892.0,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        controller.advance(533);
        const QPoint movingPosition =
            window.position();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-right"));

        model.setAttentionAnimation(
            QStringLiteral("failed"));
        controller.advance(616);
        QCOMPARE(window.position(), movingPosition);

        model.clearAttentionAnimation();
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));

        controller.advance(699);
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));

        controller.advance(1149);
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        controller.advance(1232);
        QVERIFY(window.x() < movingPosition.x());
        QCOMPARE(window.y(), movingPosition.y());
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));
    }

    void openMenuPausesRoamingWithoutCatchUp()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            0.5,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        controller.advance(0);
        controller.advance(450);
        controller.advance(533);
        const QPoint movedPosition =
            window.position();

        model.setMenuOpen(true);
        controller.advance(5000);
        model.setMenuOpen(false);
        controller.advance(5083);

        QCOMPARE(window.position(), movedPosition);
    }

    void settingsSuspensionPreservesPreferenceAndResumesFresh()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            88.0 / 892.0,
            0.0,
            0.0,
            88.0 / 892.0,
        });
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.advance(0);
        controller.advance(450);
        controller.advance(533);
        const QPoint movingPosition =
            window.position();

        controller.setRoamingSuspended(true);
        controller.advance(5000);

        QVERIFY(model.allowAutonomousMovement());
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));

        controller.setRoamingSuspended(false);
        controller.advance(5083);
        QCOMPARE(window.position(), movingPosition);

        controller.advance(5533);
        QCOMPARE(window.position(), movingPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("running-left"));

        controller.advance(5616);
        QVERIFY(window.x() < movingPosition.x());
    }

    void menuHoverTransfersFromPetToVisibleControls()
    {
        companion::PetViewModel model(
            true,
            1.0,
            true,
            true);
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(4000, 4000);
        companion::PetWindowController controller(
            model,
            QPoint(100, 100),
            [&persistedPositions](QPoint position) {
                persistedPositions.append(position);
                return companion::Result<void>::success();
            },
            [](QPoint) {
                return QRect(0, 0, 1920, 1080);
            },
            [&random] {
                return random.next();
            },
            [&cursor] {
                return cursor;
            });
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        model.setMenuOpen(true);

        controller.advance(0);
        QVERIFY(!model.controlsVisible());

        cursor = QPoint(162, 196);
        controller.advance(83);
        QVERIFY(model.pointerHovered());
        QVERIFY(model.controlsVisible());
        QSignalSpy controlsVisibilitySpy(
            &model,
            &companion::PetViewModel::
                controlsVisibleChanged);
        QVERIFY(controlsVisibilitySpy.isValid());

        // Crossing directly into the revealed strip must not collapse it.
        cursor = QPoint(120, 120);
        controller.advance(166);
        QVERIFY(!model.pointerHovered());
        QVERIFY(model.controlsHovered());
        QVERIFY(model.controlsVisible());
        QCOMPARE(controlsVisibilitySpy.count(), 0);
    }

    void windowHoverPublicationWinsUntilNativePollCatchesUp()
    {
        companion::PetViewModel model(
            true,
            1.0,
            true,
            true);
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(4000, 4000);
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random,
            &cursor);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        controller.publishHoverState(
            true,
            false);
        QVERIFY(model.pointerHovered());
        QVERIFY(model.controlsVisible());

        controller.advance(83);
        QVERIFY(model.pointerHovered());
        QVERIFY(model.controlsVisible());

        controller.advance(121);
        QVERIFY(!model.pointerHovered());
        QVERIFY(!model.controlsVisible());
    }

    void hiddenOpenMenuControlsIgnoreTheirInactiveHitRegion()
    {
        companion::PetViewModel model(
            true,
            1.0,
            true,
            true);
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(4000, 4000);
        companion::PetWindowController controller(
            model,
            QPoint(100, 100),
            [&persistedPositions](QPoint position) {
                persistedPositions.append(position);
                return companion::Result<void>::success();
            },
            [](QPoint) {
                return QRect(0, 0, 1920, 1080);
            },
            [&random] {
                return random.next();
            },
            [&cursor] {
                return cursor;
            });
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        model.setMenuOpen(true);

        controller.advance(0);
        QVERIFY(!model.controlsVisible());

        cursor = QPoint(120, 120);
        controller.advance(83);

        QVERIFY(!model.pointerHovered());
        QVERIFY(!model.controlsHovered());
        QVERIFY(!model.controlsVisible());
    }

    void closedMenuHoverUsesRightAlignedControlRegion()
    {
        companion::PetViewModel model(
            true,
            1.0,
            true,
            true);
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.5});
        QPoint cursor(202, 120);
        companion::PetWindowController controller(
            model,
            QPoint(100, 100),
            [&persistedPositions](QPoint position) {
                persistedPositions.append(position);
                return companion::Result<void>::success();
            },
            [](QPoint) {
                return QRect(0, 0, 1920, 1080);
            },
            [&random] {
                return random.next();
            },
            [&cursor] {
                return cursor;
            });
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);

        // Pet hover has already revealed the right-aligned
        // closed-menu control before the pointer crosses into it.
        model.setControlsHovered(true);
        QVERIFY(model.controlsVisible());

        controller.advance(0);

        QVERIFY(!model.pointerHovered());
        QVERIFY(model.controlsHovered());
        QVERIFY(model.controlsVisible());
    }

    void postDragPauseSuppressesAutonomousMovement()
    {
        companion::PetViewModel model;
        QVector<QPoint> persistedPositions;
        SequenceRandom random({
            0.0,
            1.0,
            0.5,
        });
        QPoint cursor(120, 120);
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random,
            &cursor);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        controller.advance(0);

        controller.beginDrag();
        cursor = QPoint(180, 120);
        controller.dragTo();
        controller.endDrag();
        const QPoint draggedPosition =
            window.position();

        controller.advance(1499);

        QCOMPARE(window.position(), draggedPosition);
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));
    }

    void inactiveSessionClearsHoverAndSuppressesMovement()
    {
        companion::PetViewModel model(
            true,
            1.0,
            true,
            true);
        QVector<QPoint> persistedPositions;
        SequenceRandom random({0.0, 1.0, 0.5});
        auto controller = makeController(
            model,
            QPoint(100, 100),
            persistedPositions,
            random);
        QQuickWindow window;
        window.resize(124, 164);
        controller.attachWindow(window);
        model.setPointerHovered(true);
        model.setControlsHovered(true);

        controller.setSessionActive(false);
        controller.advance(5000);

        QVERIFY(!model.pointerHovered());
        QVERIFY(!model.controlsHovered());
        QCOMPARE(window.position(), QPoint(100, 100));
        QCOMPARE(
            model.renderedAnimation(),
            QStringLiteral("idle"));
    }
};

QTEST_MAIN(PetWindowControllerTests)
#include "PetWindowControllerTests.moc"
