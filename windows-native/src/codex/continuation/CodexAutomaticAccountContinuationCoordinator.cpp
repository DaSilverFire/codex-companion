#include "codex/continuation/CodexAutomaticAccountContinuationCoordinator.h"

#include "codex/accounts/CodexThreadAccountHandoffService.h"
#include "codex/continuation/CodexAutomaticContinuationPolicy.h"

#include <QMutexLocker>
#include <QSet>

#include <utility>

namespace companion {
namespace {

constexpr qint64
    kCascadeGuardMilliseconds =
        30 * 60 * 1000;

bool goalCanContinue(
    const std::optional<BridgeGoal>& goal)
{
    return !goal.has_value()
        || goal->status
                == GoalStatus::Active
        || goal->status
                == GoalStatus::
                    UsageLimited;
}

} // namespace

CodexAutomaticAccountContinuationCoordinator::
    CodexAutomaticAccountContinuationCoordinator(
        CodexAccountProfileStore&
            profileStore,
        CodexThreadAccountBindingStore&
            bindingStore,
        CodexAccountRouter& router,
        CodexContinuationCommands
            commands,
        CodexAutomaticContinuationJournal&
            journal,
        Clock clock)
    : profileStore_(&profileStore),
      bindingStore_(&bindingStore),
      router_(&router),
      commands_(std::move(commands)),
      journal_(&journal),
      clock_(
          clock
              ? std::move(clock)
              : Clock([] {
                    return QDateTime::
                        currentDateTimeUtc();
                }))
{
}

QVector<
    CodexAutomaticContinuationOutcome>
CodexAutomaticAccountContinuationCoordinator::
    continueEligible(
        bool enabled,
        const QVector<
            CodexAutomaticContinuationCandidate>&
            candidates,
        std::stop_token stopToken)
{
    QVector<
        CodexAutomaticContinuationOutcome>
        outcomes;
    if (!enabled
        || candidates.isEmpty()
        || stopToken.stop_requested()) {
        return outcomes;
    }

    QMutexLocker locker(&gate_);
    QSet<QString> seen;
    for (auto candidate : candidates) {
        if (stopToken.stop_requested()) {
            break;
        }
        candidate.threadId =
            candidate.threadId
                .trimmed();
        if (candidate.threadId
                .isEmpty()
            || seen.contains(
                candidate.threadId)) {
            continue;
        }
        seen.insert(
            candidate.threadId);
        const auto outcome =
            tryContinue(
                std::move(candidate),
                stopToken);
        if (outcome.has_value()) {
            outcomes.append(*outcome);
        }
    }
    return outcomes;
}

std::optional<
    CodexAutomaticContinuationAttempt>
CodexAutomaticAccountContinuationCoordinator::
    latestCompleted(
        QStringView threadId) const
{
    return journal_ == nullptr
        ? std::nullopt
        : journal_->latestCompleted(
              threadId);
}

std::optional<
    CodexAutomaticContinuationOutcome>
CodexAutomaticAccountContinuationCoordinator::
    tryContinue(
        CodexAutomaticContinuationCandidate
            candidate,
        std::stop_token stopToken)
{
    candidate.threadId =
        candidate.threadId.trimmed();
    candidate.rolloutPath =
        candidate.rolloutPath.trimmed();
    if (profileStore_ == nullptr
        || bindingStore_ == nullptr
        || router_ == nullptr
        || journal_ == nullptr
        || journal_->loadError()
               .has_value()
        || !CodexThreadAccountHandoffService::
                canHandoff(
                    candidate
                        .runtimeStatus)
        || candidate.threadId.isEmpty()
        || candidate.rolloutPath.isEmpty()
        || !commands_.readQuota
        || !commands_.readUsage
        || !commands_.readGoal
        || !commands_.handoff
        || !commands_.send) {
        return std::nullopt;
    }

    auto attempt =
        journal_->latestPending(
            candidate.threadId);
    if (!attempt.has_value()) {
        const auto originProfileId =
            bindingStore_->profileIdFor(
                candidate.threadId);
        if (!originProfileId.has_value()) {
            return std::nullopt;
        }
        const auto originProfile =
            profileStore_->profile(
                *originProfileId);
        if (!originProfile.has_value()) {
            return std::nullopt;
        }
        const CodexAccountRoute
            originRoute =
                router_->routeProfile(
                    originProfile->id);
        if (originRoute.profileId
            != std::optional<QUuid>(
                originProfile->id)) {
            return std::nullopt;
        }
        const auto interruption =
            commands_.readQuota(
                originRoute,
                candidate.threadId,
                stopToken);
        if (!interruption.hasValue()
            || !interruption.value()
                    .has_value()) {
            return std::nullopt;
        }
        const auto goal =
            commands_.readGoal(
                originRoute,
                candidate.threadId,
                stopToken);
        if (!goal.hasValue()
            || !goalCanContinue(
                goal.value())) {
            return std::nullopt;
        }

        const QString eventKey =
            goal.value().has_value()
            ? CodexAutomaticContinuationPolicy::
                  goalEventKey(
                      candidate.threadId,
                      goal.value()
                          ->objective,
                      goal.value()
                          ->updatedAt)
            : CodexAutomaticContinuationPolicy::
                  taskEventKey(
                      candidate.threadId,
                      interruption.value()
                          ->turnId);
        if (eventKey.isEmpty()) {
            return std::nullopt;
        }
        const auto existing =
            journal_->attempt(eventKey);
        if (existing.has_value()) {
            return std::nullopt;
        }

        const QDateTime timestamp =
            clock_();
        if (!timestamp.isValid()
            || journal_->
                   hasRecentCompleted(
                       candidate.threadId,
                       timestamp.addMSecs(
                           -kCascadeGuardMilliseconds))) {
            return std::nullopt;
        }
        const auto sourceUsage =
            commands_.readUsage(
                originRoute,
                stopToken);
        if (!sourceUsage.hasValue()
            || !CodexAutomaticContinuationPolicy::
                    hasConfirmedExhaustion(
                        sourceUsage
                            .value())) {
            return std::nullopt;
        }
        const auto destination =
            findDestination(
                originProfile->id,
                stopToken);
        if (!destination.has_value()) {
            return std::nullopt;
        }
        const auto planned =
            journal_->plan(
                eventKey,
                candidate.threadId,
                originProfile->id,
                destination->id,
                destination->label,
                CodexAutomaticContinuationPolicy::
                    clientMessageId(
                        QStringLiteral(
                            "codex-companion-quota-"),
                        eventKey),
                timestamp);
        if (!planned.hasValue()) {
            return std::nullopt;
        }
        attempt = planned.value();
    }

    const auto destinationProfile =
        profileStore_->profile(
            attempt
                ->destinationProfileId);
    if (!destinationProfile.has_value()) {
        return std::nullopt;
    }
    if (bindingStore_->profileIdFor(
            candidate.threadId)
        != std::optional<QUuid>(
            destinationProfile->id)) {
        const auto handedOff =
            commands_.handoff(
                candidate.threadId,
                candidate.rolloutPath,
                candidate.runtimeStatus,
                destinationProfile->id,
                stopToken);
        if (!handedOff.hasValue()) {
            return std::nullopt;
        }
    }
    const CodexAccountRoute
        destinationRoute =
            router_->routeProfile(
                destinationProfile->id);
    if (destinationRoute.profileId
        != std::optional<QUuid>(
            destinationProfile->id)) {
        return std::nullopt;
    }
    const auto goal =
        commands_.readGoal(
            destinationRoute,
            candidate.threadId,
            stopToken);
    if (!goal.hasValue()
        || !goalCanContinue(
            goal.value())) {
        return std::nullopt;
    }
    if (goal.value().has_value()
        && goal.value()->status
            == GoalStatus::UsageLimited) {
        if (!commands_.activateGoal) {
            return std::nullopt;
        }
        const auto activated =
            commands_.activateGoal(
                destinationRoute,
                candidate.threadId,
                stopToken);
        if (!activated.hasValue()) {
            return std::nullopt;
        }
    }
    const auto sent =
        commands_.send(
            destinationRoute,
            candidate.threadId,
            QStringLiteral("continue"),
            attempt->clientMessageId,
            stopToken);
    if (!sent.hasValue()) {
        return std::nullopt;
    }
    const QDateTime completedAt =
        clock_();
    if (!completedAt.isValid()) {
        return std::nullopt;
    }
    const auto completed =
        journal_->complete(
            attempt->eventKey,
            completedAt);
    if (!completed.hasValue()) {
        return std::nullopt;
    }
    return CodexAutomaticContinuationOutcome{
        candidate.threadId,
        attempt->originProfileId,
        destinationProfile->id,
        destinationProfile->label,
        attempt->eventKey,
        completedAt,
    };
}

std::optional<CodexAccountProfile>
CodexAutomaticAccountContinuationCoordinator::
    findDestination(
        const QUuid& originProfileId,
        std::stop_token stopToken) const
{
    for (const auto& profile :
         CodexAutomaticContinuationPolicy::
             orderedTargets(
                 profileStore_->profiles(),
                 originProfileId)) {
        if (stopToken.stop_requested()) {
            return std::nullopt;
        }
        const CodexAccountRoute route =
            router_->routeProfile(
                profile.id);
        if (route.profileId
            != std::optional<QUuid>(
                profile.id)) {
            continue;
        }
        const auto usage =
            commands_.readUsage(
                route,
                stopToken);
        if (usage.hasValue()
            && CodexAutomaticContinuationPolicy::
                   hasAvailableUsage(
                       usage.value())) {
            return profile;
        }
    }
    return std::nullopt;
}

} // namespace companion
