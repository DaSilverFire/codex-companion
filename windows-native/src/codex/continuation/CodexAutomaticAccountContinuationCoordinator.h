#pragma once

#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/continuation/CodexAutomaticContinuationJournal.h"
#include "codex/continuation/CodexContinuationTransport.h"
#include "codex/models/ThreadRuntimeStatus.h"

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QUuid>
#include <QVector>

#include <functional>
#include <optional>
#include <stop_token>

namespace companion {

struct CodexAutomaticContinuationCandidate final {
    QString threadId;
    QString rolloutPath;
    ThreadRuntimeStatus runtimeStatus =
        ThreadRuntimeStatus::NotLoaded;

    friend bool operator==(
        const CodexAutomaticContinuationCandidate&,
        const CodexAutomaticContinuationCandidate&) =
        default;
};

struct CodexAutomaticContinuationOutcome final {
    QString threadId;
    QUuid originProfileId;
    QUuid destinationProfileId;
    QString destinationLabel;
    QString eventKey;
    QDateTime completedAt;

    friend bool operator==(
        const CodexAutomaticContinuationOutcome&,
        const CodexAutomaticContinuationOutcome&) =
        default;
};

class CodexAutomaticAccountContinuationCoordinator final {
public:
    using Clock =
        std::function<QDateTime()>;

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
        Clock clock = {});

    QVector<
        CodexAutomaticContinuationOutcome>
    continueEligible(
        bool enabled,
        const QVector<
            CodexAutomaticContinuationCandidate>&
            candidates,
        std::stop_token stopToken = {});

    std::optional<
        CodexAutomaticContinuationAttempt>
    latestCompleted(
        QStringView threadId) const;

private:
    std::optional<
        CodexAutomaticContinuationOutcome>
    tryContinue(
        CodexAutomaticContinuationCandidate
            candidate,
        std::stop_token stopToken);
    std::optional<CodexAccountProfile>
    findDestination(
        const QUuid& originProfileId,
        std::stop_token stopToken) const;

    CodexAccountProfileStore*
        profileStore_ = nullptr;
    CodexThreadAccountBindingStore*
        bindingStore_ = nullptr;
    CodexAccountRouter*
        router_ = nullptr;
    CodexContinuationCommands
        commands_;
    CodexAutomaticContinuationJournal*
        journal_ = nullptr;
    Clock clock_;
    QMutex gate_;
};

} // namespace companion
