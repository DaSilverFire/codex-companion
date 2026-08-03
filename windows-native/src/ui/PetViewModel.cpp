#include "ui/PetViewModel.h"

#include "ui/CompanionPresentationPolicy.h"
#include "ui/pet/PetCatalog.h"

#include <QTimer>
#include <QVariantMap>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

double normalizedSpeedScale(double value)
{
    if (std::isnan(value)) {
        return 1.15;
    }
    if (std::isinf(value)) {
        return value < 0.0 ? 0.4 : 3.0;
    }
    return std::clamp(value, 0.4, 3.0);
}

companion::CompanionError invalidAnimationError(
    const QString& animation)
{
    return {
        QStringLiteral("pet.animation-invalid"),
        QStringLiteral("The requested pet animation is not available."),
        false,
        {{QStringLiteral("animation"), animation}},
    };
}

companion::CompanionError invalidPetError(
    const QString& petId)
{
    return {
        QStringLiteral("pet.selection-invalid"),
        QStringLiteral(
            "The requested pet is not available."),
        false,
        {
            {
                QStringLiteral("petId"),
                petId,
            },
        },
    };
}

} // namespace

namespace companion {

PetViewModel::PetViewModel(
    bool visible,
    double animationSpeedScale,
    bool hideControlsUntilHover,
    bool allowAutonomousMovement,
    VisibilityPersistCommand persistVisibility,
    QObject* parent)
    : QObject(parent),
      visible_(visible),
      animationSpeedScale_(
          normalizedSpeedScale(animationSpeedScale)),
      hideControlsUntilHover_(hideControlsUntilHover),
      allowAutonomousMovement_(allowAutonomousMovement),
      persistVisibility_(std::move(persistVisibility))
{
    qRegisterMetaType<CompanionError>(
        "companion::CompanionError");
    animationTimer_ = new QTimer(this);
    animationTimer_->setObjectName(
        QStringLiteral("petAnimationTimer"));
    animationTimer_->setSingleShot(true);
    animationTimer_->setTimerType(
        Qt::PreciseTimer);
    connect(
        animationTimer_,
        &QTimer::timeout,
        this,
        [this] {
            handleAnimationTimeout();
        });
    resetAnimationSequence(false);
}

bool PetViewModel::visible() const noexcept
{
    return visible_;
}

bool PetViewModel::menuOpen() const noexcept
{
    return menuOpen_;
}

bool PetViewModel::pointerHovered() const noexcept
{
    return pointerHovered_;
}

bool PetViewModel::controlsHovered() const noexcept
{
    return controlsHovered_;
}

bool PetViewModel::dragging() const noexcept
{
    return dragging_;
}

bool PetViewModel::goalCelebrationActive() const
    noexcept
{
    return goalCelebrationActive_;
}

bool PetViewModel::controlsVisible() const noexcept
{
    return CompanionPresentationPolicy::
        showsPetMenuControls(
            menuOpen_,
            hideControlsUntilHover_,
            pointerHovered_,
            controlsHovered_);
}

QString PetViewModel::selectedAnimation() const
{
    return petAnimationName(selectedAnimation_);
}

QString PetViewModel::renderedAnimation() const
{
    return petAnimationName(renderedState());
}

int PetViewModel::frameRow() const noexcept
{
    if (directionalLookFrame_) {
        return directionalLookFrame_->row;
    }
    return animationSequence_.frames.isEmpty()
        ? 0
        : animationSequence_.frames
              .at(animationFrameIndex_)
              .row;
}

int PetViewModel::frameColumn() const noexcept
{
    if (directionalLookFrame_) {
        return directionalLookFrame_->column;
    }
    return animationSequence_.frames.isEmpty()
        ? 0
        : animationSequence_.frames
              .at(animationFrameIndex_)
              .column;
}

int PetViewModel::frameDurationMilliseconds() const noexcept
{
    if (directionalLookFrame_) {
        return 1000;
    }
    return animationSequence_.frames.isEmpty()
        ? 160
        : animationSequence_.frames
              .at(animationFrameIndex_)
              .durationMilliseconds;
}

bool PetViewModel::animationPlaybackEnabled() const noexcept
{
    return animationPlaybackEnabled_;
}

bool PetViewModel::directionalLookActive() const
    noexcept
{
    return directionalLookFrame_.has_value();
}

bool PetViewModel::attentionActive() const noexcept
{
    return attentionAnimation_.has_value();
}

double PetViewModel::animationSpeedScale() const noexcept
{
    return animationSpeedScale_;
}

bool PetViewModel::hideControlsUntilHover() const noexcept
{
    return hideControlsUntilHover_;
}

bool PetViewModel::allowAutonomousMovement() const noexcept
{
    return allowAutonomousMovement_;
}

QVariantList PetViewModel::availablePets() const
{
    return availablePets_;
}

QString PetViewModel::selectedPetId() const
{
    return selectedPetId_;
}

QUrl PetViewModel::spriteSheetSource() const
{
    return selectedPet_.has_value()
        ? selectedPet_->spriteSheetUrl()
        : QUrl();
}

int PetViewModel::spriteColumns() const noexcept
{
    return selectedPet_.has_value()
        ? selectedPet_->spriteColumns
        : 16;
}

int PetViewModel::spriteRows() const noexcept
{
    return selectedPet_.has_value()
        ? selectedPet_->spriteRows
        : 12;
}

int PetViewModel::sourceFrameWidth() const noexcept
{
    return selectedPet_.has_value()
        ? selectedPet_->sourceFrameSize.width()
        : 192;
}

int PetViewModel::sourceFrameHeight() const noexcept
{
    return selectedPet_.has_value()
        ? selectedPet_->sourceFrameSize.height()
        : 208;
}

Result<void> PetViewModel::configurePetCatalog(
    PetCatalog& catalog,
    QString selectedPetId,
    PetSelectionPersistCommand persistSelection)
{
    const auto reloaded = catalog.reload();
    if (!reloaded.hasValue()) {
        return reloaded;
    }

    const QString resolved =
        catalog.resolveSelection(selectedPetId);
    const auto selected =
        catalog.find(resolved);
    if (!resolved.isEmpty()
        && resolved != selectedPetId
        && persistSelection) {
        const auto persisted =
            persistSelection(resolved);
        if (!persisted.hasValue()) {
            return persisted;
        }
    }

    petCatalog_ = &catalog;
    persistPetSelection_ =
        std::move(persistSelection);
    availablePets_ = catalogPetRows();
    selectedPetId_ = resolved;
    if (selected.has_value()) {
        applySelectedPet(*selected, false);
    } else {
        selectedPet_.reset();
        resetAnimationSequence(false);
    }
    emit availablePetsChanged();
    emit selectedPetChanged();
    emit animationFrameChanged();
    return Result<void>::success();
}

void PetViewModel::setVisible(bool visible)
{
    if (visible_ == visible) {
        return;
    }

    if (persistVisibility_) {
        const auto persisted =
            persistVisibility_(visible);
        if (!persisted.hasValue()) {
            emit runtimeErrorOccurred(
                persisted.error());
            return;
        }
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const bool menuWasOpen = menuOpen_;
    const bool wasHovered =
        pointerHovered_ || controlsHovered_;
    const bool wasDragging = dragging_;

    visible_ = visible;
    if (!visible_) {
        clearDirectionalLook();
        menuOpen_ = false;
        pointerHovered_ = false;
        controlsHovered_ = false;
        dragging_ = false;
        interactionAnimation_.reset();
    } else if (attentionAnimation_) {
        interactionAnimation_ =
            attentionAnimation_;
    }

    emit visibilityChanged();
    if (menuWasOpen != menuOpen_) {
        emit menuOpenChanged();
    }
    if (wasHovered) {
        emit hoverChanged();
    }
    if (wasDragging != dragging_) {
        emit draggingChanged();
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setMenuOpen(bool menuOpen)
{
    if (menuOpen_ == menuOpen) {
        return;
    }
    if (menuOpen && !visible_) {
        setVisible(true);
        if (!visible_) {
            return;
        }
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    menuOpen_ = menuOpen;
    if (menuOpen_) {
        clearDirectionalLook();
    }
    emit menuOpenChanged();
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setPointerHovered(bool hovered)
{
    setHoverState(
        hovered,
        controlsHovered_);
}

void PetViewModel::setControlsHovered(bool hovered)
{
    setHoverState(
        pointerHovered_,
        hovered);
}

void PetViewModel::setHoverState(
    bool pointerHovered,
    bool controlsHovered)
{
    const bool pointerChanged =
        pointerHovered_ != pointerHovered;
    const bool controlsChanged =
        controlsHovered_ != controlsHovered;
    if (!pointerChanged && !controlsChanged) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    pointerHovered_ = pointerHovered;
    controlsHovered_ = controlsHovered;
    if (pointerChanged) {
        clearDirectionalLook();
        if (!dragging_) {
            if (pointerHovered_) {
                interactionAnimation_ =
                    PetAnimation::Jumping;
                roamingAnimation_ =
                    PetAnimation::Jumping;
            } else {
                restoreInteractionAfterPointerAction();
            }
        }
    }

    emit hoverChanged();
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setAnimationPlaybackEnabled(
    bool enabled)
{
    if (animationPlaybackEnabled_ == enabled) {
        return;
    }

    animationPlaybackEnabled_ = enabled;
    if (animationPlaybackEnabled_) {
        restartAnimationSchedule();
    } else {
        animationTimer_->stop();
    }
    emit animationPlaybackEnabledChanged();
}

void PetViewModel::setAnimationSpeedScale(double speedScale)
{
    const double normalized =
        normalizedSpeedScale(speedScale);
    if (qFuzzyCompare(
            animationSpeedScale_,
            normalized)) {
        return;
    }

    animationSpeedScale_ = normalized;
    resetAnimationSequence(false);
    emit animationFrameChanged();
    emit animationSpeedScaleChanged();
}

void PetViewModel::setHideControlsUntilHover(
    bool hideUntilHover)
{
    if (hideControlsUntilHover_ == hideUntilHover) {
        return;
    }

    const bool previousControlsVisible =
        controlsVisible();
    hideControlsUntilHover_ = hideUntilHover;
    emit preferencesChanged();
    if (previousControlsVisible != controlsVisible()) {
        emit controlsVisibleChanged();
    }
}

void PetViewModel::setAllowAutonomousMovement(bool allow)
{
    if (allowAutonomousMovement_ == allow) {
        return;
    }
    allowAutonomousMovement_ = allow;
    emit preferencesChanged();
}

void PetViewModel::toggleVisible()
{
    setVisible(!visible_);
}

void PetViewModel::toggleMenu()
{
    setMenuOpen(!menuOpen_);
}

void PetViewModel::beginDrag()
{
    if (dragging_) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    clearDirectionalLook();
    dragging_ = true;
    interactionAnimation_ =
        lastManualRunAnimation_;
    emit draggingChanged();
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::updateDrag(double dx, double dy)
{
    Q_UNUSED(dy);
    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const bool wasDragging = dragging_;
    clearDirectionalLook();
    dragging_ = true;
    const PetAnimation next =
        horizontalRunState(
            dx,
            lastManualRunAnimation_);
    lastManualRunAnimation_ = next;
    interactionAnimation_ = next;
    roamingAnimation_ = next;
    if (!wasDragging) {
        emit draggingChanged();
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::endDrag()
{
    if (!dragging_) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    dragging_ = false;
    restoreInteractionAfterPointerAction();
    emit draggingChanged();
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::advanceAnimationFrame()
{
    if (!advanceAnimationFrameIndex()) {
        return;
    }

    if (animationPlaybackEnabled_) {
        restartAnimationSchedule();
    }
    emit animationFrameChanged();
}

bool PetViewModel::setRoamingMotion(double dx, double dy)
{
    Q_UNUSED(dy);
    if (!CompanionPresentationPolicy::
            acceptsRoamingMotion(
                pointerHovered_,
                dragging_,
                interactionAnimation_.has_value())) {
        return false;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    clearDirectionalLook();
    const PetAnimation next =
        horizontalRunState(
            dx,
            lastRoamingRunAnimation_);
    lastRoamingRunAnimation_ = next;
    roamingAnimation_ = next;
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
    return true;
}

void PetViewModel::setRoamingIdle()
{
    if (pointerHovered_ || dragging_
        || roamingAnimation_ == PetAnimation::Idle) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    roamingAnimation_ = PetAnimation::Idle;
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setAnimation(const QString& animation)
{
    const auto parsed =
        petAnimationFromName(animation);
    if (!parsed.has_value()) {
        emit runtimeErrorOccurred(
            invalidAnimationError(animation));
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const bool selectedChanged =
        selectedAnimation_ != *parsed;
    clearDirectionalLook();
    if (!pointerHovered_
        && !dragging_
        && !attentionAnimation_
        && !goalCelebrationActive_) {
        interactionAnimation_.reset();
    }
    selectedAnimation_ = *parsed;
    if (selectedChanged) {
        emit selectedAnimationChanged();
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setSelectedAnimation(
    const QString& animation)
{
    const auto parsed =
        petAnimationFromName(animation);
    if (!parsed.has_value()) {
        emit runtimeErrorOccurred(
            invalidAnimationError(animation));
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const bool selectedChanged =
        selectedAnimation_ != *parsed;
    clearDirectionalLook();
    selectedAnimation_ = *parsed;
    if (selectedChanged) {
        emit selectedAnimationChanged();
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::setAttentionAnimation(
    const QString& animation)
{
    const auto parsed =
        petAnimationFromName(animation);
    if (!parsed.has_value()) {
        emit runtimeErrorOccurred(
            invalidAnimationError(animation));
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    clearDirectionalLook();
    attentionAnimation_ = *parsed;
    if (!dragging_
        && !pointerHovered_
        && interactionAnimation_
            != PetAnimation::GoalComplete) {
        interactionAnimation_ = *parsed;
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::clearAttentionAnimation()
{
    if (!attentionAnimation_) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const PetAnimation dismissedAnimation =
        *attentionAnimation_;
    attentionAnimation_.reset();
    if (!dragging_
        && interactionAnimation_
            == dismissedAnimation
        && !(goalCelebrationActive_
             && dismissedAnimation
                 == PetAnimation::GoalComplete)) {
        if (pointerHovered_) {
            interactionAnimation_ =
                PetAnimation::Jumping;
            roamingAnimation_ =
                PetAnimation::Jumping;
        } else {
            interactionAnimation_.reset();
        }
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

bool PetViewModel::selectPet(
    const QString& petId)
{
    if (petCatalog_ == nullptr) {
        emit runtimeErrorOccurred(
            invalidPetError(petId));
        return false;
    }

    const auto selected =
        petCatalog_->find(petId);
    if (!selected.has_value()) {
        emit runtimeErrorOccurred(
            invalidPetError(petId));
        return false;
    }
    if (selected->id == selectedPetId_) {
        return true;
    }

    if (persistPetSelection_) {
        const auto persisted =
            persistPetSelection_(
                selected->id);
        if (!persisted.hasValue()) {
            emit runtimeErrorOccurred(
                persisted.error());
            return false;
        }
    }

    selectedPetId_ = selected->id;
    applySelectedPet(*selected, true);
    return true;
}

bool PetViewModel::reloadPets()
{
    if (petCatalog_ == nullptr) {
        emit runtimeErrorOccurred(
            invalidPetError(selectedPetId_));
        return false;
    }

    const auto reloaded =
        petCatalog_->reload();
    if (!reloaded.hasValue()) {
        emit runtimeErrorOccurred(
            reloaded.error());
        return false;
    }

    const QString resolved =
        petCatalog_->resolveSelection(
            selectedPetId_);
    const auto selected =
        petCatalog_->find(resolved);
    if (resolved != selectedPetId_
        && !resolved.isEmpty()
        && persistPetSelection_) {
        const auto persisted =
            persistPetSelection_(
                resolved);
        if (!persisted.hasValue()) {
            emit runtimeErrorOccurred(
                persisted.error());
            return false;
        }
    }

    availablePets_ = catalogPetRows();
    selectedPetId_ = resolved;
    if (selected.has_value()) {
        applySelectedPet(*selected, false);
    } else {
        selectedPet_.reset();
        resetAnimationSequence(false);
    }
    emit availablePetsChanged();
    emit selectedPetChanged();
    emit animationFrameChanged();
    return true;
}

void PetViewModel::beginGoalCelebration()
{
    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    const bool wasActive =
        goalCelebrationActive_;
    const bool selectedChanged =
        selectedAnimation_
        != PetAnimation::GoalComplete;
    clearDirectionalLook();
    goalCelebrationActive_ = true;
    interactionAnimation_ =
        PetAnimation::GoalComplete;
    selectedAnimation_ =
        PetAnimation::GoalComplete;
    if (selectedChanged) {
        emit selectedAnimationChanged();
    }
    if (!wasActive) {
        emit goalCelebrationChanged();
    }
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

void PetViewModel::endGoalCelebration()
{
    if (!goalCelebrationActive_) {
        return;
    }

    const PetAnimation previousAnimation =
        renderedState();
    const bool previousControlsVisible =
        controlsVisible();
    bool selectedChanged = false;
    goalCelebrationActive_ = false;
    if (interactionAnimation_
            == PetAnimation::GoalComplete) {
        interactionAnimation_.reset();
    }
    if (selectedAnimation_
            == PetAnimation::GoalComplete) {
        selectedAnimation_ =
            PetAnimation::Review;
        selectedChanged = true;
    }
    if (selectedChanged) {
        emit selectedAnimationChanged();
    }
    emit goalCelebrationChanged();
    emitDerivedChanges(
        previousAnimation,
        previousControlsVisible);
}

bool PetViewModel::updateDirectionalLook(
    QPointF pointer,
    QRectF petFrame)
{
    Q_UNUSED(pointer);
    Q_UNUSED(petFrame);
    clearDirectionalLook();
    return false;
}

void PetViewModel::clearDirectionalLook()
{
    if (!directionalLookFrame_) {
        return;
    }
    directionalLookFrame_.reset();
    emit directionalLookChanged();
    emit animationFrameChanged();
}

PetAnimation PetViewModel::renderedState() const noexcept
{
    if (interactionAnimation_) {
        return *interactionAnimation_;
    }
    return menuOpen_
        ? selectedAnimation_
        : roamingAnimation_;
}

PetAnimation PetViewModel::horizontalRunState(
    double dx,
    PetAnimation fallback) noexcept
{
    constexpr double threshold = 0.12;
    if (dx < -threshold) {
        return PetAnimation::RunningLeft;
    }
    if (dx > threshold) {
        return PetAnimation::RunningRight;
    }
    return fallback == PetAnimation::RunningLeft
        ? PetAnimation::RunningLeft
        : PetAnimation::RunningRight;
}

void PetViewModel::emitDerivedChanges(
    PetAnimation previousAnimation,
    bool previousControlsVisible)
{
    if (previousAnimation != renderedState()) {
        resetAnimationSequence(true);
        emit renderedAnimationChanged();
    }
    if (previousControlsVisible != controlsVisible()) {
        emit controlsVisibleChanged();
    }
}

void PetViewModel::restoreInteractionAfterPointerAction()
{
    if (pointerHovered_) {
        interactionAnimation_ =
            PetAnimation::Jumping;
        roamingAnimation_ =
            PetAnimation::Jumping;
        return;
    }
    if (attentionAnimation_) {
        interactionAnimation_ =
            attentionAnimation_;
        return;
    }
    interactionAnimation_.reset();
    roamingAnimation_ =
        PetAnimation::Running;
}

bool PetViewModel::advanceAnimationFrameIndex()
{
    if (animationSequence_.frames.size() < 2) {
        return false;
    }

    const int frameCount =
        static_cast<int>(
            animationSequence_.frames.size());
    const int nextIndex =
        animationFrameIndex_ + 1;
    animationFrameIndex_ =
        nextIndex >= frameCount
        ? std::clamp(
              animationSequence_.loopStartIndex,
              0,
              frameCount - 1)
        : nextIndex;
    return true;
}

void PetViewModel::handleAnimationTimeout()
{
    if (!animationPlaybackEnabled_
        || animationSequence_.frames.size() < 2) {
        animationTimer_->stop();
        return;
    }

    if (advanceAnimationFrameIndex()) {
        emit animationFrameChanged();
    }
    scheduleAnimationFrame();
}

void PetViewModel::restartAnimationSchedule()
{
    animationTimer_->stop();
    scheduleAnimationFrame();
}

void PetViewModel::scheduleAnimationFrame()
{
    if (!animationPlaybackEnabled_
        || animationSequence_.frames.size() < 2) {
        animationTimer_->stop();
        return;
    }
    animationTimer_->start(
        std::max(
            40,
            frameDurationMilliseconds()));
}

void PetViewModel::resetAnimationSequence(bool notify)
{
    animationSequence_ =
        selectedPet_.has_value()
        ? selectedPet_->animationSequence(
              renderedState(),
              animationSpeedScale_)
        : PetAnimationSequence{};
    animationFrameIndex_ = 0;
    if (animationPlaybackEnabled_) {
        restartAnimationSchedule();
    } else {
        animationTimer_->stop();
    }
    if (notify) {
        emit animationFrameChanged();
    }
}

QVariantList PetViewModel::catalogPetRows() const
{
    QVariantList rows;
    if (petCatalog_ == nullptr) {
        return rows;
    }
    rows.reserve(petCatalog_->pets().size());
    for (const PetDefinition& pet :
         petCatalog_->pets()) {
        const QString sourceTitle =
            pet.sourceTitle();
        rows.append(QVariantMap{
            {QStringLiteral("id"), pet.id},
            {
                QStringLiteral("displayName"),
                pet.displayName,
            },
            {
                QStringLiteral("sourceTitle"),
                sourceTitle,
            },
            {
                QStringLiteral("label"),
                QStringLiteral("%1 \u00B7 %2")
                    .arg(
                        pet.displayName,
                        sourceTitle),
            },
        });
    }
    return rows;
}

void PetViewModel::applySelectedPet(
    PetDefinition pet,
    bool notify)
{
    selectedPet_ = std::move(pet);
    resetAnimationSequence(false);
    if (!notify) {
        return;
    }
    emit selectedPetChanged();
    emit animationFrameChanged();
}

} // namespace companion
