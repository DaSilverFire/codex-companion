#include "mobile/relay/RelayAudit.h"
#include "mobile/relay/RelayModels.h"
#include "mobile/relay/RelaySettings.h"
#include "mobile/relay/RelayWireCodec.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtTest>

#include <limits>

namespace {

using namespace companion;

QJsonObject packetFixture()
{
    QFile file(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral(
            "/mobile-v034/relay-wire-packet-v034.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(
        file.readAll()).object();
}

EncryptedEnvelope fixtureEnvelope()
{
    return {
        1,
        QStringLiteral(
            "eSyhONfJ1y6Jx2xFO1drQDpNAcFv_zWi"),
        QStringLiteral("iphone-fixture"),
        42,
        1'700'000'000'123,
        QByteArray::fromBase64(
            QByteArrayLiteral(
                "AAECAwQFBgcICQoL8tlhblpjxC7DoQXD"
                "tD9jBroD04A0VpebkKcB9lv0hEFL4Trm"
                "ka30S6IwP9ekZYEK")),
    };
}

QJsonObject decodedObject(const QByteArray& bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

QStringList sortedKeys(const QJsonObject& object)
{
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}

} // namespace

class RelayModelsTests final : public QObject {
    Q_OBJECT

private slots:
    void exactWireNamesRoundTrip_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("wire");

        QTest::newRow("register")
            << int(RelayWireType::Register)
            << QStringLiteral("register");
        QTest::newRow("registered")
            << int(RelayWireType::Registered)
            << QStringLiteral("registered");
        QTest::newRow("peer-presence")
            << int(RelayWireType::PeerPresence)
            << QStringLiteral("peerPresence");
        QTest::newRow("packet")
            << int(RelayWireType::Packet)
            << QStringLiteral("packet");
        QTest::newRow("packet-result")
            << int(RelayWireType::PacketResult)
            << QStringLiteral("packetResult");
        QTest::newRow("ping")
            << int(RelayWireType::Ping)
            << QStringLiteral("ping");
        QTest::newRow("pong")
            << int(RelayWireType::Pong)
            << QStringLiteral("pong");
        QTest::newRow("error")
            << int(RelayWireType::Error)
            << QStringLiteral("error");
    }

    void exactWireNamesRoundTrip()
    {
        QFETCH(int, type);
        QFETCH(QString, wire);

        const RelayWireType relayType =
            RelayWireType(type);
        QCOMPARE(
            RelayModels::wireName(relayType),
            wire);
        const auto decoded =
            RelayModels::wireType(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(*decoded, relayType);
    }

    void exactPacketResultNamesRoundTrip_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("wire");

        QTest::newRow("accepted")
            << int(RelayPacketResultStatus::Accepted)
            << QStringLiteral("accepted");
        QTest::newRow("undeliverable")
            << int(
                   RelayPacketResultStatus::
                       Undeliverable)
            << QStringLiteral("undeliverable");
    }

    void exactPacketResultNamesRoundTrip()
    {
        QFETCH(int, status);
        QFETCH(QString, wire);

        const RelayPacketResultStatus resultStatus =
            RelayPacketResultStatus(status);
        QCOMPARE(
            RelayModels::wireName(resultStatus),
            wire);
        const auto decoded =
            RelayModels::packetResultStatus(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(*decoded, resultStatus);
    }

    void packetMatchesV034CanonicalEnvelopeFixture()
    {
        const QJsonObject fixture = packetFixture();
        QVERIFY(!fixture.isEmpty());

        const auto packet = RelayModels::packet(
            fixtureEnvelope(),
            fixture.value(QStringLiteral("packetID"))
                .toString());
        QVERIFY(packet.hasValue());
        QVERIFY(packet.value().envelope.has_value());
        QCOMPARE(
            *packet.value().envelope,
            fixture
                .value(QStringLiteral("envelopeJSON"))
                .toString()
                .toUtf8());

        const auto encoded =
            RelayWireCodec::encode(packet.value());
        QVERIFY(encoded.hasValue());
        const QJsonObject object =
            decodedObject(encoded.value());
        QCOMPARE(
            object.value(QStringLiteral("envelope"))
                .toString(),
            fixture
                .value(QStringLiteral("envelopeBase64"))
                .toString());

        const auto decoded =
            RelayWireCodec::decode(encoded.value());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value(), packet.value());

        const auto envelope =
            RelayModels::decodedEnvelope(
                decoded.value());
        QVERIFY(envelope.hasValue());
        QCOMPARE(
            envelope.value(),
            fixtureEnvelope());
    }

    void packetEnvelopePreservesFullIntegerRange()
    {
        EncryptedEnvelope envelope =
            fixtureEnvelope();
        envelope.sequence =
            std::numeric_limits<quint64>::max();
        envelope.sentAtMilliseconds =
            std::numeric_limits<qint64>::min();

        const auto encoded =
            RelayWireCodec::encodeEnvelope(envelope);
        QVERIFY(encoded.hasValue());
        QVERIFY(encoded.value().contains(
            QByteArrayLiteral(
                "\"sequence\":18446744073709551615")));
        QVERIFY(encoded.value().contains(
            QByteArrayLiteral(
                "\"sentAtMilliseconds\":"
                "-9223372036854775808")));

        const auto decoded =
            RelayWireCodec::decodeEnvelope(
                encoded.value());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value(), envelope);
    }

    void protocolVersionIsRequiredAndMustEqualOne()
    {
        const QList<QByteArray> invalid{
            QByteArrayLiteral(
                "{\"type\":\"ping\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":2,"
                "\"type\":\"ping\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":1.0,"
                "\"type\":\"ping\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":\"1\","
                "\"type\":\"ping\"}"),
        };

        for (const QByteArray& bytes : invalid) {
            const auto decoded =
                RelayWireCodec::decode(bytes);
            QVERIFY(!decoded.hasValue());
            QCOMPARE(
                decoded.error().code,
                QStringLiteral(
                    "relay.invalid_wire_message"));
        }
    }

    void unknownAndUnusedKnownFieldsAreIgnoredButRequiredFieldsAreEnforced()
    {
        const auto ping = RelayWireCodec::decode(
            QByteArrayLiteral(
                "{\"channelID\":\"unused-channel\","
                "\"code\":\"unused-code\","
                "\"endpointID\":\"unused-endpoint\","
                "\"envelope\":\"e30=\","
                "\"future\":true,"
                "\"message\":\"unused-message\","
                "\"packetID\":\"unused-packet\","
                "\"peerCount\":1,"
                "\"protocolVersion\":1,"
                "\"senderID\":\"unused-sender\","
                "\"status\":\"accepted\","
                "\"type\":\"ping\"}"));
        QVERIFY(ping.hasValue());
        QCOMPARE(
            ping.value().type,
            RelayWireType::Ping);

        const auto bareError =
            RelayWireCodec::decode(
                QByteArrayLiteral(
                    "{\"protocolVersion\":1,"
                    "\"type\":\"error\"}"));
        QVERIFY(bareError.hasValue());
        QCOMPARE(
            bareError.value().type,
            RelayWireType::Error);
        QVERIFY(!bareError.value().code.has_value());
        QVERIFY(!bareError.value().message.has_value());

        RelayWireMessage encodedBareError;
        encodedBareError.type =
            RelayWireType::Error;
        const auto bareErrorBytes =
            RelayWireCodec::encode(
                encodedBareError);
        QVERIFY(bareErrorBytes.hasValue());

        const QList<QByteArray> invalid{
            QByteArrayLiteral(
                "{\"protocolVersion\":1,"
                "\"type\":\"register\","
                "\"channelID\":\"channel\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":1,"
                "\"type\":\"peerPresence\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":1,"
                "\"type\":\"packetResult\","
                "\"packetID\":\"packet\"}"),
            QByteArrayLiteral(
                "{\"protocolVersion\":1,"
                "\"type\":\"packet\"}"),
        };
        for (const QByteArray& bytes : invalid) {
            QVERIFY(
                !RelayWireCodec::decode(bytes)
                     .hasValue());
        }
    }

    void opaquePacketIdsUseOnlyTheV034Alphabet()
    {
        QVERIFY(RelayModels::isValidOpaqueId(
            QStringLiteral("A_z-09")));
        QVERIFY(RelayModels::isValidOpaqueId(
            QString(128, QLatin1Char('a'))));

        const QStringList invalid{
            QString(),
            QString(129, QLatin1Char('a')),
            QStringLiteral("has space"),
            QStringLiteral("slash/value"),
            QStringLiteral("cafe\u00E9"),
            QStringLiteral("emoji\U0001F680"),
        };
        for (const QString& value : invalid) {
            QVERIFY(
                !RelayModels::isValidOpaqueId(
                    value));
            const auto packet =
                RelayModels::packet(
                    fixtureEnvelope(),
                    value);
            QVERIFY(!packet.hasValue());
        }
    }

    void malformedPacketsAndMetadataMismatchFailClosed()
    {
        const auto packet = RelayModels::packet(
            fixtureEnvelope(),
            QStringLiteral("packet-1"));
        QVERIFY(packet.hasValue());
        const auto encoded =
            RelayWireCodec::encode(packet.value());
        QVERIFY(encoded.hasValue());

        QJsonObject badBase64 =
            decodedObject(encoded.value());
        badBase64.insert(
            QStringLiteral("envelope"),
            QStringLiteral("not base64!"));
        QVERIFY(
            !RelayWireCodec::decode(
                 QJsonDocument(badBase64).toJson(
                     QJsonDocument::Compact))
                 .hasValue());

        QJsonObject mismatch =
            decodedObject(encoded.value());
        mismatch.insert(
            QStringLiteral("senderID"),
            QStringLiteral("other-sender"));
        const auto decodedMismatch =
            RelayWireCodec::decode(
                QJsonDocument(mismatch).toJson(
                    QJsonDocument::Compact));
        QVERIFY(!decodedMismatch.hasValue());
        QCOMPARE(
            decodedMismatch.error().code,
            QStringLiteral(
                "relay.metadata_mismatch"));

        RelayWireMessage incomplete =
            packet.value();
        incomplete.envelope.reset();
        QVERIFY(
            !RelayWireCodec::encode(incomplete)
                 .hasValue());
    }

    void registrationAndKeepaliveHaveMinimalFields()
    {
        const auto registration =
            RelayModels::registration(
                QStringLiteral("channel"),
                QStringLiteral("endpoint"));
        QVERIFY(registration.hasValue());
        const auto registrationBytes =
            RelayWireCodec::encode(
                registration.value());
        QVERIFY(registrationBytes.hasValue());
        QCOMPARE(
            sortedKeys(
                decodedObject(
                    registrationBytes.value())),
            QStringList({
                QStringLiteral("channelID"),
                QStringLiteral("endpointID"),
                QStringLiteral("protocolVersion"),
                QStringLiteral("type"),
            }));

        for (const RelayWireMessage message :
             {RelayModels::ping(),
              RelayModels::pong()}) {
            const auto bytes =
                RelayWireCodec::encode(message);
            QVERIFY(bytes.hasValue());
            QCOMPARE(
                sortedKeys(
                    decodedObject(bytes.value())),
                QStringList({
                    QStringLiteral(
                        "protocolVersion"),
                    QStringLiteral("type"),
                }));
        }
    }

    void routingMatchesV034Precedence()
    {
        QCOMPARE(
            RelayModels::preferredRoute(
                true, true, true),
            MobileTransportRoute::Nearby);
        QCOMPARE(
            RelayModels::preferredRoute(
                true, false, false),
            MobileTransportRoute::Nearby);
        QCOMPARE(
            RelayModels::preferredRoute(
                false, true, true),
            MobileTransportRoute::Relay);
        QCOMPARE(
            RelayModels::preferredRoute(
                false, true, false),
            MobileTransportRoute::Unavailable);
        QCOMPARE(
            RelayModels::preferredRoute(
                false, false, true),
            MobileTransportRoute::Unavailable);
    }

    void relaySettingsSelectAndValidateEndpoints()
    {
        QCOMPARE(
            RelaySettings::configurationKey(),
            QStringLiteral(
                "CompanionRelayURL"));
        const RelaySettings bundled =
            RelaySettings::
                fromBundledConfiguration();
        QCOMPARE(
            bundled.bundledUrl(),
            QStringLiteral(
                "wss://codex-companion-relay."
                "silverfire-codex-companion."
                "workers.dev/relay"));
        QVERIFY(
            RelaySettings::validatedUrl(
                bundled.bundledUrl())
                .hasValue());

        RelaySettings relaySettings(
            QStringLiteral(
                "wss://relay.example.test/socket"));
        AppSettings settings;

        const auto automatic =
            relaySettings.configuredUrl(settings);
        QVERIFY(automatic.hasValue());
        QVERIFY(automatic.value().has_value());
        QCOMPARE(
            automatic.value()->toString(),
            QStringLiteral(
                "wss://relay.example.test/socket"));

        settings.relayMode = RelayMode::Disabled;
        const auto disabled =
            relaySettings.configuredUrl(settings);
        QVERIFY(disabled.hasValue());
        QVERIFY(!disabled.value().has_value());

        settings.relayMode = RelayMode::Custom;
        settings.customRelayUrl =
            QStringLiteral(
                "wss://custom.example.test/path");
        const auto custom =
            relaySettings.configuredUrl(settings);
        QVERIFY(custom.hasValue());
        QCOMPARE(
            custom.value()->toString(),
            settings.customRelayUrl);

        for (const QString& value :
             {QStringLiteral(
                  "ws://localhost:9000/socket"),
              QStringLiteral(
                  "ws://127.0.0.1/socket"),
              QStringLiteral(
                  "ws://[::1]/socket")}) {
            QVERIFY(
                RelaySettings::validatedUrl(value)
                    .hasValue());
        }
        for (const QString& value :
             {QStringLiteral(
                  "ws://relay.example.test/socket"),
              QStringLiteral(
                  "http://relay.example.test"),
              QStringLiteral("wss:///missing-host"),
              QStringLiteral("not a url")}) {
            QVERIFY(
                !RelaySettings::validatedUrl(value)
                     .hasValue());
        }
    }

    void invalidCustomInputDoesNotMutatePriorSettings()
    {
        RelaySettings relaySettings(
            QStringLiteral(
                "wss://relay.example.test/socket"));
        AppSettings settings;
        settings.relayMode = RelayMode::Custom;
        settings.customRelayUrl =
            QStringLiteral(
                "wss://prior.example.test/socket");
        const AppSettings prior = settings;

        const auto invalid =
            relaySettings.withCustomUrl(
                settings,
                QStringLiteral(
                    "ws://remote.example.test/socket"));
        QVERIFY(!invalid.hasValue());
        QCOMPARE(settings, prior);

        const auto valid =
            relaySettings.withCustomUrl(
                settings,
                QStringLiteral(
                    "  wss://next.example.test/socket  "));
        QVERIFY(valid.hasValue());
        QCOMPARE(
            valid.value().relayMode,
            RelayMode::Custom);
        QCOMPARE(
            valid.value().customRelayUrl,
            QStringLiteral(
                "wss://next.example.test/socket"));

        const AppSettings automatic =
            relaySettings.useAutomatic(
                valid.value());
        QCOMPARE(
            automatic.relayMode,
            RelayMode::Automatic);
        QVERIFY(
            automatic.customRelayUrl.isEmpty());
    }

    void channelQueryPreservesUnrelatedItemsAndReplacesStaleValues()
    {
        const auto configured =
            RelaySettings::withChannel(
                QUrl(QStringLiteral(
                    "wss://relay.example.test/socket"
                    "?token=abc&channel=stale"
                    "&feature=1&channel=older#frag")),
                QStringLiteral("fresh-channel"));
        QVERIFY(configured.hasValue());
        QCOMPARE(
            configured.value().fragment(),
            QStringLiteral("frag"));

        const QUrlQuery query(configured.value());
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
                QStringLiteral("fresh-channel")});
    }

    void auditCollapsesWhitespaceAndTruncatesUnicodeCharacters()
    {
        qint64 now = 1'000;
        RelayAudit audit([&now]() { return now; });
        const QString longText =
            QStringLiteral(" first\tline \n second ")
            + QString(221, QLatin1Char('x'))
            + QString::fromUtf8("\xF0\x9F\x9A\x80")
            + QStringLiteral("tail");

        const auto rendered = audit.render(
            QStringLiteral("format"),
            longText);
        QVERIFY(rendered.hasValue());
        QVERIFY(rendered.value().has_value());
        QCOMPARE(
            RelayAudit::unicodeCharacterCount(
                *rendered.value()),
            qsizetype(240));
        QVERIFY(!rendered.value()->contains(
            QLatin1Char('\t')));
        QVERIFY(!rendered.value()->contains(
            QLatin1Char('\n')));
        QVERIFY(!rendered.value()->contains(
            QStringLiteral("  ")));
        QVERIFY(
            rendered.value()->endsWith(
                QString::fromUtf8(
                    "\xF0\x9F\x9A\x80")));
    }

    void auditRedactsUrlsAndDeviceIdentifiers()
    {
        qint64 now = 1'000;
        RelayAudit audit([&now]() { return now; });
        const auto rendered = audit.render(
            QStringLiteral("redaction"),
            QStringLiteral("Relay rejected"),
            {
                {
                    QStringLiteral("url"),
                    QStringLiteral(
                        "wss://user:secret@relay.example.test"
                        ":8443/private?token=secret#fragment"),
                    RelayAuditValueKind::Url,
                },
                {
                    QStringLiteral("device"),
                    QStringLiteral(
                        "personal-device-identifier"),
                    RelayAuditValueKind::DeviceId,
                },
            });
        QVERIFY(rendered.hasValue());
        QVERIFY(rendered.value().has_value());
        QCOMPARE(
            *rendered.value(),
            QStringLiteral(
                "Relay rejected "
                "url=wss://relay.example.test "
                "device=cd634b1a"));
        QVERIFY(
            !rendered.value()->contains(
                QStringLiteral("secret")));
        QVERIFY(
            !rendered.value()->contains(
                QStringLiteral("personal-device")));
    }

    void auditThrottlesForSixtySecondsAndEvictsOldestKey()
    {
        qint64 now = 0;
        RelayAudit audit([&now]() { return now; });

        const auto first = audit.render(
            QStringLiteral("same"),
            QStringLiteral("First"));
        QVERIFY(first.hasValue());
        QVERIFY(first.value().has_value());

        now = 59'999;
        const auto throttled = audit.render(
            QStringLiteral("same"),
            QStringLiteral("Second"));
        QVERIFY(throttled.hasValue());
        QVERIFY(!throttled.value().has_value());

        now = 60'000;
        const auto allowed = audit.render(
            QStringLiteral("same"),
            QStringLiteral("Third"));
        QVERIFY(allowed.hasValue());
        QVERIFY(allowed.value().has_value());

        for (int index = 0; index < 64; ++index) {
            ++now;
            const auto inserted = audit.render(
                QStringLiteral("key-%1").arg(index),
                QStringLiteral("Event"));
            QVERIFY(inserted.hasValue());
            QVERIFY(inserted.value().has_value());
        }
        QCOMPARE(audit.throttleKeyCount(), qsizetype(64));

        ++now;
        const auto evicted = audit.render(
            QStringLiteral("same"),
            QStringLiteral("After eviction"));
        QVERIFY(evicted.hasValue());
        QVERIFY(evicted.value().has_value());
        QCOMPARE(audit.throttleKeyCount(), qsizetype(64));
    }

    void auditRejectsSensitiveRawFields()
    {
        RelayAudit audit;
        const auto safe = audit.render(
            QStringLiteral("safe-state"),
            QStringLiteral("Relay state changed"),
            {{
                QStringLiteral("state"),
                QStringLiteral("registered"),
                RelayAuditValueKind::Text,
            }});
        QVERIFY(safe.hasValue());
        QVERIFY(safe.value().has_value());
        QCOMPARE(
            *safe.value(),
            QStringLiteral(
                "Relay state changed state=registered"));

        for (const QString& name :
             {QStringLiteral("payload"),
              QStringLiteral("sealedPayload"),
              QStringLiteral("envelope"),
              QStringLiteral("secret"),
              QStringLiteral("attachmentBytes"),
              QStringLiteral("rawPayload"),
              QStringLiteral("body"),
              QStringLiteral("token"),
              QStringLiteral("data"),
              QStringLiteral("arbitrary")}) {
            const auto rendered = audit.render(
                QStringLiteral("sensitive-")
                    + name,
                QStringLiteral("Rejected"),
                {{
                    name,
                    QStringLiteral("raw-private-data"),
                    RelayAuditValueKind::Text,
                }});
            QVERIFY(!rendered.hasValue());
            QCOMPARE(
                rendered.error().code,
                QStringLiteral(
                    "relay.audit_sensitive_field"));
        }

        const auto unsafeValue = audit.render(
            QStringLiteral("unsafe-state-value"),
            QStringLiteral("Rejected"),
            {{
                QStringLiteral("state"),
                QStringLiteral(
                    "raw private data"),
                RelayAuditValueKind::Text,
            }});
        QVERIFY(!unsafeValue.hasValue());
        QCOMPARE(
            unsafeValue.error().code,
            QStringLiteral(
                "relay.audit_sensitive_field"));
    }
};

QTEST_GUILESS_MAIN(RelayModelsTests)
#include "RelayModelsTests.moc"
