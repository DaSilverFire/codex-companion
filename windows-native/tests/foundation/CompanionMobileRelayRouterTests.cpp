#include "app/CompanionMobileRelayRouter.h"

#include "codex/models/BridgeJsonCodec.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "mobile/security/SecretProtector.h"

#include <QDateTime>
#include <QFuture>
#include <QPromise>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace companion;

namespace {

class TestProtector final
    : public SecretProtector {
public:
    Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView entropy)
        const override
    {
        QByteArray protectedBytes(
            "test:");
        protectedBytes.append(
            entropy.data(),
            entropy.size());
        protectedBytes.append(':');
        protectedBytes.append(
            plaintext.data(),
            plaintext.size());
        return Result<QByteArray>::success(
            std::move(protectedBytes));
    }

    Result<QByteArray> unprotect(
        QByteArrayView protectedBytes,
        QByteArrayView entropy)
        const override
    {
        QByteArray prefix("test:");
        prefix.append(
            entropy.data(),
            entropy.size());
        prefix.append(':');
        if (!protectedBytes.startsWith(
                prefix)) {
            return Result<QByteArray>::failure({
                QStringLiteral(
                    "test.unprotect_failed"),
                QStringLiteral(
                    "The test payload is invalid."),
                false,
                {},
            });
        }
        return Result<QByteArray>::success(
            protectedBytes.sliced(
                prefix.size())
                .toByteArray());
    }
};

template <typename T>
QFuture<T> readyFuture(T value)
{
    QPromise<T> promise;
    promise.start();
    QFuture<T> future =
        promise.future();
    promise.addResult(
        std::move(value));
    promise.finish();
    return future;
}

PairingRecord record(
    QByteArray secret =
        QByteArray(32, '\x41'))
{
    return {
        QStringLiteral("iphone-a"),
        QStringLiteral("Harlin iPhone"),
        std::move(secret),
        QDateTime::fromMSecsSinceEpoch(
            1'700'000'000'000,
            QTimeZone::UTC),
    };
}

BridgeRequest request()
{
    BridgeRequest value;
    value.id = QUuid(
        QStringLiteral(
            "11111111-2222-3333-4444-555555555555"));
    value.operation =
        BridgeOperation::ListTasks;
    return value;
}

BridgeResponse responseFor(
    const BridgeRequest& source)
{
    BridgeResponse response;
    response.id = source.id;
    response.operation =
        source.operation;
    response.succeeded = true;
    response.message =
        QStringLiteral("routed");
    return response;
}

EncryptedEnvelope sealedRequest(
    const BridgeRequest& value,
    QByteArrayView secret,
    quint64 sequence = 7)
{
    const auto encoded =
        BridgeJsonCodec::encodeRequest(
            value,
            BridgeWireProfile::
                RelayV1Canonical);
    Q_ASSERT(encoded.hasValue());
    const auto sealed =
        BridgeSecurity::seal(
            encoded.value(),
            secret,
            QStringLiteral("iphone-a"),
            sequence,
            1'700'000'000'100);
    Q_ASSERT(sealed.hasValue());
    return sealed.value();
}

} // namespace

class CompanionMobileRelayRouterTests final
    : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<
            CompanionError>();
    }

    void authenticatesDispatchesAndEncryptsTheResponse()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired.json")),
            protector);
        PairingCoordinator pairing(
            pairingStore);
        const PairingRecord trusted =
            record();
        QVERIFY(
            pairing.remember(
                trusted)
                .hasValue());
        RelayStateStore relayState(
            directory.filePath(
                QStringLiteral(
                    "relay-state.json")),
            protector);

        int handlerCalls = 0;
        QString handledDevice;
        QVector<EncryptedEnvelope> sent;
        QString sentDevice;
        CompanionMobileRelayRouter router(
            QStringLiteral(
                "windows-endpoint"),
            pairing,
            relayState,
            [&handlerCalls,
             &handledDevice](
                QString deviceId,
                BridgeRequest incoming) {
                ++handlerCalls;
                handledDevice =
                    std::move(deviceId);
                return readyFuture(
                    responseFor(
                        incoming));
            },
            [&sent,
             &sentDevice](
                QString deviceId,
                EncryptedEnvelope envelope) {
                sentDevice =
                    std::move(deviceId);
                sent.append(
                    std::move(envelope));
                return readyFuture(
                    Result<void>::
                        success());
            },
            [] {
                return qint64(
                    1'700'000'000'200);
            });

        const BridgeRequest incoming =
            request();
        const EncryptedEnvelope envelope =
            sealedRequest(
                incoming,
                trusted.secret);
        router.receive(
            QStringLiteral("iphone-a"),
            envelope);

        QTRY_COMPARE_WITH_TIMEOUT(
            sent.size(),
            1,
            1'000);
        QCOMPARE(handlerCalls, 1);
        QCOMPARE(
            handledDevice,
            QStringLiteral("iphone-a"));
        QCOMPARE(
            sentDevice,
            QStringLiteral("iphone-a"));
        QCOMPARE(
            sent.first().senderId,
            QStringLiteral(
                "windows-endpoint"));
        QCOMPARE(
            sent.first()
                .sentAtMilliseconds,
            qint64(
                1'700'000'000'200));

        auto plaintext =
            BridgeSecurity::open(
                sent.first(),
                trusted.secret);
        QVERIFY(plaintext.hasValue());
        const auto decoded =
            BridgeJsonCodec::
                decodeResponse(
                    plaintext.value(),
                    BridgeWireProfile::
                        RelayV1Canonical);
        QVERIFY(decoded.hasValue());
        QCOMPARE(
            decoded.value().id,
            incoming.id);
        QVERIFY(
            decoded.value().succeeded);
        QCOMPARE(
            decoded.value().message,
            std::optional<QString>(
                QStringLiteral(
                    "routed")));

        router.receive(
            QStringLiteral("iphone-a"),
            envelope);
        QTest::qWait(50);
        QCOMPARE(handlerCalls, 1);
        QCOMPARE(sent.size(), 1);
    }

    void rejectsWrongIdentityAndDropsStalePairingResponses()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired.json")),
            protector);
        PairingCoordinator pairing(
            pairingStore);
        const PairingRecord original =
            record();
        QVERIFY(
            pairing.remember(
                original)
                .hasValue());
        RelayStateStore relayState(
            directory.filePath(
                QStringLiteral(
                    "relay-state.json")),
            protector);

        auto promise =
            std::make_shared<
                QPromise<BridgeResponse>>();
        promise->start();
        int handlerCalls = 0;
        int sendCalls = 0;
        CompanionMobileRelayRouter router(
            QStringLiteral(
                "windows-endpoint"),
            pairing,
            relayState,
            [&handlerCalls,
             promise](
                QString,
                BridgeRequest) {
                ++handlerCalls;
                return promise->future();
            },
            [&sendCalls](
                QString,
                EncryptedEnvelope) {
                ++sendCalls;
                return readyFuture(
                    Result<void>::
                        success());
            });

        EncryptedEnvelope wrongSender =
            sealedRequest(
                request(),
                original.secret,
                8);
        wrongSender.senderId =
            QStringLiteral("iphone-b");
        router.receive(
            QStringLiteral("iphone-a"),
            wrongSender);
        QTest::qWait(25);
        QCOMPARE(handlerCalls, 0);

        router.receive(
            QStringLiteral("iphone-a"),
            sealedRequest(
                request(),
                original.secret,
                9));
        QTRY_COMPARE_WITH_TIMEOUT(
            handlerCalls,
            1,
            1'000);
        QVERIFY(
            pairing.remember(
                record(
                    QByteArray(
                        32,
                        '\x52')))
                .hasValue());
        promise->addResult(
            responseFor(request()));
        promise->finish();
        QTest::qWait(50);
        QCOMPARE(sendCalls, 0);
    }
};

QTEST_GUILESS_MAIN(
    CompanionMobileRelayRouterTests)
#include "CompanionMobileRelayRouterTests.moc"
