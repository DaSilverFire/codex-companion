#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QStringView>
#include <QUuid>
#include <QVector>

#include <optional>

namespace companion {

struct CodexAutomaticContinuationAttempt final {
    QString eventKey;
    QString threadId;
    QUuid originProfileId;
    QUuid destinationProfileId;
    QString destinationLabel;
    QString clientMessageId;
    QDateTime startedAt;
    std::optional<QDateTime> completedAt;

    friend bool operator==(
        const CodexAutomaticContinuationAttempt&,
        const CodexAutomaticContinuationAttempt&) =
        default;
};

class CodexAutomaticContinuationJournal final {
public:
    explicit CodexAutomaticContinuationJournal(
        QString filePath);

    std::optional<CompanionError>
    loadError() const;
    std::optional<
        CodexAutomaticContinuationAttempt>
    attempt(QStringView eventKey) const;
    std::optional<
        CodexAutomaticContinuationAttempt>
    latestPending(
        QStringView threadId) const;
    std::optional<
        CodexAutomaticContinuationAttempt>
    latestCompleted(
        QStringView threadId) const;
    bool hasRecentCompleted(
        QStringView threadId,
        const QDateTime& after) const;
    qsizetype size() const noexcept;

    Result<
        CodexAutomaticContinuationAttempt>
    plan(
        QString eventKey,
        QString threadId,
        const QUuid& originProfileId,
        const QUuid&
            destinationProfileId,
        QString destinationLabel,
        QString clientMessageId,
        QDateTime startedAt);
    Result<
        CodexAutomaticContinuationAttempt>
    complete(
        QString eventKey,
        QDateTime completedAt);

private:
    void load();
    Result<void> persist(
        const QVector<
            CodexAutomaticContinuationAttempt>&
            attempts) const;
    static void trim(
        QVector<
            CodexAutomaticContinuationAttempt>&
            attempts);

    QString filePath_;
    mutable QMutex mutex_;
    QVector<
        CodexAutomaticContinuationAttempt>
        attempts_;
    std::optional<CompanionError>
        loadError_;
};

} // namespace companion
