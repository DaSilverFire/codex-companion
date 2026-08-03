#pragma once

#include "core/Result.h"

#include <QHash>
#include <QString>

namespace companion {

class SessionIndexReader final {
public:
    static Result<QHash<QString, QString>> readNames(
        const QString& sessionIndexPath);
};

} // namespace companion
