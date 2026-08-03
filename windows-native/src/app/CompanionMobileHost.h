#pragma once

#include "core/Result.h"
#include "mobile/nearby/NearbyWebSocketServer.h"
#include "mobile/relay/RelayPairingBootstrap.h"
#include "mobile/security/SecretProtector.h"
#include "platform/windows/mobile/WindowsDnsSdAdvertiser.h"
#include "platform/windows/mobile/WindowsNetworkProfileMonitor.h"

#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

#include <memory>
#include <optional>

namespace companion {

class MobilePresencePetCatalogService;
class PairingCoordinator;

struct CompanionMobileHostConfiguration final {
    bool enabled = true;
    bool allowNearbyOnPublicNetworks =
        false;
    QString installationId;
    QString computerName;
    QString hostDisplayName;
    QSslConfiguration sslConfiguration;
    QString tlsFingerprintSha256;
    QString pairingRecordsPath;
    QString relayStatePath;
    QString transferRootPath;
    std::optional<QUrl> relayUrl;
    std::shared_ptr<
        MobilePresencePetCatalogService>
        presencePetCatalogService;
};

struct CompanionMobileHostDependencies final {
    std::unique_ptr<SecretProtector>
        secretProtector;
    std::unique_ptr<IWindowsDnsSdApi>
        dnsSdApi;
    std::unique_ptr<
        IWindowsNetworkProfileApi>
        networkProfileApi;
    RelayPairingBootstrapDependencies
        relayPairingBootstrap;
    QHostAddress nearbyListenAddress =
        QHostAddress::AnyIPv4;
};

class CompanionMobileHost final
    : public QObject {
    Q_OBJECT

public:
    static Result<
        std::unique_ptr<
            CompanionMobileHost>>
    create(
        CompanionMobileHostConfiguration
            configuration,
        NearbyRequestHandler requestHandler,
        CompanionMobileHostDependencies
            dependencies = {},
        QObject* parent = nullptr);

    ~CompanionMobileHost() override;

    CompanionMobileHost(
        const CompanionMobileHost&) =
        delete;
    CompanionMobileHost& operator=(
        const CompanionMobileHost&) =
        delete;

    Result<void> start();
    void stop();
    Result<void> applyConfiguration(
        bool enabled,
        bool allowNearbyOnPublicNetworks,
        std::optional<QUrl> relayUrl);

    bool isListening() const noexcept;
    bool isAdvertising() const noexcept;
    bool isEnabled() const noexcept;
    bool allowsNearbyOnPublicNetworks()
        const noexcept;
    bool nearbyAccessAvailable() const
        noexcept;
    WindowsNetworkProfile
    networkProfile() const noexcept;
    quint16 serverPort() const noexcept;
    std::optional<QUrl>
    configuredRelayUrl() const;

    PairingCoordinator&
    pairingCoordinator() noexcept;
    RelayPairingBootstrap&
    relayPairingBootstrap() noexcept;

signals:
    void failureOccurred(
        companion::CompanionError error);
    void nearbyAccessChanged();

private:
    struct Implementation;

    explicit CompanionMobileHost(
        CompanionMobileHostConfiguration
            configuration,
        NearbyRequestHandler requestHandler,
        CompanionMobileHostDependencies
            dependencies,
        QObject* parent);

    std::unique_ptr<Implementation>
        implementation_;
};

} // namespace companion
