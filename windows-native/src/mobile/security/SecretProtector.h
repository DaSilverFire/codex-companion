#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>

namespace companion {

class SecretProtector {
public:
    virtual ~SecretProtector() = default;

    virtual Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView entropy) const = 0;

    virtual Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView entropy) const = 0;
};

} // namespace companion
