#include "update/UpdateManifestValidator.h"

#include <utility>

namespace companion {

UpdateManifestValidator::UpdateManifestValidator(
    const Ed25519Verifier& verifier)
    : verifier_(&verifier)
{
}

Result<UpdateManifest>
UpdateManifestValidator::validate(
    QByteArrayView manifestData,
    QStringView publicKeyBase64) const
{
    Result<UpdateManifest> decoded =
        UpdateManifest::decode(manifestData);
    if (!decoded.hasValue()) {
        return Result<UpdateManifest>::failure(
            decoded.error());
    }

    UpdateManifest manifest =
        std::move(decoded.value());
    const Result<void> verified =
        verifier_->verify(
            manifest.canonicalPayload(),
            manifest.signature,
            publicKeyBase64);
    if (!verified.hasValue()) {
        return Result<UpdateManifest>::failure(
            verified.error());
    }

    return Result<UpdateManifest>::success(
        std::move(manifest));
}

} // namespace companion
