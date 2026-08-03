#pragma once

#include "core/Result.h"
#include "update/Ed25519Verifier.h"
#include "update/UpdateManifest.h"

#include <QByteArrayView>
#include <QStringView>

namespace companion {

class UpdateManifestValidator final {
public:
    explicit UpdateManifestValidator(
        const Ed25519Verifier& verifier);

    Result<UpdateManifest> validate(
        QByteArrayView manifestData,
        QStringView publicKeyBase64) const;

private:
    const Ed25519Verifier* verifier_ = nullptr;
};

} // namespace companion
