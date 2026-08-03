#pragma once

#include "core/Result.h"

#include <QString>

namespace companion {

Result<QString> pairingQrCodeDataUrl(
    const QString& pairingLink);

} // namespace companion
