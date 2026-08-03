#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>

namespace companion {

struct AuthenticatedCiphertext final {
    QByteArray ciphertext;
    QByteArray tag;
};

class WindowsCrypto final {
public:
    static Result<QByteArray> randomBytes(
        qsizetype count);

    static Result<QByteArray> hmacSha256(
        QByteArrayView secret,
        QByteArrayView data);

    static Result<AuthenticatedCiphertext>
    chacha20Poly1305Seal(
        QByteArrayView plaintext,
        QByteArrayView secret,
        QByteArrayView nonce,
        QByteArrayView authenticatedData);

    static Result<QByteArray>
    chacha20Poly1305Open(
        QByteArrayView ciphertext,
        QByteArrayView tag,
        QByteArrayView secret,
        QByteArrayView nonce,
        QByteArrayView authenticatedData);

    static bool constantTimeEquals(
        QByteArrayView left,
        QByteArrayView right) noexcept;

    static void secureZero(
        QByteArray& bytes) noexcept;
};

} // namespace companion
