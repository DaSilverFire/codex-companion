#pragma once

#include "update/Ed25519Verifier.h"

namespace companion {

class MonocypherEd25519Verifier final
    : public Ed25519Verifier {
public:
    Result<void> verify(
        QByteArrayView message,
        QStringView signatureBase64,
        QStringView publicKeyBase64) const override;
};

} // namespace companion
