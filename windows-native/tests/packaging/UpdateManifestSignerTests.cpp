#include "UpdateManifestSigner.h"

#include "update/MonocypherEd25519Verifier.h"
#include "update/UpdateManifestValidator.h"

#include <array>
#include <functional>

#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {

constexpr auto kPrivateSeedBase64 =
    "nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A=";
constexpr auto kPublicKeyBase64 =
    "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";

companion::UpdateManifestSigningRequest
request()
{
    return {
        QStringLiteral("0.3.4"),
        34,
        QStringLiteral("10.0.22000"),
        QStringLiteral(
            "2026-07-26T03:15:00Z"),
        QStringLiteral(
            "https://updates.example.test/"
            "Codex-Companion-0.3.4-34-windows-x64.exe"),
        QString(
            64,
            QLatin1Char('a')),
        42'000,
        QString::fromLatin1(
            kPublicKeyBase64),
    };
}

QString errorCode(
    const companion::Result<
        companion::SignedUpdateManifest>& result)
{
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

companion::Result<
    companion::SignedUpdateManifest>
sign(
    companion::UpdateManifestSigningRequest
        value = request(),
    QByteArray privateSeed =
        QByteArray(
            kPrivateSeedBase64))
{
    return companion::signUpdateManifest(
        std::move(value),
        std::move(privateSeed));
}

QString signerPath()
{
    return QStringLiteral(
        COMPANION_MANIFEST_SIGNER_PATH);
}

QStringList signerArguments(
    const QString& outputPath)
{
    const auto value = request();
    return {
        QStringLiteral("--version"),
        value.version,
        QStringLiteral("--build"),
        QString::number(value.build),
        QStringLiteral(
            "--minimum-system-version"),
        value.minimumSystemVersion,
        QStringLiteral("--published-at"),
        value.publishedAt,
        QStringLiteral("--download-url"),
        value.downloadUrl,
        QStringLiteral("--sha256"),
        value.sha256,
        QStringLiteral("--size"),
        QString::number(value.size),
        QStringLiteral("--output"),
        outputPath,
        QStringLiteral("--expected-public-key"),
        value.expectedPublicKeyBase64,
    };
}

struct ProcessResult final {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

ProcessResult runSigner(
    const QStringList& arguments,
    const QProcessEnvironment& environment)
{
    QProcess process;
    process.setProgram(signerPath());
    process.setArguments(arguments);
    process.setProcessEnvironment(environment);
    process.start();

    ProcessResult result;
    result.started =
        process.waitForStarted(10'000);
    if (!result.started) {
        return result;
    }
    result.finished =
        process.waitForFinished(30'000);
    result.exitCode = process.exitCode();
    result.standardOutput =
        process.readAllStandardOutput();
    result.standardError =
        process.readAllStandardError();
    return result;
}

} // namespace

class UpdateManifestSignerTests final
    : public QObject {
    Q_OBJECT

private slots:
    void signsAndProductionVerifiesManifest();
    void emitsStableSortedJson();
    void rejectsInvalidReleaseFields();
    void rejectsInvalidOrMismatchedKeys();
    void commandReadsPrivateSeedOnlyFromEnvironment();
    void commandRejectsPrivateKeyArgument();
};

void UpdateManifestSignerTests::
    signsAndProductionVerifiesManifest()
{
    const auto result = sign();
    QVERIFY2(
        result.hasValue(),
        qPrintable(
            result.hasValue()
                ? QString()
                : result.error().code));
    QCOMPARE(
        result.value().publicKeyBase64,
        QString::fromLatin1(
            kPublicKeyBase64));

    const companion::MonocypherEd25519Verifier
        verifier;
    const companion::UpdateManifestValidator
        validator(verifier);
    const auto verified = validator.validate(
        result.value().json,
        result.value().publicKeyBase64);
    QVERIFY2(
        verified.hasValue(),
        qPrintable(
            verified.hasValue()
                ? QString()
                : verified.error().code));
    QCOMPARE(
        verified.value(),
        result.value().manifest);
}

void UpdateManifestSignerTests::
    emitsStableSortedJson()
{
    const auto first = sign();
    const auto second = sign();
    QVERIFY(first.hasValue());
    QVERIFY(second.hasValue());
    QCOMPARE(
        first.value().json,
        second.value().json);
    QVERIFY(first.value().json.endsWith('\n'));
    QVERIFY(
        first.value().json.contains(
            "https://updates.example.test/"));
    QVERIFY(
        !first.value().json.contains(
            "https:\\/\\/"));

    const std::array keys{
        QByteArrayLiteral("\"build\""),
        QByteArrayLiteral("\"downloadURL\""),
        QByteArrayLiteral(
            "\"minimumSystemVersion\""),
        QByteArrayLiteral("\"publishedAt\""),
        QByteArrayLiteral("\"schemaVersion\""),
        QByteArrayLiteral("\"sha256\""),
        QByteArrayLiteral("\"signature\""),
        QByteArrayLiteral("\"size\""),
        QByteArrayLiteral("\"version\""),
    };
    qsizetype previous = -1;
    for (const QByteArray& key : keys) {
        const qsizetype current =
            first.value().json.indexOf(key);
        QVERIFY(current > previous);
        previous = current;
    }
}

void UpdateManifestSignerTests::
    rejectsInvalidReleaseFields()
{
    using Request =
        companion::UpdateManifestSigningRequest;
    struct InvalidCase final {
        const char* name;
        std::function<void(Request*)> mutate;
        QString expectedCode;
    };
    const std::array cases{
        InvalidCase{
            "version",
            [](Request* value) {
                value->version =
                    QStringLiteral("01.3.4");
            },
            QStringLiteral(
                "update.signer.invalid_version"),
        },
        InvalidCase{
            "build-zero",
            [](Request* value) {
                value->build = 0;
            },
            QStringLiteral(
                "update.signer.invalid_build"),
        },
        InvalidCase{
            "build-too-large",
            [](Request* value) {
                value->build = 65'536;
            },
            QStringLiteral(
                "update.signer.invalid_build"),
        },
        InvalidCase{
            "minimum-system",
            [](Request* value) {
                value->minimumSystemVersion =
                    QStringLiteral("10");
            },
            QStringLiteral(
                "update.invalid_minimum_system_version"),
        },
        InvalidCase{
            "published-at",
            [](Request* value) {
                value->publishedAt =
                    QStringLiteral(
                        "2026-07-26T03:15:00");
            },
            QStringLiteral(
                "update.invalid_published_at"),
        },
        InvalidCase{
            "download-url",
            [](Request* value) {
                value->downloadUrl =
                    QStringLiteral(
                        "http://updates.example.test/app.exe");
            },
            QStringLiteral(
                "update.insecure_download_url"),
        },
        InvalidCase{
            "digest",
            [](Request* value) {
                value->sha256 =
                    QStringLiteral("abc");
            },
            QStringLiteral(
                "update.invalid_digest"),
        },
        InvalidCase{
            "size-zero",
            [](Request* value) {
                value->size = 0;
            },
            QStringLiteral(
                "update.invalid_size"),
        },
        InvalidCase{
            "size-too-large",
            [](Request* value) {
                value->size =
                    companion::UpdateManifest::
                        maximumSignedSize
                    + 1;
            },
            QStringLiteral(
                "update.invalid_size"),
        },
    };

    for (const InvalidCase& invalid : cases) {
        Request value = request();
        invalid.mutate(&value);
        const auto result = sign(value);
        QVERIFY2(
            errorCode(result)
                == invalid.expectedCode,
            invalid.name);
    }
}

void UpdateManifestSignerTests::
    rejectsInvalidOrMismatchedKeys()
{
    QCOMPARE(
        errorCode(sign(
            request(),
            QByteArrayLiteral("not base64!"))),
        QStringLiteral(
            "update.signer.invalid_private_key"));
    QCOMPARE(
        errorCode(sign(
            request(),
            QByteArray(31, 'k').toBase64())),
        QStringLiteral(
            "update.signer.invalid_private_key"));

    auto mismatched = request();
    mismatched.expectedPublicKeyBase64 =
        QString::fromLatin1(
            QByteArray(32, 'p').toBase64());
    QCOMPARE(
        errorCode(sign(mismatched)),
        QStringLiteral(
            "update.signer.public_key_mismatch"));
}

void UpdateManifestSignerTests::
    commandReadsPrivateSeedOnlyFromEnvironment()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString outputPath =
        temporary.filePath(
            QStringLiteral(
                "update-windows-x64.json"));
    const QStringList arguments =
        signerArguments(outputPath);
    QVERIFY(
        !arguments.join(QLatin1Char(' '))
             .contains(
                 QString::fromLatin1(
                     kPrivateSeedBase64)));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64"),
        QString::fromLatin1(
            kPrivateSeedBase64));
    const ProcessResult result =
        runSigner(arguments, environment);
    QVERIFY(result.started);
    QVERIFY(result.finished);
    QCOMPARE(result.exitCode, 0);

    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::ReadOnly));
    const QByteArray manifest = output.readAll();
    QVERIFY(
        !result.standardOutput.contains(
            kPrivateSeedBase64));
    QVERIFY(
        !result.standardError.contains(
            kPrivateSeedBase64));
    QVERIFY(
        !manifest.contains(
            kPrivateSeedBase64));

    const companion::MonocypherEd25519Verifier
        verifier;
    const companion::UpdateManifestValidator
        validator(verifier);
    QVERIFY(
        validator.validate(
            manifest,
            QString::fromLatin1(
                kPublicKeyBase64))
            .hasValue());

    QProcessEnvironment missing =
        QProcessEnvironment::systemEnvironment();
    missing.remove(
        QStringLiteral(
            "CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64"));
    const ProcessResult withoutKey =
        runSigner(arguments, missing);
    QVERIFY(withoutKey.started);
    QVERIFY(withoutKey.finished);
    QCOMPARE(withoutKey.exitCode, 2);
    QVERIFY(
        withoutKey.standardError.contains(
            "update.signer.missing_private_key"));
}

void UpdateManifestSignerTests::
    commandRejectsPrivateKeyArgument()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QStringList arguments =
        signerArguments(
            temporary.filePath(
                QStringLiteral("manifest.json")));
    arguments.append(
        {
            QStringLiteral("--private-key"),
            QStringLiteral(
                "must-not-be-accepted"),
        });

    const ProcessResult result =
        runSigner(
            arguments,
            QProcessEnvironment::
                systemEnvironment());
    QVERIFY(result.started);
    QVERIFY(result.finished);
    QVERIFY(result.exitCode != 0);
    QVERIFY(
        result.standardError.contains(
            "Unknown option"));
    QVERIFY(
        !result.standardOutput.contains(
            "must-not-be-accepted"));
    QVERIFY(
        !result.standardError.contains(
            "must-not-be-accepted"));
}

QTEST_GUILESS_MAIN(
    UpdateManifestSignerTests)
#include "UpdateManifestSignerTests.moc"
