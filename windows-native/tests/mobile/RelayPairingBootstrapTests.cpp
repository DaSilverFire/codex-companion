#include "mobile/relay/RelayPairingBootstrap.h"

#include "mobile/relay/RelayModels.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "mobile/security/SecretProtector.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QPromise>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace companion;

namespace {

constexpr qint64 kNowMilliseconds =
    1'777'777'777'000;

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

struct EndpointProbe final {
    bool started = false;
    int stopCalls = 0;
    QVector<EncryptedEnvelope> sent;
};

class FakeBootstrapEndpoint final
    : public RelayPairingBootstrapEndpoint {
public:
    explicit FakeBootstrapEndpoint(
        std::shared_ptr<EndpointProbe> probe,
        QObject* parent = nullptr)
        : RelayPairingBootstrapEndpoint(parent),
          probe_(std::move(probe))
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
        const EncryptedEnvelope& envelope)
        override
    {
        probe_->sent.append(envelope);
        return readyResult(
            Result<void>::success());
    }

    void raiseEnvelope(
        EncryptedEnvelope envelope)
    {
        emit envelopeReceived(
            std::move(envelope));
    }

    void raiseFailure(
        CompanionError error)
    {
        emit failureOccurred(
            std::move(error));
    }

private:
    std::shared_ptr<EndpointProbe> probe_;
};

QByteArray invitationJson(
    const BridgeInvitation& invitation)
{
    QJsonObject object{
        {
            QStringLiteral("version"),
            invitation.version,
        },
        {
            QStringLiteral("deviceID"),
            invitation.deviceId,
        },
        {
            QStringLiteral("displayName"),
            invitation.displayName,
        },
        {
            QStringLiteral(
                "issuedAtMilliseconds"),
            static_cast<double>(
                invitation
                    .issuedAtMilliseconds),
        },
        {
            QStringLiteral("nonce"),
            QString::fromLatin1(
                invitation.nonce
                    .toBase64()),
        },
    };
    if (invitation.authenticator
            .has_value()) {
        object.insert(
            QStringLiteral(
                "authenticator"),
            QString::fromLatin1(
                invitation
                    .authenticator
                    ->toBase64()));
    }
    if (invitation.pairingCode
            .has_value()) {
        object.insert(
            QStringLiteral(
                "pairingCode"),
            *invitation.pairingCode);
    }
    return QJsonDocument(object)
        .toJson(QJsonDocument::Compact);
}

QByteArray pairingRequestJson(
    const QUuid& id,
    const BridgeInvitation& invitation)
{
    const QJsonObject invitationObject =
        QJsonDocument::fromJson(
            invitationJson(invitation))
            .object();
    return QJsonDocument(
        QJsonObject{
            {
                QStringLiteral("id"),
                id.toString(
                    QUuid::WithoutBraces),
            },
            {
                QStringLiteral("invitation"),
                invitationObject,
            },
            {
                QStringLiteral("version"),
                1,
            },
        })
        .toJson(QJsonDocument::Compact);
}

BridgeInvitation pairingInvitation(
    QString deviceId,
    QString pairingCode)
{
    return {
        BridgeSecurity::invitationVersion,
        std::move(deviceId),
        QStringLiteral("Harlin iPhone"),
        kNowMilliseconds,
        QByteArray(16, '\x22'),
        std::nullopt,
        std::move(pairingCode),
    };
}

class BootstrapFixture final {
public:
    BootstrapFixture()
        : pairingStore(
              directory.filePath(
                  QStringLiteral(
                      "paired-devices.v1.json")),
              protector),
          pairingCoordinator(
              pairingStore,
              [] {
                  return QDateTime::
                      fromMSecsSinceEpoch(
                          kNowMilliseconds,
                          QTimeZone::UTC);
              },
              [] {
                  return Result<QString>::
                      success(
                          QStringLiteral(
                              "123456"));
              },
              [this] {
                  return Result<QByteArray>::
                      success(
                          longTermSecret);
              }),
          relayStateStore(
              directory.filePath(
                  QStringLiteral(
                      "relay-state.v1.json")),
              protector)
    {
        RelayPairingBootstrapDependencies
            dependencies;
        dependencies.endpointFactory =
            [this](
                QUrl url,
                QString channelId,
                QString endpointId) {
                endpointUrl =
                    std::move(url);
                endpointChannelId =
                    std::move(channelId);
                endpointIdValue =
                    std::move(endpointId);
                auto probe =
                    std::make_shared<
                        EndpointProbe>();
                endpointProbes.append(probe);
                auto endpoint =
                    std::make_unique<
                        FakeBootstrapEndpoint>(
                        probe);
                currentEndpoint =
                    endpoint.get();
                return endpoint;
            };
        dependencies.clock = [] {
            return QDateTime::
                fromMSecsSinceEpoch(
                    kNowMilliseconds,
                    QTimeZone::UTC);
        };
        dependencies.secretGenerator =
            [this] {
                return Result<QByteArray>::
                    success(
                        bootstrapSecret);
            };
        dependencies.secretEraser =
            [this](QByteArray& secret) {
                ++secretEraseCalls;
                secret.fill('\0');
            };
        bootstrap =
            std::make_unique<
                RelayPairingBootstrap>(
                pairingCoordinator,
                relayStateStore,
                hostDeviceId,
                QStringLiteral(
                    "Windows workstation"),
                std::move(dependencies));
        bootstrap->setRelayUrl(
            relayUrl);
    }

    Result<RelayPairingBootstrapOffer>
    begin()
    {
        return bootstrap->beginPairing();
    }

    EncryptedEnvelope envelope(
        const RelayPairingBootstrapOffer&
            offer,
        const BridgeInvitation& invitation,
        QString senderId,
        quint64 sequence = 1)
    {
        return BridgeSecurity::seal(
                   pairingRequestJson(
                       QUuid(
                           QStringLiteral(
                               "11111111-2222-3333-4444-555555555555")),
                       invitation),
                   offer.bootstrapSecret,
                   std::move(senderId),
                   sequence,
                   kNowMilliseconds)
            .value();
    }

    QTemporaryDir directory;
    TestProtector protector;
    PairingRecordStore pairingStore;
    PairingCoordinator pairingCoordinator;
    RelayStateStore relayStateStore;
    QByteArray bootstrapSecret =
        QByteArray::fromHex(
            "202122232425262728292A2B2C2D2E2F"
            "303132333435363738393A3B3C3D3E3F");
    QByteArray longTermSecret =
        QByteArray(32, '\x41');
    QString hostDeviceId =
        QStringLiteral(
            "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
    QUrl relayUrl =
        QUrl(QStringLiteral(
            "wss://relay.example.test/socket"));
    std::unique_ptr<RelayPairingBootstrap>
        bootstrap;
    QVector<std::shared_ptr<EndpointProbe>>
        endpointProbes;
    QPointer<FakeBootstrapEndpoint>
        currentEndpoint;
    QUrl endpointUrl;
    QString endpointChannelId;
    QString endpointIdValue;
    int secretEraseCalls = 0;
};

} // namespace

class RelayPairingBootstrapTests final
    : public QObject {
    Q_OBJECT

private slots:
    void offerCodecUsesExactShortLivedLink()
    {
        RelayPairingBootstrapOffer offer;
        offer.version = 1;
        offer.relayUrl = QUrl(
            QStringLiteral(
                "wss://relay.example.test/socket"));
        offer.hostDeviceId =
            QStringLiteral(
                "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
        offer.hostDisplayName =
            QStringLiteral(
                "Windows workstation");
        offer.pairingCode =
            QStringLiteral("123 456");
        offer.expiresAtMilliseconds =
            1'800'000'000'000;
        offer.bootstrapSecret =
            QByteArray::fromHex(
                "0102030405060708090A0B0C0D0E0F10"
                "1112131415161718191A1B1C1D1E1F20");

        const auto link =
            offer.pairingLink();
        QVERIFY(link.hasValue());
        QCOMPARE(
            link.value(),
            QStringLiteral(
                "codex-companion://pair?payload=eyJ2ZXJzaW9uIjoxLCJyZWxheVVSTFN0cmluZyI6IndzczovL3JlbGF5LmV4YW1wbGUudGVzdC9zb2NrZXQiLCJtYWNEZXZpY2VJRCI6IkFBQUFBQUFBLUJCQkItQ0NDQy1ERERELUVFRUVFRUVFRUVFRSIsIm1hY05hbWUiOiJXaW5kb3dzIHdvcmtzdGF0aW9uIiwicGFpcmluZ0NvZGUiOiIxMjM0NTYiLCJleHBpcmVzQXRNaWxsaXNlY29uZHMiOjE4MDAwMDAwMDAwMDAsImJvb3RzdHJhcFNlY3JldCI6IkFRSURCQVVHQndnSkNnc01EUTRQRUJFU0V4UVZGaGNZR1JvYkhCMGVIeUEifQ"));

        const auto parsed =
            RelayPairingBootstrapOffer::parse(
                link.value());
        QVERIFY(parsed.hasValue());
        QCOMPARE(parsed.value().version, 1);
        QCOMPARE(
            parsed.value().relayUrl,
            offer.relayUrl);
        QCOMPARE(
            parsed.value().hostDeviceId,
            offer.hostDeviceId);
        QCOMPARE(
            parsed.value().hostDisplayName,
            offer.hostDisplayName);
        QCOMPARE(
            parsed.value().pairingCode,
            QStringLiteral("123456"));
        QCOMPARE(
            parsed.value()
                .expiresAtMilliseconds,
            offer.expiresAtMilliseconds);
        QCOMPARE(
            parsed.value().bootstrapSecret,
            offer.bootstrapSecret);
    }

    void offerCodecRejectsUnsafeOrMalformedCredentials()
    {
        RelayPairingBootstrapOffer offer;
        offer.relayUrl = QUrl(
            QStringLiteral(
                "ws://relay.example.test/socket"));
        offer.hostDeviceId =
            QStringLiteral("invalid id");
        offer.hostDisplayName =
            QStringLiteral("Windows");
        offer.pairingCode =
            QStringLiteral("12345");
        offer.expiresAtMilliseconds =
            1'800'000'000'000;
        offer.bootstrapSecret =
            QByteArray(31, '\x42');
        QVERIFY(
            !offer.pairingLink()
                 .hasValue());

        const auto missingPayload =
            RelayPairingBootstrapOffer::parse(
                QStringLiteral(
                    "codex-companion://pair"));
        QVERIFY(!missingPayload.hasValue());

        BootstrapFixture fixture;
        const auto valid = fixture.begin();
        QVERIFY(valid.hasValue());
        const auto validLink =
            valid.value().pairingLink();
        QVERIFY(validLink.hasValue());
        const QString corrupted =
            validLink.value()
            + QStringLiteral("!");
        QVERIFY(
            !RelayPairingBootstrapOffer::
                 parse(corrupted)
                 .hasValue());
    }

    void beginUsesFiveMinuteLifetimeAndReplacementCancelsOldEndpoint()
    {
        BootstrapFixture fixture;
        const auto first = fixture.begin();
        QVERIFY(first.hasValue());
        QCOMPARE(
            first.value()
                .expiresAtMilliseconds,
            kNowMilliseconds + 300'000);
        QCOMPARE(
            fixture.endpointProbes.size(),
            1);
        QVERIFY(
            fixture.endpointProbes
                .at(0)
                ->started);
        QCOMPARE(
            fixture.endpointUrl,
            fixture.relayUrl);
        QCOMPARE(
            fixture.endpointIdValue,
            fixture.hostDeviceId);
        QVERIFY(
            !fixture.endpointChannelId
                 .isEmpty());

        const auto second = fixture.begin();
        QVERIFY(second.hasValue());
        QCOMPARE(
            fixture.endpointProbes.size(),
            2);
        QCOMPARE(
            fixture.endpointProbes
                .at(0)
                ->stopCalls,
            1);
        QVERIFY(
            fixture.secretEraseCalls >= 1);

        fixture.bootstrap->cancelPairing();
        QVERIFY(
            !fixture.bootstrap
                 ->activeOffer()
                 .has_value());
        QCOMPARE(
            fixture.endpointProbes
                .at(1)
                ->stopCalls,
            1);
        QVERIFY(
            fixture.secretEraseCalls >= 2);
    }

    void senderMismatchAndReplayAreRejected()
    {
        BootstrapFixture fixture;
        const auto offer = fixture.begin();
        QVERIFY(offer.hasValue());
        QVERIFY(fixture.currentEndpoint);

        const BridgeInvitation invitation =
            pairingInvitation(
                QStringLiteral("iphone-1"),
                QStringLiteral("000000"));
        const EncryptedEnvelope mismatch =
            fixture.envelope(
                offer.value(),
                invitation,
                QStringLiteral(
                    "different-device"),
                1);
        fixture.currentEndpoint
            ->raiseEnvelope(mismatch);
        QTest::qWait(50);
        QCOMPARE(
            fixture.endpointProbes
                .constLast()
                ->sent.size(),
            0);

        const EncryptedEnvelope rejected =
            fixture.envelope(
                offer.value(),
                invitation,
                invitation.deviceId,
                2);
        fixture.currentEndpoint
            ->raiseEnvelope(rejected);
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.endpointProbes
                .constLast()
                ->sent.size(),
            1,
            1'000);
        const auto plaintext =
            BridgeSecurity::open(
                fixture.endpointProbes
                    .constLast()
                    ->sent.constFirst(),
                offer.value()
                    .bootstrapSecret);
        QVERIFY(plaintext.hasValue());
        const QJsonObject response =
            QJsonDocument::fromJson(
                plaintext.value())
                .object();
        QVERIFY(
            !response.value(
                 QStringLiteral(
                     "succeeded"))
                 .toBool(true));
        QCOMPARE(
            response.value(
                QStringLiteral(
                    "errorCode"))
                .toString(),
            QStringLiteral(
                "pairing_rejected"));

        fixture.currentEndpoint
            ->raiseEnvelope(rejected);
        QTest::qWait(50);
        QCOMPARE(
            fixture.endpointProbes
                .constLast()
                ->sent.size(),
            1);
    }

    void successfulPairingReturnsAndStoresLongTermSecret()
    {
        BootstrapFixture fixture;
        const auto offer = fixture.begin();
        QVERIFY(offer.hasValue());
        QVERIFY(fixture.currentEndpoint);

        const BridgeInvitation invitation =
            pairingInvitation(
                QStringLiteral("iphone-1"),
                QStringLiteral("123 456"));
        fixture.currentEndpoint
            ->raiseEnvelope(
                fixture.envelope(
                    offer.value(),
                    invitation,
                    invitation.deviceId));
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.endpointProbes
                .constLast()
                ->sent.size(),
            1,
            1'000);

        const auto plaintext =
            BridgeSecurity::open(
                fixture.endpointProbes
                    .constLast()
                    ->sent.constFirst(),
                offer.value()
                    .bootstrapSecret);
        QVERIFY(plaintext.hasValue());
        const QJsonObject response =
            QJsonDocument::fromJson(
                plaintext.value())
                .object();
        QVERIFY(
            response.value(
                QStringLiteral(
                    "succeeded"))
                .toBool());
        QCOMPARE(
            response.value(
                QStringLiteral(
                    "macDeviceID"))
                .toString(),
            fixture.hostDeviceId);
        QCOMPARE(
            response.value(
                QStringLiteral(
                    "relayURLString"))
                .toString(),
            fixture.relayUrl.toString(
                QUrl::FullyEncoded));
        QCOMPARE(
            QByteArray::fromBase64(
                response.value(
                    QStringLiteral(
                        "pairingSecret"))
                    .toString()
                    .toLatin1()),
            fixture.longTermSecret);

        QTRY_VERIFY_WITH_TIMEOUT(
            !fixture.bootstrap
                 ->activeOffer()
                 .has_value(),
            1'000);
        const auto stored =
            fixture.pairingCoordinator
                .trustedRecord(
                    invitation.deviceId);
        QVERIFY(stored.has_value());
        QCOMPARE(
            stored->secret,
            fixture.longTermSecret);
        QCOMPARE(
            stored->relayUrlString,
            std::optional<QString>(
                fixture.relayUrl.toString(
                    QUrl::FullyEncoded)));
        QCOMPARE(
            fixture.endpointProbes
                .constLast()
                ->stopCalls,
            1);
    }

    void endpointFailureLeavesOfferActive()
    {
        BootstrapFixture fixture;
        const auto offer = fixture.begin();
        QVERIFY(offer.hasValue());
        QVERIFY(fixture.currentEndpoint);

        fixture.currentEndpoint
            ->raiseFailure({
                QStringLiteral(
                    "relay.transport_failed"),
                QStringLiteral(
                    "transport unavailable"),
                true,
                {},
            });

        QVERIFY(
            fixture.bootstrap
                ->activeOffer()
                .has_value());
        QVERIFY(
            fixture.bootstrap
                ->lastError()
                .has_value());
        QVERIFY(
            fixture.bootstrap
                ->lastError()
                ->message
                .startsWith(
                    QStringLiteral(
                        "Secure mobile pairing failed: ")));
    }

    void expiryAndDestructionStopEndpointAndEraseSecret()
    {
        BootstrapFixture fixture;
        const auto offer =
            fixture.bootstrap
                ->beginPairing(
                    std::chrono::seconds(1));
        QVERIFY(offer.hasValue());
        QTRY_VERIFY_WITH_TIMEOUT(
            !fixture.bootstrap
                 ->activeOffer()
                 .has_value(),
            1'500);
        QCOMPARE(
            fixture.endpointProbes
                .constLast()
                ->stopCalls,
            1);
        QVERIFY(
            fixture.secretEraseCalls >= 1);

        const auto second = fixture.begin();
        QVERIFY(second.hasValue());
        const auto lastProbe =
            fixture.endpointProbes
                .constLast();
        const int erasesBefore =
            fixture.secretEraseCalls;
        fixture.bootstrap.reset();
        QCOMPARE(lastProbe->stopCalls, 1);
        QVERIFY(
            fixture.secretEraseCalls
            > erasesBefore);
    }
};

QTEST_GUILESS_MAIN(
    RelayPairingBootstrapTests)
#include "RelayPairingBootstrapTests.moc"
