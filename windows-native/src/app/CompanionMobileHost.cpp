#include "app/CompanionMobileHost.h"

#include "app/CompanionMobileRelayRouter.h"
#include "mobile/relay/RelayEndpointManager.h"
#include "mobile/relay/RelayPairingBootstrap.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/security/WindowsDpapiProtector.h"

#include <QSslKey>
#include <QUuid>

#include <utility>

namespace companion {
namespace {

CompanionError mobileHostError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

NearbyNetworkProfile nearbyProfile(
    WindowsNetworkProfile profile)
{
    switch (profile) {
    case WindowsNetworkProfile::Public:
        return NearbyNetworkProfile::Public;
    case WindowsNetworkProfile::Private:
        return NearbyNetworkProfile::Private;
    case WindowsNetworkProfile::Domain:
        return NearbyNetworkProfile::Domain;
    case WindowsNetworkProfile::Unavailable:
        return NearbyNetworkProfile::
            Unavailable;
    }
    return NearbyNetworkProfile::
        Unavailable;
}

Result<void> validateConfiguration(
    const CompanionMobileHostConfiguration&
        configuration,
    const NearbyRequestHandler&
        requestHandler)
{
    if (!requestHandler) {
        return Result<void>::failure(
            mobileHostError(
                QStringLiteral(
                    "mobile.request_handler_unavailable"),
                QStringLiteral(
                    "The mobile request handler is unavailable.")));
    }
    if (QUuid(
            configuration
                .installationId
                .trimmed())
            .isNull()) {
        return Result<void>::failure(
            mobileHostError(
                QStringLiteral(
                    "mobile.invalid_installation_id"),
                QStringLiteral(
                    "The Companion installation identity is invalid.")));
    }
    if (configuration
            .sslConfiguration
            .localCertificate()
            .isNull()
        || configuration
               .sslConfiguration
               .privateKey()
               .isNull()) {
        return Result<void>::failure(
            mobileHostError(
                QStringLiteral(
                    "mobile.tls_identity_unavailable"),
                QStringLiteral(
                    "The nearby TLS identity is unavailable.")));
    }
    if (configuration
            .pairingRecordsPath
            .trimmed()
            .isEmpty()
        || configuration
               .relayStatePath
               .trimmed()
               .isEmpty()
        || configuration
               .transferRootPath
               .trimmed()
               .isEmpty()) {
        return Result<void>::failure(
            mobileHostError(
                QStringLiteral(
                    "mobile.storage_path_unavailable"),
                QStringLiteral(
                    "The mobile security storage paths are unavailable.")));
    }
    const auto txt =
        WindowsDnsSdAdvertiser::
            txtRecord(
                configuration
                    .installationId,
                configuration
                    .tlsFingerprintSha256);
    if (!txt.hasValue()) {
        return Result<void>::failure(
            txt.error());
    }
    return Result<void>::success();
}

} // namespace

struct CompanionMobileHost::
Implementation final {
    Implementation(
        CompanionMobileHost& requestedOwner,
        CompanionMobileHostConfiguration
            requestedConfiguration,
        NearbyRequestHandler
            requestedHandler,
        CompanionMobileHostDependencies
            dependencies)
        : owner(&requestedOwner),
          configuration(
              std::move(
                  requestedConfiguration)),
          requestHandler(
              std::move(
                  requestedHandler)),
          protector(
              dependencies.secretProtector
                  ? std::move(
                        dependencies
                            .secretProtector)
                  : std::make_unique<
                        WindowsDpapiProtector>()),
          dnsSdApi(
              std::move(
                  dependencies.dnsSdApi)),
          networkProfileApi(
              std::move(
                  dependencies
                      .networkProfileApi))
    {
        pairingStore =
            std::make_unique<
                PairingRecordStore>(
                configuration
                    .pairingRecordsPath,
                *protector);
        pairingCoordinator =
            std::make_unique<
                PairingCoordinator>(
                *pairingStore);
        relayStateStore =
            std::make_unique<
                RelayStateStore>(
                configuration
                    .relayStatePath,
                *protector);
        relayPairingBootstrap =
            std::make_unique<
                RelayPairingBootstrap>(
                *pairingCoordinator,
                *relayStateStore,
                configuration
                    .installationId,
                configuration
                    .hostDisplayName,
                std::move(
                    dependencies
                        .relayPairingBootstrap));
        relayPairingBootstrap
            ->setRelayUrl(
                configuration.relayUrl);
        relayEndpoints =
            std::make_unique<
                RelayEndpointManager>(
                configuration
                    .installationId);
        relayRouter =
            std::make_unique<
                CompanionMobileRelayRouter>(
                configuration
                    .installationId,
                *pairingCoordinator,
                *relayStateStore,
                requestHandler,
                [this](
                    QString deviceId,
                    EncryptedEnvelope
                        envelope) {
                    return relayEndpoints
                        ->send(
                            deviceId,
                            envelope);
                });
        dnsSdAdvertiser =
            std::make_unique<
                WindowsDnsSdAdvertiser>(
                dnsSdApi.get());
        networkProfileMonitor =
            std::make_unique<
                WindowsNetworkProfileMonitor>(
                networkProfileApi.get());

        NearbyWebSocketServerOptions
            nearbyOptions;
        nearbyOptions.sslConfiguration =
            configuration
                .sslConfiguration;
        nearbyOptions.installationId =
            configuration.installationId;
        nearbyOptions.computerName =
            configuration.computerName;
        nearbyOptions.hostDisplayName =
            configuration.hostDisplayName;
        nearbyOptions.listenAddress =
            dependencies
                .nearbyListenAddress;
        nearbyOptions.transferRootPath =
            configuration.transferRootPath;
        nearbyOptions
            .presencePetCatalogService =
            configuration
                .presencePetCatalogService;
        if (configuration.relayUrl
                .has_value()) {
            nearbyOptions.relayUrlString =
                configuration.relayUrl
                    ->toString(
                        QUrl::FullyEncoded);
        }
        nearbyOptions.publishAdvertisement =
            [this](
                const NearbyServiceAdvertisement&
                    advertisement) {
                return dnsSdAdvertiser
                    ->update({
                        configuration
                            .computerName,
                        configuration
                            .installationId,
                        advertisement.port,
                        configuration
                            .tlsFingerprintSha256,
                        networkProfileMonitor
                            ->profile(),
                        configuration
                            .allowNearbyOnPublicNetworks,
                        {},
                        0,
                    });
            };
        nearbyOptions.withdrawAdvertisement =
            [this] {
                const auto stopped =
                    dnsSdAdvertiser->stop();
                if (!stopped.hasValue()) {
                    reportFailure(
                        stopped.error());
                }
            };
        nearbyServer =
            std::make_unique<
                NearbyWebSocketServer>(
                *pairingCoordinator,
                requestHandler,
                std::move(
                    nearbyOptions));

        QObject::connect(
            pairingCoordinator.get(),
            &PairingCoordinator::
                pairingStateChanged,
            owner,
            [this] {
                synchronizeRelay();
            });
        QObject::connect(
            networkProfileMonitor.get(),
            &WindowsNetworkProfileMonitor::
                profileChanged,
            owner,
            [this](
                WindowsNetworkProfile
                    profile) {
                if (started
                    && configuration
                           .enabled) {
                    const auto applied =
                        applyNetworkProfile(
                            profile);
                    if (!applied.hasValue()) {
                        reportFailure(
                            applied.error());
                    }
                }
                emit owner->
                    nearbyAccessChanged();
            });
        QObject::connect(
            relayEndpoints.get(),
            &RelayEndpointManager::
                envelopeReceived,
            owner,
            [this](
                QString deviceId,
                EncryptedEnvelope
                    envelope) {
                relayRouter->receive(
                    std::move(deviceId),
                    std::move(envelope));
            });
        QObject::connect(
            relayEndpoints.get(),
            &RelayEndpointManager::
                failureOccurred,
            owner,
            [this](
                QString,
                CompanionError error) {
                reportFailure(
                    std::move(error));
            });
        QObject::connect(
            relayRouter.get(),
            &CompanionMobileRelayRouter::
                failureOccurred,
            owner,
            [this](
                QString,
                CompanionError error) {
                reportFailure(
                    std::move(error));
            });
    }

    ~Implementation()
    {
        stop();
    }

    Result<void> start()
    {
        if (started) {
            return Result<void>::success();
        }
        if (!configuration.enabled) {
            started = true;
            emit owner->
                nearbyAccessChanged();
            return Result<void>::success();
        }

        const auto profile =
            networkProfileMonitor
                ->refresh();
        if (!profile.hasValue()) {
            return Result<void>::failure(
                profile.error());
        }
        const auto applied =
            applyNetworkProfile(
                profile.value());
        if (!applied.hasValue()) {
            emit owner->
                nearbyAccessChanged();
            return applied;
        }

        started = true;
        synchronizeRelay();
        emit owner->
            nearbyAccessChanged();
        return Result<void>::success();
    }

    Result<void> applyNetworkProfile(
        WindowsNetworkProfile profile)
    {
        const auto publicAccessApplied =
            nearbyServer
                ->setAllowPublicNetwork(
                    configuration
                        .allowNearbyOnPublicNetworks);
        if (!publicAccessApplied.hasValue()) {
            return publicAccessApplied;
        }
        const auto applied =
            nearbyServer
                ->setNetworkProfile(
                    nearbyProfile(
                        profile));
        if (!applied.hasValue()) {
            return applied;
        }

        const bool mayListen =
            profile
                == WindowsNetworkProfile::
                    Private
            || profile
                == WindowsNetworkProfile::
                    Domain
            || (profile
                    == WindowsNetworkProfile::
                        Public
                && configuration
                       .allowNearbyOnPublicNetworks);
        if (mayListen) {
            return nearbyServer->start();
        }
        nearbyServer->stop();
        return Result<void>::success();
    }

    void stop()
    {
        relayPairingBootstrap
            ->cancelPairing();
        if (!started) {
            return;
        }
        started = false;
        relayEndpoints->stopAll();
        nearbyServer->stop();
        const auto stopped =
            dnsSdAdvertiser->stop();
        if (!stopped.hasValue()) {
            reportFailure(
                stopped.error());
        }
        emit owner->
            nearbyAccessChanged();
    }

    Result<void> applyConfiguration(
        bool enabled,
        bool allowNearbyOnPublicNetworks,
        std::optional<QUrl> relayUrl)
    {
        configuration.enabled = enabled;
        configuration
            .allowNearbyOnPublicNetworks =
            allowNearbyOnPublicNetworks;
        configuration.relayUrl =
            std::move(relayUrl);
        relayPairingBootstrap
            ->setRelayUrl(
                configuration.relayUrl);
        if (!configuration.enabled) {
            relayPairingBootstrap
                ->cancelPairing();
        }
        std::optional<QString>
            relayUrlString;
        if (configuration.relayUrl
                .has_value()) {
            relayUrlString =
                configuration.relayUrl
                    ->toString(
                        QUrl::FullyEncoded);
        }
        nearbyServer->setRelayUrlString(
            std::move(relayUrlString));

        if (!started) {
            emit owner->
                nearbyAccessChanged();
            return Result<void>::success();
        }
        if (!configuration.enabled) {
            relayEndpoints->synchronize(
                {},
                std::nullopt);
            nearbyServer->stop();
            emit owner->
                nearbyAccessChanged();
            return Result<void>::success();
        }

        const auto profile =
            networkProfileMonitor
                ->refresh();
        if (!profile.hasValue()) {
            return Result<void>::failure(
                profile.error());
        }
        const auto applied =
            applyNetworkProfile(
                profile.value());
        if (!applied.hasValue()) {
            emit owner->
                nearbyAccessChanged();
            return applied;
        }
        synchronizeRelay();
        emit owner->
            nearbyAccessChanged();
        return Result<void>::success();
    }

    void synchronizeRelay()
    {
        if (!started
            || !configuration.enabled) {
            relayEndpoints->synchronize(
                {},
                std::nullopt);
            return;
        }
        relayEndpoints->synchronize(
            pairingCoordinator
                ->trustedRecords(),
            configuration.relayUrl);
    }

    void reportFailure(
        CompanionError error)
    {
        emit owner->failureOccurred(
            std::move(error));
    }

    CompanionMobileHost* owner = nullptr;
    CompanionMobileHostConfiguration
        configuration;
    NearbyRequestHandler requestHandler;
    std::unique_ptr<SecretProtector>
        protector;
    std::unique_ptr<IWindowsDnsSdApi>
        dnsSdApi;
    std::unique_ptr<
        IWindowsNetworkProfileApi>
        networkProfileApi;
    std::unique_ptr<PairingRecordStore>
        pairingStore;
    std::unique_ptr<PairingCoordinator>
        pairingCoordinator;
    std::unique_ptr<RelayStateStore>
        relayStateStore;
    std::unique_ptr<
        RelayPairingBootstrap>
        relayPairingBootstrap;
    std::unique_ptr<
        RelayEndpointManager>
        relayEndpoints;
    std::unique_ptr<
        CompanionMobileRelayRouter>
        relayRouter;
    std::unique_ptr<
        WindowsDnsSdAdvertiser>
        dnsSdAdvertiser;
    std::unique_ptr<
        WindowsNetworkProfileMonitor>
        networkProfileMonitor;
    std::unique_ptr<
        NearbyWebSocketServer>
        nearbyServer;
    bool started = false;
};

Result<
    std::unique_ptr<
        CompanionMobileHost>>
CompanionMobileHost::create(
    CompanionMobileHostConfiguration
        configuration,
    NearbyRequestHandler requestHandler,
    CompanionMobileHostDependencies
        dependencies,
    QObject* parent)
{
    const auto valid =
        validateConfiguration(
            configuration,
            requestHandler);
    if (!valid.hasValue()) {
        return Result<
            std::unique_ptr<
                CompanionMobileHost>>::
            failure(valid.error());
    }

    try {
        return Result<
            std::unique_ptr<
                CompanionMobileHost>>::
            success(
                std::unique_ptr<
                    CompanionMobileHost>(
                    new CompanionMobileHost(
                        std::move(
                            configuration),
                        std::move(
                            requestHandler),
                        std::move(
                            dependencies),
                        parent)));
    } catch (...) {
        return Result<
            std::unique_ptr<
                CompanionMobileHost>>::
            failure(
                mobileHostError(
                    QStringLiteral(
                        "mobile.host_unavailable"),
                    QStringLiteral(
                        "The Windows mobile bridge could not be initialized.")));
    }
}

CompanionMobileHost::
CompanionMobileHost(
    CompanionMobileHostConfiguration
        configuration,
    NearbyRequestHandler requestHandler,
    CompanionMobileHostDependencies
        dependencies,
    QObject* parent)
    : QObject(parent),
      implementation_(
          std::make_unique<
              Implementation>(
              *this,
              std::move(configuration),
              std::move(requestHandler),
              std::move(dependencies)))
{
}

CompanionMobileHost::
~CompanionMobileHost() = default;

Result<void>
CompanionMobileHost::start()
{
    return implementation_->start();
}

void CompanionMobileHost::stop()
{
    implementation_->stop();
}

Result<void>
CompanionMobileHost::
applyConfiguration(
    bool enabled,
    bool allowNearbyOnPublicNetworks,
    std::optional<QUrl> relayUrl)
{
    return implementation_
        ->applyConfiguration(
            enabled,
            allowNearbyOnPublicNetworks,
            std::move(relayUrl));
}

bool CompanionMobileHost::
isListening() const noexcept
{
    return implementation_
        ->nearbyServer
        ->isListening();
}

bool CompanionMobileHost::
isAdvertising() const noexcept
{
    return implementation_
        ->dnsSdAdvertiser
        ->isAdvertising();
}

bool CompanionMobileHost::
isEnabled() const noexcept
{
    return implementation_
        ->configuration.enabled;
}

bool CompanionMobileHost::
allowsNearbyOnPublicNetworks()
    const noexcept
{
    return implementation_
        ->configuration
        .allowNearbyOnPublicNetworks;
}

bool CompanionMobileHost::
nearbyAccessAvailable() const noexcept
{
    return isEnabled()
        && isListening()
        && isAdvertising();
}

WindowsNetworkProfile
CompanionMobileHost::
networkProfile() const noexcept
{
    return implementation_
        ->networkProfileMonitor
        ->profile();
}

quint16 CompanionMobileHost::
serverPort() const noexcept
{
    return implementation_
        ->nearbyServer
        ->serverPort();
}

std::optional<QUrl>
CompanionMobileHost::
configuredRelayUrl() const
{
    return implementation_
        ->configuration.relayUrl;
}

PairingCoordinator&
CompanionMobileHost::
pairingCoordinator() noexcept
{
    return *implementation_
        ->pairingCoordinator;
}

RelayPairingBootstrap&
CompanionMobileHost::
relayPairingBootstrap() noexcept
{
    return *implementation_
        ->relayPairingBootstrap;
}

} // namespace companion
