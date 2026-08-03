#pragma once

#include "core/Result.h"

#include <QSslConfiguration>

#include <memory>

namespace companion {

struct WindowsTlsIdentityRequest final {
    QString installationId;
    QString keyName =
        QStringLiteral(
            "Codex Companion Nearby TLS v1");
    int validityYears = 5;
};

struct WindowsTlsIdentityDiagnostics final {
    bool reusedExisting = false;
    bool nativePrivateKeyExportable = false;
    bool qtPrivateKeyFallbackUsed = false;
    QString reason;
};

struct WindowsTlsIdentityMaterial final {
    QByteArray certificateDer;
    QSslConfiguration configuration;
    WindowsTlsIdentityDiagnostics diagnostics;
};

struct WindowsTlsIdentity final {
    QSslConfiguration sslConfiguration;
    QByteArray certificateDer;
    QString fingerprintSha256;
    WindowsTlsIdentityDiagnostics diagnostics;
};

class IWindowsTlsIdentityBackend {
public:
    virtual ~IWindowsTlsIdentityBackend() = default;

    virtual Result<WindowsTlsIdentityMaterial>
    loadOrCreate(
        const WindowsTlsIdentityRequest& request) = 0;
};

class WindowsTlsIdentityStore final {
public:
    explicit WindowsTlsIdentityStore(
        IWindowsTlsIdentityBackend* backend =
            nullptr);
    ~WindowsTlsIdentityStore();

    Result<WindowsTlsIdentity> loadOrCreate(
        const QString& installationId);

private:
    std::unique_ptr<IWindowsTlsIdentityBackend>
        ownedBackend_;
    IWindowsTlsIdentityBackend* backend_ =
        nullptr;
};

} // namespace companion
