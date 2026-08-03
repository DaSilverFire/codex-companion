#pragma once

#include "core/Result.h"
#include "platform/windows/mobile/WindowsNetworkProfileMonitor.h"

#include <QMap>
#include <QString>

#include <memory>
#include <optional>

namespace companion {

struct WindowsDnsSdService final {
    QString instanceName;
    QString serviceType;
    QString hostName;
    quint16 port = 0;
    QMap<QString, QString> txt;
    quint32 interfaceIndex = 0;

    friend bool operator==(
        const WindowsDnsSdService&,
        const WindowsDnsSdService&) = default;
};

struct WindowsDnsSdAdvertisement final {
    QString computerName;
    QString installationId;
    quint16 port = 0;
    QString tlsFingerprintSha256;
    WindowsNetworkProfile networkProfile =
        WindowsNetworkProfile::Unavailable;
    bool allowPublicNetwork = false;
    QString hostName;
    quint32 interfaceIndex = 0;
};

class IWindowsDnsSdApi {
public:
    virtual ~IWindowsDnsSdApi() = default;

    virtual Result<void> registerService(
        const WindowsDnsSdService& service) = 0;
    virtual Result<void> deregisterService(
        const WindowsDnsSdService& service) = 0;
};

class WindowsDnsSdAdvertiser final {
public:
    explicit WindowsDnsSdAdvertiser(
        IWindowsDnsSdApi* api = nullptr);
    ~WindowsDnsSdAdvertiser();

    Result<void> update(
        const WindowsDnsSdAdvertisement& advertisement);
    Result<void> stop();

    bool isAdvertising() const noexcept;
    std::optional<WindowsDnsSdService>
    activeService() const;

    static QString serviceType();
    static QString fallbackInstanceName();
    static QString instanceNameForComputerName(
        const QString& computerName);
    static Result<QMap<QString, QString>> txtRecord(
        const QString& installationId,
        const QString& tlsFingerprintSha256);

private:
    std::unique_ptr<IWindowsDnsSdApi> ownedApi_;
    IWindowsDnsSdApi* api_ = nullptr;
    std::optional<WindowsDnsSdService> active_;
};

} // namespace companion
