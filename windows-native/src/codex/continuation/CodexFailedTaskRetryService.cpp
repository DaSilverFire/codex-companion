#include "codex/continuation/CodexFailedTaskRetryService.h"

#include "codex/accounts/CodexThreadAccountHandoffService.h"
#include "codex/continuation/CodexAutomaticContinuationPolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>
#include <utility>

namespace companion {
namespace {

constexpr int kJournalVersion = 1;
constexpr qsizetype kMaximumEntries =
    120;
constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;

CompanionError retryJournalError(
    QString code,
    QString message,
    const QString& path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {
                QStringLiteral("path"),
                path,
            },
        },
    };
}

QString normalized(QString value)
{
    return value.trimmed();
}

QString goalStatusText(
    const std::optional<BridgeGoal>& goal)
{
    if (!goal.has_value()) {
        return QStringLiteral("none");
    }
    switch (goal->status) {
    case GoalStatus::Active:
        return QStringLiteral("active");
    case GoalStatus::Paused:
        return QStringLiteral("paused");
    case GoalStatus::Blocked:
        return QStringLiteral("blocked");
    case GoalStatus::UsageLimited:
        return QStringLiteral(
            "usageLimited");
    case GoalStatus::BudgetLimited:
        return QStringLiteral(
            "budgetLimited");
    case GoalStatus::Complete:
        return QStringLiteral("complete");
    }
    return QStringLiteral("none");
}

QJsonObject entryJson(
    const CodexFailedTaskRetryJournalEntry&
        entry)
{
    QJsonObject object{
        {
            QStringLiteral("eventKey"),
            entry.eventKey,
        },
        {
            QStringLiteral("threadId"),
            entry.threadId,
        },
        {
            QStringLiteral(
                "destinationLabel"),
            entry.destinationLabel,
        },
        {
            QStringLiteral(
                "clientMessageId"),
            entry.clientMessageId,
        },
        {
            QStringLiteral(
                "completedAt"),
            static_cast<double>(
                entry.completedAt
                    .toMSecsSinceEpoch()),
        },
    };
    if (entry.destinationProfileId
            .has_value()) {
        object.insert(
            QStringLiteral(
                "destinationProfileId"),
            codexAccountProfileIdString(
                *entry.destinationProfileId));
    }
    return object;
}

std::optional<
    CodexFailedTaskRetryJournalEntry>
parseEntry(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object =
        value.toObject();
    const QString eventKey =
        normalized(
            object.value(
                      QStringLiteral(
                          "eventKey"))
                .toString());
    const QString threadId =
        normalized(
            object.value(
                      QStringLiteral(
                          "threadId"))
                .toString());
    const QString destinationLabel =
        normalized(
            object.value(
                      QStringLiteral(
                          "destinationLabel"))
                .toString());
    const QString clientMessageId =
        normalized(
            object.value(
                      QStringLiteral(
                          "clientMessageId"))
                .toString());
    const QJsonValue completedValue =
        object.value(
            QStringLiteral("completedAt"));
    if (eventKey.isEmpty()
        || threadId.isEmpty()
        || destinationLabel.isEmpty()
        || clientMessageId.isEmpty()
        || !completedValue.isDouble()) {
        return std::nullopt;
    }
    std::optional<QUuid>
        destinationProfileId;
    const QJsonValue profileValue =
        object.value(
            QStringLiteral(
                "destinationProfileId"));
    if (!profileValue.isUndefined()
        && !profileValue.isNull()) {
        if (!profileValue.isString()) {
            return std::nullopt;
        }
        destinationProfileId =
            parseCodexAccountProfileId(
                profileValue.toString());
        if (!destinationProfileId
                 .has_value()) {
            return std::nullopt;
        }
    }
    const QDateTime completedAt =
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(
                completedValue.toDouble()),
            QTimeZone::UTC);
    if (!completedAt.isValid()) {
        return std::nullopt;
    }
    return CodexFailedTaskRetryJournalEntry{
        eventKey,
        threadId,
        destinationProfileId,
        destinationLabel,
        clientMessageId,
        completedAt,
    };
}

CodexFailedTaskRetryResult
failedResult(QString message)
{
    return {
        CodexFailedTaskRetryDisposition::
            Failed,
        std::nullopt,
        {},
        std::move(message),
    };
}

} // namespace

CodexFailedTaskRetryJournal::
    CodexFailedTaskRetryJournal(
        QString filePath)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath())
{
    load();
}

std::optional<CompanionError>
CodexFailedTaskRetryJournal::
    loadError() const
{
    QMutexLocker locker(&mutex_);
    return loadError_;
}

std::optional<
    CodexFailedTaskRetryJournalEntry>
CodexFailedTaskRetryJournal::entry(
    QStringView eventKey) const
{
    const QString normalizedKey =
        eventKey.toString().trimmed();
    if (normalizedKey.isEmpty()) {
        return std::nullopt;
    }
    QMutexLocker locker(&mutex_);
    const auto iterator =
        std::find_if(
            entries_.cbegin(),
            entries_.cend(),
            [&normalizedKey](
                const CodexFailedTaskRetryJournalEntry&
                    candidate) {
                return candidate.eventKey
                    == normalizedKey;
            });
    return iterator
            == entries_.cend()
        ? std::nullopt
        : std::optional<
              CodexFailedTaskRetryJournalEntry>(
              *iterator);
}

bool CodexFailedTaskRetryJournal::
    isCompleted(
        QStringView eventKey) const
{
    return entry(eventKey)
        .has_value();
}

qsizetype
CodexFailedTaskRetryJournal::
    size() const noexcept
{
    QMutexLocker locker(&mutex_);
    return entries_.size();
}

Result<
    CodexFailedTaskRetryJournalEntry>
CodexFailedTaskRetryJournal::complete(
    QString eventKey,
    QString threadId,
    std::optional<QUuid>
        destinationProfileId,
    QString destinationLabel,
    QString clientMessageId,
    QDateTime completedAt)
{
    eventKey =
        normalized(std::move(eventKey));
    threadId =
        normalized(std::move(threadId));
    destinationLabel =
        normalized(
            std::move(destinationLabel));
    clientMessageId =
        normalized(
            std::move(clientMessageId));
    if (eventKey.isEmpty()
        || threadId.isEmpty()
        || destinationLabel.isEmpty()
        || clientMessageId.isEmpty()
        || !completedAt.isValid()
        || (destinationProfileId
                .has_value()
            && destinationProfileId
                   ->isNull())) {
        return Result<
            CodexFailedTaskRetryJournalEntry>::
            failure(
                retryJournalError(
                    QStringLiteral(
                        "codex.failed_retry_journal_invalid"),
                    QStringLiteral(
                        "The failed-task retry journal entry is invalid."),
                    filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<
            CodexFailedTaskRetryJournalEntry>::
            failure(*loadError_);
    }
    const auto existing =
        std::find_if(
            entries_.cbegin(),
            entries_.cend(),
            [&eventKey](
                const CodexFailedTaskRetryJournalEntry&
                    entry) {
                return entry.eventKey
                    == eventKey;
            });
    if (existing != entries_.cend()) {
        return Result<
            CodexFailedTaskRetryJournalEntry>::
            success(*existing);
    }

    QVector<
        CodexFailedTaskRetryJournalEntry>
        candidate = entries_;
    CodexFailedTaskRetryJournalEntry
        completed{
            eventKey,
            threadId,
            destinationProfileId,
            destinationLabel,
            clientMessageId,
            completedAt.toUTC(),
        };
    candidate.append(completed);
    trim(candidate);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return Result<
            CodexFailedTaskRetryJournalEntry>::
            failure(
                persisted.error());
    }
    entries_ =
        std::move(candidate);
    loadError_.reset();
    return Result<
        CodexFailedTaskRetryJournalEntry>::
        success(
            std::move(completed));
}

void CodexFailedTaskRetryJournal::load()
{
    const QFileInfo information(
        filePath_);
    if (!information.exists()) {
        return;
    }
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)
        || file.size() < 0
        || file.size()
            > kMaximumStoreBytes) {
        loadError_ =
            retryJournalError(
                QStringLiteral(
                    "codex.failed_retry_journal_corrupt"),
                QStringLiteral(
                    "The failed-task retry journal could not be read."),
                filePath_);
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
        loadError_ =
            retryJournalError(
                QStringLiteral(
                    "codex.failed_retry_journal_corrupt"),
                QStringLiteral(
                    "The failed-task retry journal is malformed."),
                filePath_);
        return;
    }
    const QJsonObject root =
        document.object();
    const QJsonValue version =
        root.value(
            QStringLiteral("version"));
    const QJsonValue entriesValue =
        root.value(
            QStringLiteral("entries"));
    if (!version.isDouble()
        || version.toInteger(-1)
            != kJournalVersion
        || !entriesValue.isArray()) {
        loadError_ =
            retryJournalError(
                QStringLiteral(
                    "codex.failed_retry_journal_corrupt"),
                QStringLiteral(
                    "The failed-task retry journal version is invalid."),
                filePath_);
        return;
    }
    QVector<
        CodexFailedTaskRetryJournalEntry>
        loaded;
    for (const QJsonValue& value :
         entriesValue.toArray()) {
        const auto parsed =
            parseEntry(value);
        if (!parsed.has_value()) {
            loadError_ =
                retryJournalError(
                    QStringLiteral(
                        "codex.failed_retry_journal_corrupt"),
                    QStringLiteral(
                        "A failed-task retry journal entry is invalid."),
                    filePath_);
            return;
        }
        const bool duplicate =
            std::any_of(
                loaded.cbegin(),
                loaded.cend(),
                [&parsed](
                    const CodexFailedTaskRetryJournalEntry&
                        entry) {
                    return entry.eventKey
                        == parsed->eventKey;
                });
        if (!duplicate) {
            loaded.append(*parsed);
        }
    }
    trim(loaded);
    entries_ =
        std::move(loaded);
    loadError_.reset();
}

Result<void>
CodexFailedTaskRetryJournal::persist(
    const QVector<
        CodexFailedTaskRetryJournalEntry>&
        entries) const
{
    const QFileInfo information(
        filePath_);
    if (!QDir().mkpath(
            information
                .absolutePath())) {
        return Result<void>::failure(
            retryJournalError(
                QStringLiteral(
                    "codex.failed_retry_journal_write_failed"),
                QStringLiteral(
                    "The failed-task retry journal directory could not be created."),
                filePath_));
    }
    QJsonArray rows;
    for (const auto& entry :
         entries) {
        rows.append(
            entryJson(entry));
    }
    const QByteArray contents =
        QJsonDocument(
            QJsonObject{
                {
                    QStringLiteral("version"),
                    kJournalVersion,
                },
                {
                    QStringLiteral("entries"),
                    rows,
                },
            })
            .toJson(
                QJsonDocument::Indented);
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents)
            != contents.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            retryJournalError(
                QStringLiteral(
                    "codex.failed_retry_journal_write_failed"),
                QStringLiteral(
                    "The failed-task retry journal could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

void CodexFailedTaskRetryJournal::trim(
    QVector<
        CodexFailedTaskRetryJournalEntry>&
        entries)
{
    if (entries.size()
        <= kMaximumEntries) {
        return;
    }
    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](
            const CodexFailedTaskRetryJournalEntry&
                left,
            const CodexFailedTaskRetryJournalEntry&
                right) {
            return left.completedAt
                > right.completedAt;
        });
    entries.resize(
        kMaximumEntries);
    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](
            const CodexFailedTaskRetryJournalEntry&
                left,
            const CodexFailedTaskRetryJournalEntry&
                right) {
            return left.completedAt
                < right.completedAt;
        });
}

CodexFailedTaskRetryService::
    CodexFailedTaskRetryService(
        CodexAccountProfileStore&
            profileStore,
        CodexThreadAccountBindingStore&
            bindingStore,
        CodexAccountRouter& router,
        CodexContinuationCommands
            commands,
        CodexFailedTaskRetryJournal&
            journal)
    : profileStore_(&profileStore),
      bindingStore_(&bindingStore),
      router_(&router),
      commands_(std::move(commands)),
      journal_(&journal)
{
}

CodexFailedTaskRetryResult
CodexFailedTaskRetryService::retry(
    CodexFailedTaskRetryRequest
        request,
    std::stop_token stopToken)
{
    request.threadId =
        request.threadId.trimmed();
    request.rolloutPath =
        request.rolloutPath.trimmed();
    if (!isEligible(request)) {
        return {
            CodexFailedTaskRetryDisposition::
                NotEligible,
            std::nullopt,
            {},
            QStringLiteral(
                "Retry is available only after a failed task safely stops."),
        };
    }
    if (stopToken.stop_requested()) {
        return failedResult(
            QStringLiteral(
                "The Codex retry was canceled."));
    }

    QMutexLocker locker(&gate_);
    if (profileStore_ == nullptr
        || bindingStore_ == nullptr
        || router_ == nullptr
        || journal_ == nullptr
        || !commands_.send) {
        return failedResult(
            QStringLiteral(
                "The Codex retry service is unavailable."));
    }
    if (const auto error =
            journal_->loadError();
        error.has_value()) {
        return failedResult(
            error->message);
    }

    const QString retryEventKey =
        eventKey(request);
    const auto completed =
        journal_->entry(
            retryEventKey);
    if (completed.has_value()) {
        return {
            CodexFailedTaskRetryDisposition::
                AlreadyContinued,
            completed
                ->destinationProfileId,
            completed
                ->destinationLabel,
            {},
        };
    }

    const auto selectedProfile =
        profileStore_->
            selectedProfile();
    const auto boundProfileId =
        bindingStore_->profileIdFor(
            request.threadId);
    const CodexAccountRoute
        destinationRoute =
            selectedProfile
                .has_value()
        ? router_->routeProfile(
              selectedProfile->id)
        : router_->routeThread(
              request.threadId);
    QString destinationLabel =
        QStringLiteral("current account");
    if (selectedProfile.has_value()) {
        destinationLabel =
            selectedProfile->label;
    } else if (boundProfileId
                   .has_value()) {
        const auto boundProfile =
            profileStore_->profile(
                *boundProfileId);
        if (boundProfile.has_value()) {
            destinationLabel =
                boundProfile->label;
        }
    }

    if (selectedProfile.has_value()
        && boundProfileId
            != std::optional<QUuid>(
                selectedProfile->id)) {
        if (request.rolloutPath
                .isEmpty()) {
            return {
                CodexFailedTaskRetryDisposition::
                    NotEligible,
                destinationRoute.profileId,
                destinationLabel,
                QStringLiteral(
                    "The stopped task has no rollout path to resume under the selected account."),
            };
        }
        if (!commands_.handoff) {
            return failedResult(
                QStringLiteral(
                    "The Codex account handoff service is unavailable."));
        }
        const auto handedOff =
            commands_.handoff(
                request.threadId,
                request.rolloutPath,
                request.runtimeStatus,
                selectedProfile->id,
                stopToken);
        if (!handedOff.hasValue()) {
            return failedResult(
                handedOff.error()
                    .message);
        }
    }

    if (request.goal.has_value()
        && (request.goal->status
                == GoalStatus::Paused
            || request.goal->status
                == GoalStatus::Blocked
            || request.goal->status
                == GoalStatus::
                    UsageLimited)) {
        if (!commands_.activateGoal) {
            return failedResult(
                QStringLiteral(
                    "The Codex goal service is unavailable."));
        }
        const auto activated =
            commands_.activateGoal(
                destinationRoute,
                request.threadId,
                stopToken);
        if (!activated.hasValue()) {
            return failedResult(
                activated.error()
                    .message);
        }
    }

    const QString clientMessageId =
        CodexAutomaticContinuationPolicy::
            clientMessageId(
                QStringLiteral(
                    "codex-companion-retry-"),
                retryEventKey);
    const auto sent =
        commands_.send(
            destinationRoute,
            request.threadId,
            QStringLiteral("continue"),
            clientMessageId,
            stopToken);
    if (!sent.hasValue()) {
        return failedResult(
            sent.error().message);
    }
    const auto saved =
        journal_->complete(
            retryEventKey,
            request.threadId,
            destinationRoute.profileId,
            destinationLabel,
            clientMessageId,
            QDateTime::
                currentDateTimeUtc());
    if (!saved.hasValue()) {
        return failedResult(
            saved.error().message);
    }
    return {
        CodexFailedTaskRetryDisposition::
            Continued,
        destinationRoute.profileId,
        destinationLabel,
        {},
    };
}

bool CodexFailedTaskRetryService::
    isEligible(
        const CodexFailedTaskRetryRequest&
            request) noexcept
{
    if (request.threadId
            .trimmed()
            .isEmpty()
        || !CodexThreadAccountHandoffService::
                canHandoff(
                    request
                        .runtimeStatus)) {
        return false;
    }
    return !request.goal.has_value()
        || (request.goal->status
                != GoalStatus::Complete
            && request.goal->status
                != GoalStatus::
                    BudgetLimited);
}

QString CodexFailedTaskRetryService::
    eventKey(
        const CodexFailedTaskRetryRequest&
            request)
{
    return QStringLiteral(
               "failed-task-retry:")
        + request.threadId.trimmed()
        + QLatin1Char(':')
        + QString::number(
            request
                .taskUpdatedAtMilliseconds)
        + QLatin1Char(':')
        + QString::number(
            request.goal.has_value()
                ? request.goal->updatedAt
                : 0)
        + QLatin1Char(':')
        + goalStatusText(request.goal);
}

} // namespace companion
