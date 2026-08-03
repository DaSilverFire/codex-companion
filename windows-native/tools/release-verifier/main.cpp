#include "ReleaseVerifier.h"
#include "ReleaseVerifierProduction.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

namespace {

bool requireOption(
    const QCommandLineParser& parser,
    const QCommandLineOption& option,
    QString* value)
{
    *value =
        parser.value(option).trimmed();
    return parser.isSet(option)
        && !value->isEmpty();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(
        argc,
        argv);
    QCoreApplication::setApplicationName(
        QStringLiteral(
            "companion-release-verifier"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral(
            "Verify a signed Codex Companion Windows release."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption
        installerOption(
            QStringLiteral("installer"),
            QStringLiteral(
                "Signed Windows installer path."),
            QStringLiteral("path"));
    const QCommandLineOption manifestOption(
        QStringLiteral("manifest"),
        QStringLiteral(
            "Signed schema-1 manifest path."),
        QStringLiteral("path"));
    const QCommandLineOption publicKeyOption(
        QStringLiteral("public-key"),
        QStringLiteral(
            "Base64 Ed25519 public key."),
        QStringLiteral("base64"));
    const QCommandLineOption outputOption(
        QStringLiteral("output"),
        QStringLiteral(
            "Evidence JSON output path."),
        QStringLiteral("path"));
    const QCommandLineOption stageOption(
        QStringLiteral("stage"),
        QStringLiteral(
            "Portable release tree. Defaults to "
            "<installer-dir>/portable/Codex Companion."),
        QStringLiteral("path"));
    const QCommandLineOption metadataOption(
        QStringLiteral("metadata"),
        QStringLiteral(
            "Optional release-metadata.json path."),
        QStringLiteral("path"));
    parser.addOptions(
        {
            installerOption,
            manifestOption,
            publicKeyOption,
            outputOption,
            stageOption,
            metadataOption,
        });
    parser.process(application);

    companion::ReleaseVerifierOptions
        options;
    QString outputPath;
    if (!requireOption(
            parser,
            installerOption,
            &options.installerPath)
        || !requireOption(
            parser,
            manifestOption,
            &options.manifestPath)
        || !requireOption(
            parser,
            publicKeyOption,
            &options.publicKeyBase64)
        || !requireOption(
            parser,
            outputOption,
            &outputPath)) {
        QTextStream(stderr)
            << "Required: --installer --manifest "
               "--public-key --output\n";
        return 1;
    }

    options.stagePath =
        parser.isSet(stageOption)
        ? parser.value(stageOption)
              .trimmed()
        : companion::
              productionDefaultReleaseStagePath(
                  options.installerPath);
    options.metadata =
        companion::
            productionReleaseEvidenceMetadata();

    const bool explicitMetadata =
        parser.isSet(metadataOption);
    const QString metadataPath =
        explicitMetadata
        ? parser.value(metadataOption)
              .trimmed()
        : companion::
              productionDefaultReleaseMetadataPath(
                  options.installerPath);
    if (explicitMetadata
        || QFileInfo(metadataPath).isFile()) {
        const auto metadata =
            companion::
                loadReleaseEvidenceMetadata(
                    metadataPath);
        if (metadata.hasValue()) {
            options.metadata =
                companion::
                    mergeReleaseEvidenceMetadata(
                        std::move(
                            options.metadata),
                        metadata.value());
        } else {
            options.metadataErrorCode =
                metadata.error().code;
        }
    }

    const companion::ReleaseVerifier
        verifier(
            companion::
                makeProductionReleaseVerifierDependencies());
    const companion::
        ReleaseVerificationEvidence evidence =
            verifier.verify(options);
    const companion::Result<void> written =
        companion::
            writeReleaseVerificationEvidence(
                outputPath,
                evidence);
    if (!written.hasValue()) {
        QTextStream(stderr)
            << written.error().code
            << '\n';
        return 1;
    }

    QTextStream(stdout)
        << (evidence.passed
                ? "release verification passed\n"
                : "release verification failed\n");
    return companion::
        releaseVerifierExitCode(evidence);
}
