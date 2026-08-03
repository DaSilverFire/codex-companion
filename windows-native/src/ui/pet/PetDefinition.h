#pragma once

#include "core/Result.h"
#include "ui/pet/PetAnimation.h"

#include <QHash>
#include <QSize>
#include <QString>
#include <QUrl>

#include <optional>

namespace companion {

enum class PetSourceKind {
    Custom,
    BuiltIn,
};

struct PetMobilePresence final {
    QString directory;
    QString packageId;
    QString contentHash;

    friend bool operator==(
        const PetMobilePresence&,
        const PetMobilePresence&) = default;
};

struct PetDefinition final {
    QString id;
    QString displayName;
    QString description;
    QString spritesheetPath;
    int spriteColumns = 8;
    int spriteRows = 9;
    QHash<QString, int> animationFrameCounts;
    PetSourceKind source = PetSourceKind::Custom;
    QString sourceDirectory;
    QSize sourceFrameSize;
    std::optional<PetMobilePresence>
        mobilePresence;
    std::optional<CompanionError>
        mobilePresenceDiagnostic;

    static Result<PetDefinition> load(
        const QString& manifestPath,
        PetSourceKind source);

    QString sourceTitle() const;
    QUrl spriteSheetUrl() const;
    bool hasNativeRow(PetAnimation animation) const noexcept;
    PetAnimation resolvedAnimation(
        PetAnimation animation) const noexcept;
    int frameCount(PetAnimation animation) const noexcept;
    PetAnimationSequence animationSequence(
        PetAnimation animation,
        double speedScale) const;
    bool usesShadowStyle() const;
};

} // namespace companion
