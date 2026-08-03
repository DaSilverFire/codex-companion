#pragma once

#include "codex/discovery/CodexEnvironment.h"
#include "codex/state/HistoryCoordinator.h"
#include "core/Result.h"

#include <QDateTime>
#include <QSet>
#include <QString>

#include <stop_token>

namespace companion {

class HistorySnapshotLoader final {
public:
    explicit HistorySnapshotLoader(
        CodexEnvironment environment);

    Result<HistorySnapshot> load(
        const HistoryKey& key,
        const QSet<QString>&
            pendingApprovalThreadIds,
        const QDateTime& now,
        std::stop_token stopToken = {}) const;

private:
    CodexEnvironment environment_;
};

} // namespace companion
