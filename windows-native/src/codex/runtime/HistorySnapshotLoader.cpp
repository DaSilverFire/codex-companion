#include "codex/runtime/HistorySnapshotLoader.h"

#include "codex/state/CodexStateDatabaseReader.h"
#include "codex/state/SubagentReader.h"
#include "codex/state/TimelineProjector.h"

#include <optional>
#include <utility>

namespace companion {

namespace {

CompanionError historyFailure()
{
    return {
        QStringLiteral(
            "codex.history_load_failed"),
        QStringLiteral(
            "Could not load Codex task history."),
        true,
        {},
    };
}

CompanionError canceledFailure()
{
    return {
        QStringLiteral(
            "codex.operation_canceled"),
        QStringLiteral(
            "The Codex operation was canceled."),
        false,
        {},
    };
}

std::optional<CompanionError> cancellation(
    std::stop_token stopToken)
{
    return stopToken.stop_requested()
        ? std::optional<CompanionError>(
              canceledFailure())
        : std::nullopt;
}

} // namespace

HistorySnapshotLoader::HistorySnapshotLoader(
    CodexEnvironment environment)
    : environment_(std::move(environment))
{
}

Result<HistorySnapshot>
HistorySnapshotLoader::load(
    const HistoryKey& key,
    const QSet<QString>&
        pendingApprovalThreadIds,
    const QDateTime& now,
    std::stop_token stopToken) const
{
    try {
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }

        const QString threadId =
            key.threadId.trimmed();
        if (threadId.isEmpty()
            || key.limit < 1
            || !now.isValid()) {
            return Result<HistorySnapshot>::failure(
                historyFailure());
        }

        auto thread =
            CodexStateDatabaseReader::
                readThreadById(
                    environment_.stateDatabase,
                    threadId);
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }
        if (!thread.hasValue()
            || !thread.value().has_value()) {
            return Result<HistorySnapshot>::failure(
                historyFailure());
        }

        auto messages =
            TimelineProjector::loadMessages(
                thread.value()->rolloutPath,
                key.cursor,
                key.limit);
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }
        if (!messages.hasValue()) {
            return Result<HistorySnapshot>::failure(
                historyFailure());
        }

        auto timeline =
            TimelineProjector::loadTimeline(
                thread.value()->rolloutPath,
                key.cursor,
                key.limit);
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }
        if (!timeline.hasValue()) {
            return Result<HistorySnapshot>::failure(
                historyFailure());
        }

        auto subagents = SubagentReader::read(
            environment_.stateDatabase,
            threadId,
            pendingApprovalThreadIds,
            now.toUTC(),
            8);
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }
        if (!subagents.hasValue()) {
            return Result<HistorySnapshot>::failure(
                historyFailure());
        }

        return Result<HistorySnapshot>::success({
            std::move(messages.value().messages),
            std::move(messages.value().nextCursor),
            std::move(timeline.value().items),
            std::move(timeline.value().revision),
            std::move(timeline.value().nextCursor),
            std::move(subagents.value()),
            std::move(
                timeline.value().contextUsage),
        });
    } catch (...) {
        if (const auto stopped =
                cancellation(stopToken);
            stopped.has_value()) {
            return Result<HistorySnapshot>::failure(
                *stopped);
        }
        return Result<HistorySnapshot>::failure(
            historyFailure());
    }
}

} // namespace companion
