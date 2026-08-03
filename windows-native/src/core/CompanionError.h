#pragma once

#include <QMetaType>
#include <QVariantMap>
#include <QString>

namespace companion {

struct CompanionError final {
    QString code;
    QString message;
    bool retryable = false;
    QVariantMap context;

    friend bool operator==(const CompanionError&, const CompanionError&) = default;
};

} // namespace companion

Q_DECLARE_METATYPE(companion::CompanionError)
