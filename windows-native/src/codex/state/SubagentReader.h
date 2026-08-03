#pragma once

#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QDateTime>
#include <QSet>
#include <QString>
#include <QVector>

namespace companion {

class SubagentReader final {
public:
    static Result<QVector<BridgeSubagent>> read(
        const QString& databasePath,
        const QString& parentThreadId,
        const QSet<QString>& pendingApprovalThreadIds,
        const QDateTime& now,
        int limit);
};

} // namespace companion
