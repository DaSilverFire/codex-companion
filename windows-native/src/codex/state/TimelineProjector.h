#pragma once

#include "codex/state/HistoryModels.h"
#include "core/Result.h"

#include <QString>
#include <QtGlobal>

#include <optional>

namespace companion {

class TimelineProjector final {
public:
    inline static constexpr qint64 kHistoryChunkBytes =
        512 * 1024;
    inline static constexpr qint64 kMaximumHistoryLineBytes =
        2 * 1024 * 1024;
    inline static constexpr qint64 kMaximumInlineMediaBytes =
        1 * 1024 * 1024;
    inline static constexpr qsizetype kMaximumToolDetailCharacters =
        2000;

    static Result<MessagePage> loadMessages(
        const QString& rolloutPath,
        const std::optional<QString>& cursor,
        int limit);

    static Result<TimelinePage> loadTimeline(
        const QString& rolloutPath,
        const std::optional<QString>& cursor,
        int limit);
};

} // namespace companion
