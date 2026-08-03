#include "mobile/security/BridgeSecurity.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <QtTest>

#include <optional>
#include <utility>

namespace companion {

class BridgeSecurityTestAccess final {
public:
    static Result<EncryptedEnvelope> seal(
        QByteArrayView plaintext,
        QByteArrayView secret,
        QString senderId,
        quint64 sequence,
        qint64 sentAtMilliseconds,
        QByteArray nonce)
    {
        return BridgeSecurity::sealWithNonce(
            plaintext,
            secret,
            std::move(senderId),
            sequence,
            sentAtMilliseconds,
            std::move(nonce));
    }

    static Result<QByteArray>
    envelopeAuthenticationData(
        const EncryptedEnvelope& envelope)
    {
        return BridgeSecurity::
            envelopeAuthenticationData(
                envelope);
    }
};

} // namespace companion

namespace {

using namespace companion;

constexpr qint64 kInvitationTime = 1770000000123;
constexpr qint64 kEnvelopeTime = 1700000000123;

QByteArray vectorSecret()
{
    QByteArray secret(32, Qt::Uninitialized);
    for (qsizetype index = 0;
         index < secret.size();
         ++index) {
        secret[index] = static_cast<char>(index);
    }
    return secret;
}

QByteArray sequentialBytes(
    qsizetype count,
    unsigned char start = 0)
{
    QByteArray bytes(count, Qt::Uninitialized);
    for (qsizetype index = 0;
         index < count;
         ++index) {
        bytes[index] = static_cast<char>(
            start + static_cast<unsigned char>(
                index));
    }
    return bytes;
}

QDateTime instant(qint64 milliseconds)
{
    return QDateTime::fromMSecsSinceEpoch(
        milliseconds,
        QTimeZone::UTC);
}

QJsonObject relayFixture()
{
    QFile file(
        QStringLiteral(COMPANION_FIXTURE_ROOT)
        + QStringLiteral(
            "/mobile-v034/swift-relay-vector.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(
        file.readAll()).object();
}

QByteArray fixtureBytes(
    const QJsonObject& root,
    const QString& name)
{
    return QByteArray::fromHex(
        root.value(name)
            .toObject()
            .value(QStringLiteral("hex"))
            .toString()
            .toLatin1());
}

BridgeInvitation authenticatedVectorInvitation()
{
    const auto result =
        BridgeSecurity::authenticatedInvitation(
            QStringLiteral("iphone-alpha"),
            QStringLiteral("Harlin iPhone"),
            vectorSecret(),
            instant(kInvitationTime),
            sequentialBytes(16, 0xA0));
    if (!result.hasValue()) {
        return {};
    }
    return result.value();
}

void verifyOpenFails(
    const EncryptedEnvelope& envelope,
    QByteArrayView secret,
    const QString& expectedCode)
{
    const auto opened =
        BridgeSecurity::open(envelope, secret);
    QVERIFY(!opened.hasValue());
    QCOMPARE(
        opened.error().code,
        expectedCode);
}

} // namespace

class BridgeSecurityTests final : public QObject {
    Q_OBJECT

private slots:
    void invitationAuthenticationMatchesV034Vector()
    {
        const auto result =
            BridgeSecurity::authenticatedInvitation(
                QStringLiteral("iphone-alpha"),
                QStringLiteral("Harlin iPhone"),
                vectorSecret(),
                instant(kInvitationTime),
                sequentialBytes(16, 0xA0));

        QVERIFY(result.hasValue());
        const BridgeInvitation& invitation =
            result.value();
        QCOMPARE(
            invitation.version,
            BridgeSecurity::invitationVersion);
        QCOMPARE(
            invitation.issuedAtMilliseconds,
            kInvitationTime);
        QVERIFY(invitation.authenticator.has_value());
        QCOMPARE(
            invitation.authenticator->toHex().toUpper(),
            QByteArray(
                "3C52C01A9DF061CED0C244ED61B88AB82"
                "C42632761EC6DBC22515B0100AC90D7"));
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                vectorSecret(),
                std::nullopt,
                instant(kInvitationTime)),
            InvitationDecision::AcceptTrusted);

        BridgeInvitation metadataOnly =
            invitation;
        metadataOnly.pairingCode =
            QStringLiteral("123456");
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                metadataOnly,
                vectorSecret(),
                std::nullopt,
                instant(kInvitationTime)),
            InvitationDecision::AcceptTrusted);
    }

    void invitationDecisionEnforcesVersionSkewAndTrustedPrecedence()
    {
        BridgeInvitation invitation =
            authenticatedVectorInvitation();
        QVERIFY(invitation.authenticator.has_value());
        invitation.pairingCode =
            QStringLiteral("123-456");
        const ActivePairing active{
            QStringLiteral("123 456"),
            instant(kInvitationTime + 300000),
        };

        BridgeInvitation wrongVersion =
            invitation;
        wrongVersion.version = 2;
        wrongVersion.issuedAtMilliseconds = 0;
        wrongVersion.authenticator.reset();
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                wrongVersion,
                vectorSecret(),
                active,
                instant(kInvitationTime)),
            InvitationDecision::RejectVersion);

        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                vectorSecret(),
                active,
                instant(kInvitationTime + 120000)),
            InvitationDecision::AcceptTrusted);
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                vectorSecret(),
                active,
                instant(kInvitationTime + 120001)),
            InvitationDecision::RejectExpired);
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                vectorSecret(),
                active,
                instant(kInvitationTime - 120000)),
            InvitationDecision::AcceptTrusted);
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                vectorSecret(),
                active,
                instant(kInvitationTime - 120001)),
            InvitationDecision::RejectExpired);

        BridgeInvitation missingAuth =
            invitation;
        missingAuth.authenticator.reset();
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                missingAuth,
                vectorSecret(),
                active,
                instant(kInvitationTime)),
            InvitationDecision::
                RejectAuthentication);
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                QByteArray(31, '\x01'),
                active,
                instant(kInvitationTime)),
            InvitationDecision::
                RejectAuthentication);

        BridgeInvitation invalidAuth =
            invitation;
        (*invalidAuth.authenticator)[0] ^= 0x01;
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invalidAuth,
                vectorSecret(),
                active,
                instant(kInvitationTime)),
            InvitationDecision::
                RejectAuthentication);
    }

    void pairingNormalizationMatchesSwiftNumberSemantics()
    {
        const QString decorated =
            QString::fromUtf8(
                "x\xEF\xBC\x91\xEF\xBC\x92"
                "\xEF\xBC\x93-\xE2\x85\xAB"
                "\xC2\xB2y");
        const auto normalized =
            BridgeSecurity::normalizedPairingCode(
                decorated);
        QVERIFY(normalized.has_value());
        QCOMPARE(
            *normalized,
            QString::fromUtf8(
                "\xEF\xBC\x91\xEF\xBC\x92"
                "\xEF\xBC\x93\xE2\x85\xAB"
                "\xC2\xB2"));

        const QString supplementaryDigit =
            QString::fromUtf8(
                "\xF0\x9D\x9F\x99");
        QCOMPARE(
            BridgeSecurity::normalizedPairingCode(
                QStringLiteral("x")
                + supplementaryDigit
                + QStringLiteral("y")),
            std::optional<QString>(
                supplementaryDigit));

        const QString markedDigit =
            QStringLiteral("1")
            + QChar(0x0301);
        QCOMPARE(
            BridgeSecurity::normalizedPairingCode(
                QStringLiteral("x")
                + markedDigit
                + QStringLiteral("y")),
            std::optional<QString>(
                markedDigit));
        QVERIFY(
            !BridgeSecurity::normalizedPairingCode(
                QStringLiteral(" - "))
                 .has_value());

        BridgeInvitation invitation;
        invitation.deviceId =
            QStringLiteral("iphone-alpha");
        invitation.displayName =
            QStringLiteral("Harlin iPhone");
        invitation.issuedAtMilliseconds =
            kInvitationTime;
        invitation.nonce = QByteArray(16, '\0');
        invitation.pairingCode =
            QString::fromUtf8(
                "\xEF\xBC\x91\xEF\xBC\x92"
                "\xEF\xBC\x93-456");
        const ActivePairing active{
            QString::fromUtf8(
                "\xEF\xBC\x91\xEF\xBC\x92"
                "\xEF\xBC\x93 456"),
            instant(kInvitationTime + 300000),
        };

        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                std::nullopt,
                active,
                instant(kInvitationTime)),
            InvitationDecision::AcceptPairing);

        ActivePairing expired = active;
        expired.expiresAt =
            instant(kInvitationTime - 1);
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                std::nullopt,
                expired,
                instant(kInvitationTime)),
            InvitationDecision::RejectUnpaired);

        invitation.pairingCode =
            supplementaryDigit
            + QStringLiteral("12345");
        const ActivePairing supplementary{
            supplementaryDigit
                + QStringLiteral(" 12-345"),
            instant(kInvitationTime + 300000),
        };
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                std::nullopt,
                supplementary,
                instant(kInvitationTime)),
            InvitationDecision::AcceptPairing);

        invitation.pairingCode =
            markedDigit
            + QStringLiteral("23456");
        const ActivePairing unmarked{
            QStringLiteral("123456"),
            instant(kInvitationTime + 300000),
        };
        QCOMPARE(
            BridgeSecurity::decideInvitation(
                invitation,
                std::nullopt,
                unmarked,
                instant(kInvitationTime)),
            InvitationDecision::RejectUnpaired);
    }

    void productionRandomValuesHaveExactSizes()
    {
        const QRegularExpression asciiCode(
            QStringLiteral("^[0-9]{6}$"));
        for (int iteration = 0;
             iteration < 64;
             ++iteration) {
            const auto code =
                BridgeSecurity::randomPairingCode();
            QVERIFY(code.hasValue());
            QVERIFY(
                asciiCode.match(code.value())
                    .hasMatch());
        }

        const auto secret =
            BridgeSecurity::randomSecret();
        QVERIFY(secret.hasValue());
        QCOMPARE(secret.value().size(), 32);

        const auto nonce =
            BridgeSecurity::randomInvitationNonce();
        QVERIFY(nonce.hasValue());
        QCOMPARE(nonce.value().size(), 16);
    }

    void relayCryptographyMatchesSwiftFixture()
    {
        const QJsonObject fixture =
            relayFixture();
        QVERIFY(!fixture.isEmpty());
        const QByteArray secret =
            vectorSecret();
        const QByteArray nonce =
            fixtureBytes(
                fixture,
                QStringLiteral("nonce"));
        const QByteArray plaintext =
            fixtureBytes(
                fixture,
                QStringLiteral("payload"));

        const auto channel =
            BridgeSecurity::channelId(secret);
        QVERIFY(channel.hasValue());
        QCOMPARE(
            channel.value(),
            fixture.value(
                QStringLiteral("channelID"))
                .toString());

        const auto sealed =
            BridgeSecurityTestAccess::seal(
                plaintext,
                secret,
                QStringLiteral("iphone-fixture"),
                42,
                kEnvelopeTime,
                nonce);
        QVERIFY(sealed.hasValue());
        const auto authenticationData =
            BridgeSecurityTestAccess::
                envelopeAuthenticationData(
                    sealed.value());
        QVERIFY(authenticationData.hasValue());
        QCOMPARE(
            authenticationData.value(),
            fixtureBytes(
                fixture,
                QStringLiteral("header")));
        QCOMPARE(
            sealed.value().sealedPayload,
            fixtureBytes(
                fixture,
                QStringLiteral("combined")));
        QCOMPARE(
            sealed.value().sealedPayload.mid(
                12,
                plaintext.size()),
            fixtureBytes(
                fixture,
                QStringLiteral("ciphertext")));
        QCOMPARE(
            sealed.value().sealedPayload.last(16),
            fixtureBytes(
                fixture,
                QStringLiteral("tag")));

        const auto opened =
            BridgeSecurity::open(
                sealed.value(),
                secret);
        QVERIFY(opened.hasValue());
        QCOMPARE(opened.value(), plaintext);

        const auto productionSealed =
            BridgeSecurity::seal(
                plaintext,
                secret,
                QStringLiteral("iphone-fixture"),
                43,
                kEnvelopeTime);
        QVERIFY(productionSealed.hasValue());
        QCOMPARE(
            productionSealed.value()
                .sealedPayload.size(),
            plaintext.size() + 28);
        const auto productionOpened =
            BridgeSecurity::open(
                productionSealed.value(),
                secret);
        QVERIFY(productionOpened.hasValue());
        QCOMPARE(
            productionOpened.value(),
            plaintext);
    }

    void relayAuthenticationDataMatchesSwiftEscaping()
    {
        const EncryptedEnvelope envelope{
            1,
            QStringLiteral("channel"),
            QStringLiteral("a/b"),
            42,
            7,
            {},
        };
        const auto authenticationData =
            BridgeSecurityTestAccess::
                envelopeAuthenticationData(
                    envelope);
        QVERIFY(authenticationData.hasValue());
        QCOMPARE(
            authenticationData.value(),
            QByteArray(
                "{\"channelID\":\"channel\","
                "\"senderID\":\"a\\/b\","
                "\"sentAtMilliseconds\":7,"
                "\"sequence\":42,"
                "\"version\":1}"));
    }

    void relayCryptographyRejectsInvalidInputsAndTampering()
    {
        const QJsonObject fixture =
            relayFixture();
        QVERIFY(!fixture.isEmpty());
        const QByteArray secret =
            vectorSecret();
        const QByteArray plaintext =
            fixtureBytes(
                fixture,
                QStringLiteral("payload"));
        const auto sealed =
            BridgeSecurityTestAccess::seal(
                plaintext,
                secret,
                QStringLiteral("iphone-fixture"),
                42,
                kEnvelopeTime,
                fixtureBytes(
                    fixture,
                    QStringLiteral("nonce")));
        QVERIFY(sealed.hasValue());

        const auto shortChannel =
            BridgeSecurity::channelId(
                QByteArray(31, '\0'));
        QVERIFY(shortChannel.hasValue());
        QCOMPARE(
            shortChannel.value(),
            QStringLiteral(
                "37EVOwU7VM_88g2e0GXkviaztUyMjkXy"));
        const auto emptyChannel =
            BridgeSecurity::channelId(
                QByteArray());
        QVERIFY(emptyChannel.hasValue());
        QCOMPARE(
            emptyChannel.value(),
            shortChannel.value());
        QVERIFY(
            !BridgeSecurityTestAccess::seal(
                plaintext,
                QByteArray(31, '\0'),
                QStringLiteral("iphone-fixture"),
                42,
                kEnvelopeTime,
                QByteArray(12, '\0'))
                 .hasValue());
        QVERIFY(
            !BridgeSecurityTestAccess::seal(
                plaintext,
                secret,
                QStringLiteral("iphone-fixture"),
                42,
                kEnvelopeTime,
                QByteArray(11, '\0'))
                 .hasValue());

        EncryptedEnvelope changed =
            sealed.value();
        changed.version += 1;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.security.invalid_channel"));

        changed = sealed.value();
        changed.channelId.append(
            QLatin1Char('x'));
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.security.invalid_channel"));

        changed = sealed.value();
        changed.senderId.append(
            QLatin1Char('x'));
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        changed = sealed.value();
        changed.sequence += 1;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        changed = sealed.value();
        changed.sentAtMilliseconds += 1;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        changed = sealed.value();
        changed.sealedPayload[0] ^= 0x01;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        changed = sealed.value();
        changed.sealedPayload[12] ^= 0x01;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        changed = sealed.value();
        changed.sealedPayload[
            changed.sealedPayload.size() - 1]
            ^= 0x01;
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.crypto.authentication_failed"));

        QByteArray wrongSecret = secret;
        wrongSecret[0] ^= 0x01;
        verifyOpenFails(
            sealed.value(),
            wrongSecret,
            QStringLiteral(
                "mobile.security.invalid_channel"));

        changed = sealed.value();
        changed.sealedPayload =
            QByteArray(27, '\0');
        verifyOpenFails(
            changed,
            secret,
            QStringLiteral(
                "mobile.security.truncated_envelope"));
    }
};

QTEST_MAIN(BridgeSecurityTests)

#include "BridgeSecurityTests.moc"
