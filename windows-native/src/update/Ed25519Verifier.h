#pragma once

#include "core/Result.h"

#include <QByteArrayView>
#include <QStringView>

namespace companion {

class Ed25519Verifier {
public:
    virtual ~Ed25519Verifier() = default;

    virtual Result<void> verify(
        QByteArrayView message,
        QStringView signatureBase64,
        QStringView publicKeyBase64) const = 0;
};

} // namespace companion
