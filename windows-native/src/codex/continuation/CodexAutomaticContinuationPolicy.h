#pragma once

#include "codex/accounts/CodexAccountProfile.h"
#include "codex/models/BridgeModels.h"

#include <QString>
#include <QUuid>
#include <QVector>

#include <optional>

namespace companion {

class CodexAutomaticContinuationPolicy final {
public:
    static bool hasConfirmedExhaustion(
        const BridgeUsageSnapshot&
            snapshot);
    static bool hasAvailableUsage(
        const BridgeUsageSnapshot&
            snapshot);

    static QVector<CodexAccountProfile>
    orderedTargets(
        const QVector<CodexAccountProfile>&
            profiles,
        std::optional<QUuid>
            afterProfileId);

    static QString taskEventKey(
        QString threadId,
        QString turnId);
    static QString goalEventKey(
        QString threadId,
        QString goalIdentity,
        qint64 goalUpdatedAt);
    static QString clientMessageId(
        QStringView prefix,
        QStringView eventKey);
};

} // namespace companion
