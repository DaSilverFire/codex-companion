#pragma once

#include "core/Result.h"
#include "ui/pet/PetAnimation.h"
#include "ui/pet/PetDefinition.h"
#include "ui/pet/PetDirectionalLook.h"

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <optional>

class QTimer;

namespace companion {

class PetCatalog;

class PetViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibilityChanged)
    Q_PROPERTY(bool menuOpen READ menuOpen WRITE setMenuOpen NOTIFY menuOpenChanged)
    Q_PROPERTY(bool pointerHovered READ pointerHovered WRITE setPointerHovered NOTIFY hoverChanged)
    Q_PROPERTY(bool controlsHovered READ controlsHovered WRITE setControlsHovered NOTIFY hoverChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    Q_PROPERTY(bool goalCelebrationActive READ goalCelebrationActive NOTIFY goalCelebrationChanged)
    Q_PROPERTY(bool controlsVisible READ controlsVisible NOTIFY controlsVisibleChanged)
    Q_PROPERTY(QString selectedAnimation READ selectedAnimation WRITE setSelectedAnimation NOTIFY selectedAnimationChanged)
    Q_PROPERTY(QString renderedAnimation READ renderedAnimation NOTIFY renderedAnimationChanged)
    Q_PROPERTY(int frameRow READ frameRow NOTIFY animationFrameChanged)
    Q_PROPERTY(int frameColumn READ frameColumn NOTIFY animationFrameChanged)
    Q_PROPERTY(int frameDurationMilliseconds READ frameDurationMilliseconds NOTIFY animationFrameChanged)
    Q_PROPERTY(bool animationPlaybackEnabled READ animationPlaybackEnabled WRITE setAnimationPlaybackEnabled NOTIFY animationPlaybackEnabledChanged)
    Q_PROPERTY(bool directionalLookActive READ directionalLookActive NOTIFY directionalLookChanged)
    Q_PROPERTY(double animationSpeedScale READ animationSpeedScale WRITE setAnimationSpeedScale NOTIFY animationSpeedScaleChanged)
    Q_PROPERTY(bool hideControlsUntilHover READ hideControlsUntilHover WRITE setHideControlsUntilHover NOTIFY preferencesChanged)
    Q_PROPERTY(bool allowAutonomousMovement READ allowAutonomousMovement WRITE setAllowAutonomousMovement NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantList availablePets READ availablePets NOTIFY availablePetsChanged)
    Q_PROPERTY(QString selectedPetId READ selectedPetId NOTIFY selectedPetChanged)
    Q_PROPERTY(QUrl spriteSheetSource READ spriteSheetSource NOTIFY selectedPetChanged)
    Q_PROPERTY(int spriteColumns READ spriteColumns NOTIFY selectedPetChanged)
    Q_PROPERTY(int spriteRows READ spriteRows NOTIFY selectedPetChanged)
    Q_PROPERTY(int sourceFrameWidth READ sourceFrameWidth NOTIFY selectedPetChanged)
    Q_PROPERTY(int sourceFrameHeight READ sourceFrameHeight NOTIFY selectedPetChanged)

public:
    using VisibilityPersistCommand =
        std::function<Result<void>(bool)>;
    using PetSelectionPersistCommand =
        std::function<
            Result<void>(const QString&)>;

    explicit PetViewModel(
        bool visible = true,
        double animationSpeedScale = 1.15,
        bool hideControlsUntilHover = false,
        bool allowAutonomousMovement = true,
        VisibilityPersistCommand persistVisibility = {},
        QObject* parent = nullptr);

    bool visible() const noexcept;
    bool menuOpen() const noexcept;
    bool pointerHovered() const noexcept;
    bool controlsHovered() const noexcept;
    bool dragging() const noexcept;
    bool goalCelebrationActive() const noexcept;
    bool controlsVisible() const noexcept;
    QString selectedAnimation() const;
    QString renderedAnimation() const;
    int frameRow() const noexcept;
    int frameColumn() const noexcept;
    int frameDurationMilliseconds() const noexcept;
    bool animationPlaybackEnabled() const noexcept;
    bool directionalLookActive() const noexcept;
    bool attentionActive() const noexcept;
    double animationSpeedScale() const noexcept;
    bool hideControlsUntilHover() const noexcept;
    bool allowAutonomousMovement() const noexcept;
    QVariantList availablePets() const;
    QString selectedPetId() const;
    QUrl spriteSheetSource() const;
    int spriteColumns() const noexcept;
    int spriteRows() const noexcept;
    int sourceFrameWidth() const noexcept;
    int sourceFrameHeight() const noexcept;

    Result<void> configurePetCatalog(
        PetCatalog& catalog,
        QString selectedPetId,
        PetSelectionPersistCommand persistSelection = {});

    void setVisible(bool visible);
    void setMenuOpen(bool menuOpen);
    void setPointerHovered(bool hovered);
    void setControlsHovered(bool hovered);
    Q_INVOKABLE void setHoverState(
        bool pointerHovered,
        bool controlsHovered);
    void setAnimationPlaybackEnabled(bool enabled);
    void setAnimationSpeedScale(double speedScale);
    void setHideControlsUntilHover(bool hideUntilHover);
    void setAllowAutonomousMovement(bool allow);

    Q_INVOKABLE void toggleVisible();
    Q_INVOKABLE void toggleMenu();
    Q_INVOKABLE void beginDrag();
    Q_INVOKABLE void updateDrag(double dx, double dy);
    Q_INVOKABLE void endDrag();
    Q_INVOKABLE void advanceAnimationFrame();
    Q_INVOKABLE bool setRoamingMotion(double dx, double dy);
    Q_INVOKABLE void setRoamingIdle();
    Q_INVOKABLE void setAnimation(const QString& animation);
    Q_INVOKABLE void setSelectedAnimation(const QString& animation);
    Q_INVOKABLE void setAttentionAnimation(const QString& animation);
    Q_INVOKABLE void clearAttentionAnimation();
    Q_INVOKABLE bool selectPet(const QString& petId);
    Q_INVOKABLE bool reloadPets();
    void beginGoalCelebration();
    void endGoalCelebration();
    bool updateDirectionalLook(
        QPointF pointer,
        QRectF petFrame);
    void clearDirectionalLook();

signals:
    void visibilityChanged();
    void menuOpenChanged();
    void hoverChanged();
    void draggingChanged();
    void goalCelebrationChanged();
    void controlsVisibleChanged();
    void selectedAnimationChanged();
    void renderedAnimationChanged();
    void animationFrameChanged();
    void animationPlaybackEnabledChanged();
    void directionalLookChanged();
    void animationSpeedScaleChanged();
    void preferencesChanged();
    void availablePetsChanged();
    void selectedPetChanged();
    void runtimeErrorOccurred(CompanionError error);

private:
    PetAnimation renderedState() const noexcept;
    static PetAnimation horizontalRunState(
        double dx,
        PetAnimation fallback) noexcept;
    void emitDerivedChanges(
        PetAnimation previousAnimation,
        bool previousControlsVisible);
    bool advanceAnimationFrameIndex();
    void handleAnimationTimeout();
    void restartAnimationSchedule();
    void scheduleAnimationFrame();
    void restoreInteractionAfterPointerAction();
    void resetAnimationSequence(bool notify);
    QVariantList catalogPetRows() const;
    void applySelectedPet(
        PetDefinition pet,
        bool notify);

    bool visible_ = true;
    bool menuOpen_ = false;
    bool pointerHovered_ = false;
    bool controlsHovered_ = false;
    bool dragging_ = false;
    bool goalCelebrationActive_ = false;
    bool animationPlaybackEnabled_ = false;
    double animationSpeedScale_ = 1.15;
    bool hideControlsUntilHover_ = false;
    bool allowAutonomousMovement_ = true;
    VisibilityPersistCommand persistVisibility_;
    PetCatalog* petCatalog_ = nullptr;
    PetSelectionPersistCommand
        persistPetSelection_;
    QVariantList availablePets_;
    QString selectedPetId_;
    std::optional<PetDefinition>
        selectedPet_;
    PetAnimation selectedAnimation_ = PetAnimation::Idle;
    PetAnimation roamingAnimation_ = PetAnimation::Running;
    PetAnimation lastManualRunAnimation_ =
        PetAnimation::RunningRight;
    PetAnimation lastRoamingRunAnimation_ =
        PetAnimation::RunningRight;
    std::optional<PetAnimation> attentionAnimation_;
    std::optional<PetAnimation> interactionAnimation_;
    std::optional<PetDirectionalLookFrame>
        directionalLookFrame_;
    PetAnimationSequence animationSequence_;
    int animationFrameIndex_ = 0;
    QTimer* animationTimer_ = nullptr;
};

} // namespace companion
