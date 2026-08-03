#pragma once

#include "codex/models/CodexModels.h"
#include "core/Result.h"

#include <QString>
#include <QtGlobal>

namespace companion {

class RolloutReader final {
public:
    inline static constexpr qint64 kMaximumTailBytes =
        8 * 1024 * 1024;
    inline static constexpr qint64 kMaximumPreviewLineBytes =
        512 * 1024;
    inline static constexpr qint64 kMaximumLifecycleLineBytes =
        2 * 1024 * 1024;
    inline static constexpr qint64 kMobileTaskTailBytes =
        1 * 1024 * 1024;
    inline static constexpr qint64 kMobileTaskMaximumLineBytes =
        2 * 1024 * 1024;

    static Result<RolloutSnapshot> readTail(
        const QString& rawPath,
        const QString& codexHome,
        qint64 maximumBytes = kMaximumTailBytes);

    static Result<RolloutSnapshot> readMobileTaskTail(
        const QString& rawPath,
        const QString& codexHome);
};

} // namespace companion
