#include "update/MonocypherEd25519Verifier.h"
#include "update/UpdateManifest.h"

#include <array>

#include <QFile>
#include <QtTest>

namespace {

constexpr auto kPublicKeyBase64 =
    "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
constexpr auto kWrongPublicKeyBase64 =
    "8ptr4CW1oZjhZhosAq8336xOHUNMzrhOsdTEbqutGk8=";

QByteArray fixture(const QString& name)
{
    QFile file(QStringLiteral(COMPANION_UPDATE_FIXTURE_ROOT)
               + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("update fixture missing");
    }
    return file.readAll();
}

companion::UpdateManifest decodedFixture(const QString& name)
{
    const auto decoded = companion::UpdateManifest::decode(fixture(name));
    if (!decoded.hasValue()) {
        qFatal("signed manifest fixture did not decode");
    }
    return decoded.value();
}

QString verifyErrorCode(
    const companion::MonocypherEd25519Verifier& verifier,
    QByteArrayView message,
    QStringView signature,
    QStringView publicKey)
{
    const auto verified =
        verifier.verify(message, signature, publicKey);
    return verified.hasValue()
        ? QStringLiteral("<success>")
        : verified.error().code;
}

} // namespace

class Ed25519VerifierTests final : public QObject {
    Q_OBJECT

private slots:
    void acceptsValidManifestAndPublicKey()
    {
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                manifest.signature,
                QString::fromLatin1(kPublicKeyBase64)),
            QStringLiteral("<success>"));
    }

    void rejectsMalformedOrWrongLengthPublicKey_data()
    {
        QTest::addColumn<QString>("publicKey");
        QTest::newRow("malformed")
            << QStringLiteral("not base64!");
        QTest::newRow("31-bytes")
            << QString::fromLatin1(QByteArray(31, 'k').toBase64());
        QTest::newRow("33-bytes")
            << QString::fromLatin1(QByteArray(33, 'k').toBase64());
    }

    void rejectsMalformedOrWrongLengthPublicKey()
    {
        QFETCH(QString, publicKey);
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                manifest.signature,
                publicKey),
            QStringLiteral("update.invalid_public_key"));
    }

    void rejectsMalformedOrWrongLengthSignature_data()
    {
        QTest::addColumn<QString>("signature");
        QTest::newRow("malformed")
            << QStringLiteral("not base64!");
        QTest::newRow("63-bytes")
            << QString::fromLatin1(QByteArray(63, 's').toBase64());
        QTest::newRow("65-bytes")
            << QString::fromLatin1(QByteArray(65, 's').toBase64());
    }

    void rejectsMalformedOrWrongLengthSignature()
    {
        QFETCH(QString, signature);
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                signature,
                QString::fromLatin1(kPublicKeyBase64)),
            QStringLiteral("update.invalid_signature"));
    }

    void rejectsCryptographicallyInvalidSignature()
    {
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;
        const QString zeroSignature =
            QString::fromLatin1(QByteArray(64, '\0').toBase64());

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                zeroSignature,
                QString::fromLatin1(kPublicKeyBase64)),
            QStringLiteral("update.invalid_signature"));
    }

    void rejectsValidSignatureUnderWrongPublicKey()
    {
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                manifest.signature,
                QString::fromLatin1(kWrongPublicKeyBase64)),
            QStringLiteral("update.invalid_signature"));
    }

    void rejectsManifestSignedByWrongKeyFixture()
    {
        const companion::UpdateManifest manifest =
            decodedFixture(QStringLiteral("manifest-wrong-key.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                manifest.signature,
                QString::fromLatin1(kPublicKeyBase64)),
            QStringLiteral("update.invalid_signature"));
    }

    void tamperedBuildFixtureFailsSignature()
    {
        const companion::UpdateManifest manifest =
            decodedFixture(
                QStringLiteral("manifest-tampered-build.json"));
        const companion::MonocypherEd25519Verifier verifier;

        QCOMPARE(
            verifyErrorCode(
                verifier,
                manifest.canonicalPayload(),
                manifest.signature,
                QString::fromLatin1(kPublicKeyBase64)),
            QStringLiteral("update.invalid_signature"));
    }

    void mutatingEverySignedFieldInvalidatesSignature()
    {
        using Manifest = companion::UpdateManifest;
        struct Mutation final {
            const char* name;
            void (*apply)(Manifest*);
        };
        const std::array mutations{
            Mutation{"schemaVersion", [](Manifest* value) {
                value->schemaVersion += 1;
            }},
            Mutation{"version", [](Manifest* value) {
                value->version += QStringLiteral(".1");
            }},
            Mutation{"build", [](Manifest* value) {
                value->build += 1;
            }},
            Mutation{"minimumSystemVersion", [](Manifest* value) {
                value->minimumSystemVersion =
                    QStringLiteral("10.0.22621");
            }},
            Mutation{"publishedAt", [](Manifest* value) {
                value->publishedAt =
                    QStringLiteral("2026-07-19T12:34:57Z");
            }},
            Mutation{"downloadURL", [](Manifest* value) {
                value->downloadUrl =
                    QStringLiteral(
                        "https://updates.example.test/tampered.exe");
            }},
            Mutation{"sha256", [](Manifest* value) {
                value->sha256[0] = QLatin1Char('0');
            }},
            Mutation{"size", [](Manifest* value) {
                value->size += 1;
            }},
        };

        const Manifest original =
            decodedFixture(QStringLiteral("manifest-valid.json"));
        const companion::MonocypherEd25519Verifier verifier;
        for (const Mutation& mutation : mutations) {
            Manifest changed = original;
            mutation.apply(&changed);
            const QString code = verifyErrorCode(
                verifier,
                changed.canonicalPayload(),
                changed.signature,
                QString::fromLatin1(kPublicKeyBase64));
            QVERIFY2(
                code == QStringLiteral("update.invalid_signature"),
                mutation.name);
        }
    }
};

QTEST_GUILESS_MAIN(Ed25519VerifierTests)
#include "Ed25519VerifierTests.moc"
