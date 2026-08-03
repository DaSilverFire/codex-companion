#include "ui/pet/PetAnimation.h"

#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

using companion::PetAnimation;
using companion::PetFrame;

struct AnimationLayout final {
    int row;
    int frameCount;
    double previewDurationMilliseconds;
    double finalDurationMilliseconds;
    bool loopsContinuously;
    double targetCycleDurationMilliseconds;
    double minimumFrameDurationMilliseconds;
};

AnimationLayout layoutFor(PetAnimation animation)
{
    // Keep this fixed package contract aligned with the macOS
    // PetAnimationState timing and row definitions.
    switch (animation) {
    case PetAnimation::Idle:
        return {0, 16, 160, 240, true, 5200, 80};
    case PetAnimation::RunningRight:
        return {1, 16, 80, 80, true, 2650, 55};
    case PetAnimation::RunningLeft:
        return {2, 16, 80, 80, true, 2650, 55};
    case PetAnimation::Waving:
        return {3, 16, 120, 200, false, 3600, 80};
    case PetAnimation::Jumping:
        return {4, 16, 90, 200, false, 2750, 65};
    case PetAnimation::Failed:
        return {5, 16, 150, 240, false, 4800, 80};
    case PetAnimation::Waiting:
        return {6, 16, 140, 240, false, 5000, 80};
    case PetAnimation::Running:
        return {7, 16, 80, 80, true, 2650, 55};
    case PetAnimation::Review:
        return {8, 16, 140, 240, false, 4400, 80};
    case PetAnimation::GoalComplete:
        return {9, 16, 100, 180, false, 3900, 65};
    case PetAnimation::Thinking:
        return {10, 16, 130, 180, true, 3600, 80};
    case PetAnimation::Talking:
        return {11, 16, 75, 80, true, 1800, 55};
    }
    return {0, 16, 160, 240, true, 5200, 80};
}

double normalizedSpeedScale(double value)
{
    if (std::isnan(value)) {
        return 1.0;
    }
    if (std::isinf(value)) {
        return value < 0.0 ? 0.4 : 3.0;
    }
    return std::clamp(value, 0.4, 3.0);
}

QVector<PetFrame> makeFrames(
    const AnimationLayout& layout,
    double speedScale)
{
    const double finalDuration =
        std::max(
            layout.previewDurationMilliseconds,
            layout.finalDurationMilliseconds);
    const double bodyDuration =
        std::max(
            0.0,
            layout.targetCycleDurationMilliseconds
                - finalDuration);
    const double baseDuration =
        layout.frameCount <= 12
        ? layout.previewDurationMilliseconds
        : std::max(
              layout.minimumFrameDurationMilliseconds,
              bodyDuration
                  / static_cast<double>(
                      std::max(1, layout.frameCount - 1)));

    QVector<PetFrame> frames;
    frames.reserve(layout.frameCount);
    for (int column = 0; column < layout.frameCount; ++column) {
        const double duration =
            column == layout.frameCount - 1
            ? finalDuration
            : baseDuration;
        frames.append({
            layout.row,
            column,
            qMax(40, qRound(duration * speedScale)),
        });
    }
    return frames;
}

} // namespace

namespace companion {

QString petAnimationName(PetAnimation animation)
{
    switch (animation) {
    case PetAnimation::Idle:
        return QStringLiteral("idle");
    case PetAnimation::RunningRight:
        return QStringLiteral("running-right");
    case PetAnimation::RunningLeft:
        return QStringLiteral("running-left");
    case PetAnimation::Waving:
        return QStringLiteral("waving");
    case PetAnimation::Jumping:
        return QStringLiteral("jumping");
    case PetAnimation::Failed:
        return QStringLiteral("failed");
    case PetAnimation::Waiting:
        return QStringLiteral("waiting");
    case PetAnimation::Running:
        return QStringLiteral("running");
    case PetAnimation::Review:
        return QStringLiteral("review");
    case PetAnimation::GoalComplete:
        return QStringLiteral("goal-complete");
    case PetAnimation::Thinking:
        return QStringLiteral("thinking");
    case PetAnimation::Talking:
        return QStringLiteral("talking");
    }
    return QStringLiteral("idle");
}

std::optional<PetAnimation> petAnimationFromName(QStringView name)
{
    constexpr std::array animations = {
        PetAnimation::Idle,
        PetAnimation::RunningRight,
        PetAnimation::RunningLeft,
        PetAnimation::Waving,
        PetAnimation::Jumping,
        PetAnimation::Failed,
        PetAnimation::Waiting,
        PetAnimation::Running,
        PetAnimation::Review,
        PetAnimation::GoalComplete,
        PetAnimation::Thinking,
        PetAnimation::Talking,
    };
    for (const PetAnimation animation : animations) {
        if (name == petAnimationName(animation)) {
            return animation;
        }
    }
    return std::nullopt;
}

PetAnimationSequence makeShadowAnimationSequence(
    PetAnimation animation,
    double speedScale)
{
    const double normalizedScale =
        normalizedSpeedScale(speedScale);
    if (animation == PetAnimation::GoalComplete) {
        const QVector<PetFrame> nativeFrames =
            makeFrames(
                layoutFor(PetAnimation::GoalComplete),
                normalizedScale);
        const QVector<PetFrame> idleFrames =
            makeFrames(
                layoutFor(PetAnimation::Idle),
                normalizedScale);

        QVector<PetFrame> frames;
        frames.reserve(
            nativeFrames.size() * 2
            + idleFrames.size());
        frames.append(nativeFrames);
        if (nativeFrames.size() <= 16) {
            frames.append(nativeFrames);
        }
        const int loopStartIndex = frames.size();
        frames.append(idleFrames);
        return {std::move(frames), loopStartIndex};
    }

    const AnimationLayout actionLayout =
        layoutFor(animation);
    const QVector<PetFrame> actionFrames =
        makeFrames(actionLayout, normalizedScale);

    if (animation == PetAnimation::Idle
        || actionLayout.loopsContinuously) {
        return {actionFrames, 0};
    }

    QVector<PetFrame> prelude;
    const int repeatCount =
        actionLayout.frameCount > 16
        ? 1
        : 3;
    prelude.reserve(actionFrames.size() * repeatCount);
    for (int repeat = 0; repeat < repeatCount; ++repeat) {
        prelude.append(actionFrames);
    }

    const QVector<PetFrame> idleFrames =
        makeFrames(
            layoutFor(PetAnimation::Idle),
            normalizedScale);
    const int loopStartIndex = prelude.size();
    prelude.append(idleFrames);
    return {std::move(prelude), loopStartIndex};
}

} // namespace companion
