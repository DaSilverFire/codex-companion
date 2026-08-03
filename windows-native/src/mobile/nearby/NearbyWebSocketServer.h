#pragma once

#include "mobile/nearby/NearbyTransport.h"

#include <QFuture>
#include <QHostAddress>
#include <QMap>
#include <QObject>
#include <QSslConfiguration>

#include <functional>
#include <memory>
#include <optional>

namespace companion {

class MobilePresencePetCatalogService;

enum class NearbyNetworkProfile {
    Unavailable,
    Public,
    Private,
    Domain,
};

struct NearbyServiceAdvertisement final {
    QString serviceType;
    QString instanceName;
    quint16 port = 0;
    QMap<QString, QString> txt;

    friend bool operator==(
        const NearbyServiceAdvertisement&,
        const NearbyServiceAdvertisement&) =
        default;
};

using NearbyRequestHandler =
    std::function<QFuture<BridgeResponse>(
        QString deviceId,
        BridgeRequest request)>;

struct NearbyWebSocketServerOptions final {
    QSslConfiguration sslConfiguration;
    QString installationId;
    QString computerName;
    QString hostDisplayName;
    QHostAddress listenAddress =
        QHostAddress::AnyIPv4;
    quint16 listenPort = 0;
    QString transferRootPath;
    std::optional<QString> relayUrlString;
    std::shared_ptr<
        MobilePresencePetCatalogService>
        presencePetCatalogService;
    std::function<Result<void>(
        const NearbyServiceAdvertisement&)>
        publishAdvertisement;
    std::function<void()>
        withdrawAdvertisement;
};

class PairingCoordinator;

class NearbyWebSocketServer final
    : public QObject,
      public NearbyTransport {
    Q_OBJECT

public:
    static constexpr QStringView
        serviceType =
            u"_codex-companion._tcp.local";
    static constexpr QStringView
        requestPath =
            u"/companion/v1";

    NearbyWebSocketServer(
        PairingCoordinator& pairingCoordinator,
        NearbyRequestHandler requestHandler,
        NearbyWebSocketServerOptions options,
        QObject* parent = nullptr);
    ~NearbyWebSocketServer() override;

    NearbyWebSocketServer(
        const NearbyWebSocketServer&) =
        delete;
    NearbyWebSocketServer& operator=(
        const NearbyWebSocketServer&) =
        delete;

    Result<void> start() override;
    QFuture<void> stop() override;
    bool hasAuthenticatedDevice(
        const QString& deviceId)
        const override;
    QFuture<Result<void>> send(
        const QString& deviceId,
        const BridgeResponse& response)
        override;

    Result<void> setNetworkProfile(
        NearbyNetworkProfile profile);
    Result<void> setAllowPublicNetwork(
        bool allowed);
    void setRelayUrlString(
        std::optional<QString> value);
    NearbyNetworkProfile
    networkProfile() const noexcept;
    bool isListening() const noexcept;
    quint16 serverPort() const noexcept;

    static QString serviceInstanceName(
        QString computerName);

private:
    struct Implementation;
    std::unique_ptr<Implementation>
        implementation_;
};

} // namespace companion
