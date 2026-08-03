#include "ui/pet/PetMenuPlacement.h"

#include <QtTest/QTest>

namespace companion {

class PetMenuPlacementTests final : public QObject
{
    Q_OBJECT

private slots:
    void anchorDropsOnlyTheControlBaselineWhenControlsAreVisible()
    {
        const QRect petFrame(100, 200, 124, 164);

        QCOMPARE(
            PetMenuPlacement::anchorFrame(petFrame, true),
            QRect(100, 218, 124, 146));
        QCOMPARE(
            PetMenuPlacement::anchorFrame(petFrame, false),
            QRect(100, 246, 124, 118));
    }

    void prefersAboveWhenTheTrayFits()
    {
        QCOMPARE(
            PetMenuPlacement::positionedOrigin(
                QRect(400, 400, 124, 146),
                QSize(292, 94),
                QRect(0, 0, 1000, 800)),
            QPoint(316, 296));
    }

    void fallsBackToLeftThenRight()
    {
        const QSize traySize(292, 416);
        const QRect available(0, 0, 1000, 800);

        QCOMPARE(
            PetMenuPlacement::positionedOrigin(
                QRect(600, 20, 124, 146),
                traySize,
                available),
            QPoint(298, 8));
        QCOMPARE(
            PetMenuPlacement::positionedOrigin(
                QRect(20, 20, 124, 146),
                traySize,
                available),
            QPoint(154, 8));
    }

    void usesBelowWhenHorizontalSidesCannotFit()
    {
        QCOMPARE(
            PetMenuPlacement::positionedOrigin(
                QRect(98, 30, 124, 146),
                QSize(292, 100),
                QRect(0, 0, 320, 800)),
            QPoint(14, 186));
    }

    void clampsToTheVisibleFrameWhenNoSideFits()
    {
        QCOMPARE(
            PetMenuPlacement::positionedOrigin(
                QRect(98, 30, 124, 146),
                QSize(292, 180),
                QRect(0, 0, 320, 200)),
            QPoint(14, 12));
    }

    void positionsAuxiliarySurfaceBesideThePrimarySurface()
    {
        const QRect available(0, 0, 1000, 800);
        const QSize auxiliarySize(292, 360);

        QCOMPARE(
            PetMenuPlacement::
                positionedAuxiliaryOrigin(
                    QRect(316, 296, 292, 94),
                    auxiliarySize,
                    available),
            QPoint(14, 163));
        QCOMPARE(
            PetMenuPlacement::
                positionedAuxiliaryOrigin(
                    QRect(20, 296, 292, 94),
                    auxiliarySize,
                    available),
            QPoint(322, 163));
    }

    void auxiliarySurfaceFallsBackAboveThenClamps()
    {
        QCOMPARE(
            PetMenuPlacement::
                positionedAuxiliaryOrigin(
                    QRect(104, 400, 292, 94),
                    QSize(292, 100),
                    QRect(0, 0, 500, 800)),
            QPoint(104, 290));
        QCOMPARE(
            PetMenuPlacement::
                positionedAuxiliaryOrigin(
                    QRect(104, 30, 292, 94),
                    QSize(292, 360),
                    QRect(0, 0, 500, 200)),
            QPoint(104, 8));
    }

    void positionsAttentionAboveThePet()
    {
        QCOMPARE(
            PetMenuPlacement::
                positionedAttentionOrigin(
                    QRect(400, 400, 124, 164),
                    QSize(250, 68),
                    QRect(0, 0, 1000, 800)),
            QPoint(337, 327));
    }

    void attentionFallsBelowWhenTopIsBlocked()
    {
        QCOMPARE(
            PetMenuPlacement::
                positionedAttentionOrigin(
                    QRect(20, 12, 124, 164),
                    QSize(250, 68),
                    QRect(0, 0, 1000, 800)),
            QPoint(8, 181));
    }

    void attentionClampsWhenNeitherSideFits()
    {
        QCOMPARE(
            PetMenuPlacement::
                positionedAttentionOrigin(
                    QRect(98, 20, 124, 60),
                    QSize(250, 68),
                    QRect(0, 0, 320, 100)),
            QPoint(35, 24));
    }
};

} // namespace companion

QTEST_GUILESS_MAIN(companion::PetMenuPlacementTests)

#include "PetMenuPlacementTests.moc"
