#include "platform/windows/mobile/WindowsDnsSdAdvertiser.h"
#include "platform/windows/mobile/WindowsNetworkProfileMonitor.h"
#include "platform/windows/mobile/WindowsTlsIdentityStore.h"

#include <QSslConfiguration>
#include <QtTest>

using namespace companion;

namespace {

class FakeDnsSdApi final : public IWindowsDnsSdApi {
public:
    Result<void> registerService(
        const WindowsDnsSdService& service) override
    {
        registered.append(service);
        return Result<void>::success();
    }

    Result<void> deregisterService(
        const WindowsDnsSdService& service) override
    {
        deregistered.append(service);
        return Result<void>::success();
    }

    QVector<WindowsDnsSdService> registered;
    QVector<WindowsDnsSdService> deregistered;
};

class FakeNetworkApi final
    : public IWindowsNetworkProfileApi {
public:
    Result<WindowsNetworkProfile> currentProfile()
        const override
    {
        return Result<WindowsNetworkProfile>::success(
            profile);
    }

    void setChangeCallback(
        std::function<void()> callback) override
    {
        onChanged = std::move(callback);
    }

    WindowsNetworkProfile profile =
        WindowsNetworkProfile::Unavailable;
    std::function<void()> onChanged;
};

class FakeTlsBackend final
    : public IWindowsTlsIdentityBackend {
public:
    Result<WindowsTlsIdentityMaterial> loadOrCreate(
        const WindowsTlsIdentityRequest& request) override
    {
        requests.append(request);
        return Result<WindowsTlsIdentityMaterial>::success(
            material);
    }

    QVector<WindowsTlsIdentityRequest> requests;
    WindowsTlsIdentityMaterial material;
};

QByteArray derBytes()
{
    return QByteArray::fromHex(
        "3082010a0282010100aabbccddeeff");
}

} // namespace

class WindowsNearbyPlatformTests final : public QObject {
    Q_OBJECT

private slots:
    void dnsSdAdvertisesExactTxtOnPrivateNetworks()
    {
        FakeDnsSdApi api;
        WindowsDnsSdAdvertiser advertiser(&api);

        const auto result = advertiser.update({
            QStringLiteral("Office PC"),
            QStringLiteral("install-123"),
            49152,
            QStringLiteral(
                "0123456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef"),
            WindowsNetworkProfile::Private,
        });

        QVERIFY(result.hasValue());
        QCOMPARE(api.registered.size(), 1);
        const WindowsDnsSdService service =
            api.registered.first();
        QCOMPARE(
            service.instanceName,
            QStringLiteral("Office PC"));
        QCOMPARE(
            service.serviceType,
            QStringLiteral(
                "_codex-companion._tcp.local"));
        QCOMPARE(service.port, quint16(49152));
        QCOMPARE(service.txt.value(QStringLiteral("pv")),
                 QStringLiteral("1"));
        QCOMPARE(service.txt.value(QStringLiteral("id")),
                 QStringLiteral("install-123"));
        QCOMPARE(
            service.txt.value(QStringLiteral("transport")),
            QStringLiteral("wss"));
        QCOMPARE(service.txt.value(QStringLiteral("path")),
                 QStringLiteral("/companion/v1"));
        QCOMPARE(service.txt.value(QStringLiteral("frame")),
                 QStringLiteral("1"));
        QCOMPARE(
            service.txt.value(QStringLiteral("tlsfp")),
            QStringLiteral(
                "0123456789abcdef0123456789abcdef"
                "0123456789abcdef0123456789abcdef"));
    }

    void dnsSdTrimsUtf8InstanceNameAndWithdrawsStaleRegistration()
    {
        FakeDnsSdApi api;
        WindowsDnsSdAdvertiser advertiser(&api);
        const QString longName =
            QStringLiteral("Desk-")
            + QString(80, QChar(0x00e9));

        QVERIFY(
            advertiser
                .update({
                    longName,
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Domain,
                })
                .hasValue());
        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("New Desk"),
                    QStringLiteral("install-123"),
                    40001,
                    QString(64, QLatin1Char('b')),
                    WindowsNetworkProfile::Domain,
                })
                .hasValue());

        QCOMPARE(api.registered.size(), 2);
        QCOMPARE(api.deregistered.size(), 1);
        QVERIFY(
            api.registered.first()
                .instanceName.toUtf8()
                .size()
            <= 63);
        QVERIFY(
            QString::fromUtf8(
                api.registered.first()
                    .instanceName.toUtf8())
            == api.registered.first().instanceName);
        QCOMPARE(
            api.deregistered.first().port,
            quint16(40000));
        QCOMPARE(
            api.registered.last().port,
            quint16(40001));
    }

    void dnsSdStopsOnPublicOrUnavailableNetworks()
    {
        FakeDnsSdApi api;
        WindowsDnsSdAdvertiser advertiser(&api);

        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("Desk"),
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Private,
                })
                .hasValue());
        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("Desk"),
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Public,
                })
                .hasValue());
        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("Desk"),
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Unavailable,
                })
                .hasValue());

        QCOMPARE(api.registered.size(), 1);
        QCOMPARE(api.deregistered.size(), 1);
        QVERIFY(!advertiser.isAdvertising());
    }

    void dnsSdAdvertisesOnPublicNetworkOnlyWithExplicitOptIn()
    {
        FakeDnsSdApi api;
        WindowsDnsSdAdvertiser advertiser(&api);

        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("Desk"),
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Public,
                    true,
                })
                .hasValue());

        QCOMPARE(api.registered.size(), 1);
        QVERIFY(advertiser.isAdvertising());

        QVERIFY(
            advertiser
                .update({
                    QStringLiteral("Desk"),
                    QStringLiteral("install-123"),
                    40000,
                    QString(64, QLatin1Char('a')),
                    WindowsNetworkProfile::Public,
                    false,
                })
                .hasValue());

        QCOMPARE(api.deregistered.size(), 1);
        QVERIFY(!advertiser.isAdvertising());
    }

    void networkProfileMonitorPublishesChanges()
    {
        FakeNetworkApi api;
        api.profile = WindowsNetworkProfile::Public;
        WindowsNetworkProfileMonitor monitor(&api);
        QSignalSpy spy(
            &monitor,
            &WindowsNetworkProfileMonitor::
                profileChanged);

        QCOMPARE(
            monitor.profile(),
            WindowsNetworkProfile::Public);

        api.profile = WindowsNetworkProfile::Private;
        QVERIFY(api.onChanged);
        api.onChanged();

        QCOMPARE(
            monitor.profile(),
            WindowsNetworkProfile::Private);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(
            spy.first().at(0).value<
                WindowsNetworkProfile>(),
            WindowsNetworkProfile::Private);
    }

    void tlsIdentityPublishesLowercaseFingerprintAndFallbackDiagnostics()
    {
        FakeTlsBackend backend;
        backend.material.certificateDer = derBytes();
        backend.material.configuration =
            QSslConfiguration::defaultConfiguration();
        backend.material.diagnostics = {
            true,
            false,
            true,
            QStringLiteral(
                "qt_schannel_requires_exportable_key_copy"),
        };
        WindowsTlsIdentityStore store(&backend);

        const auto identity =
            store.loadOrCreate(
                QStringLiteral("install-123"));

        QVERIFY(identity.hasValue());
        QCOMPARE(backend.requests.size(), 1);
        QCOMPARE(
            backend.requests.first().installationId,
            QStringLiteral("install-123"));
        QCOMPARE(
            identity.value().fingerprintSha256,
            QString::fromLatin1(
                QCryptographicHash::hash(
                    derBytes(),
                    QCryptographicHash::Sha256)
                    .toHex()));
        QCOMPARE(
            identity.value().fingerprintSha256,
            identity.value()
                .fingerprintSha256.toLower());
        QVERIFY(
            identity.value()
                .diagnostics
                .qtPrivateKeyFallbackUsed);
        QVERIFY(
            !identity.value()
                 .diagnostics
                 .nativePrivateKeyExportable);
        QVERIFY(
            !identity.value()
                 .diagnostics
                 .reason.contains(
                     QStringLiteral("BEGIN PRIVATE KEY")));
    }
};

QTEST_GUILESS_MAIN(WindowsNearbyPlatformTests)

#include "WindowsNearbyPlatformTests.moc"
