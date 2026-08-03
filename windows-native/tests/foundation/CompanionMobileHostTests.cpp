#include "app/CompanionMobileHost.h"

#include "mobile/relay/RelayPairingBootstrap.h"
#include "mobile/security/SecretProtector.h"
#include "platform/windows/mobile/WindowsDnsSdAdvertiser.h"
#include "platform/windows/mobile/WindowsNetworkProfileMonitor.h"

#include <QCryptographicHash>
#include <QFile>
#include <QPromise>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <utility>

using namespace companion;

namespace {

class TestProtector final
    : public SecretProtector {
public:
    Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView) const override
    {
        return Result<QByteArray>::success(
            plaintext.toByteArray());
    }

    Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView) const override
    {
        return Result<QByteArray>::success(
            protectedData.toByteArray());
    }
};

class FakeDnsSdApi final
    : public IWindowsDnsSdApi {
public:
    Result<void> registerService(
        const WindowsDnsSdService& service)
        override
    {
        ++registerCalls;
        active = service;
        return Result<void>::success();
    }

    Result<void> deregisterService(
        const WindowsDnsSdService& service)
        override
    {
        ++deregisterCalls;
        if (!active.has_value()
            || *active != service) {
            return Result<void>::failure({
                QStringLiteral(
                    "test.registration_mismatch"),
                QStringLiteral(
                    "The DNS-SD registration changed unexpectedly."),
                false,
                {},
            });
        }
        active.reset();
        return Result<void>::success();
    }

    int registerCalls = 0;
    int deregisterCalls = 0;
    std::optional<WindowsDnsSdService>
        active;
};

class FakeNetworkProfileApi final
    : public IWindowsNetworkProfileApi {
public:
    explicit FakeNetworkProfileApi(
        WindowsNetworkProfile profile)
        : profile_(profile)
    {
    }

    Result<WindowsNetworkProfile>
    currentProfile() const override
    {
        return Result<WindowsNetworkProfile>::
            success(profile_);
    }

    void setChangeCallback(
        std::function<void()> callback)
        override
    {
        callback_ = std::move(callback);
    }

    void setProfile(
        WindowsNetworkProfile profile)
    {
        profile_ = profile;
        if (callback_) {
            callback_();
        }
    }

private:
    WindowsNetworkProfile profile_;
    std::function<void()> callback_;
};

QFuture<Result<void>> readyResult(
    Result<void> result)
{
    QPromise<Result<void>> promise;
    promise.start();
    QFuture<Result<void>> future =
        promise.future();
    promise.addResult(
        std::move(result));
    promise.finish();
    return future;
}

struct BootstrapEndpointProbe final {
    bool started = false;
    int stopCalls = 0;
};

class FakeBootstrapEndpoint final
    : public RelayPairingBootstrapEndpoint {
public:
    explicit FakeBootstrapEndpoint(
        std::shared_ptr<
            BootstrapEndpointProbe> probe)
        : probe_(std::move(probe))
    {
    }

    void start() override
    {
        probe_->started = true;
    }

    void stop() override
    {
        ++probe_->stopCalls;
    }

    QFuture<Result<void>> send(
        const EncryptedEnvelope&) override
    {
        return readyResult(
            Result<void>::success());
    }

private:
    std::shared_ptr<
        BootstrapEndpointProbe> probe_;
};

QSslConfiguration testTlsConfiguration()
{
    const QString fixtureRoot =
        QStringLiteral(
            COMPANION_QTWEBSOCKET_FIXTURE_ROOT);
    QFile certificateFile(
        fixtureRoot
        + QStringLiteral(
            "/localhost.cert"));
    QFile keyFile(
        fixtureRoot
        + QStringLiteral(
            "/localhost.key"));
    if (!certificateFile.open(
            QIODevice::ReadOnly)
        || !keyFile.open(
            QIODevice::ReadOnly)) {
        return {};
    }

    const QSslCertificate certificate(
        &certificateFile,
        QSsl::Pem);
    const QSslKey key(
        &keyFile,
        QSsl::Rsa,
        QSsl::Pem);
    QSslConfiguration configuration =
        QSslConfiguration::
            defaultConfiguration();
    configuration.setPeerVerifyMode(
        QSslSocket::VerifyNone);
    configuration.setLocalCertificate(
        certificate);
    configuration.setPrivateKey(key);
    return configuration;
}

QString fingerprint(
    const QSslConfiguration& configuration)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            configuration
                .localCertificate()
                .toDer(),
            QCryptographicHash::Sha256)
            .toHex());
}

QFuture<BridgeResponse> readyResponse(
    QString,
    BridgeRequest request)
{
    BridgeResponse response;
    response.id = request.id;
    response.operation =
        request.operation;
    response.succeeded = true;

    QPromise<BridgeResponse> promise;
    promise.start();
    QFuture<BridgeResponse> future =
        promise.future();
    promise.addResult(
        std::move(response));
    promise.finish();
    return future;
}

CompanionMobileHostConfiguration
configuration(
    const QTemporaryDir& directory,
    bool enabled)
{
    CompanionMobileHostConfiguration value;
    value.enabled = enabled;
    value.installationId =
        QStringLiteral(
            "11111111-2222-3333-4444-555555555555");
    value.computerName =
        QStringLiteral("Harlin-PC");
    value.hostDisplayName =
        QStringLiteral(
            "Codex Companion Windows");
    value.sslConfiguration =
        testTlsConfiguration();
    value.tlsFingerprintSha256 =
        fingerprint(
            value.sslConfiguration);
    value.pairingRecordsPath =
        directory.filePath(
            QStringLiteral(
                "paired-devices.json"));
    value.relayStatePath =
        directory.filePath(
            QStringLiteral(
                "relay-state.json"));
    value.transferRootPath =
        directory.filePath(
            QStringLiteral(
                "transfers"));
    return value;
}

} // namespace

class CompanionMobileHostTests final
    : public QObject {
    Q_OBJECT

private slots:
    void disabledHostOpensNoListenerOrDiscovery()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        auto dns =
            std::make_unique<
                FakeDnsSdApi>();
        FakeDnsSdApi* const dnsApi =
            dns.get();
        auto network =
            std::make_unique<
                FakeNetworkProfileApi>(
                WindowsNetworkProfile::
                    Private);

        CompanionMobileHostDependencies
            dependencies;
        dependencies.secretProtector =
            std::make_unique<
                TestProtector>();
        dependencies.dnsSdApi =
            std::move(dns);
        dependencies.networkProfileApi =
            std::move(network);

        auto created =
            CompanionMobileHost::create(
                configuration(
                    directory,
                    false),
                &readyResponse,
                std::move(dependencies));
        QVERIFY(created.hasValue());
        std::unique_ptr<
            CompanionMobileHost> host =
            std::move(created.value());

        QVERIFY(host->start().hasValue());
        QVERIFY(!host->isListening());
        QVERIFY(!host->isAdvertising());
        QCOMPARE(dnsApi->registerCalls, 0);
    }

    void discoveryTracksPrivateDomainAndPublicProfiles()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());
        QVERIFY(!tls.privateKey().isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        auto dns =
            std::make_unique<
                FakeDnsSdApi>();
        FakeDnsSdApi* const dnsApi =
            dns.get();
        auto network =
            std::make_unique<
                FakeNetworkProfileApi>(
                WindowsNetworkProfile::
                    Private);
        FakeNetworkProfileApi*
            const networkApi =
                network.get();

        CompanionMobileHostDependencies
            dependencies;
        dependencies.secretProtector =
            std::make_unique<
                TestProtector>();
        dependencies.dnsSdApi =
            std::move(dns);
        dependencies.networkProfileApi =
            std::move(network);

        auto created =
            CompanionMobileHost::create(
                configuration(
                    directory,
                    true),
                &readyResponse,
                std::move(dependencies));
        QVERIFY(created.hasValue());
        std::unique_ptr<
            CompanionMobileHost> host =
            std::move(created.value());

        QVERIFY(host->start().hasValue());
        QVERIFY(host->isListening());
        QVERIFY(host->isAdvertising());
        QVERIFY(host->nearbyAccessAvailable());
        QCOMPARE(
            host->networkProfile(),
            WindowsNetworkProfile::Private);
        QCOMPARE(dnsApi->registerCalls, 1);
        QVERIFY(dnsApi->active.has_value());
        QCOMPARE(
            dnsApi->active->port,
            host->serverPort());
        QCOMPARE(
            dnsApi->active->txt.value(
                QStringLiteral("id")),
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555"));

        networkApi->setProfile(
            WindowsNetworkProfile::
                Public);
        QTRY_VERIFY(
            !host->isAdvertising());
        QTRY_VERIFY(
            !host->isListening());
        QTRY_VERIFY(
            !host->nearbyAccessAvailable());
        QCOMPARE(
            host->networkProfile(),
            WindowsNetworkProfile::Public);
        QCOMPARE(
            dnsApi->deregisterCalls,
            1);

        QVERIFY(
            host->applyConfiguration(
                    true,
                    true,
                    std::nullopt)
                .hasValue());
        QTRY_VERIFY(
            host->isAdvertising());
        QTRY_VERIFY(
            host->isListening());
        QVERIFY(
            host->nearbyAccessAvailable());
        QVERIFY(
            host->
                allowsNearbyOnPublicNetworks());
        QCOMPARE(dnsApi->registerCalls, 2);

        QVERIFY(
            host->applyConfiguration(
                    true,
                    false,
                    std::nullopt)
                .hasValue());
        QTRY_VERIFY(
            !host->isAdvertising());
        QTRY_VERIFY(
            !host->isListening());
        QVERIFY(
            !host->
                 allowsNearbyOnPublicNetworks());
        QCOMPARE(
            dnsApi->deregisterCalls,
            2);

        networkApi->setProfile(
            WindowsNetworkProfile::
                Domain);
        QTRY_VERIFY(
            host->isAdvertising());
        QCOMPARE(dnsApi->registerCalls, 3);
        QVERIFY(host->isListening());
        QVERIFY(host->nearbyAccessAvailable());
        QCOMPARE(
            host->networkProfile(),
            WindowsNetworkProfile::Domain);
    }

    void runningHostAppliesAvailabilityAndRelayChangesWithoutRestart()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        auto dns =
            std::make_unique<
                FakeDnsSdApi>();
        FakeDnsSdApi* const dnsApi =
            dns.get();
        auto network =
            std::make_unique<
                FakeNetworkProfileApi>(
                WindowsNetworkProfile::
                    Private);

        CompanionMobileHostDependencies
            dependencies;
        dependencies.secretProtector =
            std::make_unique<
                TestProtector>();
        dependencies.dnsSdApi =
            std::move(dns);
        dependencies.networkProfileApi =
            std::move(network);

        auto created =
            CompanionMobileHost::create(
                configuration(
                    directory,
                    false),
                &readyResponse,
                std::move(dependencies));
        QVERIFY(created.hasValue());
        std::unique_ptr<
            CompanionMobileHost> host =
            std::move(created.value());
        QVERIFY(host->start().hasValue());

        const QUrl relay(
            QStringLiteral(
                "wss://relay.example.test/socket"));
        QVERIFY(
            host->applyConfiguration(
                    true,
                    false,
                    relay)
                .hasValue());
        QVERIFY(host->isListening());
        QVERIFY(host->isAdvertising());
        QCOMPARE(dnsApi->registerCalls, 1);
        QCOMPARE(
            host->configuredRelayUrl(),
            std::optional<QUrl>(relay));

        QVERIFY(
            host->applyConfiguration(
                    false,
                    false,
                    std::nullopt)
                .hasValue());
        QVERIFY(!host->isEnabled());
        QVERIFY(!host->isListening());
        QVERIFY(!host->isAdvertising());
        QVERIFY(!host->nearbyAccessAvailable());
        QCOMPARE(dnsApi->deregisterCalls, 1);
        QCOMPARE(
            host->configuredRelayUrl(),
            std::optional<QUrl>());
    }

    void relayPairingBootstrapStopsWhenMobileIsDisabledOrHostStops()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        auto dns =
            std::make_unique<
                FakeDnsSdApi>();
        auto network =
            std::make_unique<
                FakeNetworkProfileApi>(
                WindowsNetworkProfile::
                    Private);
        QVector<std::shared_ptr<
            BootstrapEndpointProbe>> probes;

        CompanionMobileHostDependencies
            dependencies;
        dependencies.secretProtector =
            std::make_unique<
                TestProtector>();
        dependencies.dnsSdApi =
            std::move(dns);
        dependencies.networkProfileApi =
            std::move(network);
        dependencies
            .relayPairingBootstrap
            .endpointFactory =
            [&probes](
                QUrl,
                QString,
                QString) {
                auto probe =
                    std::make_shared<
                        BootstrapEndpointProbe>();
                probes.append(probe);
                return std::make_unique<
                    FakeBootstrapEndpoint>(
                    probe);
            };
        dependencies
            .relayPairingBootstrap
            .secretGenerator = [] {
                return Result<QByteArray>::
                    success(
                        QByteArray(
                            32,
                            '\x42'));
            };

        CompanionMobileHostConfiguration
            requested =
                configuration(
                    directory,
                    true);
        const QUrl relay(
            QStringLiteral(
                "wss://relay.example.test/socket"));
        requested.relayUrl = relay;
        auto created =
            CompanionMobileHost::create(
                std::move(requested),
                &readyResponse,
                std::move(dependencies));
        QVERIFY(created.hasValue());
        std::unique_ptr<
            CompanionMobileHost> host =
            std::move(created.value());
        QVERIFY(host->start().hasValue());

        const auto first =
            host->relayPairingBootstrap()
                .beginPairing();
        QVERIFY(first.hasValue());
        QCOMPARE(probes.size(), 1);
        QVERIFY(probes.constFirst()->started);

        QVERIFY(
            host->applyConfiguration(
                    false,
                    false,
                    relay)
                .hasValue());
        QVERIFY(
            !host->relayPairingBootstrap()
                 .activeOffer()
                 .has_value());
        QCOMPARE(
            probes.constFirst()->stopCalls,
            1);

        QVERIFY(
            host->applyConfiguration(
                    true,
                    false,
                    relay)
                .hasValue());
        const auto second =
            host->relayPairingBootstrap()
                .beginPairing();
        QVERIFY(second.hasValue());
        QCOMPARE(probes.size(), 2);

        host->stop();
        QVERIFY(
            !host->relayPairingBootstrap()
                 .activeOffer()
                 .has_value());
        QCOMPARE(
            probes.constLast()->stopCalls,
            1);
    }
};

QTEST_GUILESS_MAIN(
    CompanionMobileHostTests)
#include "CompanionMobileHostTests.moc"
