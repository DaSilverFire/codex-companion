#pragma once

#include <QString>
#include <QStringView>
#include <QVector>
#include <optional>

namespace companion {

enum class PetAnimation {
    Idle,
    RunningRight,
    RunningLeft,
    Waving,
    Jumping,
    Failed,
    Waiting,
    Running,
    Review,
    GoalComplete,
    Thinking,
    Talking,
};

struct PetFrame final {
    int row = 0;
    int column = 0;
    int durationMilliseconds = 160;

    friend bool operator==(const PetFrame&, const PetFrame&) = default;
};

struct PetAnimationSequence final {
    QVector<PetFrame> frames;
    int loopStartIndex = 0;

    friend bool operator==(
        const PetAnimationSequence&,
        const PetAnimationSequence&) = default;
};

QString petAnimationName(PetAnimation animation);
std::optional<PetAnimation> petAnimationFromName(QStringView name);
PetAnimationSequence makeShadowAnimationSequence(
    PetAnimation animation,
    double speedScale);

} // namespace companion
