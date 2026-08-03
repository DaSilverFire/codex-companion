#pragma once

#include "mobile/security/SecretProtector.h"

namespace companion {

class WindowsDpapiProtector final
    : public SecretProtector {
public:
    Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView entropy)
        const override;

    Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView entropy)
        const override;
};

} // namespace companion
