#include "UpdateManifestSigner.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

namespace {

bool requireValue(
    const QCommandLineParser& parser,
    const QCommandLineOption& option,
    QString* value)
{
    *value = parser.value(option);
    return parser.isSet(option)
        && !value->isEmpty();
}

bool parsePositiveInteger(
    QStringView value,
    qint64* parsed)
{
    bool ok = false;
    const qint64 number =
        value.toLongLong(&ok, 10);
    if (!ok || number <= 0) {
        return false;
    }
    *parsed = number;
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral(
            "companion-update-manifest-signer"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(
            COMPANION_PROJECT_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral(
            "Create a signed Codex Companion Windows update manifest."));
    const QCommandLineOption helpOption =
        parser.addHelpOption();

    const QCommandLineOption versionOption(
        QStringLiteral("version"),
        QStringLiteral("Release version."),
        QStringLiteral("version"));
    const QCommandLineOption buildOption(
        QStringLiteral("build"),
        QStringLiteral("Monotonic Windows build."),
        QStringLiteral("number"));
    const QCommandLineOption minimumOption(
        QStringLiteral(
            "minimum-system-version"),
        QStringLiteral(
            "Minimum Windows version."),
        QStringLiteral("version"));
    const QCommandLineOption publishedOption(
        QStringLiteral("published-at"),
        QStringLiteral(
            "RFC 3339 UTC publication time."),
        QStringLiteral("timestamp"));
    const QCommandLineOption downloadOption(
        QStringLiteral("download-url"),
        QStringLiteral(
            "Immutable HTTPS installer URL."),
        QStringLiteral("url"));
    const QCommandLineOption digestOption(
        QStringLiteral("sha256"),
        QStringLiteral(
            "Installer SHA-256 digest."),
        QStringLiteral("hex"));
    const QCommandLineOption sizeOption(
        QStringLiteral("size"),
        QStringLiteral(
            "Installer size in bytes."),
        QStringLiteral("bytes"));
    const QCommandLineOption outputOption(
        QStringLiteral("output"),
        QStringLiteral(
            "Manifest output path."),
        QStringLiteral("path"));
    const QCommandLineOption expectedPublicKeyOption(
        QStringLiteral(
            "expected-public-key"),
        QStringLiteral(
            "Optional expected base64 Ed25519 public key."),
        QStringLiteral("base64"));
    parser.addOptions(
        {
            versionOption,
            buildOption,
            minimumOption,
            publishedOption,
            downloadOption,
            digestOption,
            sizeOption,
            outputOption,
            expectedPublicKeyOption,
        });

    if (!parser.parse(
            QCoreApplication::arguments())) {
        QTextStream(stderr)
            << parser.errorText()
            << '\n';
        return 1;
    }
    if (parser.isSet(helpOption)) {
        parser.showHelp(0);
    }

    companion::UpdateManifestSigningRequest
        request;
    QString buildText;
    QString sizeText;
    QString outputPath;
    if (!requireValue(
            parser,
            versionOption,
            &request.version)
        || !requireValue(
            parser,
            buildOption,
            &buildText)
        || !requireValue(
            parser,
            minimumOption,
            &request.minimumSystemVersion)
        || !requireValue(
            parser,
            publishedOption,
            &request.publishedAt)
        || !requireValue(
            parser,
            downloadOption,
            &request.downloadUrl)
        || !requireValue(
            parser,
            digestOption,
            &request.sha256)
        || !requireValue(
            parser,
            sizeOption,
            &sizeText)
        || !requireValue(
            parser,
            outputOption,
            &outputPath)
        || !parsePositiveInteger(
            buildText,
            &request.build)
        || !parsePositiveInteger(
            sizeText,
            &request.size)) {
        QTextStream(stderr)
            << "Required: --version --build "
               "--minimum-system-version --published-at "
               "--download-url --sha256 --size --output\n";
        return 1;
    }
    request.expectedPublicKeyBase64 =
        parser.value(expectedPublicKeyOption);

    constexpr auto privateKeyEnvironment =
        "CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64";
    QByteArray privateSeedBase64 =
        qgetenv(privateKeyEnvironment);
    qunsetenv(privateKeyEnvironment);
    if (privateSeedBase64.isEmpty()) {
        QTextStream(stderr)
            << "update.signer.missing_private_key\n";
        return 2;
    }

    const auto signedManifest =
        companion::signUpdateManifest(
            std::move(request),
            std::move(privateSeedBase64));
    if (!signedManifest.hasValue()) {
        QTextStream(stderr)
            << signedManifest.error().code
            << '\n';
        return 2;
    }

    const auto written =
        companion::writeSignedUpdateManifest(
            outputPath,
            signedManifest.value().json);
    if (!written.hasValue()) {
        QTextStream(stderr)
            << written.error().code
            << '\n';
        return 2;
    }

    QTextStream(stdout)
        << "manifest written\n";
    return 0;
}
