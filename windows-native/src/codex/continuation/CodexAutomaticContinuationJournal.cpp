#include "codex/continuation/CodexAutomaticContinuationJournal.h"

#include "codex/accounts/CodexAccountProfile.h"

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
constexpr qsizetype kMaximumAttempts =
    120;
constexpr qint64 kMaximumStoreBytes =
    4 * 1024 * 1024;

CompanionError journalError(
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

QJsonObject attemptJson(
    const CodexAutomaticContinuationAttempt&
        attempt)
{
    QJsonObject object{
        {
            QStringLiteral("eventKey"),
            attempt.eventKey,
        },
        {
            QStringLiteral("threadId"),
            attempt.threadId,
        },
        {
            QStringLiteral(
                "originProfileId"),
            codexAccountProfileIdString(
                attempt.originProfileId),
        },
        {
            QStringLiteral(
                "destinationProfileId"),
            codexAccountProfileIdString(
                attempt.destinationProfileId),
        },
        {
            QStringLiteral(
                "destinationLabel"),
            attempt.destinationLabel,
        },
        {
            QStringLiteral(
                "clientMessageId"),
            attempt.clientMessageId,
        },
        {
            QStringLiteral("startedAt"),
            static_cast<double>(
                attempt.startedAt
                    .toMSecsSinceEpoch()),
        },
    };
    if (attempt.completedAt
            .has_value()) {
        object.insert(
            QStringLiteral(
                "completedAt"),
            static_cast<double>(
                attempt.completedAt
                    ->toMSecsSinceEpoch()));
    }
    return object;
}

std::optional<
    CodexAutomaticContinuationAttempt>
parseAttempt(const QJsonValue& value)
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
    const auto originProfileId =
        parseCodexAccountProfileId(
            object.value(
                      QStringLiteral(
                          "originProfileId"))
                .toString());
    const auto destinationProfileId =
        parseCodexAccountProfileId(
            object.value(
                      QStringLiteral(
                          "destinationProfileId"))
                .toString());
    const QJsonValue startedValue =
        object.value(
            QStringLiteral("startedAt"));
    if (eventKey.isEmpty()
        || threadId.isEmpty()
        || destinationLabel.isEmpty()
        || clientMessageId.isEmpty()
        || !originProfileId.has_value()
        || !destinationProfileId
                .has_value()
        || !startedValue.isDouble()) {
        return std::nullopt;
    }
    const QDateTime startedAt =
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(
                startedValue.toDouble()),
            QTimeZone::UTC);
    if (!startedAt.isValid()) {
        return std::nullopt;
    }
    std::optional<QDateTime>
        completedAt;
    const QJsonValue completedValue =
        object.value(
            QStringLiteral("completedAt"));
    if (!completedValue.isUndefined()
        && !completedValue.isNull()) {
        if (!completedValue.isDouble()) {
            return std::nullopt;
        }
        QDateTime completed =
            QDateTime::
                fromMSecsSinceEpoch(
                    static_cast<qint64>(
                        completedValue
                            .toDouble()),
                    QTimeZone::UTC);
        if (!completed.isValid()) {
            return std::nullopt;
        }
        completedAt =
            std::move(completed);
    }
    return CodexAutomaticContinuationAttempt{
        eventKey,
        threadId,
        *originProfileId,
        *destinationProfileId,
        destinationLabel,
        clientMessageId,
        startedAt,
        completedAt,
    };
}

} // namespace

CodexAutomaticContinuationJournal::
    CodexAutomaticContinuationJournal(
        QString filePath)
    : filePath_(
          QFileInfo(std::move(filePath))
              .absoluteFilePath())
{
    load();
}

std::optional<CompanionError>
CodexAutomaticContinuationJournal::
    loadError() const
{
    QMutexLocker locker(&mutex_);
    return loadError_;
}

std::optional<
    CodexAutomaticContinuationAttempt>
CodexAutomaticContinuationJournal::
    attempt(
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
            attempts_.cbegin(),
            attempts_.cend(),
            [&normalizedKey](
                const CodexAutomaticContinuationAttempt&
                    candidate) {
                return candidate.eventKey
                    == normalizedKey;
            });
    return iterator
            == attempts_.cend()
        ? std::nullopt
        : std::optional<
              CodexAutomaticContinuationAttempt>(
              *iterator);
}

std::optional<
    CodexAutomaticContinuationAttempt>
CodexAutomaticContinuationJournal::
    latestPending(
        QStringView threadId) const
{
    const QString normalizedThread =
        threadId.toString().trimmed();
    if (normalizedThread.isEmpty()) {
        return std::nullopt;
    }
    QMutexLocker locker(&mutex_);
    std::optional<
        CodexAutomaticContinuationAttempt>
        latest;
    for (const auto& candidate :
         attempts_) {
        if (candidate.threadId
                != normalizedThread
            || candidate.completedAt
                   .has_value()) {
            continue;
        }
        if (!latest.has_value()
            || candidate.startedAt
                > latest->startedAt) {
            latest = candidate;
        }
    }
    return latest;
}

std::optional<
    CodexAutomaticContinuationAttempt>
CodexAutomaticContinuationJournal::
    latestCompleted(
        QStringView threadId) const
{
    const QString normalizedThread =
        threadId.toString().trimmed();
    if (normalizedThread.isEmpty()) {
        return std::nullopt;
    }
    QMutexLocker locker(&mutex_);
    std::optional<
        CodexAutomaticContinuationAttempt>
        latest;
    for (const auto& candidate :
         attempts_) {
        if (candidate.threadId
                != normalizedThread
            || !candidate.completedAt
                    .has_value()) {
            continue;
        }
        if (!latest.has_value()
            || *candidate.completedAt
                > *latest->completedAt) {
            latest = candidate;
        }
    }
    return latest;
}

bool CodexAutomaticContinuationJournal::
    hasRecentCompleted(
        QStringView threadId,
        const QDateTime& after) const
{
    const auto latest =
        latestCompleted(threadId);
    return latest.has_value()
        && latest->completedAt
               .has_value()
        && *latest->completedAt
            >= after;
}

qsizetype
CodexAutomaticContinuationJournal::
    size() const noexcept
{
    QMutexLocker locker(&mutex_);
    return attempts_.size();
}

Result<
    CodexAutomaticContinuationAttempt>
CodexAutomaticContinuationJournal::plan(
    QString eventKey,
    QString threadId,
    const QUuid& originProfileId,
    const QUuid& destinationProfileId,
    QString destinationLabel,
    QString clientMessageId,
    QDateTime startedAt)
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
        || originProfileId.isNull()
        || destinationProfileId
               .isNull()
        || destinationLabel.isEmpty()
        || clientMessageId.isEmpty()
        || !startedAt.isValid()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                journalError(
                    QStringLiteral(
                        "codex.continuation_plan_invalid"),
                    QStringLiteral(
                        "The Codex continuation plan is invalid."),
                    filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(*loadError_);
    }
    const auto existing =
        std::find_if(
            attempts_.cbegin(),
            attempts_.cend(),
            [&eventKey](
                const CodexAutomaticContinuationAttempt&
                    candidate) {
                return candidate.eventKey
                    == eventKey;
            });
    if (existing
        != attempts_.cend()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            success(*existing);
    }

    QVector<
        CodexAutomaticContinuationAttempt>
        candidate = attempts_;
    candidate.append({
        eventKey,
        threadId,
        originProfileId,
        destinationProfileId,
        destinationLabel,
        clientMessageId,
        startedAt.toUTC(),
        std::nullopt,
    });
    trim(candidate);
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                persisted.error());
    }
    attempts_ =
        std::move(candidate);
    loadError_.reset();
    const auto stored =
        std::find_if(
            attempts_.cbegin(),
            attempts_.cend(),
            [&eventKey](
                const CodexAutomaticContinuationAttempt&
                    attempt) {
                return attempt.eventKey
                    == eventKey;
            });
    if (stored == attempts_.cend()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                journalError(
                    QStringLiteral(
                        "codex.continuation_plan_trimmed"),
                    QStringLiteral(
                        "The Codex continuation plan could not be retained."),
                    filePath_));
    }
    return Result<
        CodexAutomaticContinuationAttempt>::
        success(*stored);
}

Result<
    CodexAutomaticContinuationAttempt>
CodexAutomaticContinuationJournal::
    complete(
        QString eventKey,
        QDateTime completedAt)
{
    eventKey =
        normalized(std::move(eventKey));
    if (eventKey.isEmpty()
        || !completedAt.isValid()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                journalError(
                    QStringLiteral(
                        "codex.continuation_completion_invalid"),
                    QStringLiteral(
                        "The Codex continuation completion is invalid."),
                    filePath_));
    }

    QMutexLocker locker(&mutex_);
    if (loadError_.has_value()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(*loadError_);
    }
    QVector<
        CodexAutomaticContinuationAttempt>
        candidate = attempts_;
    const auto iterator =
        std::find_if(
            candidate.begin(),
            candidate.end(),
            [&eventKey](
                const CodexAutomaticContinuationAttempt&
                    attempt) {
                return attempt.eventKey
                    == eventKey;
            });
    if (iterator == candidate.end()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                journalError(
                    QStringLiteral(
                        "codex.continuation_not_planned"),
                    QStringLiteral(
                        "The Codex continuation was not planned."),
                    filePath_));
    }
    iterator->completedAt =
        completedAt.toUTC();
    const auto completed =
        *iterator;
    const auto persisted =
        persist(candidate);
    if (!persisted.hasValue()) {
        return Result<
            CodexAutomaticContinuationAttempt>::
            failure(
                persisted.error());
    }
    attempts_ =
        std::move(candidate);
    return Result<
        CodexAutomaticContinuationAttempt>::
        success(completed);
}

void CodexAutomaticContinuationJournal::
    load()
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
            journalError(
                QStringLiteral(
                    "codex.continuation_journal_corrupt"),
                QStringLiteral(
                    "The Codex continuation journal could not be read."),
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
            journalError(
                QStringLiteral(
                    "codex.continuation_journal_corrupt"),
                QStringLiteral(
                    "The Codex continuation journal is malformed."),
                filePath_);
        return;
    }
    const QJsonObject root =
        document.object();
    const QJsonValue version =
        root.value(
            QStringLiteral("version"));
    const QJsonValue attemptsValue =
        root.value(
            QStringLiteral("attempts"));
    if (!version.isDouble()
        || version.toInteger(-1)
            != kJournalVersion
        || !attemptsValue.isArray()) {
        loadError_ =
            journalError(
                QStringLiteral(
                    "codex.continuation_journal_corrupt"),
                QStringLiteral(
                    "The Codex continuation journal version is invalid."),
                filePath_);
        return;
    }
    QVector<
        CodexAutomaticContinuationAttempt>
        loaded;
    for (const QJsonValue& value :
         attemptsValue.toArray()) {
        const auto parsed =
            parseAttempt(value);
        if (!parsed.has_value()) {
            loadError_ =
                journalError(
                    QStringLiteral(
                        "codex.continuation_journal_corrupt"),
                    QStringLiteral(
                        "A Codex continuation journal entry is invalid."),
                    filePath_);
            return;
        }
        const bool duplicate =
            std::any_of(
                loaded.cbegin(),
                loaded.cend(),
                [&parsed](
                    const CodexAutomaticContinuationAttempt&
                        attempt) {
                    return attempt.eventKey
                        == parsed->eventKey;
                });
        if (!duplicate) {
            loaded.append(*parsed);
        }
    }
    trim(loaded);
    attempts_ =
        std::move(loaded);
    loadError_.reset();
}

Result<void>
CodexAutomaticContinuationJournal::
    persist(
        const QVector<
            CodexAutomaticContinuationAttempt>&
            attempts) const
{
    const QFileInfo information(
        filePath_);
    if (!QDir().mkpath(
            information
                .absolutePath())) {
        return Result<void>::failure(
            journalError(
                QStringLiteral(
                    "codex.continuation_journal_write_failed"),
                QStringLiteral(
                    "The Codex continuation journal directory could not be created."),
                filePath_));
    }
    QJsonArray rows;
    for (const auto& attempt :
         attempts) {
        rows.append(
            attemptJson(attempt));
    }
    const QByteArray contents =
        QJsonDocument(
            QJsonObject{
                {
                    QStringLiteral("version"),
                    kJournalVersion,
                },
                {
                    QStringLiteral("attempts"),
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
            journalError(
                QStringLiteral(
                    "codex.continuation_journal_write_failed"),
                QStringLiteral(
                    "The Codex continuation journal could not be written."),
                filePath_));
    }
    return Result<void>::success();
}

void CodexAutomaticContinuationJournal::
    trim(
        QVector<
            CodexAutomaticContinuationAttempt>&
            attempts)
{
    if (attempts.size()
        <= kMaximumAttempts) {
        return;
    }
    std::stable_sort(
        attempts.begin(),
        attempts.end(),
        [](
            const CodexAutomaticContinuationAttempt&
                left,
            const CodexAutomaticContinuationAttempt&
                right) {
            if (left.completedAt.has_value()
                != right.completedAt
                       .has_value()) {
                return !left.completedAt
                            .has_value();
            }
            const QDateTime leftDate =
                left.completedAt
                    .value_or(
                        left.startedAt);
            const QDateTime rightDate =
                right.completedAt
                    .value_or(
                        right.startedAt);
            return leftDate > rightDate;
        });
    attempts.resize(
        kMaximumAttempts);
    std::stable_sort(
        attempts.begin(),
        attempts.end(),
        [](
            const CodexAutomaticContinuationAttempt&
                left,
            const CodexAutomaticContinuationAttempt&
                right) {
            return left.startedAt
                < right.startedAt;
        });
}

} // namespace companion
