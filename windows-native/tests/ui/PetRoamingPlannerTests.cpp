#include "ui/pet/PetRoamingPlanner.h"

#include <QtTest>

class PetRoamingPlannerTests final : public QObject {
    Q_OBJECT

private slots:
    void clampsWindowOriginToSixDipWorkAreaMargin()
    {
        const QRectF workArea(
            0.0, 0.0, 1920.0, 1080.0);
        const QSizeF windowSize(
            124.0, 164.0);

        QCOMPARE(
            companion::PetRoamingPlanner::clamp(
                {-100.0, -50.0},
                windowSize,
                workArea),
            QPointF(6.0, 6.0));
        QCOMPARE(
            companion::PetRoamingPlanner::clamp(
                {2000.0, 1400.0},
                windowSize,
                workArea),
            QPointF(1790.0, 910.0));
    }

    void targetUsesTwelveDipInsetAndUnitCoordinates()
    {
        const QRectF workArea(
            0.0, 0.0, 1920.0, 1080.0);
        const QSizeF windowSize(
            124.0, 164.0);

        QCOMPARE(
            companion::PetRoamingPlanner::
                targetFromUnit(
                    workArea,
                    windowSize,
                    0.0,
                    0.0),
            QPointF(12.0, 12.0));
        QCOMPARE(
            companion::PetRoamingPlanner::
                targetFromUnit(
                    workArea,
                    windowSize,
                    1.0,
                    1.0),
            QPointF(1784.0, 904.0));
    }

    void stepMaintainsFiftyFourDipsPerSecondAtSourceCadence()
    {
        const QPointF next =
            companion::PetRoamingPlanner::stepToward(
                {100.0, 100.0},
                {500.0, 100.0},
                companion::PetRoamingPlanner::
                    tickIntervalSeconds);

        QCOMPARE(next, QPointF(104.5, 100.0));
    }

    void elapsedTimeIsBoundedAgainstCatchUpAndZeroDelta()
    {
        QCOMPARE(
            companion::PetRoamingPlanner::stepToward(
                {0.0, 0.0},
                {100.0, 0.0},
                5.0),
            QPointF(6.75, 0.0));
        QCOMPARE(
            companion::PetRoamingPlanner::stepToward(
                {0.0, 0.0},
                {100.0, 0.0},
                0.0),
            QPointF(0.9, 0.0));
    }

    void arrivalAndIdlePauseMatchMacContract()
    {
        QVERIFY(
            companion::PetRoamingPlanner::
                hasArrived(
                    {0.0, 0.0},
                    {3.99, 0.0}));
        QVERIFY(
            !companion::PetRoamingPlanner::
                hasArrived(
                    {0.0, 0.0},
                    {4.0, 0.0}));
        QCOMPARE(
            companion::PetRoamingPlanner::
                idlePauseMilliseconds(0.0),
            450);
        QCOMPARE(
            companion::PetRoamingPlanner::
                idlePauseMilliseconds(0.5),
            825);
        QCOMPARE(
            companion::PetRoamingPlanner::
                idlePauseMilliseconds(1.0),
            1200);
        QCOMPARE(
            companion::PetRoamingPlanner::
                postDragPauseMilliseconds,
            1500);
    }
};

QTEST_GUILESS_MAIN(PetRoamingPlannerTests)
#include "PetRoamingPlannerTests.moc"
