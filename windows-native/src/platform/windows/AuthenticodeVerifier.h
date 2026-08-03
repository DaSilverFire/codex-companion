#pragma once

#include "core/Result.h"

#include <functional>

#include <QString>
#include <QStringList>
#include <QStringView>

namespace companion {

struct AuthenticodeIdentity final {
    QString sha256Thumbprint;
    QString subject;

    friend bool operator==(
        const AuthenticodeIdentity&,
        const AuthenticodeIdentity&) = default;
};

struct AuthenticodePolicy final {
    QStringList allowedSignerSha256;
    bool cacheOnlyUrlRetrieval = false;

    static AuthenticodePolicy
    fromBuildConfiguration();
};

class AuthenticodeVerifier final {
public:
    using NativeVerifier = std::function<
        Result<AuthenticodeIdentity>(
            QStringView,
            bool)>;

    AuthenticodeVerifier();
    explicit AuthenticodeVerifier(
        NativeVerifier verifier);

    Result<AuthenticodeIdentity> verify(
        QStringView path,
        const AuthenticodePolicy& policy) const;

    static QString normalizeThumbprint(
        QStringView value);

private:
    NativeVerifier verifier_;
};

} // namespace companion
