#pragma once

#include "core/Result.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringView>
#include <QVector>

#include <functional>
#include <optional>

namespace companion {

enum class RelayAuditValueKind {
    Text,
    Url,
    DeviceId,
};

struct RelayAuditField final {
    QString name;
    QString value;
    RelayAuditValueKind kind;

    friend bool operator==(
        const RelayAuditField&,
        const RelayAuditField&) = default;
};

class RelayAudit final {
public:
    using Clock = std::function<qint64()>;

    explicit RelayAudit(
        Clock clock = {});

    Result<std::optional<QString>> render(
        QStringView throttleKey,
        QStringView message,
        QVector<RelayAuditField> fields = {});

    qsizetype throttleKeyCount() const;

    static qsizetype unicodeCharacterCount(
        QStringView value);

private:
    struct ThrottleEntry final {
        qint64 emittedAtMilliseconds = 0;
        quint64 order = 0;
    };

    static QString sanitizeText(
        QStringView value);
    static QString truncateCharacters(
        QStringView value,
        qsizetype maximumCharacters);
    static Result<QString> renderedField(
        const RelayAuditField& field);

    Clock clock_;
    mutable QMutex mutex_;
    QHash<QString, ThrottleEntry>
        throttleEntries_;
    quint64 nextOrder_ = 0;
};

} // namespace companion
