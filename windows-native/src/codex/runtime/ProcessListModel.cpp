#include "codex/runtime/ProcessListModel.h"

#include "codex/state/SidebarOrderingSnapshot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTimeZone>
#include <QVariantMap>

#include <algorithm>
#include <optional>
#include <utility>

namespace companion {

namespace {

constexpr double kSwiftReferenceDateUnixSeconds =
    978307200.0;
constexpr qint64 kCompletedJobDisplayMilliseconds =
    5 * 60 * 1000;
constexpr qint64 kCompletedThreadDisplayMilliseconds =
    (3 + 5) * 60 * 1000;
constexpr qint64 kCompletedGoalDisplayMilliseconds =
    30 * 60 * 1000;
constexpr qsizetype kMaximumVisibleProcesses = 10;
constexpr int kFailureStateVersion = 1;

QString taskStatusText(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Running:
        return QStringLiteral("running");
    case TaskStatus::Waiting:
        return QStringLiteral("waiting");
    case TaskStatus::Completed:
        return QStringLiteral("completed");
    case TaskStatus::Failed:
        return QStringLiteral("failed");
    }
    return {};
}

QString normalizedJobStatus(QString status)
{
    return status.trimmed().toLower();
}

bool isActiveJobStatus(const QString& status)
{
    if (status == QStringLiteral("running")
        || status == QStringLiteral("pending")
        || status == QStringLiteral("active")
        || status == QStringLiteral("queued")
        || status == QStringLiteral("in_progress")) {
        return true;
    }
    return false;
}

bool isCompletedJobStatus(const QString& status)
{
    return status == QStringLiteral("completed")
        || status == QStringLiteral("complete")
        || status == QStringLiteral("succeeded")
        || status == QStringLiteral("success")
        || status == QStringLiteral("done");
}

bool isFailedJobStatus(const QString& status)
{
    return status == QStringLiteral("failed")
        || status == QStringLiteral("failure")
        || status == QStringLiteral("error")
        || status == QStringLiteral("disconnected")
        || status == QStringLiteral("cancelled")
        || status == QStringLiteral("canceled");
}

TaskStatus jobStatus(QString status)
{
    status = normalizedJobStatus(std::move(status));
    if (isActiveJobStatus(status)) {
        return TaskStatus::Running;
    }
    if (isCompletedJobStatus(status)) {
        return TaskStatus::Completed;
    }
    if (isFailedJobStatus(status)) {
        return TaskStatus::Failed;
    }
    return TaskStatus::Waiting;
}

bool shouldLoadJob(
    const CodexJobRecord& job,
    const QDateTime& now)
{
    const QString status =
        normalizedJobStatus(job.status);
    if (isActiveJobStatus(status)
        || isCompletedJobStatus(status)
        || isFailedJobStatus(status)) {
        return true;
    }
    return job.updatedAt.isValid()
        && now.isValid()
        && job.updatedAt.msecsTo(now)
            <= kCompletedJobDisplayMilliseconds;
}

int statusRank(TaskStatus status)
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

QString taskGroupKindText(TaskGroupKind kind)
{
    switch (kind) {
    case TaskGroupKind::Chats:
        return QStringLiteral("chats");
    case TaskGroupKind::Project:
        return QStringLiteral("project");
    }
    return {};
}

QString goalStatusText(GoalStatus status)
{
    switch (status) {
    case GoalStatus::Active:
        return QStringLiteral("active");
    case GoalStatus::Paused:
        return QStringLiteral("paused");
    case GoalStatus::Blocked:
        return QStringLiteral("blocked");
    case GoalStatus::UsageLimited:
        return QStringLiteral("usageLimited");
    case GoalStatus::BudgetLimited:
        return QStringLiteral("budgetLimited");
    case GoalStatus::Complete:
        return QStringLiteral("complete");
    }
    return {};
}

std::optional<TaskGroupKind> taskGroupKindFromText(
    const QString& value)
{
    if (value == QStringLiteral("chats")) {
        return TaskGroupKind::Chats;
    }
    if (value == QStringLiteral("project")) {
        return TaskGroupKind::Project;
    }
    return std::nullopt;
}

std::optional<GoalStatus> goalStatusFromText(
    const QString& value)
{
    if (value == QStringLiteral("active")) {
        return GoalStatus::Active;
    }
    if (value == QStringLiteral("paused")) {
        return GoalStatus::Paused;
    }
    if (value == QStringLiteral("blocked")) {
        return GoalStatus::Blocked;
    }
    if (value == QStringLiteral("usageLimited")) {
        return GoalStatus::UsageLimited;
    }
    if (value == QStringLiteral("budgetLimited")) {
        return GoalStatus::BudgetLimited;
    }
    if (value == QStringLiteral("complete")) {
        return GoalStatus::Complete;
    }
    return std::nullopt;
}

QString runtimeStatusText(ThreadRuntimeStatus status)
{
    switch (status) {
    case ThreadRuntimeStatus::NotLoaded:
        return QStringLiteral("notLoaded");
    case ThreadRuntimeStatus::Idle:
        return QStringLiteral("idle");
    case ThreadRuntimeStatus::Active:
        return QStringLiteral("active");
    case ThreadRuntimeStatus::WaitingOnApproval:
        return QStringLiteral("waitingOnApproval");
    case ThreadRuntimeStatus::WaitingOnUserInput:
        return QStringLiteral("waitingOnUserInput");
    case ThreadRuntimeStatus::SystemError:
        return QStringLiteral("systemError");
    }
    return {};
}

std::optional<ThreadRuntimeStatus>
runtimeStatusFromText(const QString& value)
{
    if (value == QStringLiteral("notLoaded")) {
        return ThreadRuntimeStatus::NotLoaded;
    }
    if (value == QStringLiteral("idle")) {
        return ThreadRuntimeStatus::Idle;
    }
    if (value == QStringLiteral("active")) {
        return ThreadRuntimeStatus::Active;
    }
    if (value == QStringLiteral("waitingOnApproval")) {
        return ThreadRuntimeStatus::WaitingOnApproval;
    }
    if (value == QStringLiteral("waitingOnUserInput")) {
        return ThreadRuntimeStatus::WaitingOnUserInput;
    }
    if (value == QStringLiteral("systemError")) {
        return ThreadRuntimeStatus::SystemError;
    }
    return std::nullopt;
}

std::optional<QString> nonempty(
    const std::optional<QString>& value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    const QString normalized = value->trimmed();
    return normalized.isEmpty()
        ? std::nullopt
        : std::optional<QString>(normalized);
}

QVariant optionalString(
    const std::optional<QString>& value)
{
    return value.has_value()
        ? QVariant(*value)
        : QVariant();
}

QVariant goalVariant(
    const std::optional<BridgeGoal>& goal)
{
    if (!goal.has_value()) {
        return {};
    }

    QVariantMap result{
        {QStringLiteral("threadId"), goal->threadId},
        {QStringLiteral("objective"), goal->objective},
        {QStringLiteral("status"),
         goalStatusText(goal->status)},
        {QStringLiteral("tokensUsed"), goal->tokensUsed},
        {QStringLiteral("elapsedSeconds"),
         goal->elapsedSeconds},
        {QStringLiteral("createdAt"), goal->createdAt},
        {QStringLiteral("updatedAt"), goal->updatedAt},
    };
    if (goal->tokenBudget.has_value()) {
        result.insert(
            QStringLiteral("tokenBudget"),
            *goal->tokenBudget);
    }
    return result;
}

double bridgeSeconds(const QDateTime& date)
{
    if (!date.isValid()) {
        return 0.0;
    }
    return static_cast<double>(
               date.toMSecsSinceEpoch())
        / 1000.0
        - kSwiftReferenceDateUnixSeconds;
}

QDateTime dateFromBridgeSeconds(double seconds)
{
    if (seconds <= 0.0) {
        return {};
    }
    const qint64 milliseconds =
        static_cast<qint64>(
            (seconds
             + kSwiftReferenceDateUnixSeconds)
            * 1000.0);
    return QDateTime::fromMSecsSinceEpoch(
        milliseconds,
        QTimeZone::UTC);
}

QDateTime dateFromUnixTimestamp(qint64 value)
{
    constexpr qint64 secondsMagnitudeThreshold =
        100000000000LL;
    if (value > -secondsMagnitudeThreshold
        && value < secondsMagnitudeThreshold) {
        value *= 1000;
    }
    return QDateTime::fromMSecsSinceEpoch(
        value,
        QTimeZone::UTC);
}

bool goalKeepsThreadCurrent(
    const BridgeGoal& goal,
    const QDateTime& now)
{
    if (goal.status != GoalStatus::Complete) {
        return true;
    }
    if (!now.isValid()) {
        return false;
    }
    const QDateTime updatedAt =
        dateFromUnixTimestamp(goal.updatedAt);
    return updatedAt.isValid()
        && updatedAt.msecsTo(now)
            < kCompletedGoalDisplayMilliseconds;
}

bool shouldLoadThread(
    const BridgeTask& task,
    const CodexProcessSnapshot& snapshot,
    const QDateTime& now)
{
    const QString threadId = task.id.trimmed();
    if (task.needsApproval
        || snapshot.pendingApprovals.contains(threadId)
        || snapshot.attentionPromotedThreadIds.contains(
            threadId)
        || snapshot.runtimeStatuses.contains(threadId)
        || snapshot.goalCandidateThreadIds.contains(
            threadId)) {
        return true;
    }
    if (task.goal.has_value()
        && goalKeepsThreadCurrent(*task.goal, now)) {
        return true;
    }
    if (!now.isValid()) {
        return true;
    }
    const QDateTime activityAt =
        dateFromBridgeSeconds(
            task.updatedAt.secondsSinceReferenceDate);
    return activityAt.isValid()
        && activityAt
            >= now.addMSecs(
                -kCompletedThreadDisplayMilliseconds);
}

QString displayStatus(QString status)
{
    status = status.trimmed();
    if (status.isEmpty()) {
        return QStringLiteral("Waiting");
    }
    status.replace(QLatin1Char('_'), QLatin1Char(' '));
    status.replace(QLatin1Char('-'), QLatin1Char(' '));
    const QStringList words =
        status.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList displayWords;
    displayWords.reserve(words.size());
    for (QString word : words) {
        word = word.toLower();
        if (!word.isEmpty()) {
            word[0] = word.at(0).toUpper();
        }
        displayWords.append(std::move(word));
    }
    return displayWords.join(QLatin1Char(' '));
}

ProcessListItem processItem(
    const BridgeTask& task,
    std::optional<ThreadRuntimeStatus>
        runtimeStatus)
{
    return {
        task.id,
        task.id,
        QStringLiteral("thread"),
        task.title,
        task.preview,
        task.updatedAt.secondsSinceReferenceDate,
        task.cwd,
        task.status,
        task.needsApproval,
        task.activeTurnId,
        task.model,
        task.reasoningEffort,
        task.taskGroup,
        task.goal,
        dateFromBridgeSeconds(
            task.updatedAt.secondsSinceReferenceDate),
        runtimeStatus,
        {},
        task.rolloutPath,
    };
}

ProcessListItem processItem(
    const CodexJobRecord& job,
    const QHash<QString, const BridgeTask*>& tasksById)
{
    const std::optional<QString> assignedThreadId =
        nonempty(job.threadId);
    const BridgeTask* assignedTask = nullptr;
    if (assignedThreadId.has_value()) {
        assignedTask =
            tasksById.value(*assignedThreadId, nullptr);
    }

    QString title;
    if (assignedTask != nullptr) {
        title = assignedTask->title.trimmed();
    }
    if (title.isEmpty()) {
        title = job.name.trimmed();
    }
    if (title.isEmpty()) {
        title = job.instruction.trimmed();
    }
    if (title.isEmpty()) {
        title = QStringLiteral("Codex job");
    }

    TaskStatus status = jobStatus(job.status);

    QString preview;
    if (status == TaskStatus::Failed) {
        const std::optional<QString> error =
            nonempty(job.error);
        if (error.has_value()) {
            preview = *error;
        }
    }
    if (preview.isEmpty()) {
        preview = job.instruction.trimmed();
        if (preview.isEmpty()) {
            preview = QStringLiteral(
                          "Codex job status: %1")
                          .arg(
                              displayStatus(job.status));
        }
    }

    return {
        QStringLiteral("job-") + job.id,
        assignedThreadId.value_or(QString()),
        QStringLiteral("job"),
        std::move(title),
        std::move(preview),
        bridgeSeconds(job.updatedAt),
        assignedTask != nullptr
            ? assignedTask->cwd
            : std::nullopt,
        status,
        false,
        assignedTask != nullptr
            ? assignedTask->activeTurnId
            : std::nullopt,
        assignedTask != nullptr
            ? assignedTask->model
            : std::nullopt,
        assignedTask != nullptr
            ? assignedTask->reasoningEffort
            : std::nullopt,
        assignedTask != nullptr
            ? assignedTask->taskGroup
            : std::nullopt,
        std::nullopt,
        job.updatedAt,
        std::nullopt,
        job.status.trimmed(),
        assignedTask != nullptr
            ? assignedTask->rolloutPath
            : QString(),
    };
}

bool shouldRetainProcess(
    const ProcessListItem& item,
    const QDateTime& now)
{
    if (item.status != TaskStatus::Completed) {
        return true;
    }
    if (item.kind == QStringLiteral("thread")
        && item.goal.has_value()
        && item.goal->status == GoalStatus::Complete) {
        const QDateTime goalUpdatedAt =
            dateFromUnixTimestamp(
                item.goal->updatedAt);
        if (goalUpdatedAt.isValid()
            && now.isValid()
            && goalUpdatedAt.msecsTo(now)
                < kCompletedGoalDisplayMilliseconds) {
            return true;
        }
    }
    if (!item.activityAt.isValid()
        || !now.isValid()) {
        return false;
    }
    const qint64 displayWindow =
        item.kind == QStringLiteral("job")
        ? kCompletedJobDisplayMilliseconds
        : kCompletedThreadDisplayMilliseconds;
    return item.activityAt.msecsTo(now)
        < displayWindow;
}

bool failuresReferToSameProcess(
    const ProcessListItem& left,
    const ProcessListItem& right)
{
    if (left.processId == right.processId) {
        return true;
    }
    return !left.threadId.isEmpty()
        && left.threadId == right.threadId;
}

bool failureMatchesIdentity(
    const ProcessListItem& failure,
    const QString& processId,
    const QString& threadId)
{
    return failure.processId == processId
        || (!threadId.isEmpty()
            && failure.threadId == threadId);
}

bool failureIsResolved(
    const ProcessListItem& failure,
    const ProcessListItem& refreshed)
{
    if (refreshed.runtimeStatus.has_value()) {
        switch (*refreshed.runtimeStatus) {
        case ThreadRuntimeStatus::Active:
        case ThreadRuntimeStatus::
            WaitingOnApproval:
        case ThreadRuntimeStatus::
            WaitingOnUserInput:
            return true;
        case ThreadRuntimeStatus::SystemError:
            return false;
        case ThreadRuntimeStatus::Idle:
        case ThreadRuntimeStatus::NotLoaded:
            break;
        }
    }
    return failure.activityAt.isValid()
        && refreshed.activityAt.isValid()
        && failure.activityAt
               < refreshed.activityAt;
}

QString optionalText(
    const std::optional<QString>& value)
{
    return value.value_or(QString());
}

std::optional<QString> optionalText(
    const QJsonObject& object,
    const QString& key)
{
    const QString value =
        object.value(key).toString().trimmed();
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(value);
}

QJsonObject taskGroupJson(
    const BridgeTaskGroup& taskGroup)
{
    QJsonObject object{
        {
            QStringLiteral("kind"),
            taskGroupKindText(taskGroup.kind),
        },
        {
            QStringLiteral("title"),
            taskGroup.title,
        },
    };
    if (taskGroup.path.has_value()) {
        object.insert(
            QStringLiteral("path"),
            *taskGroup.path);
    }
    return object;
}

std::optional<BridgeTaskGroup> taskGroupFromJson(
    const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const auto kind = taskGroupKindFromText(
        object.value(QStringLiteral("kind"))
            .toString());
    if (!kind.has_value()) {
        return std::nullopt;
    }
    return BridgeTaskGroup{
        *kind,
        object.value(QStringLiteral("title"))
            .toString(),
        optionalText(
            object,
            QStringLiteral("path")),
    };
}

QJsonObject goalJson(const BridgeGoal& goal)
{
    QJsonObject object{
        {
            QStringLiteral("threadId"),
            goal.threadId,
        },
        {
            QStringLiteral("objective"),
            goal.objective,
        },
        {
            QStringLiteral("status"),
            goalStatusText(goal.status),
        },
        {
            QStringLiteral("tokensUsed"),
            QJsonValue(goal.tokensUsed),
        },
        {
            QStringLiteral("elapsedSeconds"),
            QJsonValue(goal.elapsedSeconds),
        },
        {
            QStringLiteral("createdAt"),
            QJsonValue(goal.createdAt),
        },
        {
            QStringLiteral("updatedAt"),
            QJsonValue(goal.updatedAt),
        },
    };
    if (goal.tokenBudget.has_value()) {
        object.insert(
            QStringLiteral("tokenBudget"),
            QJsonValue(*goal.tokenBudget));
    }
    return object;
}

std::optional<BridgeGoal> goalFromJson(
    const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const QString threadId =
        object.value(QStringLiteral("threadId"))
            .toString()
            .trimmed();
    const auto status = goalStatusFromText(
        object.value(QStringLiteral("status"))
            .toString());
    if (threadId.isEmpty() || !status.has_value()) {
        return std::nullopt;
    }
    std::optional<qint64> tokenBudget;
    const QJsonValue tokenBudgetValue =
        object.value(QStringLiteral("tokenBudget"));
    if (tokenBudgetValue.isDouble()) {
        tokenBudget =
            tokenBudgetValue.toInteger();
    }
    return BridgeGoal{
        threadId,
        object.value(QStringLiteral("objective"))
            .toString(),
        *status,
        tokenBudget,
        object.value(QStringLiteral("tokensUsed"))
            .toInteger(),
        object.value(
                  QStringLiteral("elapsedSeconds"))
            .toInteger(),
        object.value(QStringLiteral("createdAt"))
            .toInteger(),
        object.value(QStringLiteral("updatedAt"))
            .toInteger(),
    };
}

QJsonObject failureJson(
    const ProcessListItem& item)
{
    QJsonObject object{
        {QStringLiteral("processId"),
         item.processId},
        {QStringLiteral("threadId"),
         item.threadId},
        {QStringLiteral("kind"), item.kind},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("preview"), item.preview},
        {QStringLiteral("sourceStatus"),
         item.sourceStatus},
        {QStringLiteral("updatedAt"),
         item.updatedAt},
        {QStringLiteral("cwd"),
         optionalText(item.cwd)},
        {QStringLiteral("needsApproval"),
         item.needsApproval},
        {QStringLiteral("activeTurnId"),
         optionalText(item.activeTurnId)},
        {QStringLiteral("model"),
         optionalText(item.model)},
        {QStringLiteral("reasoningEffort"),
         optionalText(item.reasoningEffort)},
        {QStringLiteral("rolloutPath"),
         item.rolloutPath},
        {QStringLiteral("activityAtMs"),
         static_cast<double>(
             item.activityAt
                 .toMSecsSinceEpoch())},
    };
    if (item.taskGroup.has_value()) {
        object.insert(
            QStringLiteral("taskGroup"),
            taskGroupJson(*item.taskGroup));
    }
    if (item.goal.has_value()) {
        object.insert(
            QStringLiteral("goal"),
            goalJson(*item.goal));
    }
    if (item.runtimeStatus.has_value()) {
        object.insert(
            QStringLiteral("runtimeStatus"),
            runtimeStatusText(
                *item.runtimeStatus));
    }
    return object;
}

std::optional<ProcessListItem> failureFromJson(
    const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const QString processId =
        object.value(
                  QStringLiteral("processId"))
            .toString()
            .trimmed();
    if (processId.isEmpty()) {
        return std::nullopt;
    }

    ProcessListItem item;
    item.processId = processId;
    item.threadId =
        object.value(
                  QStringLiteral("threadId"))
            .toString()
            .trimmed();
    item.kind =
        object.value(QStringLiteral("kind"))
            .toString();
    if (item.kind.isEmpty()) {
        item.kind = QStringLiteral("thread");
    }
    item.title =
        object.value(QStringLiteral("title"))
            .toString();
    item.preview =
        object.value(QStringLiteral("preview"))
            .toString();
    item.sourceStatus =
        object.value(
                  QStringLiteral("sourceStatus"))
            .toString();
    item.updatedAt =
        object.value(
                  QStringLiteral("updatedAt"))
            .toDouble();
    item.cwd = optionalText(
        object,
        QStringLiteral("cwd"));
    item.status = TaskStatus::Failed;
    item.needsApproval =
        object.value(
                  QStringLiteral("needsApproval"))
            .toBool();
    item.activeTurnId = optionalText(
        object,
        QStringLiteral("activeTurnId"));
    item.model = optionalText(
        object,
        QStringLiteral("model"));
    item.reasoningEffort = optionalText(
        object,
        QStringLiteral("reasoningEffort"));
    item.rolloutPath =
        object.value(
                  QStringLiteral(
                      "rolloutPath"))
            .toString();
    item.taskGroup = taskGroupFromJson(
        object.value(QStringLiteral("taskGroup")));
    item.goal = goalFromJson(
        object.value(QStringLiteral("goal")));
    item.activityAt =
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(
                object.value(
                          QStringLiteral(
                              "activityAtMs"))
                    .toDouble()),
            QTimeZone::UTC);
    item.runtimeStatus = runtimeStatusFromText(
        object.value(
                  QStringLiteral("runtimeStatus"))
            .toString());
    return item;
}

bool hasUniqueIds(
    const QVector<ProcessListItem>& items)
{
    QSet<QString> ids;
    ids.reserve(items.size());
    for (const ProcessListItem& item : items) {
        if (ids.contains(item.processId)) {
            return false;
        }
        ids.insert(item.processId);
    }
    return true;
}

int indexOfId(
    const QVector<ProcessListItem>& items,
    const QString& id,
    int firstRow)
{
    const int count = static_cast<int>(items.size());
    for (int row = firstRow; row < count; ++row) {
        if (items.at(row).processId == id) {
            return row;
        }
    }
    return -1;
}

QList<int> changedRoles(
    const ProcessListItem& current,
    const ProcessListItem& updated)
{
    QList<int> roles;
    if (current.title != updated.title) {
        roles.append(ProcessListModel::TitleRole);
    }
    if (current.preview != updated.preview) {
        roles.append(ProcessListModel::PreviewRole);
    }
    if (current.sourceStatus
        != updated.sourceStatus) {
        roles.append(
            ProcessListModel::SourceStatusRole);
    }
    if (current.updatedAt != updated.updatedAt) {
        roles.append(ProcessListModel::UpdatedAtRole);
    }
    if (current.cwd != updated.cwd) {
        roles.append(ProcessListModel::CwdRole);
    }
    if (current.status != updated.status) {
        roles.append(ProcessListModel::StatusRole);
    }
    if (current.needsApproval
        != updated.needsApproval) {
        roles.append(
            ProcessListModel::NeedsApprovalRole);
    }
    if (current.runtimeStatus
        != updated.runtimeStatus) {
        roles.append(
            ProcessListModel::RuntimeStatusRole);
    }
    if (current.activeTurnId
        != updated.activeTurnId) {
        roles.append(
            ProcessListModel::ActiveTurnIdRole);
    }
    if (current.model != updated.model) {
        roles.append(ProcessListModel::ModelRole);
    }
    if (current.reasoningEffort
        != updated.reasoningEffort) {
        roles.append(
            ProcessListModel::ReasoningEffortRole);
    }
    if (current.taskGroup != updated.taskGroup) {
        roles.append(ProcessListModel::GroupKindRole);
        roles.append(ProcessListModel::GroupTitleRole);
    }
    if (current.goal != updated.goal) {
        roles.append(ProcessListModel::GoalRole);
    }
    if (current.threadId != updated.threadId) {
        roles.append(ProcessListModel::ThreadIdRole);
    }
    if (current.kind != updated.kind) {
        roles.append(ProcessListModel::KindRole);
    }
    if (current.rolloutPath
        != updated.rolloutPath) {
        roles.append(
            ProcessListModel::
                RolloutPathRole);
    }
    return roles;
}

} // namespace

ProcessListModel::ProcessListModel(QObject* parent)
    : ProcessListModel({}, {}, parent)
{
}

ProcessListModel::ProcessListModel(
    QString sidebarStatePath,
    QObject* parent)
    : ProcessListModel(
          std::move(sidebarStatePath),
          {},
          parent)
{
}

ProcessListModel::ProcessListModel(
    QString sidebarStatePath,
    QString failureStatePath,
    QObject* parent)
    : QAbstractListModel(parent),
      sidebarStatePath_(
          std::move(sidebarStatePath)),
      failureStatePath_(
          std::move(failureStatePath))
{
    loadFailureState();
}

QString ProcessListModel::defaultFailureStatePath()
{
    const QString root =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
    if (root.trimmed().isEmpty()) {
        return {};
    }
    return QDir(root).filePath(
        QStringLiteral(
            "process-failures.v1.json"));
}

int ProcessListModel::rowCount(
    const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(snapshot_.size());
}

QVariant ProcessListModel::data(
    const QModelIndex& index,
    int role) const
{
    if (!index.isValid()
        || index.column() != 0
        || index.row() < 0
        || index.row() >= rowCount()) {
        return {};
    }

    const ProcessListItem& item =
        snapshot_.at(index.row());
    switch (role) {
    case IdRole:
    case ProcessIdRole:
        return item.processId;
    case TitleRole:
        return item.title;
    case PreviewRole:
        return item.preview;
    case SourceStatusRole:
        return item.sourceStatus;
    case UpdatedAtRole:
        return item.updatedAt;
    case CwdRole:
        return optionalString(item.cwd);
    case StatusRole:
        return taskStatusText(item.status);
    case NeedsApprovalRole:
        return item.needsApproval;
    case RuntimeStatusRole:
        return item.runtimeStatus.has_value()
            ? QVariant(
                  runtimeStatusText(
                      *item.runtimeStatus))
            : QVariant();
    case ActiveTurnIdRole:
        return optionalString(item.activeTurnId);
    case ModelRole:
        return optionalString(item.model);
    case ReasoningEffortRole:
        return optionalString(item.reasoningEffort);
    case GroupKindRole:
        return item.taskGroup.has_value()
            ? QVariant(
                  taskGroupKindText(
                      item.taskGroup->kind))
            : QVariant();
    case GroupTitleRole:
        return item.taskGroup.has_value()
            ? QVariant(item.taskGroup->title)
            : QVariant();
    case GoalRole:
        return goalVariant(item.goal);
    case ThreadIdRole:
        return item.threadId;
    case KindRole:
        return item.kind;
    case RolloutPathRole:
        return item.rolloutPath;
    default:
        return {};
    }
}

QHash<int, QByteArray>
ProcessListModel::roleNames() const
{
    return {
        {IdRole, QByteArrayLiteral("id")},
        {TitleRole, QByteArrayLiteral("title")},
        {PreviewRole, QByteArrayLiteral("preview")},
        {UpdatedAtRole, QByteArrayLiteral("updatedAt")},
        {CwdRole, QByteArrayLiteral("cwd")},
        {StatusRole, QByteArrayLiteral("status")},
        {NeedsApprovalRole,
         QByteArrayLiteral("needsApproval")},
        {RuntimeStatusRole,
         QByteArrayLiteral("runtimeStatus")},
        {ActiveTurnIdRole,
         QByteArrayLiteral("activeTurnId")},
        {ModelRole, QByteArrayLiteral("model")},
        {ReasoningEffortRole,
         QByteArrayLiteral("reasoningEffort")},
        {GroupKindRole, QByteArrayLiteral("groupKind")},
        {GroupTitleRole,
         QByteArrayLiteral("groupTitle")},
        {GoalRole, QByteArrayLiteral("goal")},
        {ProcessIdRole,
         QByteArrayLiteral("processId")},
        {ThreadIdRole,
         QByteArrayLiteral("threadId")},
        {KindRole, QByteArrayLiteral("kind")},
        {SourceStatusRole,
         QByteArrayLiteral("sourceStatus")},
        {RolloutPathRole,
         QByteArrayLiteral("rolloutPath")},
    };
}

void ProcessListModel::loadFailureState()
{
    if (failureStatePath_.trimmed().isEmpty()) {
        return;
    }

    QFile file(failureStatePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            file.readAll(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return;
    }
    const QJsonObject root =
        document.object();
    if (root.value(
                QStringLiteral("version"))
            .toInt()
        != kFailureStateVersion) {
        return;
    }

    const QJsonArray unresolved =
        root.value(
                QStringLiteral("unresolved"))
            .toArray();
    for (const QJsonValue& value : unresolved) {
        const auto item = failureFromJson(value);
        if (item.has_value()) {
            unresolvedFailures_.insert(
                item->processId,
                *item);
        }
    }

    const QJsonArray handled =
        root.value(QStringLiteral("handled"))
            .toArray();
    for (const QJsonValue& value : handled) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object =
            value.toObject();
        const QString processId =
            object.value(
                      QStringLiteral(
                          "processId"))
                .toString()
                .trimmed();
        if (processId.isEmpty()) {
            continue;
        }
        QDateTime activityAt;
        const QJsonValue activityValue =
            object.value(
                QStringLiteral(
                    "activityAtMs"));
        if (activityValue.isDouble()) {
            activityAt =
                QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(
                        activityValue.toDouble()),
                    QTimeZone::UTC);
        }
        handledFailures_.insert(
            processId,
            {
                processId,
                object.value(
                          QStringLiteral(
                              "threadId"))
                    .toString()
                    .trimmed(),
                activityAt,
            });
    }
}

void ProcessListModel::persistFailureState() const
{
    if (failureStatePath_.trimmed().isEmpty()) {
        return;
    }

    QStringList unresolvedIds =
        unresolvedFailures_.keys();
    unresolvedIds.sort();
    QJsonArray unresolved;
    for (const QString& id : unresolvedIds) {
        unresolved.append(
            failureJson(
                unresolvedFailures_.value(id)));
    }

    QStringList handledIds =
        handledFailures_.keys();
    handledIds.sort();
    QJsonArray handled;
    for (const QString& id : handledIds) {
        const HandledFailure failure =
            handledFailures_.value(id);
        QJsonObject object{
            {
                QStringLiteral("processId"),
                failure.processId,
            },
            {
                QStringLiteral("threadId"),
                failure.threadId,
            },
        };
        if (failure.activityAt.isValid()) {
            object.insert(
                QStringLiteral(
                    "activityAtMs"),
                static_cast<double>(
                    failure.activityAt
                        .toMSecsSinceEpoch()));
        }
        handled.append(object);
    }

    const QFileInfo fileInfo(
        failureStatePath_);
    if (!QDir().mkpath(
            fileInfo.absolutePath())) {
        return;
    }
    QSaveFile file(failureStatePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    const QByteArray contents =
        QJsonDocument(
            QJsonObject{
                {
                    QStringLiteral("version"),
                    kFailureStateVersion,
                },
                {
                    QStringLiteral(
                        "unresolved"),
                    unresolved,
                },
                {
                    QStringLiteral("handled"),
                    handled,
                },
            })
            .toJson(
                QJsonDocument::Compact);
    if (file.write(contents)
        != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

void ProcessListModel::setSnapshot(
    const CodexProcessSnapshot& snapshot,
    const QDateTime& now)
{
    QHash<QString, const BridgeTask*> tasksById;
    tasksById.reserve(snapshot.tasks.size());
    for (const BridgeTask& task : snapshot.tasks) {
        tasksById.insert(task.id, &task);
    }

    QVector<ProcessListItem> projected;
    projected.reserve(
        snapshot.jobs.size()
        + snapshot.tasks.size());
    for (const CodexJobRecord& job : snapshot.jobs) {
        if (!shouldLoadJob(job, now)) {
            continue;
        }
        ProcessListItem item =
            processItem(job, tasksById);
        if (shouldRetainProcess(item, now)) {
            projected.append(std::move(item));
        }
    }
    for (const BridgeTask& task : snapshot.tasks) {
        if (!shouldLoadThread(task, snapshot, now)) {
            continue;
        }
        const auto runtimeIterator =
            snapshot.runtimeStatuses.constFind(
                task.id);
        ProcessListItem item = processItem(
            task,
            runtimeIterator
                    == snapshot.runtimeStatuses.cend()
                ? std::nullopt
                : std::optional<
                      ThreadRuntimeStatus>(
                      runtimeIterator.value()));
        if (shouldRetainProcess(item, now)) {
            projected.append(std::move(item));
        }
    }

    const SidebarOrderingSnapshot ordering =
        sidebarStatePath_.isEmpty()
        ? SidebarOrderingSnapshot()
        : SidebarOrderingSnapshot::read(
              sidebarStatePath_);
    const auto sortProjected =
        [&ordering, &snapshot](
            QVector<ProcessListItem>& items) {
            std::stable_sort(
                items.begin(),
                items.end(),
                [&ordering, &snapshot](
                    const ProcessListItem& left,
                    const ProcessListItem& right) {
                    const auto entry =
                        [&snapshot](
                            const ProcessListItem& item) {
                            return SidebarOrderingSnapshot::Entry{
                                item.threadId.isEmpty()
                                    ? std::nullopt
                                    : std::optional<QString>(
                                          item.threadId),
                                item.kind
                                        == QStringLiteral(
                                            "job")
                                    ? std::nullopt
                                    : item.cwd,
                                statusRank(item.status),
                                item.activityAt.isValid()
                                    ? std::optional<QDateTime>(
                                          item.activityAt)
                                    : std::nullopt,
                                !item.threadId.isEmpty()
                                    && (snapshot
                                            .attentionPromotedThreadIds
                                            .contains(
                                                item.threadId)
                                        || snapshot
                                               .pendingApprovals
                                               .contains(
                                                   item.threadId)),
                            };
                        };
                    return ordering.orders(
                        entry(left),
                        entry(right));
                });
        };
    sortProjected(projected);

    const auto handledIdsForFailure =
        [this](const ProcessListItem& failure) {
            QStringList matches;
            for (auto iterator =
                     handledFailures_.cbegin();
                 iterator
                 != handledFailures_.cend();
                 ++iterator) {
                const HandledFailure& handled =
                    iterator.value();
                if (handled.processId
                        == failure.processId
                    || (!handled.threadId.isEmpty()
                        && handled.threadId
                            == failure.threadId)) {
                    matches.append(
                        iterator.key());
                }
            }
            return matches;
        };
    const auto suppressesHandledFailure =
        [this, &handledIdsForFailure](
            const ProcessListItem& failure) {
            const QStringList matches =
                handledIdsForFailure(failure);
            if (matches.isEmpty()) {
                return false;
            }

            bool isNewer = false;
            if (failure.activityAt.isValid()) {
                for (const QString& id : matches) {
                    const HandledFailure handled =
                        handledFailures_.value(id);
                    if (!handled.activityAt.isValid()
                        || handled.activityAt
                            < failure.activityAt) {
                        isNewer = true;
                        break;
                    }
                }
            }
            if (!isNewer) {
                return true;
            }
            for (const QString& id : matches) {
                handledFailures_.remove(id);
            }
            return false;
        };
    const auto removeMatchingFailure =
        [](QHash<QString, ProcessListItem>& failures,
           const ProcessListItem& failure) {
            for (auto iterator = failures.begin();
                 iterator != failures.end();) {
                if (failuresReferToSameProcess(
                        iterator.value(),
                        failure)) {
                    iterator =
                        failures.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        };
    const auto replaceMatchingFailure =
        [&removeMatchingFailure](
            QHash<QString, ProcessListItem>& failures,
            const ProcessListItem& failure) {
            removeMatchingFailure(
                failures,
                failure);
            failures.insert(
                failure.processId,
                failure);
        };

    QVector<ProcessListItem> reconciled;
    reconciled.reserve(
        projected.size()
        + unresolvedFailures_.size());
    QHash<QString, ProcessListItem>
        nextUnresolvedFailures;
    for (const ProcessListItem& item : projected) {
        if (item.status == TaskStatus::Failed
            && suppressesHandledFailure(item)) {
            continue;
        }
        reconciled.append(item);
        if (item.status == TaskStatus::Failed) {
            replaceMatchingFailure(
                nextUnresolvedFailures,
                item);
        }
    }

    for (auto iterator =
             unresolvedFailures_.cbegin();
         iterator
         != unresolvedFailures_.cend();
         ++iterator) {
        const ProcessListItem& failure =
            iterator.value();
        if (suppressesHandledFailure(failure)) {
            continue;
        }

        const auto matching =
            std::find_if(
                reconciled.begin(),
                reconciled.end(),
                [&failure](
                    const ProcessListItem& item) {
                    return failuresReferToSameProcess(
                        item,
                        failure);
                });
        if (matching == reconciled.end()) {
            const bool alreadyRetained =
                std::any_of(
                    nextUnresolvedFailures.cbegin(),
                    nextUnresolvedFailures.cend(),
                    [&failure](
                        const ProcessListItem& item) {
                        return failuresReferToSameProcess(
                            item,
                            failure);
                    });
            if (!alreadyRetained) {
                reconciled.append(failure);
                replaceMatchingFailure(
                    nextUnresolvedFailures,
                    failure);
            }
            continue;
        }

        if (matching->status
            == TaskStatus::Failed) {
            replaceMatchingFailure(
                nextUnresolvedFailures,
                *matching);
        } else if (failureIsResolved(
                       failure,
                       *matching)) {
            removeMatchingFailure(
                nextUnresolvedFailures,
                failure);
        } else {
            *matching = failure;
            replaceMatchingFailure(
                nextUnresolvedFailures,
                failure);
        }
    }

    unresolvedFailures_ =
        std::move(nextUnresolvedFailures);
    projected = std::move(reconciled);
    sortProjected(projected);
    if (projected.size()
        > kMaximumVisibleProcesses) {
        QVector<ProcessListItem> capped =
            projected.mid(
                0,
                kMaximumVisibleProcesses);
        for (qsizetype index =
                 kMaximumVisibleProcesses;
             index < projected.size();
             ++index) {
            const ProcessListItem& candidate =
                projected.at(index);
            if (!candidate.goal.has_value()) {
                continue;
            }
            const auto replacement =
                std::find_if(
                    capped.rbegin(),
                    capped.rend(),
                    [](const ProcessListItem&
                           item) {
                        return !item.goal
                                    .has_value();
                    });
            if (replacement
                == capped.rend()) {
                break;
            }
            *replacement = candidate;
        }
        projected = std::move(capped);
        sortProjected(projected);
    }
    persistFailureState();

    if (!hasUniqueIds(snapshot_)
        || !hasUniqueIds(projected)) {
        beginResetModel();
        snapshot_ = std::move(projected);
        endResetModel();
        emit snapshotChanged();
        return;
    }

    QSet<QString> incomingIds;
    incomingIds.reserve(projected.size());
    for (const ProcessListItem& item : projected) {
        incomingIds.insert(item.processId);
    }

    int row = static_cast<int>(snapshot_.size()) - 1;
    while (row >= 0) {
        if (incomingIds.contains(
                snapshot_.at(row).processId)) {
            --row;
            continue;
        }
        const int last = row;
        while (row >= 0
               && !incomingIds.contains(
                   snapshot_.at(row).processId)) {
            --row;
        }
        const int first = row + 1;
        beginRemoveRows({}, first, last);
        snapshot_.remove(
            first,
            last - first + 1);
        endRemoveRows();
    }

    const int targetCount =
        static_cast<int>(projected.size());
    int targetRow = 0;
    while (targetRow < targetCount) {
        if (targetRow
                < static_cast<int>(
                    snapshot_.size())
            && snapshot_.at(targetRow).processId
                == projected.at(targetRow)
                       .processId) {
            ++targetRow;
            continue;
        }

        const int sourceRow = indexOfId(
            snapshot_,
            projected.at(targetRow).processId,
            targetRow + 1);
        if (sourceRow >= 0) {
            const bool moving = beginMoveRows(
                {},
                sourceRow,
                sourceRow,
                {},
                targetRow);
            Q_ASSERT(moving);
            Q_UNUSED(moving);
            snapshot_.move(sourceRow, targetRow);
            endMoveRows();
            ++targetRow;
            continue;
        }

        int insertLast = targetRow;
        while (insertLast + 1 < targetCount
               && indexOfId(
                      snapshot_,
                      projected.at(insertLast + 1)
                          .processId,
                      targetRow)
                   < 0) {
            ++insertLast;
        }
        beginInsertRows(
            {},
            targetRow,
            insertLast);
        for (int insertRow = targetRow;
             insertRow <= insertLast;
             ++insertRow) {
            snapshot_.insert(
                insertRow,
                projected.at(insertRow));
        }
        endInsertRows();
        targetRow = insertLast + 1;
    }

    for (int updateRow = 0;
         updateRow < targetCount;
         ++updateRow) {
        const QList<int> roles = changedRoles(
            snapshot_.at(updateRow),
            projected.at(updateRow));
        snapshot_[updateRow] =
            projected.at(updateRow);
        if (!roles.isEmpty()) {
            const QModelIndex changed =
                index(updateRow, 0);
            emit dataChanged(
                changed,
                changed,
                roles);
        }
    }
    emit snapshotChanged();
}

void ProcessListModel::markFailureHandled(
    const QString& processId,
    const QString& threadId)
{
    const QString normalizedProcessId =
        processId.trimmed();
    const QString normalizedThreadId =
        threadId.trimmed();
    if (normalizedProcessId.isEmpty()
        && normalizedThreadId.isEmpty()) {
        return;
    }

    std::optional<ProcessListItem>
        handledFailure;
    const auto consider =
        [&handledFailure,
         &normalizedProcessId,
         &normalizedThreadId](
            const ProcessListItem& item) {
            if (item.status
                    != TaskStatus::Failed
                || !failureMatchesIdentity(
                    item,
                    normalizedProcessId,
                    normalizedThreadId)) {
                return;
            }
            if (!handledFailure.has_value()
                || (!handledFailure->activityAt.isValid()
                    && item.activityAt.isValid())
                || (handledFailure->activityAt.isValid()
                    && item.activityAt.isValid()
                    && handledFailure->activityAt
                        < item.activityAt)) {
                handledFailure = item;
            }
        };
    for (auto iterator =
             unresolvedFailures_.cbegin();
         iterator
         != unresolvedFailures_.cend();
         ++iterator) {
        consider(iterator.value());
    }
    for (const ProcessListItem& item : snapshot_) {
        consider(item);
    }
    if (!handledFailure.has_value()) {
        return;
    }

    for (auto iterator =
             unresolvedFailures_.begin();
         iterator
         != unresolvedFailures_.end();) {
        if (failureMatchesIdentity(
                iterator.value(),
                normalizedProcessId,
                normalizedThreadId)) {
            iterator =
                unresolvedFailures_.erase(
                    iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator =
             handledFailures_.begin();
         iterator
         != handledFailures_.end();) {
        const HandledFailure& handled =
            iterator.value();
        if (handled.processId
                == handledFailure->processId
            || (!handled.threadId.isEmpty()
                && handled.threadId
                    == handledFailure->threadId)) {
            iterator =
                handledFailures_.erase(
                    iterator);
        } else {
            ++iterator;
        }
    }
    handledFailures_.insert(
        handledFailure->processId,
        {
            handledFailure->processId,
            handledFailure->threadId,
            handledFailure->activityAt,
        });

    bool removed = false;
    for (int row =
             static_cast<int>(
                 snapshot_.size())
             - 1;
         row >= 0;
         --row) {
        const ProcessListItem& item =
            snapshot_.at(row);
        if (item.status
                != TaskStatus::Failed
            || !failureMatchesIdentity(
                item,
                normalizedProcessId,
                normalizedThreadId)) {
            continue;
        }
        beginRemoveRows({}, row, row);
        snapshot_.removeAt(row);
        endRemoveRows();
        removed = true;
    }

    persistFailureState();
    if (removed) {
        emit snapshotChanged();
    }
}

const QVector<ProcessListItem>&
ProcessListModel::snapshot() const noexcept
{
    return snapshot_;
}

} // namespace companion
