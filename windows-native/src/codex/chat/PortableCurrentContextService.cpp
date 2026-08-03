#include "codex/chat/PortableCurrentContextService.h"

#include <QOperatingSystemVersion>
#include <QSysInfo>

#include <utility>

namespace companion {
namespace {

CompanionError contextError(
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

Result<PortableCurrentContextSnapshot>
defaultSnapshot()
{
    QString operatingSystem =
        QSysInfo::prettyProductName().trimmed();
    if (operatingSystem.isEmpty()) {
        operatingSystem =
            QOperatingSystemVersion::
                current()
                    .name()
                    .trimmed();
    }
    if (operatingSystem.isEmpty()) {
        operatingSystem =
            QStringLiteral("Windows");
    }

    QTimeZone localTimeZone =
        QTimeZone::systemTimeZone();
    if (!localTimeZone.isValid()) {
        localTimeZone = QTimeZone::UTC;
    }
    return Result<
        PortableCurrentContextSnapshot>::success({
        QDateTime::currentDateTimeUtc(),
        localTimeZone,
        QLocale::system(),
        operatingSystem,
    });
}

} // namespace

PortableCurrentContextService::
    PortableCurrentContextService()
    : provider_(defaultSnapshot)
{
}

PortableCurrentContextService::
    PortableCurrentContextService(
        SnapshotProvider provider)
    : provider_(std::move(provider))
{
}

Result<QString>
PortableCurrentContextService::summary(
    QStringView timeZoneIdentifier) const
{
    if (!provider_) {
        return Result<QString>::failure(
            contextError(
                QStringLiteral(
                    "portable_tool.context_unavailable"),
                QStringLiteral(
                    "The current context provider is unavailable.")));
    }

    const Result<
        PortableCurrentContextSnapshot>
        loaded = provider_();
    if (!loaded.hasValue()) {
        return Result<QString>::failure(
            loaded.error());
    }
    const PortableCurrentContextSnapshot&
        snapshot = loaded.value();
    if (!snapshot.nowUtc.isValid()
        || snapshot.operatingSystem
               .trimmed()
               .isEmpty()) {
        return Result<QString>::failure(
            contextError(
                QStringLiteral(
                    "portable_tool.context_invalid"),
                QStringLiteral(
                    "The current context provider returned invalid data.")));
    }

    QTimeZone localTimeZone =
        snapshot.localTimeZone;
    if (!localTimeZone.isValid()) {
        localTimeZone = QTimeZone::UTC;
    }
    const QString requested =
        timeZoneIdentifier
            .toString()
            .trimmed();
    QTimeZone timeZone = localTimeZone;
    if (!requested.isEmpty()
        && requested.compare(
               QStringLiteral("local"),
               Qt::CaseInsensitive)
            != 0) {
        const QTimeZone candidate(
            requested.toUtf8());
        if (candidate.isValid()) {
            timeZone = candidate;
        }
    }

    const QDateTime localDateTime =
        snapshot.nowUtc.toTimeZone(
            timeZone);
    const QString renderedDateTime =
        snapshot.locale.toString(
            localDateTime,
            QLocale::LongFormat);
    return Result<QString>::success(
        QStringLiteral(
            "Current date and time: %1\n"
            "Time zone: %2\n"
            "Locale: %3\n"
            "Operating system: %4")
            .arg(
                renderedDateTime,
                QString::fromUtf8(
                    timeZone.id()),
                snapshot.locale.name(),
                snapshot.operatingSystem
                    .trimmed()));
}

} // namespace companion
