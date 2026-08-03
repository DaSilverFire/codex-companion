#pragma once

#include "codex/models/BridgeModels.h"

#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct MessagePage final {
    QVector<BridgeMessage> messages;
    std::optional<QString> nextCursor;

    friend bool operator==(
        const MessagePage&,
        const MessagePage&) = default;
};

struct TimelinePage final {
    QVector<BridgeTimelineItem> items;
    std::optional<QString> nextCursor;
    QString revision;
    std::optional<BridgeContextUsage> contextUsage;

    friend bool operator==(
        const TimelinePage&,
        const TimelinePage&) = default;
};

struct HistorySnapshot final {
    QVector<BridgeMessage> messages;
    std::optional<QString> nextCursor;
    QVector<BridgeTimelineItem> timelineItems;
    QString revision;
    std::optional<QString> timelineNextCursor;
    QVector<BridgeSubagent> subagents;
    std::optional<BridgeContextUsage> contextUsage;

    friend bool operator==(
        const HistorySnapshot&,
        const HistorySnapshot&) = default;
};

} // namespace companion
