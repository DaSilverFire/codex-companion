#pragma once

#include "codex/models/BridgeModels.h"
#include "codex/models/CodexModels.h"
#include "core/Result.h"

#include <QDateTime>
#include <QString>

#include <optional>

namespace companion {

struct CodexHistoryThreadRecord final {
    QString id;
    QString rolloutPath;

    friend bool operator==(
        const CodexHistoryThreadRecord&,
        const CodexHistoryThreadRecord&) = default;
};

struct CodexGoalCandidateRecord final {
    BridgeGoal goal;
    QDateTime activityAt;

    friend bool operator==(
        const CodexGoalCandidateRecord&,
        const CodexGoalCandidateRecord&) = default;
};

class CodexStateDatabaseReader final {
public:
    static Result<
        std::optional<
            CodexHistoryThreadRecord>>
    readThreadById(
        const QString& databasePath,
        const QString& threadId);
    static Result<QVector<CodexThreadRecord>> readThreads(
        const QString& databasePath);
    static Result<QVector<CodexGoalCandidateRecord>>
    readGoalCandidates(
        const QString& databasePath);
    static Result<QVector<CodexGoalCandidateRecord>>
    readGoalCandidates(
        const QString& databasePath,
        const QDateTime& now);
    static Result<QVector<QString>>
    readGoalCandidateThreadIds(
        const QString& databasePath);
    static Result<QVector<QString>>
    readGoalCandidateThreadIds(
        const QString& databasePath,
        const QDateTime& now);
    static Result<QVector<CodexJobRecord>> readJobs(
        const QString& databasePath);
    static Result<CodexStateSnapshot> readSnapshot(
        const QString& databasePath);
};

} // namespace companion
