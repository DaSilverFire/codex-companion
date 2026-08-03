#include "mobile/relay/RelayConnection.h"
#include "mobile/relay/RelayEndpointManager.h"
#include "mobile/relay/RelayModels.h"
#include "mobile/relay/RelayWireCodec.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingRecordStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFuture>
#include <QHostAddress>
#include <QPointer>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

#include <algorithm>
#include <limits>

namespace {

using namespace companion;

struct ReceivedWire final {
    int connectionIndex = -1;
    QByteArray bytes;
};

class FakeRelayServer final {
public:
    FakeRelayServer()
        : server_(
              QStringLiteral(
                  "Codex Companion relay test"),
              QWebSocketServer::NonSecureMode)
    {
        QObject::connect(
            &server_,
            &QWebSocketServer::newConnection,
            &server_,
            [this]() {
                QWebSocket* socket =
                    server_.nextPendingConnection();
                if (socket == nullptr) {
                    return;
                }
                const int connectionIndex =
                    totalConnections_++;
                ++activeConnections_;
                sockets_.append(socket);
                requestUrls_.append(
                    socket->requestUrl());

                QObject::connect(
                    socket,
                    &QWebSocket::
                        textMessageReceived,
                    socket,
                    [this,
                     connectionIndex](
                        const QString& text) {
                        received_.append({
                            connectionIndex,
                            text.toUtf8(),
                        });
                    });
                QObject::connect(
                    socket,
                    &QWebSocket::
                        binaryMessageReceived,
                    socket,
                    [this,
                     connectionIndex](
                        const QByteArray& bytes) {
                        received_.append({
                            connectionIndex,
                            bytes,
                        });
                    });
                QObject::connect(
                    socket,
                    &QWebSocket::disconnected,
                    socket,
                    [this, socket]() {
                        --activeConnections_;
                        socket->deleteLater();
                    });
            });
    }

    ~FakeRelayServer()
    {
        for (const QPointer<QWebSocket>& socket :
             sockets_) {
            if (socket != nullptr) {
                socket->abort();
            }
        }
        server_.close();
    }

    bool listen()
    {
        return server_.listen(
            QHostAddress::LocalHost,
            0);
    }

    QUrl url(
        const QString& query = {}) const
    {
        QUrl value;
        value.setScheme(
            QStringLiteral("ws"));
        value.setHost(
            QStringLiteral("127.0.0.1"));
        value.setPort(
            server_.serverPort());
        value.setPath(
            QStringLiteral("/relay"));
        value.setQuery(query);
        return value;
    }

    int totalConnections() const
    {
        return totalConnections_;
    }

    int activeConnections() const
    {
        return activeConnections_;
    }

    int receivedCount() const
    {
        return received_.size();
    }

    const QVector<ReceivedWire>&
    received() const
    {
        return received_;
    }

    const QVector<QUrl>& requestUrls() const
    {
        return requestUrls_;
    }

    Result<RelayWireMessage> decoded(
        int index) const
    {
        if (index < 0
            || index >= received_.size()) {
            return Result<RelayWireMessage>::
                failure({
                    QStringLiteral(
                        "test.missing_message"),
                    QStringLiteral(
                        "The requested relay test message does not exist."),
                    false,
                    {},
                });
        }
        return RelayWireCodec::decode(
            received_.at(index).bytes);
    }

    bool send(
        RelayWireMessage message)
    {
        const auto encoded =
            RelayWireCodec::encode(message);
        if (!encoded.hasValue()) {
            return false;
        }
        return sendRaw(encoded.value());
    }

    bool sendRaw(const QByteArray& bytes)
    {
        QWebSocket* socket =
            latestSocket();
        if (socket == nullptr) {
            return false;
        }
        return socket->sendTextMessage(
                   QString::fromUtf8(bytes))
            == bytes.size();
    }

    void closeLatest()
    {
        QWebSocket* socket =
            latestSocket();
        if (socket != nullptr) {
            socket->close();
        }
    }

private:
    QWebSocket* latestSocket() const
    {
        for (auto iterator =
                 sockets_.crbegin();
             iterator != sockets_.crend();
             ++iterator) {
            if (*iterator != nullptr
                && (*iterator)->state()
                    != QAbstractSocket::
                        UnconnectedState) {
                return *iterator;
            }
        }
        return nullptr;
    }

    QWebSocketServer server_;
    QVector<QPointer<QWebSocket>> sockets_;
    QVector<QUrl> requestUrls_;
    QVector<ReceivedWire> received_;
    int totalConnections_ = 0;
    int activeConnections_ = 0;
};

RelayConnectionTiming fastTiming()
{
    RelayConnectionTiming timing;
    timing.packetResultTimeoutMilliseconds =
        250;
    timing.pingIntervalMilliseconds =
        25;
    timing.reconnectDelayMilliseconds = {
        10,
        20,
        40,
        80,
        160,
        300,
    };
    return timing;
}

EncryptedEnvelope outboundEnvelope()
{
    return {
        1,
        QStringLiteral("channel"),
        QStringLiteral("windows-endpoint"),
        1,
        1'700'000'000'000,
        QByteArrayLiteral("sealed-outbound"),
    };
}

EncryptedEnvelope inboundEnvelope(
    QString senderId =
        QStringLiteral("iphone-alpha"))
{
    return {
        1,
        QStringLiteral("channel"),
        std::move(senderId),
        2,
        1'700'000'000'001,
        QByteArrayLiteral("sealed-inbound"),
    };
}

RelayWireMessage registeredMessage()
{
    RelayWireMessage message;
    message.type =
        RelayWireType::Registered;
    return message;
}

RelayWireMessage peerPresenceMessage(
    int peerCount)
{
    RelayWireMessage message;
    message.type =
        RelayWireType::PeerPresence;
    message.peerCount = peerCount;
    return message;
}

RelayWireMessage packetResultMessage(
    QString packetId,
    RelayPacketResultStatus status)
{
    RelayWireMessage message;
    message.type =
        RelayWireType::PacketResult;
    message.packetId =
        std::move(packetId);
    message.status = status;
    return message;
}

void establishRegisteredConnection(
    FakeRelayServer& server,
    RelayConnection& connection)
{
    connection.start();
    QTRY_COMPARE_WITH_TIMEOUT(
        server.totalConnections(),
        1,
        1'000);
    QTRY_COMPARE_WITH_TIMEOUT(
        server.receivedCount(),
        1,
        1'000);
    QVERIFY(
        server.send(
            registeredMessage()));
    QVERIFY(
        server.send(
            peerPresenceMessage(1)));
    QTRY_COMPARE_WITH_TIMEOUT(
        connection.state(),
        RelayConnectionState::Registered,
        1'000);
}

QByteArray packetIdFromMessage(
    const FakeRelayServer& server,
    int messageIndex)
{
    const auto decoded =
        server.decoded(messageIndex);
    if (!decoded.hasValue()
        || !decoded.value()
                .packetId.has_value()) {
        return {};
    }
    return decoded.value()
        .packetId->toUtf8();
}

PairingRecord pairingRecord(
    QString deviceId,
    char secretFill)
{
    return PairingRecord(
        std::move(deviceId),
        QStringLiteral("Phone"),
        QByteArray(32, secretFill),
        QDateTime::fromMSecsSinceEpoch(
            1'700'000'000'000,
            QTimeZone::UTC));
}

} // namespace

class RelayConnectionTests final
    : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<
            RelayConnectionState>();
        qRegisterMetaType<
            EncryptedEnvelope>();
        qRegisterMetaType<
            CompanionError>();
    }

    void lifecycleRegistersFirstAndUsesOneChannelQuery()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(
                QStringLiteral(
                    "token=abc&channel=stale"
                    "&feature=1&channel=older")),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy stateSpy(
            &connection,
            &RelayConnection::stateChanged);

        const auto beforeStart =
            connection.send(
                outboundEnvelope());
        QVERIFY(beforeStart.isFinished());
        QVERIFY(
            !beforeStart.result()
                 .hasValue());
        QCOMPARE(
            beforeStart.result()
                .error()
                .code,
            QStringLiteral(
                "relay.not_registered"));

        connection.start();
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            1,
            1'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            1,
            1'000);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);

        const auto registration =
            server.decoded(0);
        QVERIFY(registration.hasValue());
        QCOMPARE(
            registration.value().type,
            RelayWireType::Register);
        QCOMPARE(
            registration.value().channelId,
            std::optional<QString>(
                QStringLiteral("channel")));
        QCOMPARE(
            registration.value().endpointId,
            std::optional<QString>(
                QStringLiteral(
                    "windows-endpoint")));

        QCOMPARE(
            server.requestUrls().size(),
            1);
        const QUrlQuery query(
            server.requestUrls().first());
        QCOMPARE(
            query.queryItemValue(
                QStringLiteral("token")),
            QStringLiteral("abc"));
        QCOMPARE(
            query.queryItemValue(
                QStringLiteral("feature")),
            QStringLiteral("1"));
        QCOMPARE(
            query.allQueryItemValues(
                QStringLiteral("channel")),
            QStringList{
                QStringLiteral("channel")});

        QVERIFY(
            server.send(
                registeredMessage()));
        QTest::qWait(10);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);

        QVERIFY(
            server.send(
                peerPresenceMessage(0)));
        QTest::qWait(10);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);

        QVERIFY(
            server.send(
                peerPresenceMessage(1)));
        QTRY_COMPARE_WITH_TIMEOUT(
            connection.state(),
            RelayConnectionState::Registered,
            1'000);
        QVERIFY(stateSpy.count() >= 2);

        const auto firstStop =
            connection.stop();
        const auto secondStop =
            connection.stop();
        QVERIFY(firstStop.isFinished());
        QVERIFY(secondStop.isFinished());
        QVERIFY(
            firstStop.result().hasValue());
        QVERIFY(
            secondStop.result().hasValue());
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Stopped);
    }

    void sendResolvesAcceptedUndeliverableAndTimeoutExactlyOnce()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        establishRegisteredConnection(
            server,
            connection);

        const int acceptedIndex =
            server.receivedCount();
        auto acceptedFuture =
            connection.send(
                outboundEnvelope());
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            acceptedIndex + 1,
            1'000);
        QVERIFY(!acceptedFuture.isFinished());
        const QByteArray acceptedId =
            packetIdFromMessage(
                server,
                acceptedIndex);
        QVERIFY(!acceptedId.isEmpty());
        QVERIFY(
            server.send(
                packetResultMessage(
                    QString::fromUtf8(
                        acceptedId),
                    RelayPacketResultStatus::
                        Accepted)));
        QTRY_VERIFY_WITH_TIMEOUT(
            acceptedFuture.isFinished(),
            1'000);
        QVERIFY(
            acceptedFuture.result()
                .hasValue());

        const int unavailableIndex =
            server.receivedCount();
        auto unavailableFuture =
            connection.send(
                outboundEnvelope());
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            unavailableIndex + 1,
            1'000);
        const QByteArray unavailableId =
            packetIdFromMessage(
                server,
                unavailableIndex);
        QVERIFY(!unavailableId.isEmpty());
        QVERIFY(
            server.send(
                packetResultMessage(
                    QString::fromUtf8(
                        unavailableId),
                    RelayPacketResultStatus::
                        Undeliverable)));
        QTRY_VERIFY_WITH_TIMEOUT(
            unavailableFuture.isFinished(),
            1'000);
        QVERIFY(
            !unavailableFuture.result()
                 .hasValue());
        QCOMPARE(
            unavailableFuture.result()
                .error()
                .code,
            QStringLiteral(
                "relay.peer_unavailable"));
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);

        QVERIFY(
            server.send(
                peerPresenceMessage(1)));
        QTRY_COMPARE_WITH_TIMEOUT(
            connection.state(),
            RelayConnectionState::Registered,
            1'000);

        const int timeoutIndex =
            server.receivedCount();
        auto timeoutFuture =
            connection.send(
                outboundEnvelope());
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            timeoutIndex + 1,
            1'000);
        const QByteArray timeoutId =
            packetIdFromMessage(
                server,
                timeoutIndex);
        QVERIFY(!timeoutId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            timeoutFuture.isFinished(),
            1'000);
        QVERIFY(
            !timeoutFuture.result()
                 .hasValue());
        QCOMPARE(
            timeoutFuture.result()
                .error()
                .code,
            QStringLiteral(
                "relay.packet_result_timeout"));
        QVERIFY(
            server.send(
                packetResultMessage(
                    QString::fromUtf8(
                        timeoutId),
                    RelayPacketResultStatus::
                        Accepted)));
        QTest::qWait(20);
        QVERIFY(timeoutFuture.isFinished());
        QCOMPARE(
            timeoutFuture.result()
                .error()
                .code,
            QStringLiteral(
                "relay.packet_result_timeout"));

        const int packetCountBeforeReconnect =
            server.receivedCount();
        server.closeLatest();
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            2,
            1'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            packetCountBeforeReconnect + 1,
            1'000);
        const auto registration =
            server.decoded(
                server.receivedCount() - 1);
        QVERIFY(registration.hasValue());
        QCOMPARE(
            registration.value().type,
            RelayWireType::Register);
    }

    void receivesPacketsAnswersPingAndRejectsInvalidWire()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy envelopeSpy(
            &connection,
            &RelayConnection::
                envelopeReceived);
        QSignalSpy reconnectSpy(
            &connection,
            &RelayConnection::
                reconnectScheduled);
        establishRegisteredConnection(
            server,
            connection);

        const int pongIndex =
            server.receivedCount();
        QVERIFY(
            server.send(
                RelayModels::ping()));
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            pongIndex + 1,
            1'000);
        const auto pong =
            server.decoded(pongIndex);
        QVERIFY(pong.hasValue());
        QCOMPARE(
            pong.value().type,
            RelayWireType::Pong);

        const auto packet =
            RelayModels::packet(
                inboundEnvelope(),
                QStringLiteral(
                    "incoming-packet"));
        QVERIFY(packet.hasValue());
        QVERIFY(
            server.send(packet.value()));
        QTRY_COMPARE_WITH_TIMEOUT(
            envelopeSpy.count(),
            1,
            1'000);
        const auto received =
            qvariant_cast<
                EncryptedEnvelope>(
                envelopeSpy.at(0).at(0));
        QCOMPARE(
            received,
            inboundEnvelope());

        QVERIFY(
            server.sendRaw(
                QByteArrayLiteral("{")));
        QTRY_COMPARE_WITH_TIMEOUT(
            reconnectSpy.count(),
            1,
            1'000);
        QCOMPARE(
            reconnectSpy.at(0)
                .at(0)
                .toInt(),
            10);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            2,
            1'000);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);
    }

    void bootstrapUnknownSenderDoesNotWeakenPairedEndpoints()
    {
        FakeRelayServer bootstrapServer;
        QVERIFY(bootstrapServer.listen());
        RelayConnection bootstrapConnection(
            bootstrapServer.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            RelaySenderMode::BootstrapUnknown,
            fastTiming());
        QSignalSpy bootstrapEnvelopeSpy(
            &bootstrapConnection,
            &RelayConnection::envelopeReceived);
        establishRegisteredConnection(
            bootstrapServer,
            bootstrapConnection);

        const auto bootstrapPacket =
            RelayModels::packet(
                inboundEnvelope(
                    QStringLiteral(
                        "iphone-new")),
                QStringLiteral(
                    "bootstrap-packet"));
        QVERIFY(bootstrapPacket.hasValue());
        QVERIFY(
            bootstrapServer.send(
                bootstrapPacket.value()));
        QTRY_COMPARE_WITH_TIMEOUT(
            bootstrapEnvelopeSpy.count(),
            1,
            1'000);

        FakeRelayServer pairedServer;
        QVERIFY(pairedServer.listen());
        RelayConnection pairedConnection(
            pairedServer.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy pairedEnvelopeSpy(
            &pairedConnection,
            &RelayConnection::envelopeReceived);
        QSignalSpy pairedReconnectSpy(
            &pairedConnection,
            &RelayConnection::reconnectScheduled);
        establishRegisteredConnection(
            pairedServer,
            pairedConnection);

        const auto wrongSenderPacket =
            RelayModels::packet(
                inboundEnvelope(
                    QStringLiteral(
                        "iphone-new")),
                QStringLiteral(
                    "wrong-sender-packet"));
        QVERIFY(wrongSenderPacket.hasValue());
        QVERIFY(
            pairedServer.send(
                wrongSenderPacket.value()));
        QTRY_COMPARE_WITH_TIMEOUT(
            pairedReconnectSpy.count(),
            1,
            1'000);
        QCOMPARE(
            pairedEnvelopeSpy.count(),
            0);
    }

    void transportPingStartsAfterRegistrationAcknowledgement()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy pingSpy(
            &connection,
            &RelayConnection::
                transportPingSent);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            1,
            1'000);
        QTest::qWait(60);
        QCOMPARE(pingSpy.count(), 0);

        QVERIFY(
            server.send(
                registeredMessage()));
        QTRY_VERIFY_WITH_TIMEOUT(
            pingSpy.count() >= 2,
            1'000);
    }

    void oversizedPacketFailsBeforeTransport()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnectionTiming timing =
            fastTiming();
        timing.packetIdGenerator = [] {
            return QStringLiteral(
                "oversized-packet");
        };
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            timing);
        establishRegisteredConnection(
            server,
            connection);

        const int beforeSend =
            server.receivedCount();
        EncryptedEnvelope envelope =
            outboundEnvelope();
        envelope.sealedPayload =
            QByteArray(
                RelayConnection::
                    kMaximumMessageBytes,
                'x');
        const auto future =
            connection.send(envelope);
        QVERIFY(future.isFinished());
        QVERIFY(!future.result().hasValue());
        QCOMPARE(
            future.result().error().code,
            QStringLiteral(
                "relay.payload_too_large"));
        QCOMPARE(
            server.receivedCount(),
            beforeSend);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Registered);
    }

    void bareRelayErrorUsesFallbackAndReconnects()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy failureSpy(
            &connection,
            &RelayConnection::
                failureOccurred);
        QSignalSpy reconnectSpy(
            &connection,
            &RelayConnection::
                reconnectScheduled);
        establishRegisteredConnection(
            server,
            connection);

        RelayWireMessage error;
        error.type = RelayWireType::Error;
        QVERIFY(server.send(error));
        QTRY_COMPARE_WITH_TIMEOUT(
            reconnectSpy.count(),
            1,
            1'000);
        QCOMPARE(failureSpy.count(), 1);
        const CompanionError failure =
            qvariant_cast<CompanionError>(
                failureSpy.at(0).at(0));
        QCOMPARE(
            failure.code,
            QStringLiteral(
                "relay.rejected"));
        QCOMPARE(
            failure.message,
            QStringLiteral(
                "The relay rejected the connection."));
    }

    void reconnectBackoffMatchesV034AndResetsAfterRegistration()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            fastTiming());
        QSignalSpy reconnectSpy(
            &connection,
            &RelayConnection::
                reconnectScheduled);
        connection.start();

        const QList<int> expected{
            10, 20, 40, 80, 160, 300,
        };
        for (int index = 0;
             index < expected.size();
             ++index) {
            QTRY_COMPARE_WITH_TIMEOUT(
                server.totalConnections(),
                index + 1,
                2'000);
            server.closeLatest();
            QTRY_COMPARE_WITH_TIMEOUT(
                reconnectSpy.count(),
                index + 1,
                2'000);
            QCOMPARE(
                reconnectSpy.at(index)
                    .at(0)
                    .toInt(),
                expected.at(index));
        }

        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            expected.size() + 1,
            2'000);
        QVERIFY(
            server.send(
                registeredMessage()));
        QVERIFY(
            server.send(
                peerPresenceMessage(1)));
        QTRY_COMPARE_WITH_TIMEOUT(
            connection.state(),
            RelayConnectionState::Registered,
            1'000);
        server.closeLatest();
        QTRY_COMPARE_WITH_TIMEOUT(
            reconnectSpy.count(),
            expected.size() + 1,
            1'000);
        QCOMPARE(
            reconnectSpy.last()
                .at(0)
                .toInt(),
            expected.first());
    }

    void stopStartCancelsPriorReconnectGeneration()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayConnectionTiming timing =
            fastTiming();
        timing.reconnectDelayMilliseconds = {
            200,
        };
        RelayConnection connection(
            server.url(),
            QStringLiteral("channel"),
            QStringLiteral("windows-endpoint"),
            QStringLiteral("iphone-alpha"),
            timing);
        QSignalSpy reconnectSpy(
            &connection,
            &RelayConnection::
                reconnectScheduled);
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            1,
            1'000);

        server.closeLatest();
        QTRY_COMPARE_WITH_TIMEOUT(
            reconnectSpy.count(),
            1,
            1'000);
        QVERIFY(
            connection.stop()
                .result()
                .hasValue());
        connection.start();
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            2,
            1'000);
        QTest::qWait(250);
        QCOMPARE(
            server.totalConnections(),
            2);
        QCOMPARE(
            connection.state(),
            RelayConnectionState::Connecting);
    }

    void finalWireSizeBoundaryIsExact()
    {
        const auto maximum =
            RelayConnection::
                validateFinalMessageSize(
                    QByteArray(
                        RelayConnection::
                            kMaximumMessageBytes,
                        'x'));
        QVERIFY(maximum.hasValue());

        const auto oversized =
            RelayConnection::
                validateFinalMessageSize(
                    QByteArray(
                        RelayConnection::
                                kMaximumMessageBytes
                            + 1,
                        'x'));
        QVERIFY(!oversized.hasValue());
        QCOMPARE(
            oversized.error().code,
            QStringLiteral(
                "relay.payload_too_large"));
    }

    void endpointManagerReusesAndReplacesOnlyChangedRecords()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayEndpointManager manager(
            QStringLiteral("windows-endpoint"),
            fastTiming());

        QVector<PairingRecord> records{
            pairingRecord(
                QStringLiteral("iphone-a"),
                'a'),
            pairingRecord(
                QStringLiteral("iphone-b"),
                'b'),
        };
        manager.synchronize(
            records,
            server.url(
                QStringLiteral("token=abc")));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.endpointCount(),
            qsizetype(2),
            1'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            2,
            1'000);

        manager.synchronize(
            records,
            server.url(
                QStringLiteral("token=abc")));
        QTest::qWait(50);
        QCOMPARE(
            server.totalConnections(),
            2);

        records[0] =
            pairingRecord(
                QStringLiteral("iphone-a"),
                'c');
        manager.synchronize(
            records,
            server.url(
                QStringLiteral("token=abc")));
        QTRY_COMPARE_WITH_TIMEOUT(
            server.totalConnections(),
            3,
            1'000);
        QCOMPARE(
            manager.endpointCount(),
            qsizetype(2));

        records.removeLast();
        manager.synchronize(
            records,
            server.url(
                QStringLiteral("token=abc")));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.endpointCount(),
            qsizetype(1),
            1'000);

        manager.synchronize(
            records,
            std::nullopt);
        QCOMPARE(
            manager.endpointCount(),
            qsizetype(0));
        const auto stopped =
            manager.stopAll();
        QVERIFY(stopped.isFinished());
    }

    void endpointManagerSendsToTheSelectedPairedDevice()
    {
        FakeRelayServer server;
        QVERIFY(server.listen());
        RelayEndpointManager manager(
            QStringLiteral("windows-endpoint"),
            fastTiming());
        QSignalSpy stateSpy(
            &manager,
            &RelayEndpointManager::
                endpointStateChanged);
        const PairingRecord record =
            pairingRecord(
                QStringLiteral("iphone-a"),
                'a');
        manager.synchronize(
            {record},
            server.url());
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            1,
            1'000);
        QVERIFY(
            server.send(
                registeredMessage()));
        QVERIFY(
            server.send(
                peerPresenceMessage(1)));
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(
                stateSpy.cbegin(),
                stateSpy.cend(),
                [](const QList<QVariant>&
                       arguments) {
                    return arguments.size()
                            == 2
                        && arguments.at(0)
                               .toString()
                            == QStringLiteral(
                                "iphone-a")
                        && qvariant_cast<
                               RelayConnectionState>(
                               arguments.at(1))
                            == RelayConnectionState::
                                   Registered;
                }),
            1'000);

        const auto channel =
            BridgeSecurity::channelId(
                record.secret);
        QVERIFY(channel.hasValue());
        EncryptedEnvelope envelope =
            outboundEnvelope();
        envelope.channelId =
            channel.value();
        const int packetIndex =
            server.receivedCount();
        auto delivered =
            manager.send(
                QStringLiteral("iphone-a"),
                envelope);
        QTRY_COMPARE_WITH_TIMEOUT(
            server.receivedCount(),
            packetIndex + 1,
            1'000);
        const QByteArray packetId =
            packetIdFromMessage(
                server,
                packetIndex);
        QVERIFY(!packetId.isEmpty());
        QVERIFY(
            server.send(
                packetResultMessage(
                    QString::fromUtf8(
                        packetId),
                    RelayPacketResultStatus::
                        Accepted)));
        QTRY_VERIFY_WITH_TIMEOUT(
            delivered.isFinished(),
            1'000);
        QVERIFY(
            delivered.result().hasValue());

        const auto missing =
            manager.send(
                QStringLiteral(
                    "iphone-missing"),
                envelope);
        QVERIFY(missing.isFinished());
        QVERIFY(
            !missing.result().hasValue());
        QCOMPARE(
            missing.result().error().code,
            QStringLiteral(
                "relay.endpoint_unavailable"));
    }
};

QTEST_GUILESS_MAIN(RelayConnectionTests)
#include "RelayConnectionTests.moc"
