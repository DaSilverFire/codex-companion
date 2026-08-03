#include "codex/state/TaskProjector.h"

#include <QDir>
#include <QFileInfo>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace companion {

namespace {

inline constexpr qint64 kReferenceDateUnixMilliseconds =
    978307200000LL;
inline constexpr qint64 kRecentTaskMilliseconds =
    3 * 60 * 1000;
inline constexpr qint64 kCompletedThreadDisplayMilliseconds =
    kRecentTaskMilliseconds + 5 * 60 * 1000;
inline constexpr qint64 kRecentGoalCompletionDisplayMilliseconds =
    30 * 60 * 1000;
inline constexpr qsizetype kMaximumProjectedTasks = 50;

std::optional<QString> nonempty(QString value)
{
    value = value.trimmed();
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(std::move(value));
}

QString boundedByGraphemes(
    const QString& text,
    qsizetype maximum,
    qsizetype prefixLength)
{
    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme, text);
    QVector<qsizetype> boundaries;
    boundaries.reserve(text.size() + 1);
    boundaries.append(0);
    finder.toStart();
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            break;
        }
        boundaries.append(boundary);
    }

    const qsizetype graphemeCount = boundaries.size() - 1;
    if (graphemeCount <= maximum) {
        return text;
    }

    return text.left(boundaries.at(prefixLength)) +
        QStringLiteral("...");
}

QString capitalizedWords(QString value)
{
    value = value.toLower();
    bool startsWord = true;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character.isLetterOrNumber()) {
            if (startsWord) {
                value[index] = character.toUpper();
            }
            startsWord = false;
        } else {
            startsWord = true;
        }
    }
    return value;
}

std::optional<QString> workingFolder(const QString& cwd)
{
    const auto directory = nonempty(cwd);
    if (!directory.has_value()) {
        return std::nullopt;
    }

    const QString normalized = QDir::cleanPath(
        QDir::fromNativeSeparators(*directory));
    QString folder = QFileInfo(normalized).fileName();
    if (folder.isEmpty()) {
        folder = QDir(normalized).dirName();
    }
    folder = folder.trimmed();
    if (folder.isEmpty()) {
        return std::nullopt;
    }

    folder.replace(QLatin1Char('-'), QLatin1Char(' '));
    return capitalizedWords(std::move(folder));
}

QString displayTitle(
    const CodexThreadRecord& thread,
    const TaskProjectionContext& context)
{
    const auto indexed = nonempty(
        context.sessionNames.value(thread.id));
    if (indexed.has_value()) {
        return boundedByGraphemes(*indexed, 96, 93);
    }

    const auto stored = nonempty(thread.title);
    const auto first = nonempty(thread.firstUserMessage);
    if (stored.has_value() &&
        (!first.has_value() || *stored != *first)) {
        return boundedByGraphemes(*stored, 96, 93);
    }

    const auto folder = workingFolder(thread.workingDirectory);
    if (folder.has_value()) {
        return *folder;
    }

    if (first.has_value()) {
        return boundedByGraphemes(*first, 64, 61);
    }
    return QStringLiteral("Codex task");
}

QString preview(
    const CodexThreadRecord& thread,
    const RolloutSnapshot& rollout)
{
    if (rollout.latestAssistantMessage.has_value()) {
        const auto assistant =
            nonempty(rollout.latestAssistantMessage->text);
        if (assistant.has_value()) {
            return *assistant;
        }
    }
    if (const auto stored = nonempty(thread.preview);
        stored.has_value()) {
        return *stored;
    }
    if (const auto first = nonempty(thread.firstUserMessage);
        first.has_value()) {
        return *first;
    }
    return QStringLiteral("No messages yet");
}

TaskStatus projectedStatus(
    bool needsApproval,
    const std::optional<BridgeGoal>& goal,
    const QDateTime& updatedAt,
    const QDateTime& now)
{
    if (needsApproval) {
        return TaskStatus::Waiting;
    }

    if (goal.has_value()) {
        switch (goal->status) {
        case GoalStatus::Active:
            return TaskStatus::Running;
        case GoalStatus::Paused:
        case GoalStatus::Blocked:
        case GoalStatus::UsageLimited:
        case GoalStatus::BudgetLimited:
            return TaskStatus::Waiting;
        case GoalStatus::Complete:
            return TaskStatus::Completed;
        }
    }

    if (!updatedAt.isValid() || !now.isValid()) {
        return TaskStatus::Completed;
    }
    return updatedAt.msecsTo(now) < kRecentTaskMilliseconds
        ? TaskStatus::Running
        : TaskStatus::Completed;
}

int orderingStatusRank(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Waiting:
        return 0;
    case TaskStatus::Running:
        return 1;
    case TaskStatus::Failed:
        return 2;
    case TaskStatus::Completed:
        return 3;
    }
    return 3;
}

QDateTime unixTimestamp(qint64 value)
{
    constexpr qint64 secondsMagnitudeThreshold = 100000000000LL;
    if (value > -secondsMagnitudeThreshold
        && value < secondsMagnitudeThreshold) {
        value *= 1000;
    }
    return QDateTime::fromMSecsSinceEpoch(value, QTimeZone::UTC);
}

QDateTime bridgeTaskUpdatedAt(
    const BridgeDate& updatedAt)
{
    const double milliseconds =
        static_cast<double>(
            kReferenceDateUnixMilliseconds)
        + updatedAt.secondsSinceReferenceDate
            * 1000.0;
    if (!std::isfinite(milliseconds)
        || milliseconds
            < static_cast<double>(
                std::numeric_limits<qint64>::min())
        || milliseconds
            > static_cast<double>(
                std::numeric_limits<qint64>::max())) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(
        qRound64(milliseconds),
        QTimeZone::UTC);
}

void applyRuntimePresentation(
    BridgeTask& task,
    const std::optional<ThreadRuntimeStatus>&
        runtimeStatus)
{
    if (!runtimeStatus.has_value()) {
        return;
    }

    task.needsApproval =
        *runtimeStatus
        == ThreadRuntimeStatus::
            WaitingOnApproval;
    switch (*runtimeStatus) {
    case ThreadRuntimeStatus::Active:
        task.status = TaskStatus::Running;
        break;
    case ThreadRuntimeStatus::WaitingOnApproval:
        task.status = TaskStatus::Waiting;
        task.preview = QStringLiteral(
            "This task is waiting for your approval.");
        break;
    case ThreadRuntimeStatus::WaitingOnUserInput:
        task.status = TaskStatus::Waiting;
        task.preview = QStringLiteral(
            "This task is waiting for your input.");
        break;
    case ThreadRuntimeStatus::SystemError:
        task.status = TaskStatus::Failed;
        break;
    case ThreadRuntimeStatus::Idle:
    case ThreadRuntimeStatus::NotLoaded:
        break;
    }
}

std::optional<BridgeGoal> displayedGoal(
    const std::optional<BridgeGoal>& goal,
    const QDateTime& now)
{
    if (!goal.has_value() || goal->status != GoalStatus::Complete) {
        return goal;
    }

    const QDateTime updatedAt = unixTimestamp(goal->updatedAt);
    if (!updatedAt.isValid()
        || !now.isValid()
        || updatedAt.msecsTo(now)
            >= kRecentGoalCompletionDisplayMilliseconds) {
        return std::nullopt;
    }
    return goal;
}

bool shouldRetainTask(
    const BridgeTask& task,
    const QDateTime& activityAt,
    const QDateTime& now)
{
    if (task.status != TaskStatus::Completed) {
        return true;
    }
    if (task.goal.has_value()
        && task.goal->status == GoalStatus::Complete) {
        return true;
    }
    if (!activityAt.isValid() || !now.isValid()) {
        return false;
    }
    return activityAt.msecsTo(now)
        < kCompletedThreadDisplayMilliseconds;
}

BridgeDate bridgeDate(const QDateTime& date)
{
    if (!date.isValid()) {
        return {};
    }
    return {
        static_cast<double>(
            date.toMSecsSinceEpoch() -
            kReferenceDateUnixMilliseconds) /
        1000.0,
    };
}

} // namespace

BridgeTask TaskProjector::project(
    const CodexThreadRecord& thread,
    const RolloutSnapshot& rollout,
    const std::optional<BridgeGoal>& goal,
    const TaskProjectionContext& context)
{
    const auto runtimeIterator =
        context.runtimeStatuses.constFind(thread.id);
    const bool hasRuntimeStatus =
        runtimeIterator
        != context.runtimeStatuses.cend();
    const std::optional<ThreadRuntimeStatus>
        runtimeStatus =
            hasRuntimeStatus
            ? std::optional<ThreadRuntimeStatus>(
                  runtimeIterator.value())
            : std::nullopt;
    const bool needsApproval =
        runtimeStatus.has_value()
        ? *runtimeStatus
            == ThreadRuntimeStatus::
                WaitingOnApproval
        : context.pendingApprovalThreadIds.contains(
              thread.id);
    const std::optional<BridgeGoal> visibleGoal =
        displayedGoal(goal, context.now);
    TaskStatus status = projectedStatus(
        needsApproval,
        visibleGoal,
        thread.updatedAt,
        context.now);
    QString projectedPreview =
        preview(thread, rollout);

    std::optional<QString> activeTurnId;
    if (rollout.lifecycle.has_value() &&
        rollout.lifecycle->isActive()) {
        activeTurnId = rollout.lifecycle->turnId;
    }

    const std::optional<QString> cwd =
        nonempty(thread.workingDirectory);
    const BridgeTaskGroup taskGroup =
        context.sidebarOrdering.taskGroup(thread.id, cwd);

    BridgeTask task{
        thread.id,
        displayTitle(thread, context),
        std::move(projectedPreview),
        bridgeDate(thread.updatedAt),
        cwd,
        status,
        needsApproval,
        activeTurnId,
        thread.model.has_value()
            ? nonempty(*thread.model)
            : std::nullopt,
        thread.reasoningEffort.has_value()
            ? nonempty(*thread.reasoningEffort)
            : std::nullopt,
        taskGroup,
        visibleGoal,
        thread.rolloutPath,
    };
    applyRuntimePresentation(
        task,
        runtimeStatus);
    return task;
}

BridgeTask TaskProjector::applyingGoal(
    BridgeTask task,
    const std::optional<BridgeGoal>& goal,
    const std::optional<ThreadRuntimeStatus>&
        runtimeStatus,
    const QDateTime& now)
{
    task.goal = displayedGoal(goal, now);
    task.needsApproval =
        runtimeStatus.has_value()
        ? *runtimeStatus
            == ThreadRuntimeStatus::
                WaitingOnApproval
        : task.needsApproval;
    task.status = projectedStatus(
        task.needsApproval,
        task.goal,
        bridgeTaskUpdatedAt(task.updatedAt),
        now);
    applyRuntimePresentation(
        task,
        runtimeStatus);
    return task;
}

QVector<BridgeTask> TaskProjector::projectAll(
    const CodexStateSnapshot& snapshot,
    const QHash<QString, RolloutSnapshot>& rollouts,
    const QHash<QString, BridgeGoal>& goals,
    const TaskProjectionContext& context)
{
    struct ProjectedTask final {
        BridgeTask task;
        SidebarOrderingSnapshot::Entry ordering;
        QDateTime activityAt;
        std::optional<ThreadRuntimeStatus>
            runtimeStatus;
    };

    QVector<ProjectedTask> projected;
    projected.reserve(snapshot.threads.size());
    for (const CodexThreadRecord& thread : snapshot.threads) {
        RolloutSnapshot rollout;
        const auto rolloutIterator = rollouts.constFind(thread.id);
        if (rolloutIterator != rollouts.constEnd()) {
            rollout = rolloutIterator.value();
        }

        std::optional<BridgeGoal> goal;
        const auto goalIterator = goals.constFind(thread.id);
        if (goalIterator != goals.constEnd()) {
            goal = goalIterator.value();
        }

        const auto runtimeIterator =
            context.runtimeStatuses.constFind(thread.id);
        const std::optional<ThreadRuntimeStatus>
            runtimeStatus =
                runtimeIterator
                    == context.runtimeStatuses.cend()
                ? std::nullopt
                : std::optional<ThreadRuntimeStatus>(
                      runtimeIterator.value());
        BridgeTask task = project(thread, rollout, goal, context);
        QDateTime activityAt = thread.updatedAt;
        if (goal.has_value()) {
            const QDateTime goalUpdatedAt =
                unixTimestamp(goal->updatedAt);
            if (goalUpdatedAt.isValid()
                && (!activityAt.isValid()
                    || goalUpdatedAt > activityAt)) {
                activityAt = goalUpdatedAt;
            }
        }
        const int statusRank =
            orderingStatusRank(task.status);
        projected.append({
            std::move(task),
            {
                thread.id,
                nonempty(thread.workingDirectory),
                statusRank,
                thread.recencyAt.isValid()
                    ? std::optional<QDateTime>(thread.recencyAt)
                    : std::nullopt,
                context.attentionPromotedThreadIds.contains(
                    thread.id),
            },
            activityAt,
            runtimeStatus,
        });
    }

    std::sort(
        projected.begin(),
        projected.end(),
        [&context](
            const ProjectedTask& left,
            const ProjectedTask& right) {
            return context.sidebarOrdering.orders(
                left.ordering, right.ordering);
        });

    QVector<BridgeTask> tasks;
    tasks.reserve(std::min(
        projected.size(),
        kMaximumProjectedTasks));
    for (ProjectedTask& item : projected) {
        if (!shouldRetainTask(
                item.task,
                item.activityAt,
                context.now)) {
            continue;
        }
        tasks.append(std::move(item.task));
        if (tasks.size()
            == kMaximumProjectedTasks) {
            break;
        }
    }
    return tasks;
}

} // namespace companion
