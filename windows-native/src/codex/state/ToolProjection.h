#pragma once

#include "codex/models/BridgeModels.h"

#include <QJsonValue>
#include <QString>
#include <QVector>

#include <optional>

namespace companion {

struct ToolProjection final {
    QString title;
    std::optional<QString> detail;
    bool omitsWrapper = false;

    friend bool operator==(
        const ToolProjection&,
        const ToolProjection&) = default;
};

class ToolProjector final {
public:
    static ToolProjection project(
        QString name,
        std::optional<QString> input = std::nullopt,
        std::optional<QString> server = std::nullopt);

    static std::optional<QString> editedFilePathsFromChanges(
        const QJsonValue& changes);
    static std::optional<QString> editedFilePathsFromToolOutput(
        const QString& output);

    static TimelineStatus callStatus(
        const std::optional<QString>& rawStatus);
    static TimelineStatus resolvedStatus(
        TimelineStatus callStatus,
        const QVector<TimelineStatus>& outputStatuses);
};

} // namespace companion
