#include "codex/runtime/TaskListModel.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace companion {

namespace {

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

bool hasUniqueIds(const QVector<BridgeTask>& tasks)
{
    QSet<QString> ids;
    ids.reserve(tasks.size());
    for (const BridgeTask& task : tasks) {
        if (ids.contains(task.id)) {
            return false;
        }
        ids.insert(task.id);
    }
    return true;
}

int indexOfId(
    const QVector<BridgeTask>& tasks,
    const QString& id,
    int firstRow)
{
    const int count = static_cast<int>(tasks.size());
    for (int row = firstRow; row < count; ++row) {
        if (tasks.at(row).id == id) {
            return row;
        }
    }
    return -1;
}

QList<int> changedRoles(
    const BridgeTask& current,
    const BridgeTask& updated)
{
    QList<int> roles;
    if (current.title != updated.title) {
        roles.append(TaskListModel::TitleRole);
    }
    if (current.preview != updated.preview) {
        roles.append(TaskListModel::PreviewRole);
    }
    if (current.updatedAt != updated.updatedAt) {
        roles.append(TaskListModel::UpdatedAtRole);
    }
    if (current.cwd != updated.cwd) {
        roles.append(TaskListModel::CwdRole);
    }
    if (current.status != updated.status) {
        roles.append(TaskListModel::StatusRole);
    }
    if (current.needsApproval != updated.needsApproval) {
        roles.append(TaskListModel::NeedsApprovalRole);
    }
    if (current.activeTurnId != updated.activeTurnId) {
        roles.append(TaskListModel::ActiveTurnIdRole);
    }
    if (current.model != updated.model) {
        roles.append(TaskListModel::ModelRole);
    }
    if (current.reasoningEffort
        != updated.reasoningEffort) {
        roles.append(TaskListModel::ReasoningEffortRole);
    }

    const bool currentHasGroup =
        current.taskGroup.has_value();
    const bool updatedHasGroup =
        updated.taskGroup.has_value();
    if (currentHasGroup != updatedHasGroup
        || (currentHasGroup
            && current.taskGroup->kind
                != updated.taskGroup->kind)) {
        roles.append(TaskListModel::GroupKindRole);
    }
    if (currentHasGroup != updatedHasGroup
        || (currentHasGroup
            && current.taskGroup->title
                != updated.taskGroup->title)) {
        roles.append(TaskListModel::GroupTitleRole);
    }
    if (current.goal != updated.goal) {
        roles.append(TaskListModel::GoalRole);
    }
    return roles;
}

} // namespace

TaskListModel::TaskListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int TaskListModel::rowCount(
    const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(snapshot_.size());
}

QVariant TaskListModel::data(
    const QModelIndex& index,
    int role) const
{
    if (!index.isValid()
        || index.column() != 0
        || index.row() < 0
        || index.row() >= rowCount()) {
        return {};
    }

    const BridgeTask& task = snapshot_.at(index.row());
    switch (role) {
    case IdRole:
        return task.id;
    case TitleRole:
        return task.title;
    case PreviewRole:
        return task.preview;
    case UpdatedAtRole:
        return task.updatedAt.secondsSinceReferenceDate;
    case CwdRole:
        return optionalString(task.cwd);
    case StatusRole:
        return taskStatusText(task.status);
    case NeedsApprovalRole:
        return task.needsApproval;
    case ActiveTurnIdRole:
        return optionalString(task.activeTurnId);
    case ModelRole:
        return optionalString(task.model);
    case ReasoningEffortRole:
        return optionalString(task.reasoningEffort);
    case GroupKindRole:
        return task.taskGroup.has_value()
            ? QVariant(
                  taskGroupKindText(task.taskGroup->kind))
            : QVariant();
    case GroupTitleRole:
        return task.taskGroup.has_value()
            ? QVariant(task.taskGroup->title)
            : QVariant();
    case GoalRole:
        return goalVariant(task.goal);
    case ProcessIdRole:
        return task.id;
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const
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
        {ActiveTurnIdRole,
         QByteArrayLiteral("activeTurnId")},
        {ModelRole, QByteArrayLiteral("model")},
        {ReasoningEffortRole,
         QByteArrayLiteral("reasoningEffort")},
        {GroupKindRole, QByteArrayLiteral("groupKind")},
        {GroupTitleRole,
         QByteArrayLiteral("groupTitle")},
        {GoalRole, QByteArrayLiteral("goal")},
        {ProcessIdRole, QByteArrayLiteral("processId")},
    };
}

void TaskListModel::setSnapshot(
    const QVector<BridgeTask>& tasks)
{
    if (!hasUniqueIds(snapshot_)
        || !hasUniqueIds(tasks)) {
        beginResetModel();
        snapshot_ = tasks;
        endResetModel();
        emit snapshotChanged();
        return;
    }

    QSet<QString> incomingIds;
    incomingIds.reserve(tasks.size());
    for (const BridgeTask& task : tasks) {
        incomingIds.insert(task.id);
    }

    int row = static_cast<int>(snapshot_.size()) - 1;
    while (row >= 0) {
        if (incomingIds.contains(snapshot_.at(row).id)) {
            --row;
            continue;
        }
        const int last = row;
        while (row >= 0
               && !incomingIds.contains(
                   snapshot_.at(row).id)) {
            --row;
        }
        const int first = row + 1;
        beginRemoveRows({}, first, last);
        snapshot_.remove(first, last - first + 1);
        endRemoveRows();
    }

    const int targetCount = static_cast<int>(tasks.size());
    int targetRow = 0;
    while (targetRow < targetCount) {
        if (targetRow
                < static_cast<int>(snapshot_.size())
            && snapshot_.at(targetRow).id
                == tasks.at(targetRow).id) {
            ++targetRow;
            continue;
        }

        const int sourceRow = indexOfId(
            snapshot_,
            tasks.at(targetRow).id,
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
                      tasks.at(insertLast + 1).id,
                      targetRow)
                   < 0) {
            ++insertLast;
        }
        beginInsertRows({}, targetRow, insertLast);
        for (int insertRow = targetRow;
             insertRow <= insertLast;
             ++insertRow) {
            snapshot_.insert(
                insertRow,
                tasks.at(insertRow));
        }
        endInsertRows();
        targetRow = insertLast + 1;
    }

    for (int updateRow = 0;
         updateRow < targetCount;
         ++updateRow) {
        const QList<int> roles = changedRoles(
            snapshot_.at(updateRow),
            tasks.at(updateRow));
        snapshot_[updateRow] = tasks.at(updateRow);
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

const QVector<BridgeTask>&
TaskListModel::snapshot() const noexcept
{
    return snapshot_;
}

} // namespace companion
