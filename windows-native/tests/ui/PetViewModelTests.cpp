#include "ui/CompanionPresentationPolicy.h"
#include "ui/PetViewModel.h"
#include "ui/pet/PetAnimation.h"
#include "ui/pet/PetCatalog.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <array>

namespace {

bool writePetPackage(
    const QString& root,
    const QString& directoryName,
    const QString& id,
    const QString& displayName,
    int columns,
    int rows)
{
    const QString directory =
        QDir(root).filePath(directoryName);
    if (!QDir().mkpath(directory)) {
        return false;
    }

    QImage sheet(
        columns * 4,
        rows * 4,
        QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::transparent);
    sheet.setPixelColor(1, 1, QColor(255, 214, 32));
    if (!sheet.save(
            QDir(directory).filePath(
                QStringLiteral("spritesheet.png")))) {
        return false;
    }

    const QJsonObject manifest{
        {QStringLiteral("id"), id},
        {QStringLiteral("displayName"), displayName},
        {QStringLiteral("spritesheetPath"),
         QStringLiteral("spritesheet.png")},
        {QStringLiteral("spriteColumns"), columns},
        {QStringLiteral("spriteRows"), rows},
        {QStringLiteral("animationFrameCounts"),
         QJsonObject{
             {QStringLiteral("idle"), columns},
             {QStringLiteral("running-left"), columns},
             {QStringLiteral("running-right"), columns},
             {QStringLiteral("running"), columns},
         }},
    };
    QFile file(
        QDir(directory).filePath(
            QStringLiteral("pet.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(
               QJsonDocument(manifest).toJson(
                   QJsonDocument::Compact))
        > 0;
}

} // namespace

class PetViewModelTests final : public QObject {
    Q_OBJECT

private slots:
    void implicitDefaultsUseMacAnimationTiming()
    {
        companion::PetViewModel viewModel;

        QCOMPARE(viewModel.animationSpeedScale(), 1.15);
    }

    void controlsVisibilityMatchesMacPresentationPolicy()
    {
        using companion::CompanionPresentationPolicy;

        QVERIFY(CompanionPresentationPolicy::showsPetMenuControls(
            false, false, false, false));
        QVERIFY(!CompanionPresentationPolicy::showsPetMenuControls(
            false, true, false, false));
        QVERIFY(CompanionPresentationPolicy::showsPetMenuControls(
            false, true, true, false));
        QVERIFY(CompanionPresentationPolicy::showsPetMenuControls(
            true, false, true, false));
        QVERIFY(CompanionPresentationPolicy::showsPetMenuControls(
            true, false, false, true));
        QVERIFY(!CompanionPresentationPolicy::showsPetMenuControls(
            true, false, false, false));
    }

    void settingsPlacementAvoidsAnOverlappingPet()
    {
        using companion::CompanionPresentationPolicy;

        const QRect workArea(
            -2560,
            738,
            2560,
            1414);
        const QSize settingsSize(576, 559);
        const QRect petFrame(
            -1237,
            1160,
            124,
            164);

        const QPoint origin =
            CompanionPresentationPolicy::
                settingsWindowOrigin(
                    workArea,
                    settingsSize,
                    petFrame);

        QCOMPARE(origin, QPoint(-1568, 1340));
        QVERIFY(
            !QRect(origin, settingsSize)
                 .intersects(petFrame));
        QVERIFY(
            workArea.contains(
                QRect(origin, settingsSize)));
    }

    void settingsPlacementStaysCenteredWithoutAnOverlap()
    {
        using companion::CompanionPresentationPolicy;

        QCOMPARE(
            CompanionPresentationPolicy::
                settingsWindowOrigin(
                    QRect(0, 26, 3840, 2134),
                    QSize(576, 559),
                    QRect(100, 100, 124, 164)),
            QPoint(1632, 813));
    }

    void openMenuRevealsControlsOnlyWhileHovering()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            true,
            true);

        QVERIFY(!viewModel.controlsVisible());

        viewModel.setMenuOpen(true);

        QVERIFY(viewModel.menuOpen());
        QVERIFY(!viewModel.controlsVisible());
        QVERIFY(!viewModel.pointerHovered());
        QVERIFY(!viewModel.controlsHovered());

        // Open menus still require pointer presence to reveal the control strip.
        viewModel.setPointerHovered(true);
        QVERIFY(viewModel.controlsVisible());

        viewModel.setPointerHovered(false);
        QVERIFY(!viewModel.controlsVisible());

        viewModel.setControlsHovered(true);
        QVERIFY(viewModel.controlsVisible());
    }

    void defaultsStartVisibleAndRunning()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        QVERIFY(viewModel.visible());
        QVERIFY(!viewModel.menuOpen());
        QVERIFY(!viewModel.pointerHovered());
        QVERIFY(!viewModel.controlsHovered());
        QVERIFY(!viewModel.dragging());
        QVERIFY(viewModel.controlsVisible());
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running"));
        QCOMPARE(viewModel.animationSpeedScale(), 1.0);
        QVERIFY(viewModel.allowAutonomousMovement());
    }

    void emptyCatalogStartsWithoutASelectedPetOrArtwork()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        companion::PetCatalog catalog({
            temporary.filePath(
                QStringLiteral("custom")),
            temporary.filePath(
                QStringLiteral("native")),
            temporary.filePath(
                QStringLiteral("bundled")),
        });
        companion::PetViewModel viewModel;
        int persistenceCalls = 0;

        const auto configured =
            viewModel.configurePetCatalog(
                catalog,
                QStringLiteral("missing-pet"),
                [&persistenceCalls](
                    const QString&) {
                    ++persistenceCalls;
                    return companion::Result<void>::
                        success();
                });

        QVERIFY(configured.hasValue());
        QVERIFY(viewModel.availablePets().isEmpty());
        QVERIFY(viewModel.selectedPetId().isEmpty());
        QVERIFY(viewModel.spriteSheetSource().isEmpty());
        QCOMPARE(persistenceCalls, 0);
    }

    void petSelectionUsesCatalogGeometryWithoutInterruptingMovement()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePetPackage(
            customRoot,
            QStringLiteral("compact"),
            QStringLiteral("compact"),
            QStringLiteral("Compact"),
            8,
            9));
        QVERIFY(writePetPackage(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12));

        companion::PetCatalog catalog({
            customRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);
        QString persistedPetId;
        int persistenceCalls = 0;

        const auto configured =
            viewModel.configurePetCatalog(
                catalog,
                QStringLiteral("shadow-16"),
                [&persistedPetId,
                 &persistenceCalls](
                    const QString& petId) {
                    ++persistenceCalls;
                    persistedPetId = petId;
                    return companion::Result<void>::
                        success();
                });

        QVERIFY(configured.hasValue());
        QCOMPARE(viewModel.availablePets().size(), 2);
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("shadow-16"));
        QCOMPARE(viewModel.spriteColumns(), 16);
        QCOMPARE(viewModel.spriteRows(), 12);
        QCOMPARE(viewModel.sourceFrameWidth(), 4);
        QCOMPARE(viewModel.sourceFrameHeight(), 4);
        QCOMPARE(persistenceCalls, 0);

        QVERIFY(viewModel.setRoamingMotion(-1.0, 0.0));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-left"));

        QVERIFY(viewModel.selectPet(
            QStringLiteral("compact")));

        QCOMPARE(persistenceCalls, 1);
        QCOMPARE(
            persistedPetId,
            QStringLiteral("compact"));
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("compact"));
        QCOMPARE(viewModel.spriteColumns(), 8);
        QCOMPARE(viewModel.spriteRows(), 9);
        QCOMPARE(viewModel.sourceFrameWidth(), 4);
        QCOMPARE(viewModel.sourceFrameHeight(), 4);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-left"));
        QCOMPARE(viewModel.frameRow(), 2);
    }

    void legacyNativeShadowSelectionPersistsCurrentShadowOnce()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePetPackage(
            customRoot,
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("shadow-native-v2"),
            QStringLiteral("Legacy Shadow"),
            8,
            11));
        QVERIFY(writePetPackage(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12));

        companion::PetCatalog catalog({
            customRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        companion::PetViewModel viewModel;
        QString persistedPetId;
        int persistenceCalls = 0;

        const auto configured =
            viewModel.configurePetCatalog(
                catalog,
                QStringLiteral(
                    "custom:shadow-native-v2"),
                [&persistedPetId,
                 &persistenceCalls](
                    const QString& petId) {
                    ++persistenceCalls;
                    persistedPetId = petId;
                    return companion::Result<void>::
                        success();
                });

        QVERIFY(configured.hasValue());
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("shadow-16"));
        QCOMPARE(persistenceCalls, 1);
        QCOMPARE(
            persistedPetId,
            QStringLiteral("shadow-16"));
    }

    void reloadPetsPreservesSelectionAndPublishesNewPackages()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePetPackage(
            customRoot,
            QStringLiteral("compact"),
            QStringLiteral("compact"),
            QStringLiteral("Compact"),
            8,
            9));
        QVERIFY(writePetPackage(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12));

        companion::PetCatalog catalog({
            customRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        companion::PetViewModel viewModel;
        QVERIFY(
            viewModel.configurePetCatalog(
                catalog,
                QStringLiteral("compact"),
                [](const QString&) {
                    return companion::Result<void>::
                        success();
                })
                .hasValue());
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("compact"));

        QVERIFY(writePetPackage(
            customRoot,
            QStringLiteral("late"),
            QStringLiteral("late"),
            QStringLiteral("Late"),
            8,
            9));
        QVERIFY(viewModel.reloadPets());

        QCOMPARE(viewModel.availablePets().size(), 3);
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("compact"));
    }

    void failedPetSelectionPersistenceKeepsTheCurrentPet()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString customRoot =
            temporary.filePath(
                QStringLiteral("custom"));
        const QString bundledRoot =
            temporary.filePath(
                QStringLiteral("bundled"));
        QVERIFY(writePetPackage(
            customRoot,
            QStringLiteral("compact"),
            QStringLiteral("compact"),
            QStringLiteral("Compact"),
            8,
            9));
        QVERIFY(writePetPackage(
            bundledRoot,
            QStringLiteral("shadow-16"),
            QStringLiteral("shadow-16"),
            QStringLiteral("Shadow"),
            16,
            12));

        companion::PetCatalog catalog({
            customRoot,
            temporary.filePath(
                QStringLiteral("native")),
            bundledRoot,
        });
        companion::PetViewModel viewModel;
        QVERIFY(
            viewModel.configurePetCatalog(
                catalog,
                QStringLiteral("shadow-16"),
                [](const QString&) {
                    return companion::Result<void>::
                        failure({
                            QStringLiteral(
                                "settings.write-failed"),
                            QStringLiteral(
                                "Could not persist pet selection."),
                            false,
                            {},
                        });
                })
                .hasValue());
        QSignalSpy errorSpy(
            &viewModel,
            &companion::PetViewModel::
                runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        QVERIFY(!viewModel.selectPet(
            QStringLiteral("compact")));

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(
            viewModel.selectedPetId(),
            QStringLiteral("shadow-16"));
        QCOMPARE(viewModel.spriteColumns(), 16);
        QCOMPARE(viewModel.spriteRows(), 12);
    }

    void selectedAnimationRemainsStableAcrossHoverAndMovement()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);
        QSignalSpy selectedSpy(
            &viewModel,
            &companion::PetViewModel::
                selectedAnimationChanged);
        QVERIFY(selectedSpy.isValid());

        QCOMPARE(
            viewModel.selectedAnimation(),
            QStringLiteral("idle"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running"));

        viewModel.setSelectedAnimation(
            QStringLiteral("review"));

        QCOMPARE(
            viewModel.selectedAnimation(),
            QStringLiteral("review"));
        QCOMPARE(selectedSpy.count(), 1);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running"));

        viewModel.setPointerHovered(true);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
        QCOMPARE(
            viewModel.selectedAnimation(),
            QStringLiteral("review"));

        viewModel.setPointerHovered(false);
        QVERIFY(viewModel.setRoamingMotion(1.0, 0.0));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));
        QCOMPARE(
            viewModel.selectedAnimation(),
            QStringLiteral("review"));

        viewModel.setMenuOpen(true);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("review"));
        QCOMPARE(selectedSpy.count(), 1);
    }

    void repeatedHoverDoesNotRestartRenderedAnimation()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            true,
            true);
        QSignalSpy animationSpy(
            &viewModel,
            &companion::PetViewModel::
                renderedAnimationChanged);
        QSignalSpy hoverSpy(
            &viewModel,
            &companion::PetViewModel::hoverChanged);
        QVERIFY(animationSpy.isValid());
        QVERIFY(hoverSpy.isValid());

        viewModel.setPointerHovered(true);

        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
        QCOMPARE(animationSpy.count(), 1);
        QCOMPARE(hoverSpy.count(), 1);

        viewModel.setPointerHovered(true);

        QCOMPARE(animationSpy.count(), 1);
        QCOMPARE(hoverSpy.count(), 1);
    }

    void atomicHoverHandoffKeepsControlBridgeVisible()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            true,
            true);
        viewModel.setMenuOpen(true);
        QSignalSpy controlsSpy(
            &viewModel,
            &companion::PetViewModel::
                controlsVisibleChanged);
        QVERIFY(controlsSpy.isValid());

        viewModel.setHoverState(true, false);
        QVERIFY(viewModel.controlsVisible());
        QCOMPARE(controlsSpy.count(), 1);

        controlsSpy.clear();
        viewModel.setHoverState(false, true);

        QVERIFY(!viewModel.pointerHovered());
        QVERIFY(viewModel.controlsHovered());
        QVERIFY(viewModel.controlsVisible());
        QCOMPARE(controlsSpy.count(), 0);

        viewModel.setHoverState(false, false);
        QVERIFY(!viewModel.controlsVisible());
        QCOMPARE(controlsSpy.count(), 1);
    }

    void hoverLeaveRestoresAttentionThenRoaming()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setAttentionAnimation(
            QStringLiteral("failed"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.setPointerHovered(true);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));

        viewModel.setPointerHovered(false);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.clearAttentionAnimation();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
    }

    void activityReactionYieldsToHoverAndDragBeforeManualPreview()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);
        viewModel.setMenuOpen(true);

        viewModel.setAnimation(
            QStringLiteral("talking"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("talking"));

        viewModel.setAttentionAnimation(
            QStringLiteral("failed"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.setAnimation(
            QStringLiteral("waiting"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.setPointerHovered(true);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));

        viewModel.setAttentionAnimation(
            QStringLiteral("review"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));

        viewModel.setAnimation(
            QStringLiteral("talking"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));

        viewModel.setPointerHovered(false);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("review"));

        viewModel.beginDrag();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));

        viewModel.setAttentionAnimation(
            QStringLiteral("failed"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));

        viewModel.setAnimation(
            QStringLiteral("waiting"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));

        viewModel.endDrag();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.clearAttentionAnimation();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("waiting"));
    }

    void attentionDismissDuringHoverPreservesHoverAnimation()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setPointerHovered(true);
        viewModel.setAttentionAnimation(
            QStringLiteral("talking"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
        QCOMPARE(viewModel.frameRow(), 4);

        viewModel.clearAttentionAnimation();

        QVERIFY(!viewModel.attentionActive());
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
        QCOMPARE(viewModel.frameRow(), 4);
        QCOMPARE(viewModel.frameColumn(), 0);
    }

    void attentionDismissRestoresExistingRoamingState()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setRoamingIdle();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("idle"));

        viewModel.setAttentionAnimation(
            QStringLiteral("failed"));
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("failed"));

        viewModel.clearAttentionAnimation();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("idle"));
    }

    void dragDirectionKeepsLastSideInsideDeadZone()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.beginDrag();
        QVERIFY(viewModel.dragging());
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));

        viewModel.updateDrag(-1.0, 0.0);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-left"));

        viewModel.updateDrag(0.05, 0.0);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-left"));

        viewModel.updateDrag(1.0, 0.0);
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running-right"));

        viewModel.endDrag();
        QVERIFY(!viewModel.dragging());
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("running"));
    }

    void hidingPetClosesMenuAndClearsInteractionState()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setMenuOpen(true);
        viewModel.setPointerHovered(true);
        viewModel.setControlsHovered(true);
        viewModel.beginDrag();
        viewModel.setVisible(false);

        QVERIFY(!viewModel.visible());
        QVERIFY(!viewModel.menuOpen());
        QVERIFY(!viewModel.pointerHovered());
        QVERIFY(!viewModel.controlsHovered());
        QVERIFY(!viewModel.dragging());
    }

    void openingMenuMakesHiddenPetVisibleAndPersistsIt()
    {
        bool persistedVisibility = false;
        int persistenceCalls = 0;
        companion::PetViewModel viewModel(
            false,
            1.0,
            false,
            true,
            [&persistedVisibility, &persistenceCalls](
                bool visible) {
                ++persistenceCalls;
                persistedVisibility = visible;
                return companion::Result<void>::success();
            });

        viewModel.setMenuOpen(true);

        QVERIFY(viewModel.visible());
        QVERIFY(viewModel.menuOpen());
        QVERIFY(persistedVisibility);
        QCOMPARE(persistenceCalls, 1);
    }

    void animationSpeedClampsToPersistedBounds()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setAnimationSpeedScale(0.1);
        QCOMPARE(viewModel.animationSpeedScale(), 0.4);

        viewModel.setAnimationSpeedScale(4.0);
        QCOMPARE(viewModel.animationSpeedScale(), 3.0);
    }

    void animationSpeedChangeRestartsSequenceLikeMac()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.advanceAnimationFrame();
        viewModel.advanceAnimationFrame();
        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 2);
        QCOMPARE(
            viewModel.frameDurationMilliseconds(),
            171);

        viewModel.setAnimationSpeedScale(2.0);

        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 0);
        QCOMPARE(
            viewModel.frameDurationMilliseconds(),
            343);
    }

    void frameSequenceRestartsOnlyWhenRenderedStateChanges()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 0);
        QCOMPARE(
            viewModel.frameDurationMilliseconds(),
            171);

        viewModel.advanceAnimationFrame();
        QCOMPARE(viewModel.frameColumn(), 1);

        viewModel.setPointerHovered(false);
        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 1);

        viewModel.setPointerHovered(true);
        QCOMPARE(viewModel.frameRow(), 4);
        QCOMPARE(viewModel.frameColumn(), 0);

        viewModel.advanceAnimationFrame();
        QCOMPARE(viewModel.frameColumn(), 1);

        viewModel.setPointerHovered(true);
        QCOMPARE(viewModel.frameRow(), 4);
        QCOMPARE(viewModel.frameColumn(), 1);

        viewModel.setPointerHovered(false);
        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 0);
    }

    void manualFrameAdvanceTraversesJumpPreludeThenLoopsIdle()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);

        viewModel.setPointerHovered(true);
        QCOMPARE(viewModel.frameRow(), 4);
        QCOMPARE(viewModel.frameColumn(), 0);

        for (int timeout = 1;
             timeout <= 47;
             ++timeout) {
            viewModel.advanceAnimationFrame();
            QCOMPARE(viewModel.frameRow(), 4);
            QCOMPARE(
                viewModel.frameColumn(),
                timeout % 16);
        }

        viewModel.advanceAnimationFrame();
        QCOMPARE(viewModel.frameRow(), 0);
        QCOMPARE(viewModel.frameColumn(), 0);
    }

    void playbackUsesPreciseSingleShotDeadlineTimer()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);
        QTimer* timer =
            viewModel.findChild<QTimer*>(
                QStringLiteral(
                    "petAnimationTimer"));
        QVERIFY(timer);
        QCOMPARE(
            timer->timerType(),
            Qt::PreciseTimer);
        QVERIFY(timer->isSingleShot());
        QVERIFY(!timer->isActive());

        viewModel.setPointerHovered(true);
        viewModel.setAnimationPlaybackEnabled(true);

        QVERIFY(
            viewModel.animationPlaybackEnabled());
        QVERIFY(timer->isActive());
        QCOMPARE(timer->interval(), 170);

        viewModel.setPointerHovered(false);
        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 0);
        QCOMPARE(timer->interval(), 171);

        viewModel.setAnimationPlaybackEnabled(
            false);
        QVERIFY(
            !viewModel.animationPlaybackEnabled());
        QVERIFY(!timer->isActive());
    }

    void delayedAnimationCallbackAdvancesOneFrameLikeMac()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            true);
        QTimer* timer =
            viewModel.findChild<QTimer*>(
                QStringLiteral(
                    "petAnimationTimer"));
        QSignalSpy frameSpy(
            &viewModel,
            &companion::PetViewModel::
                animationFrameChanged);
        QVERIFY(timer);
        QVERIFY(frameSpy.isValid());

        viewModel.setAnimationPlaybackEnabled(true);
        QThread::msleep(600);
        QCoreApplication::processEvents(
            QEventLoop::AllEvents);

        QCOMPARE(viewModel.frameRow(), 7);
        QCOMPARE(viewModel.frameColumn(), 1);
        QCOMPARE(frameSpy.count(), 1);
    }

    void shadowAtlasSequencesMatchBundledCompanionExtendedLayout()
    {
        const companion::PetAnimationSequence idle =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::Idle,
                1.0);

        QCOMPARE(idle.frames.size(), 16);
        QCOMPARE(idle.loopStartIndex, 0);
        for (int column = 0; column < 16; ++column) {
            QCOMPARE(idle.frames.at(column).row, 0);
            QCOMPARE(idle.frames.at(column).column, column);
        }
        QCOMPARE(idle.frames.constFirst().durationMilliseconds, 331);
        QCOMPARE(idle.frames.constLast().durationMilliseconds, 240);

        const companion::PetAnimationSequence runningLeft =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::RunningLeft,
                1.0);

        QCOMPARE(runningLeft.frames.size(), 16);
        QCOMPARE(runningLeft.loopStartIndex, 0);
        QCOMPARE(runningLeft.frames.constFirst().row, 2);
        QCOMPARE(runningLeft.frames.constLast().column, 15);
        QCOMPARE(
            runningLeft.frames.constFirst().durationMilliseconds,
            171);
        QCOMPARE(
            runningLeft.frames.constLast().durationMilliseconds,
            80);

        const companion::PetAnimationSequence goalComplete =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::GoalComplete,
                1.0);
        QCOMPARE(goalComplete.frames.size(), 48);
        QCOMPARE(goalComplete.loopStartIndex, 32);
        QCOMPARE(goalComplete.frames.constFirst().row, 9);
        QCOMPARE(goalComplete.frames.constFirst().column, 0);
        QCOMPARE(
            goalComplete.frames.constFirst().durationMilliseconds,
            248);
        for (int index = 0; index < 32; ++index) {
            QCOMPARE(goalComplete.frames.at(index).row, 9);
            QCOMPARE(goalComplete.frames.at(index).column, index % 16);
            QCOMPARE(
                goalComplete.frames.at(index).durationMilliseconds,
                index % 16 == 15
                    ? 180
                    : 248);
        }
        for (int index = 32; index < goalComplete.frames.size(); ++index) {
            QCOMPARE(goalComplete.frames.at(index).row, 0);
            QCOMPARE(goalComplete.frames.at(index).column, index - 32);
        }

        const companion::PetAnimationSequence thinking =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::Thinking,
                1.0);
        QCOMPARE(thinking.frames.size(), 16);
        QCOMPARE(thinking.frames.constFirst().row, 10);
        QCOMPARE(thinking.frames.constLast().column, 15);
        QCOMPARE(thinking.loopStartIndex, 0);
        QCOMPARE(
            thinking.frames.constFirst().durationMilliseconds,
            228);
        QCOMPARE(
            thinking.frames.constLast().durationMilliseconds,
            180);

        const companion::PetAnimationSequence talking =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::Talking,
                1.0);
        QCOMPARE(talking.frames.size(), 16);
        QCOMPARE(talking.frames.constFirst().row, 11);
        QCOMPARE(talking.frames.constLast().column, 15);
        QCOMPARE(talking.loopStartIndex, 0);
        QCOMPARE(
            talking.frames.constFirst().durationMilliseconds,
            115);
        QCOMPARE(
            talking.frames.constLast().durationMilliseconds,
            80);

        const companion::PetAnimationSequence review =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::Review,
                1.0);
        QCOMPARE(review.frames.size(), 64);
        QCOMPARE(review.loopStartIndex, 48);
        for (int index = 0; index < 48; ++index) {
            QCOMPARE(review.frames.at(index).row, 8);
            QCOMPARE(review.frames.at(index).column, index % 16);
        }
        for (int index = 48; index < review.frames.size(); ++index) {
            QCOMPARE(review.frames.at(index).row, 0);
            QCOMPARE(review.frames.at(index).column, index - 48);
        }
        QCOMPARE(
            review.frames.constFirst().durationMilliseconds,
            277);
        QCOMPARE(
            review.frames.at(15).durationMilliseconds,
            240);
    }

    void currentShadowRunningCadenceMatchesMacDefaultScale()
    {
        const companion::PetAnimationSequence running =
            companion::makeShadowAnimationSequence(
                companion::PetAnimation::RunningRight,
                1.15);

        QCOMPARE(running.frames.size(), 16);
        QCOMPARE(running.loopStartIndex, 0);
        QCOMPARE(
            running.frames.constFirst().durationMilliseconds,
            197);
        QCOMPARE(
            running.frames.constLast().durationMilliseconds,
            92);

        int cycleDurationMilliseconds = 0;
        for (const companion::PetFrame& frame :
             running.frames) {
            cycleDurationMilliseconds +=
                frame.durationMilliseconds;
        }
        QCOMPARE(
            cycleDurationMilliseconds,
            3047);
    }

    void shadowActionSequencesUseEveryExtendedAtlasRow()
    {
        const std::array actions = {
            std::pair{companion::PetAnimation::Idle, 0},
            std::pair{companion::PetAnimation::RunningRight, 1},
            std::pair{companion::PetAnimation::RunningLeft, 2},
            std::pair{companion::PetAnimation::Waving, 3},
            std::pair{companion::PetAnimation::Jumping, 4},
            std::pair{companion::PetAnimation::Failed, 5},
            std::pair{companion::PetAnimation::Waiting, 6},
            std::pair{companion::PetAnimation::Running, 7},
            std::pair{companion::PetAnimation::Review, 8},
            std::pair{companion::PetAnimation::GoalComplete, 9},
            std::pair{companion::PetAnimation::Thinking, 10},
            std::pair{companion::PetAnimation::Talking, 11},
        };

        for (const auto& [animation, expectedRow] : actions) {
            const companion::PetAnimationSequence sequence =
                companion::makeShadowAnimationSequence(
                    animation,
                    1.0);
            QVERIFY(!sequence.frames.isEmpty());
            QCOMPARE(sequence.frames.constFirst().row, expectedRow);
            QCOMPARE(sequence.frames.constFirst().column, 0);
            for (const companion::PetFrame& frame : sequence.frames) {
                QVERIFY2(
                    frame.row >= 0
                        && frame.row <= 11
                        && frame.column >= 0
                        && frame.column <= 15,
                    qPrintable(
                        companion::petAnimationName(animation)));
            }
        }
    }

    void shadowExtendedAtlasDoesNotExposeDirectionalLookRows()
    {
        companion::PetViewModel viewModel(
            true,
            1.0,
            false,
            false);
        viewModel.setRoamingIdle();
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("idle"));
        QCOMPARE(viewModel.frameRow(), 0);

        QVERIFY(!viewModel.updateDirectionalLook(
            QPointF(162.0, 0.0),
            QRectF(100.0, 100.0, 124.0, 164.0)));
        QVERIFY(!viewModel.directionalLookActive());
        QCOMPARE(viewModel.frameRow(), 0);
        QCOMPARE(viewModel.frameColumn(), 0);
    }

    void directionalLookRemainsInactiveWithoutSeparateLookSheet()
    {
        companion::PetViewModel viewModel;
        const QRectF petFrame(
            100.0, 100.0, 124.0, 164.0);

        QVERIFY(!viewModel.updateDirectionalLook(
            QPointF(162.0, 0.0),
            petFrame));

        viewModel.setRoamingIdle();
        QVERIFY(!viewModel.updateDirectionalLook(
            QPointF(500.0, 500.0),
            petFrame));
        QVERIFY(!viewModel.directionalLookActive());

        QVERIFY(!viewModel.updateDirectionalLook(
            QPointF(162.0, 0.0),
            petFrame));
        QCOMPARE(viewModel.frameRow(), 0);
        QCOMPARE(viewModel.frameColumn(), 0);

        viewModel.setPointerHovered(true);
        QVERIFY(!viewModel.directionalLookActive());
        QCOMPARE(
            viewModel.renderedAnimation(),
            QStringLiteral("jumping"));
    }
};

QTEST_GUILESS_MAIN(PetViewModelTests)
#include "PetViewModelTests.moc"
