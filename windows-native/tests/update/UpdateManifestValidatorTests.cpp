#include "update/MonocypherEd25519Verifier.h"
#include "update/UpdateManifestValidator.h"

#include <QFile>
#include <QtTest>

namespace {

constexpr auto kPublicKeyBase64 =
    "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";

QByteArray fixture(const QString& name)
{
    QFile file(
        QStringLiteral(
            COMPANION_UPDATE_FIXTURE_ROOT)
        + QLatin1Char('/')
        + name);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("update fixture missing");
    }
    return file.readAll();
}

class RecordingVerifier final
    : public companion::Ed25519Verifier {
public:
    companion::Result<void> verify(
        QByteArrayView message,
        QStringView signatureBase64,
        QStringView publicKeyBase64)
        const override
    {
        ++calls;
        messageSeen = message.toByteArray();
        signatureSeen =
            signatureBase64.toString();
        publicKeySeen =
            publicKeyBase64.toString();
        return result;
    }

    mutable int calls = 0;
    mutable QByteArray messageSeen;
    mutable QString signatureSeen;
    mutable QString publicKeySeen;
    companion::Result<void> result =
        companion::Result<void>::success();
};

QString errorCode(
    const companion::Result<
        companion::UpdateManifest>& result)
{
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

} // namespace

class UpdateManifestValidatorTests final
    : public QObject {
    Q_OBJECT

private slots:
    void acceptsSignedManifest()
    {
        const companion::MonocypherEd25519Verifier
            verifier;
        const companion::UpdateManifestValidator
            validator(verifier);

        const auto result = validator.validate(
            fixture(
                QStringLiteral(
                    "manifest-valid.json")),
            QString::fromLatin1(
                kPublicKeyBase64));

        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message));
        QCOMPARE(
            result.value().version,
            QStringLiteral("0.3.4"));
        QCOMPARE(result.value().build, 34);
    }

    void verifiesExactCanonicalPayload()
    {
        RecordingVerifier verifier;
        const companion::UpdateManifestValidator
            validator(verifier);
        const QByteArray manifestData =
            fixture(
                QStringLiteral(
                    "manifest-valid.json"));

        const auto result = validator.validate(
            manifestData,
            QStringLiteral("test-public-key"));

        QVERIFY(result.hasValue());
        QCOMPARE(verifier.calls, 1);
        QCOMPARE(
            verifier.messageSeen,
            fixture(
                QStringLiteral(
                    "manifest-valid.canonical")));
        QCOMPARE(
            verifier.signatureSeen,
            result.value().signature);
        QCOMPARE(
            verifier.publicKeySeen,
            QStringLiteral("test-public-key"));
    }

    void rejectsMalformedManifestBeforeVerification()
    {
        RecordingVerifier verifier;
        const companion::UpdateManifestValidator
            validator(verifier);

        const auto result = validator.validate(
            QByteArrayLiteral("{"),
            QStringLiteral("unused"));

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.invalid_manifest"));
        QCOMPARE(verifier.calls, 0);
    }

    void propagatesVerifierFailure()
    {
        RecordingVerifier verifier;
        verifier.result =
            companion::Result<void>::failure({
                QStringLiteral(
                    "update.invalid_public_key"),
                QStringLiteral(
                    "The update signing key is invalid."),
            });
        const companion::UpdateManifestValidator
            validator(verifier);

        const auto result = validator.validate(
            fixture(
                QStringLiteral(
                    "manifest-valid.json")),
            QStringLiteral("bad-key"));

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.invalid_public_key"));
        QCOMPARE(verifier.calls, 1);
    }

    void rejectsTamperedSignedManifest()
    {
        const companion::MonocypherEd25519Verifier
            verifier;
        const companion::UpdateManifestValidator
            validator(verifier);

        const auto result = validator.validate(
            fixture(
                QStringLiteral(
                    "manifest-tampered-build.json")),
            QString::fromLatin1(
                kPublicKeyBase64));

        QCOMPARE(
            errorCode(result),
            QStringLiteral(
                "update.invalid_signature"));
    }
};

QTEST_GUILESS_MAIN(
    UpdateManifestValidatorTests)
#include "UpdateManifestValidatorTests.moc"
