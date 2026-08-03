#include "mobile/relay/RelayAudit.h"

#include <algorithm>
#include <chrono>

#include <QCryptographicHash>
#include <QMutexLocker>
#include <QSet>
#include <QUrl>

namespace companion {
namespace {

constexpr qint64 kThrottleMilliseconds =
    60'000;
constexpr qsizetype kMaximumThrottleKeys =
    64;
constexpr qsizetype kMaximumAuditCharacters =
    240;

CompanionError auditError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

QString urlOrigin(const QString& value)
{
    const QUrl url(value, QUrl::StrictMode);
    const QString scheme =
        url.scheme().toLower();
    QString host = url.host().toLower();
    if (!url.isValid()
        || scheme.isEmpty()
        || host.isEmpty()) {
        return QStringLiteral("<invalid-url>");
    }
    if (host.contains(QLatin1Char(':'))) {
        host = QLatin1Char('[')
            + host
            + QLatin1Char(']');
    }
    return scheme
        + QStringLiteral("://")
        + host;
}

QString normalizedFieldName(
    QStringView name)
{
    QString normalized;
    normalized.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber()) {
            normalized.append(
                character.toLower());
        }
    }
    return normalized;
}

bool isSafeTextValue(QStringView value)
{
    if (value.isEmpty()
        || value.size() > 64) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code =
            character.unicode();
        const bool allowed =
            (code >= 'A' && code <= 'Z')
            || (code >= 'a'
                && code <= 'z')
            || (code >= '0'
                && code <= '9')
            || code == '_'
            || code == '-'
            || code == '.'
            || code == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

CompanionError sensitiveAuditField()
{
    return auditError(
        QStringLiteral(
            "relay.audit_sensitive_field"),
        QStringLiteral(
            "Relay audit fields must use an approved redaction class."));
}

} // namespace

RelayAudit::RelayAudit(Clock clock)
    : clock_(
              clock
              ? std::move(clock)
              : Clock([]() {
                    return qint64(
                        std::chrono::
                            duration_cast<
                                std::chrono::
                                    milliseconds>(
                                std::chrono::
                                    steady_clock::now()
                                        .time_since_epoch())
                                .count());
                }))
{
}

Result<std::optional<QString>>
RelayAudit::render(
    QStringView throttleKey,
    QStringView message,
    QVector<RelayAuditField> fields)
{
    const QString key =
        throttleKey.toString();
    if (key.isEmpty()) {
        return Result<std::optional<QString>>
            ::failure(auditError(
                QStringLiteral(
                    "relay.audit_invalid_key"),
                QStringLiteral(
                    "Relay audit throttle key must not be empty.")));
    }

    QString rendered =
        sanitizeText(message);
    for (const RelayAuditField& field :
         fields) {
        const auto fieldText =
            renderedField(field);
        if (!fieldText.hasValue()) {
            return Result<std::optional<QString>>
                ::failure(fieldText.error());
        }
        if (!rendered.isEmpty()) {
            rendered.append(QLatin1Char(' '));
        }
        rendered.append(fieldText.value());
    }
    rendered = truncateCharacters(
        sanitizeText(rendered),
        kMaximumAuditCharacters);

    const qint64 now = clock_();
    QMutexLocker locker(&mutex_);
    const auto existing =
        throttleEntries_.constFind(key);
    if (existing
        != throttleEntries_.constEnd()) {
        const qint64 emittedAt =
            existing->emittedAtMilliseconds;
        if (now < emittedAt
            || now - emittedAt
                < kThrottleMilliseconds) {
            return Result<
                std::optional<QString>>
                ::success(std::nullopt);
        }
    }

    throttleEntries_.insert(
        key,
        {now, nextOrder_++});
    while (throttleEntries_.size()
           > kMaximumThrottleKeys) {
        auto oldest =
            throttleEntries_.begin();
        for (auto iterator =
                 throttleEntries_.begin();
             iterator
             != throttleEntries_.end();
             ++iterator) {
            if (iterator->order
                < oldest->order) {
                oldest = iterator;
            }
        }
        throttleEntries_.erase(oldest);
    }
    return Result<std::optional<QString>>
        ::success(std::move(rendered));
}

qsizetype RelayAudit::throttleKeyCount()
    const
{
    QMutexLocker locker(&mutex_);
    return throttleEntries_.size();
}

qsizetype RelayAudit::unicodeCharacterCount(
    QStringView value)
{
    qsizetype count = 0;
    for (qsizetype index = 0;
         index < value.size();
         ++index) {
        const QChar current =
            value.at(index);
        if (current.isHighSurrogate()
            && index + 1 < value.size()
            && value.at(index + 1)
                   .isLowSurrogate()) {
            ++index;
        }
        ++count;
    }
    return count;
}

QString RelayAudit::sanitizeText(
    QStringView value)
{
    QString output;
    output.reserve(value.size());
    bool pendingSpace = false;
    for (const QChar character : value) {
        if (character.isSpace()) {
            pendingSpace =
                !output.isEmpty();
            continue;
        }
        if (pendingSpace) {
            output.append(QLatin1Char(' '));
            pendingSpace = false;
        }
        output.append(character);
    }
    return output;
}

QString RelayAudit::truncateCharacters(
    QStringView value,
    qsizetype maximumCharacters)
{
    QString output;
    output.reserve(
        std::min(
            value.size(),
            maximumCharacters));
    qsizetype count = 0;
    for (qsizetype index = 0;
         index < value.size()
         && count < maximumCharacters;
         ++index, ++count) {
        const QChar current =
            value.at(index);
        output.append(current);
        if (current.isHighSurrogate()
            && index + 1 < value.size()
            && value.at(index + 1)
                   .isLowSurrogate()) {
            output.append(
                value.at(++index));
        }
    }
    return output;
}

Result<QString> RelayAudit::renderedField(
    const RelayAuditField& field)
{
    const QString name =
        sanitizeText(field.name);
    if (name.isEmpty()) {
        return Result<QString>::failure(
            auditError(
                QStringLiteral(
                    "relay.audit_invalid_field"),
                QStringLiteral(
                    "Relay audit field name must not be empty.")));
    }

    const QString normalizedName =
        normalizedFieldName(name);
    static const QSet<QString>
        safeTextNames{
            QStringLiteral("attempt"),
            QStringLiteral("bytes"),
            QStringLiteral("code"),
            QStringLiteral("delaymilliseconds"),
            QStringLiteral("operation"),
            QStringLiteral("peercount"),
            QStringLiteral("phase"),
            QStringLiteral("protocolversion"),
            QStringLiteral("result"),
            QStringLiteral("route"),
            QStringLiteral("state"),
            QStringLiteral("status"),
            QStringLiteral("transport"),
        };
    static const QSet<QString>
        safeUrlNames{
            QStringLiteral("origin"),
            QStringLiteral("relayurl"),
            QStringLiteral("url"),
        };
    static const QSet<QString>
        safeDeviceNames{
            QStringLiteral("device"),
            QStringLiteral("deviceid"),
            QStringLiteral("endpoint"),
            QStringLiteral("endpointid"),
            QStringLiteral("peer"),
            QStringLiteral("peerid"),
        };

    QString value;
    switch (field.kind) {
    case RelayAuditValueKind::Text:
        value = sanitizeText(
            field.value);
        if (!safeTextNames.contains(
                normalizedName)
            || !isSafeTextValue(value)) {
            return Result<QString>::failure(
                sensitiveAuditField());
        }
        break;
    case RelayAuditValueKind::Url:
        if (!safeUrlNames.contains(
                normalizedName)) {
            return Result<QString>::failure(
                sensitiveAuditField());
        }
        value = urlOrigin(field.value);
        break;
    case RelayAuditValueKind::DeviceId:
        if (!safeDeviceNames.contains(
                normalizedName)) {
            return Result<QString>::failure(
                sensitiveAuditField());
        }
        value =
            QString::fromLatin1(
                QCryptographicHash::hash(
                    field.value.toUtf8(),
                    QCryptographicHash::Sha256)
                    .toHex()
                    .left(8));
        break;
    }
    return Result<QString>::success(
        name + QLatin1Char('=') + value);
}

} // namespace companion
