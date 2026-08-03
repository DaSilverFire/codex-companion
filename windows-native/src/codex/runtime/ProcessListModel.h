#pragma once

#include "codex/models/BridgeModels.h"
#include "codex/runtime/CodexProcessSnapshot.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVector>

#include <optional>

namespace companion {

struct ProcessListItem final {
    QString processId;
    QString threadId;
    QString kind;
    QString title;
    QString preview;
    double updatedAt = 0.0;
    std::optional<QString> cwd;
    TaskStatus status = TaskStatus::Waiting;
    bool needsApproval = false;
    std::optional<QString> activeTurnId;
    std::optional<QString> model;
    std::optional<QString> reasoningEffort;
    std::optional<BridgeTaskGroup> taskGroup;
    std::optional<BridgeGoal> goal;
    QDateTime activityAt;
    std::optional<ThreadRuntimeStatus>
        runtimeStatus;
    QString sourceStatus;
    QString rolloutPath;

    friend bool operator==(
        const ProcessListItem&,
        const ProcessListItem&) = default;
};

class ProcessListModel final : public QAbstractListModel {
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
        RuntimeStatusRole,
        ActiveTurnIdRole,
        ModelRole,
        ReasoningEffortRole,
        GroupKindRole,
        GroupTitleRole,
        GoalRole,
        ProcessIdRole,
        ThreadIdRole,
        KindRole,
        SourceStatusRole,
        RolloutPathRole,
    };
    Q_ENUM(Role)

    explicit ProcessListModel(QObject* parent = nullptr);
    ProcessListModel(
        QString sidebarStatePath,
        QObject* parent = nullptr);
    ProcessListModel(
        QString sidebarStatePath,
        QString failureStatePath,
        QObject* parent = nullptr);

    static QString defaultFailureStatePath();

    int rowCount(
        const QModelIndex& parent = {}) const override;
    QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSnapshot(
        const CodexProcessSnapshot& snapshot,
        const QDateTime& now =
            QDateTime::currentDateTimeUtc());
    Q_INVOKABLE void markFailureHandled(
        const QString& processId,
        const QString& threadId = {});
    const QVector<ProcessListItem>&
    snapshot() const noexcept;

signals:
    void snapshotChanged();

private:
    struct HandledFailure final {
        QString processId;
        QString threadId;
        QDateTime activityAt;
    };

    void loadFailureState();
    void persistFailureState() const;

    QString sidebarStatePath_;
    QString failureStatePath_;
    QHash<QString, ProcessListItem>
        unresolvedFailures_;
    QHash<QString, HandledFailure>
        handledFailures_;
    QVector<ProcessListItem> snapshot_;
};

} // namespace companion
