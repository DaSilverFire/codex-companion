#pragma once

#include "codex/models/BridgeModels.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <optional>

namespace companion {

class PetViewModel;
class ProcessListModel;
struct ProcessListItem;

class PetProcessReactionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        bool hasAttention
        READ hasAttention
        NOTIFY attentionChanged)
    Q_PROPERTY(
        QVariantMap attentionMessage
        READ attentionMessage
        NOTIFY attentionChanged)
    Q_PROPERTY(
        QVariantMap latestAttentionHighlight
        READ latestAttentionHighlight
        NOTIFY latestAttentionChanged)
    Q_PROPERTY(
        int goalConfettiTrigger
        READ goalConfettiTrigger
        NOTIFY goalConfettiTriggerChanged)

public:
    struct Timings final {
        int runningSettlementMilliseconds = 2400;
        int attentionDismissMilliseconds = 9000;
        int goalCompletionMilliseconds = 6200;
        int reactionPresentationMilliseconds = 0;
    };

    explicit PetProcessReactionController(
        PetViewModel& petViewModel,
        Timings timings = {},
        QObject* parent = nullptr);
    PetProcessReactionController(
        PetViewModel& petViewModel,
        Timings timings,
        QString settingsFilePath,
        QObject* parent = nullptr);

    void setProcessModel(
        ProcessListModel* processModel);
    void setProcessSurfaceVisible(bool visible);
    bool hasAttention() const noexcept;
    QVariantMap attentionMessage() const;
    QVariantMap latestAttentionHighlight() const;
    int goalConfettiTrigger() const noexcept;

    Q_INVOKABLE void dismissAttention();

signals:
    void attentionChanged();
    void latestAttentionChanged();
    void goalConfettiTriggerChanged();

private:
    struct TaskSnapshot final {
        TaskStatus status = TaskStatus::Waiting;
        std::optional<GoalStatus> goalStatus;
        QString goalId;
        QString preview;
    };

    struct Reaction final {
        int priority = 0;
        QString animation;
        QVariantMap message;
    };

    void handleSnapshot();
    void handlePetMenuChanged();
    void handlePetVisibilityChanged();
    void syncMenuAnimation(
        const QVector<ProcessListItem>& items);
    void updateLatestHighlight(
        const Reaction& reaction);
    void stageReaction(
        const Reaction& reaction);
    void cancelPendingReaction();
    void dismissPresentedAttention(
        bool cancelPending);
    void presentReaction(const Reaction& reaction);
    void triggerGoalCelebration(
        const ProcessListItem& item);
    bool shouldCelebrateGoal(
        const ProcessListItem& item) const;
    static QString goalIdentity(
        const ProcessListItem& item);
    static QString goalCompletionKey(
        const ProcessListItem& item);
    void loadGoalCelebrationState();
    void persistGoalCelebrationState() const;
    static QHash<QString, TaskSnapshot>
    processSnapshots(
        const QVector<ProcessListItem>& items);
    static std::optional<Reaction> appearance(
        const ProcessListItem& item);
    static std::optional<Reaction> transition(
        const TaskSnapshot& previous,
        const ProcessListItem& current);
    static Reaction reaction(
        QString kind,
        QString title,
        QString animation,
        const ProcessListItem& item,
        QString detail,
        int priority);

    PetViewModel& petViewModel_;
    Timings timings_;
    QPointer<ProcessListModel> processModel_;
    QHash<QString, TaskSnapshot> snapshots_;
    QVariantMap attentionMessage_;
    QVariantMap latestAttentionHighlight_;
    QString settingsFilePath_;
    QTimer runningSettlementTimer_;
    QTimer attentionDismissTimer_;
    QTimer goalCompletionTimer_;
    QTimer reactionPresentationTimer_;
    std::optional<Reaction> pendingReaction_;
    QSet<QString> seenGoalCompletionKeys_;
    int goalConfettiTrigger_ = 0;
    bool seeded_ = false;
    bool runningSettled_ = false;
    bool processSurfaceVisible_ = true;
};

} // namespace companion
