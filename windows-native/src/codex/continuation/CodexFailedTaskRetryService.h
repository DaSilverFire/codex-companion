#pragma once

#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/continuation/CodexContinuationTransport.h"
#include "codex/models/BridgeModels.h"
#include "codex/models/ThreadRuntimeStatus.h"
#include "core/Result.h"

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QStringView>
#include <QUuid>
#include <QVector>

#include <optional>
#include <stop_token>

namespace companion {

enum class CodexFailedTaskRetryDisposition {
    Continued,
    AlreadyContinued,
    NotEligible,
    Failed,
};

struct CodexFailedTaskRetryRequest final {
    QString threadId;
    QString rolloutPath;
    ThreadRuntimeStatus runtimeStatus =
        ThreadRuntimeStatus::NotLoaded;
    qint64 taskUpdatedAtMilliseconds = 0;
    std::optional<BridgeGoal> goal;
};

struct CodexFailedTaskRetryResult final {
    CodexFailedTaskRetryDisposition
        disposition =
            CodexFailedTaskRetryDisposition::
                Failed;
    std::optional<QUuid>
        destinationProfileId;
    QString destinationLabel;
    QString message;
};

struct CodexFailedTaskRetryJournalEntry final {
    QString eventKey;
    QString threadId;
    std::optional<QUuid>
        destinationProfileId;
    QString destinationLabel;
    QString clientMessageId;
    QDateTime completedAt;

    friend bool operator==(
        const CodexFailedTaskRetryJournalEntry&,
        const CodexFailedTaskRetryJournalEntry&) =
        default;
};

class CodexFailedTaskRetryJournal final {
public:
    explicit CodexFailedTaskRetryJournal(
        QString filePath);

    std::optional<CompanionError>
    loadError() const;
    std::optional<
        CodexFailedTaskRetryJournalEntry>
    entry(QStringView eventKey) const;
    bool isCompleted(
        QStringView eventKey) const;
    qsizetype size() const noexcept;

    Result<
        CodexFailedTaskRetryJournalEntry>
    complete(
        QString eventKey,
        QString threadId,
        std::optional<QUuid>
            destinationProfileId,
        QString destinationLabel,
        QString clientMessageId,
        QDateTime completedAt);

private:
    void load();
    Result<void> persist(
        const QVector<
            CodexFailedTaskRetryJournalEntry>&
            entries) const;
    static void trim(
        QVector<
            CodexFailedTaskRetryJournalEntry>&
            entries);

    QString filePath_;
    mutable QMutex mutex_;
    QVector<
        CodexFailedTaskRetryJournalEntry>
        entries_;
    std::optional<CompanionError>
        loadError_;
};

class CodexFailedTaskRetryService final {
public:
    CodexFailedTaskRetryService(
        CodexAccountProfileStore&
            profileStore,
        CodexThreadAccountBindingStore&
            bindingStore,
        CodexAccountRouter& router,
        CodexContinuationCommands
            commands,
        CodexFailedTaskRetryJournal&
            journal);

    CodexFailedTaskRetryResult retry(
        CodexFailedTaskRetryRequest
            request,
        std::stop_token stopToken = {});

    static bool isEligible(
        const CodexFailedTaskRetryRequest&
            request) noexcept;

private:
    static QString eventKey(
        const CodexFailedTaskRetryRequest&
            request);

    CodexAccountProfileStore*
        profileStore_ = nullptr;
    CodexThreadAccountBindingStore*
        bindingStore_ = nullptr;
    CodexAccountRouter*
        router_ = nullptr;
    CodexContinuationCommands
        commands_;
    CodexFailedTaskRetryJournal*
        journal_ = nullptr;
    QMutex gate_;
};

} // namespace companion
