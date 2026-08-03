#pragma once

#include "core/Result.h"

#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QStringView>
#include <QTimeZone>

#include <functional>

namespace companion {

struct PortableCurrentContextSnapshot final {
    QDateTime nowUtc;
    QTimeZone localTimeZone;
    QLocale locale;
    QString operatingSystem;
};

class PortableCurrentContextService final {
public:
    using SnapshotProvider =
        std::function<Result<
            PortableCurrentContextSnapshot>()>;

    PortableCurrentContextService();
    explicit PortableCurrentContextService(
        SnapshotProvider provider);

    Result<QString> summary(
        QStringView timeZoneIdentifier) const;

private:
    SnapshotProvider provider_;
};

} // namespace companion
