#include "app/PetProcessReactionController.h"

#include "codex/runtime/ProcessListModel.h"
#include "ui/PetViewModel.h"

#include <QSettings>
#include <QString>

#include <utility>

namespace {

QString trimmed(QString value)
{
    return value.trimmed();
}

bool isGoalActive(
    const companion::ProcessListItem& item)
{
    return item.goal.has_value()
        && item.goal->status
            == companion::GoalStatus::Active;
}

bool isGoalComplete(
    const companion::ProcessListItem& item)
{
    return item.goal.has_value()
        && item.goal->status
            == companion::GoalStatus::Complete;
}

} // namespace

namespace companion {

PetProcessReactionController::
    PetProcessReactionController(
        PetViewModel& petViewModel,
        Timings timings,
        QObject* parent)
    : PetProcessReactionController(
          petViewModel,
          timings,
          {},
          parent)
{
}

PetProcessReactionController::
    PetProcessReactionController(
        PetViewModel& petViewModel,
        Timings timings,
        QString settingsFilePath,
        QObject* parent)
    : QObject(parent),
      petViewModel_(petViewModel),
      timings_(timings),
      settingsFilePath_(
          std::move(settingsFilePath))
{
    loadGoalCelebrationState();
    runningSettlementTimer_.setSingleShot(true);
    attentionDismissTimer_.setSingleShot(true);
    goalCompletionTimer_.setSingleShot(true);
    reactionPresentationTimer_.setSingleShot(true);

    connect(
        &runningSettlementTimer_,
        &QTimer::timeout,
        this,
        [this] {
            runningSettled_ = true;
            if (processModel_) {
                syncMenuAnimation(
                    processModel_->snapshot());
            }
        });
    connect(
        &attentionDismissTimer_,
        &QTimer::timeout,
        this,
        [this] {
            dismissPresentedAttention(false);
        });
    connect(
        &goalCompletionTimer_,
        &QTimer::timeout,
        &petViewModel_,
        &PetViewModel::endGoalCelebration);
    connect(
        &reactionPresentationTimer_,
        &QTimer::timeout,
        this,
        [this] {
            if (!pendingReaction_.has_value()) {
                return;
            }
            const Reaction reaction =
                std::move(*pendingReaction_);
            pendingReaction_.reset();
            if (!petViewModel_.visible()
                || petViewModel_.menuOpen()) {
                return;
            }
            presentReaction(reaction);
        });
    connect(
        &petViewModel_,
        &PetViewModel::menuOpenChanged,
        this,
        &PetProcessReactionController::
            handlePetMenuChanged);
    connect(
        &petViewModel_,
        &PetViewModel::visibilityChanged,
        this,
        &PetProcessReactionController::
            handlePetVisibilityChanged);
}

void PetProcessReactionController::setProcessModel(
    ProcessListModel* processModel)
{
    if (processModel_ == processModel) {
        return;
    }
    if (processModel_) {
        disconnect(
            processModel_,
            nullptr,
            this,
            nullptr);
    }

    processModel_ = processModel;
    snapshots_.clear();
    seeded_ = false;
    runningSettled_ = false;
    runningSettlementTimer_.stop();
    goalCompletionTimer_.stop();
    petViewModel_.endGoalCelebration();
    dismissAttention();

    if (!processModel_) {
        return;
    }
    connect(
        processModel_,
        &ProcessListModel::snapshotChanged,
        this,
        &PetProcessReactionController::
            handleSnapshot);
    handleSnapshot();
}

void PetProcessReactionController::
    setProcessSurfaceVisible(bool visible)
{
    if (processSurfaceVisible_ == visible) {
        return;
    }

    processSurfaceVisible_ = visible;
    runningSettlementTimer_.stop();
    runningSettled_ = false;
    if (processSurfaceVisible_
        && processModel_
        && petViewModel_.menuOpen()) {
        syncMenuAnimation(
            processModel_->snapshot());
    }
}

bool PetProcessReactionController::hasAttention()
    const noexcept
{
    return !attentionMessage_.isEmpty();
}

QVariantMap
PetProcessReactionController::attentionMessage()
    const
{
    return attentionMessage_;
}

QVariantMap PetProcessReactionController::
    latestAttentionHighlight() const
{
    return latestAttentionHighlight_;
}

int PetProcessReactionController::
    goalConfettiTrigger() const noexcept
{
    return goalConfettiTrigger_;
}

void PetProcessReactionController::dismissAttention()
{
    dismissPresentedAttention(true);
}

void PetProcessReactionController::
    dismissPresentedAttention(
    bool cancelPending)
{
    if (cancelPending) {
        cancelPendingReaction();
    }
    attentionDismissTimer_.stop();
    if (attentionMessage_.isEmpty()) {
        petViewModel_.clearAttentionAnimation();
        return;
    }

    attentionMessage_.clear();
    petViewModel_.clearAttentionAnimation();
    emit attentionChanged();
}

void PetProcessReactionController::handleSnapshot()
{
    if (!processModel_) {
        return;
    }
    const QVector<ProcessListItem>& items =
        processModel_->snapshot();
    syncMenuAnimation(items);

    const auto nextSnapshots =
        processSnapshots(items);
    if (!seeded_) {
        snapshots_ = nextSnapshots;
        seeded_ = true;
        for (const ProcessListItem& item :
             items) {
            if (shouldCelebrateGoal(item)) {
                triggerGoalCelebration(item);
                break;
            }
        }
        return;
    }

    for (const ProcessListItem& item : items) {
        if (!shouldCelebrateGoal(item)) {
            continue;
        }
        const auto previous =
            snapshots_.constFind(
                item.processId);
        const QString nextGoalId =
            goalIdentity(item);
        const bool goalWasIncomplete =
            previous == snapshots_.constEnd()
            || previous->goalStatus
                != GoalStatus::Complete
            || previous->goalId
                != nextGoalId;
        const bool processJustCompleted =
            previous != snapshots_.constEnd()
            && previous->status
                == TaskStatus::Running
            && item.status
                == TaskStatus::Completed;
        if (goalWasIncomplete
            || processJustCompleted) {
            triggerGoalCelebration(item);
            break;
        }
    }

    std::optional<Reaction> selected;
    for (const ProcessListItem& item : items) {
        const auto previous =
            snapshots_.constFind(
                item.processId);
        const auto candidate =
            previous == snapshots_.constEnd()
            ? appearance(item)
            : transition(
                  previous.value(),
                  item);
        if (candidate.has_value()
            && (!selected.has_value()
                || candidate->priority
                    > selected->priority)) {
            selected = candidate;
        }
    }
    snapshots_ = nextSnapshots;

    if (selected.has_value()) {
        updateLatestHighlight(*selected);
        if (petViewModel_.visible()
            && !petViewModel_.menuOpen()) {
            stageReaction(*selected);
        }
    }
}

void PetProcessReactionController::
    handlePetMenuChanged()
{
    runningSettlementTimer_.stop();
    runningSettled_ = false;
    if (!processModel_) {
        return;
    }
    if (petViewModel_.menuOpen()) {
        cancelPendingReaction();
        syncMenuAnimation(
            processModel_->snapshot());
    } else if (hasAttention()) {
        petViewModel_.setAttentionAnimation(
            attentionMessage_
                .value(
                    QStringLiteral(
                        "animation"))
                .toString());
    }
}

void PetProcessReactionController::
    handlePetVisibilityChanged()
{
    if (!petViewModel_.visible()) {
        dismissAttention();
    }
}

void PetProcessReactionController::syncMenuAnimation(
    const QVector<ProcessListItem>& items)
{
    if (!processSurfaceVisible_
        || !petViewModel_.menuOpen()) {
        return;
    }

    bool hasFailed = false;
    bool hasRunning = false;
    bool hasCompleted = false;
    for (const ProcessListItem& item : items) {
        if (item.kind
            == QStringLiteral("notice")) {
            continue;
        }
        hasFailed = hasFailed
            || item.status
                == TaskStatus::Failed;
        hasRunning = hasRunning
            || item.status
                == TaskStatus::Running;
        hasCompleted = hasCompleted
            || item.status
                == TaskStatus::Completed;
    }

    QString animation;
    if (hasFailed) {
        animation = QStringLiteral("failed");
    } else if (hasRunning) {
        animation = runningSettled_
            ? QStringLiteral("idle")
            : QStringLiteral("running");
        if (!runningSettled_
            && !runningSettlementTimer_
                    .isActive()) {
            runningSettlementTimer_.start(
                qMax(
                    0,
                    timings_
                        .runningSettlementMilliseconds));
        }
    } else if (hasCompleted) {
        animation =
            QStringLiteral("review");
    } else {
        animation =
            QStringLiteral("waiting");
    }

    if (!hasRunning) {
        runningSettlementTimer_.stop();
        runningSettled_ = false;
    }
    petViewModel_.setSelectedAnimation(animation);
}

void PetProcessReactionController::
    updateLatestHighlight(
    const Reaction& reaction)
{
    const QVariantMap highlight{
        {
            QStringLiteral("processId"),
            reaction.message.value(
                QStringLiteral("processId")),
        },
        {
            QStringLiteral("kind"),
            reaction.message.value(
                QStringLiteral("kind")),
        },
    };
    if (latestAttentionHighlight_
        != highlight) {
        latestAttentionHighlight_ =
            highlight;
        emit latestAttentionChanged();
    }
}

void PetProcessReactionController::stageReaction(
    const Reaction& reaction)
{
    const int delayMilliseconds =
        qMax(
            0,
            timings_
                .reactionPresentationMilliseconds);
    if (delayMilliseconds == 0) {
        cancelPendingReaction();
        presentReaction(reaction);
        return;
    }

    pendingReaction_ = reaction;
    reactionPresentationTimer_.start(
        delayMilliseconds);
}

void PetProcessReactionController::
    cancelPendingReaction()
{
    reactionPresentationTimer_.stop();
    pendingReaction_.reset();
}

void PetProcessReactionController::presentReaction(
    const Reaction& reaction)
{
    attentionMessage_ = reaction.message;
    petViewModel_.setAttentionAnimation(
        reaction.animation);
    attentionDismissTimer_.start(
        qMax(
            0,
            timings_
                .attentionDismissMilliseconds));
    emit attentionChanged();
}

void PetProcessReactionController::
    triggerGoalCelebration(
        const ProcessListItem& item)
{
    const QString key =
        goalCompletionKey(item);
    seenGoalCompletionKeys_.insert(key);
    if (seenGoalCompletionKeys_.size() > 80) {
        QStringList keys =
            seenGoalCompletionKeys_.values();
        keys.sort();
        seenGoalCompletionKeys_ =
            QSet<QString>(
                keys.cend() - 80,
                keys.cend());
    }
    persistGoalCelebrationState();

    ++goalConfettiTrigger_;
    emit goalConfettiTriggerChanged();
    petViewModel_.beginGoalCelebration();
    goalCompletionTimer_.start(
        qMax(
            0,
            timings_
                .goalCompletionMilliseconds));
}

bool PetProcessReactionController::
    shouldCelebrateGoal(
        const ProcessListItem& item) const
{
    return item.kind
            != QStringLiteral("notice")
        && isGoalComplete(item)
        && !seenGoalCompletionKeys_.contains(
            goalCompletionKey(item));
}

QString PetProcessReactionController::goalIdentity(
    const ProcessListItem& item)
{
    if (!item.goal.has_value()) {
        return {};
    }
    if (item.goal->createdAt != 0) {
        return QString::number(
            item.goal->createdAt);
    }
    return QStringLiteral("goal");
}

QString PetProcessReactionController::
    goalCompletionKey(
        const ProcessListItem& item)
{
    return QStringList{
        QStringLiteral("goal-celebration-v2"),
        item.processId,
        goalIdentity(item),
    }.join(QLatin1Char('|'));
}

void PetProcessReactionController::
    loadGoalCelebrationState()
{
    if (settingsFilePath_.trimmed().isEmpty()) {
        return;
    }
    QSettings settings(
        settingsFilePath_,
        QSettings::IniFormat);
    const QStringList keys =
        settings.value(
                    QStringLiteral(
                        "pet/seenGoalCompletionKeys"))
            .toStringList();
    for (const QString& key : keys) {
        const QString normalized =
            key.trimmed();
        if (!normalized.isEmpty()) {
            seenGoalCompletionKeys_.insert(
                normalized);
        }
    }
}

void PetProcessReactionController::
    persistGoalCelebrationState() const
{
    if (settingsFilePath_.trimmed().isEmpty()) {
        return;
    }
    QStringList keys =
        seenGoalCompletionKeys_.values();
    keys.sort();
    QSettings settings(
        settingsFilePath_,
        QSettings::IniFormat);
    settings.setValue(
        QStringLiteral(
            "pet/seenGoalCompletionKeys"),
        keys);
    settings.sync();
}

QHash<QString,
      PetProcessReactionController::TaskSnapshot>
PetProcessReactionController::processSnapshots(
    const QVector<ProcessListItem>& items)
{
    QHash<QString, TaskSnapshot> result;
    result.reserve(items.size());
    for (const ProcessListItem& item : items) {
        result.insert(
            item.processId,
            {
                item.status,
                item.goal.has_value()
                    ? std::optional<GoalStatus>(
                          item.goal->status)
                    : std::nullopt,
                item.goal.has_value()
                    ? goalIdentity(item)
                    : QString(),
                trimmed(item.preview),
            });
    }
    return result;
}

std::optional<
    PetProcessReactionController::Reaction>
PetProcessReactionController::appearance(
    const ProcessListItem& item)
{
    if (item.kind == QStringLiteral("notice")) {
        return std::nullopt;
    }
    if (item.status == TaskStatus::Failed) {
        return reaction(
            QStringLiteral("failure"),
            QStringLiteral(
                "Uh-oh, I hit a snag."),
            QStringLiteral("failed"),
            item,
            item.preview,
            5);
    }
    if (isGoalActive(item)) {
        return reaction(
            QStringLiteral("goal"),
            QStringLiteral(
                "I am on it! The new goal has started."),
            QStringLiteral("running"),
            item,
            item.goal->objective,
            4);
    }
    if (item.status == TaskStatus::Waiting) {
        return reaction(
            QStringLiteral("attention"),
            QStringLiteral(
                "Psst, I need your approval."),
            QStringLiteral("waiting"),
            item,
            item.preview,
            2);
    }
    if (item.status == TaskStatus::Completed) {
        return reaction(
            QStringLiteral("completion"),
            isGoalComplete(item)
                ? QStringLiteral(
                      "We did it! That goal is complete.")
                : QStringLiteral(
                      "All done! I wrapped that one up."),
            QStringLiteral(
                "goal-complete"),
            item,
            item.preview,
            3);
    }
    return std::nullopt;
}

std::optional<
PetProcessReactionController::Reaction>
PetProcessReactionController::transition(
    const TaskSnapshot& previous,
    const ProcessListItem& current)
{
    if (current.kind
        == QStringLiteral("notice")) {
        return std::nullopt;
    }
    if (current.status == TaskStatus::Failed
        && previous.status
            != TaskStatus::Failed) {
        return appearance(current);
    }
    if (isGoalActive(current)
        && (previous.goalStatus
                != GoalStatus::Active
            || previous.goalId
                != goalIdentity(current))) {
        return appearance(current);
    }
    if (current.status == TaskStatus::Completed
        && previous.status
            != TaskStatus::Completed) {
        return appearance(current);
    }
    if (current.status == TaskStatus::Waiting
        && previous.status
            == TaskStatus::Running) {
        return reaction(
            QStringLiteral("attention"),
            QStringLiteral(
                "Psst, I need your approval."),
            QStringLiteral("waiting"),
            current,
            current.preview,
            2);
    }

    const QString nextPreview =
        trimmed(current.preview);
    if (!nextPreview.isEmpty()
        && nextPreview
            != previous.preview) {
        return reaction(
            QStringLiteral("response"),
            QStringLiteral(
                "I found something for you!"),
            QStringLiteral("talking"),
            current,
            nextPreview,
            1);
    }
    return std::nullopt;
}

PetProcessReactionController::Reaction
PetProcessReactionController::reaction(
    QString kind,
    QString title,
    QString animation,
    const ProcessListItem& item,
    QString detail,
    int priority)
{
    detail = trimmed(std::move(detail));
    const QString processTitle =
        trimmed(item.title);
    QVariantMap message{
        {QStringLiteral("kind"), kind},
        {QStringLiteral("title"), title},
        {QStringLiteral("detail"), detail},
        {
            QStringLiteral("processTitle"),
            processTitle,
        },
        {
            QStringLiteral("processId"),
            item.processId,
        },
        {
            QStringLiteral("threadId"),
            item.threadId,
        },
        {
            QStringLiteral("animation"),
            animation,
        },
    };
    return {
        priority,
        std::move(animation),
        std::move(message),
    };
}

} // namespace companion
