#include "app/CompanionMobileHost.h"
#include "core/CompanionInstallationIdentityStore.h"
#include "core/SettingsRepository.h"
#include "mobile/nearby/NearbyTransferAssembler.h"
#include "mobile/relay/RelaySettings.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/mobile/WindowsTlsIdentityStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include <optional>
#include <utility>

namespace {

using namespace companion;

int fail(
    QString stage,
    const CompanionError& error)
{
    const QJsonObject report{
        {QStringLiteral("passed"), false},
        {QStringLiteral("stage"), std::move(stage)},
        {QStringLiteral("code"), error.code},
        {QStringLiteral("message"), error.message},
        {QStringLiteral("activeTlsBackend"),
         QSslSocket::activeBackend()},
        {QStringLiteral("availableTlsBackends"),
         QJsonArray::fromStringList(
             QSslSocket::availableBackends())},
        {QStringLiteral("context"),
         QJsonObject::fromVariantMap(error.context)},
    };
    QTextStream(stderr)
        << QJsonDocument(report).toJson(
               QJsonDocument::Indented);
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(
        QStringLiteral("DaSilverFire"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Codex Companion"));

    const QString configRoot =
        QStandardPaths::writableLocation(
            QStandardPaths::AppConfigLocation);
    if (configRoot.isEmpty()) {
        return fail(
            QStringLiteral("settings-path"),
            {
                QStringLiteral(
                    "settings.path-unavailable"),
                QStringLiteral(
                    "The application configuration location is unavailable."),
                false,
                {},
            });
    }

    SettingsRepository settingsRepository(
        QDir(configRoot).filePath(
            QStringLiteral(
                "CodexCompanion.ini")));
    const auto settings =
        settingsRepository.load();
    if (!settings.hasValue()) {
        return fail(
            QStringLiteral("settings"),
            settings.error());
    }

    CompanionInstallationIdentityStore
        identityStore;
    const auto identity =
        identityStore.loadOrCreate();
    if (!identity.hasValue()) {
        return fail(
            QStringLiteral("installation-identity"),
            identity.error());
    }

    const RelaySettings relaySettings =
        RelaySettings::fromBundledConfiguration();
    const auto relayUrl =
        relaySettings.configuredUrl(
            settings.value());
    if (!relayUrl.hasValue()) {
        return fail(
            QStringLiteral("relay-url"),
            relayUrl.error());
    }

    WindowsTlsIdentityStore tlsStore;
    const auto tlsIdentity =
        tlsStore.loadOrCreate(
            identity.value());
    if (!tlsIdentity.hasValue()) {
        return fail(
            QStringLiteral("tls-identity"),
            tlsIdentity.error());
    }

    QString hostName =
        QSysInfo::machineHostName().trimmed();
    if (hostName.isEmpty()) {
        hostName =
            QStringLiteral(
                "Codex Companion Windows");
    }

    CompanionMobileHostConfiguration
        configuration;
    configuration.enabled =
        settings.value().mobileEnabled;
    configuration
        .allowNearbyOnPublicNetworks =
        settings.value()
            .allowNearbyOnPublicNetworks;
    configuration.installationId =
        identity.value();
    configuration.computerName =
        hostName;
    configuration.hostDisplayName =
        hostName;
    configuration.sslConfiguration =
        tlsIdentity.value()
            .sslConfiguration;
    configuration.tlsFingerprintSha256 =
        tlsIdentity.value()
            .fingerprintSha256;
    configuration.pairingRecordsPath =
        PairingRecordStore::defaultFilePath();
    configuration.relayStatePath =
        RelayStateStore::defaultFilePath();
    configuration.transferRootPath =
        NearbyTransferAssembler::
            defaultRootPath();
    configuration.relayUrl =
        relayUrl.value();

    auto mobileHost =
        CompanionMobileHost::create(
            std::move(configuration),
            [](QString, BridgeRequest) {
                return QFuture<
                    BridgeResponse>{};
            });
    if (!mobileHost.hasValue()) {
        return fail(
            QStringLiteral("mobile-host"),
            mobileHost.error());
    }

    const auto started =
        mobileHost.value()->start();
    if (!started.hasValue()) {
        return fail(
            QStringLiteral("mobile-start"),
            started.error());
    }
    const bool listening =
        mobileHost.value()->isListening();
    const bool advertising =
        mobileHost.value()->isAdvertising();
    const bool nearbyAccessAvailable =
        mobileHost.value()
            ->nearbyAccessAvailable();
    const quint16 port =
        mobileHost.value()->serverPort();

    const auto pairing =
        mobileHost.value()
            ->pairingCoordinator()
            .beginPairing();
    if (!pairing.hasValue()) {
        mobileHost.value()->stop();
        return fail(
            QStringLiteral("pairing-code"),
            pairing.error());
    }
    const bool pairingCodeValid =
        pairing.value().code.size() == 6;
    mobileHost.value()
        ->pairingCoordinator()
        .cancelPairing();
    mobileHost.value()->stop();

    const QJsonObject report{
        {QStringLiteral("passed"), true},
        {QStringLiteral("stage"),
         QStringLiteral("mobile-host")},
        {QStringLiteral("mobileEnabled"),
         settings.value().mobileEnabled},
        {QStringLiteral(
             "allowNearbyOnPublicNetworks"),
         settings.value()
             .allowNearbyOnPublicNetworks},
        {QStringLiteral("pairingAvailable"),
         true},
        {QStringLiteral("pairingCodeValid"),
         pairingCodeValid},
        {QStringLiteral("listening"),
         listening},
        {QStringLiteral("advertising"),
         advertising},
        {QStringLiteral(
             "nearbyAccessAvailable"),
         nearbyAccessAvailable},
        {QStringLiteral("serverPort"),
         int(port)},
        {QStringLiteral("certificateBytes"),
         tlsIdentity.value()
             .certificateDer.size()},
        {QStringLiteral("fingerprintLength"),
         tlsIdentity.value()
             .fingerprintSha256.size()},
        {QStringLiteral("privateKeyAvailable"),
         !tlsIdentity.value()
              .sslConfiguration
              .privateKey()
              .isNull()},
        {QStringLiteral(
             "qtPrivateKeyFallbackUsed"),
         tlsIdentity.value()
             .diagnostics
             .qtPrivateKeyFallbackUsed},
        {QStringLiteral("reusedExisting"),
         tlsIdentity.value()
             .diagnostics
             .reusedExisting},
        {QStringLiteral("diagnosticReason"),
         tlsIdentity.value()
             .diagnostics
             .reason},
    };
    QTextStream(stdout)
        << QJsonDocument(report).toJson(
               QJsonDocument::Indented);
    return 0;
}
