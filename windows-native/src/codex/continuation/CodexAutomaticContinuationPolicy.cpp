#include "codex/continuation/CodexAutomaticContinuationPolicy.h"

#include <QCryptographicHash>

#include <algorithm>

namespace companion {
namespace {

const BridgeUsageGroup* codexGroup(
    const BridgeUsageSnapshot& snapshot)
{
    const auto iterator =
        std::find_if(
            snapshot.groups.cbegin(),
            snapshot.groups.cend(),
            [](const BridgeUsageGroup&
                   group) {
                return group.id
                    == QStringLiteral(
                        "codex");
            });
    return iterator
            == snapshot.groups.cend()
        ? nullptr
        : &*iterator;
}

QVector<const BridgeUsageWindow*>
windows(const BridgeUsageGroup& group)
{
    QVector<const BridgeUsageWindow*>
        result;
    if (group.shortWindow.has_value()) {
        result.append(
            &*group.shortWindow);
    }
    if (group.weeklyWindow.has_value()) {
        result.append(
            &*group.weeklyWindow);
    }
    return result;
}

QString normalized(QString value)
{
    return value.trimmed();
}

} // namespace

bool CodexAutomaticContinuationPolicy::
    hasConfirmedExhaustion(
        const BridgeUsageSnapshot&
            snapshot)
{
    if (snapshot.rateLimitReachedType
            .has_value()
        && !snapshot.rateLimitReachedType
                ->trimmed()
                .isEmpty()) {
        return true;
    }
    const BridgeUsageGroup* group =
        codexGroup(snapshot);
    if (group == nullptr) {
        return false;
    }
    const auto limits =
        windows(*group);
    return std::any_of(
        limits.cbegin(),
        limits.cend(),
        [](const BridgeUsageWindow*
               window) {
            return window
                ->remainingPercent
                <= 0.0;
        });
}

bool CodexAutomaticContinuationPolicy::
    hasAvailableUsage(
        const BridgeUsageSnapshot&
            snapshot)
{
    if (snapshot.rateLimitReachedType
            .has_value()
        && !snapshot.rateLimitReachedType
                ->trimmed()
                .isEmpty()) {
        return false;
    }
    const BridgeUsageGroup* group =
        codexGroup(snapshot);
    if (group == nullptr) {
        return false;
    }
    const auto limits =
        windows(*group);
    return !limits.isEmpty()
        && std::all_of(
            limits.cbegin(),
            limits.cend(),
            [](const BridgeUsageWindow*
                   window) {
                return window
                    ->remainingPercent
                    > 0.0;
            });
}

QVector<CodexAccountProfile>
CodexAutomaticContinuationPolicy::
    orderedTargets(
        const QVector<CodexAccountProfile>&
            profiles,
        std::optional<QUuid>
            afterProfileId)
{
    QVector<CodexAccountProfile>
        result;
    if (profiles.isEmpty()) {
        return result;
    }

    qsizetype originIndex = -1;
    if (afterProfileId.has_value()) {
        for (qsizetype index = 0;
             index < profiles.size();
             ++index) {
            if (profiles.at(index).id
                == *afterProfileId) {
                originIndex = index;
                break;
            }
        }
    }
    result.reserve(
        profiles.size());
    for (qsizetype offset = 1;
         offset <= profiles.size();
         ++offset) {
        const qsizetype index =
            (originIndex + offset)
            % profiles.size();
        const CodexAccountProfile&
            profile =
                profiles.at(index);
        if (!afterProfileId.has_value()
            || profile.id
                != *afterProfileId) {
            result.append(profile);
        }
    }
    return result;
}

QString CodexAutomaticContinuationPolicy::
    taskEventKey(
        QString threadId,
        QString turnId)
{
    threadId =
        normalized(std::move(threadId));
    turnId =
        normalized(std::move(turnId));
    if (threadId.isEmpty()
        || turnId.isEmpty()) {
        return {};
    }
    return threadId + QLatin1Char('|')
        + turnId;
}

QString CodexAutomaticContinuationPolicy::
    goalEventKey(
        QString threadId,
        QString goalIdentity,
        qint64 goalUpdatedAt)
{
    threadId =
        normalized(std::move(threadId));
    goalIdentity =
        normalized(
            std::move(goalIdentity));
    if (threadId.isEmpty()
        || goalIdentity.isEmpty()) {
        return {};
    }
    return threadId + QLatin1Char('|')
        + goalIdentity
        + QLatin1Char('|')
        + QString::number(
            goalUpdatedAt);
}

QString CodexAutomaticContinuationPolicy::
    clientMessageId(
        QStringView prefix,
        QStringView eventKey)
{
    const QByteArray digest =
        QCryptographicHash::hash(
            eventKey.toString()
                .toUtf8(),
            QCryptographicHash::
                Sha256);
    return prefix.toString()
        + QString::fromLatin1(
            digest.left(12)
                .toHex());
}

} // namespace companion
