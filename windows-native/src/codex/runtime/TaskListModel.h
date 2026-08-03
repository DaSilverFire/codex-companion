#pragma once

#include "codex/models/BridgeModels.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QVariant>
#include <QVector>

namespace companion {

class TaskListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        PreviewRole,
        UpdatedAtRole,
        CwdRole,
        StatusRole,
        NeedsApprovalRole,
        ActiveTurnIdRole,
        ModelRole,
        ReasoningEffortRole,
        GroupKindRole,
        GroupTitleRole,
        GoalRole,
        ProcessIdRole,
    };
    Q_ENUM(Role)

    explicit TaskListModel(QObject* parent = nullptr);

    int rowCount(
        const QModelIndex& parent = {}) const override;
    QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSnapshot(const QVector<BridgeTask>& tasks);
    const QVector<BridgeTask>& snapshot() const noexcept;

signals:
    void snapshotChanged();

private:
    QVector<BridgeTask> snapshot_;
};

} // namespace companion
