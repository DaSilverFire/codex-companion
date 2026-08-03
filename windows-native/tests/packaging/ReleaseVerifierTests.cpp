#include "ReleaseVerifier.h"

#include <functional>
#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

constexpr auto kTrustedSigner =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";

class TestFailure final
    : public std::runtime_error {
public:
    explicit TestFailure(
        const QString& message)
        : std::runtime_error(
              message.toStdString())
    {
    }
};

void require(
    bool condition,
    const QString& message)
{
    if (!condition) {
        throw TestFailure(message);
    }
}

bool writeFile(
    const QString& path,
    const QByteArray& bytes)
{
    if (!QDir().mkpath(
            QFileInfo(path)
                .absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(
               QIODevice::WriteOnly
               | QIODevice::Truncate)
        && file.write(bytes)
            == bytes.size();
}

QByteArray sha256(
    QByteArrayView bytes)
{
    return QCryptographicHash::hash(
        bytes,
        QCryptographicHash::Sha256);
}

companion::CompanionError testError(
    QString code,
    QString message =
        QStringLiteral("fixture failed"),
    QString path =
        QStringLiteral(
            "C:\\Users\\secret\\fixture.exe"))
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {
                QStringLiteral("path"),
                std::move(path),
            },
        },
    };
}

const companion::ReleaseVerificationCheck&
check(
    const companion::
        ReleaseVerificationEvidence&
            evidence,
    QStringView id)
{
    for (const auto& candidate :
         evidence.checks) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    throw TestFailure(
        QStringLiteral(
            "Missing check: %1")
            .arg(id));
}

void requireStatus(
    const companion::
        ReleaseVerificationEvidence&
            evidence,
    QStringView id,
    companion::ReleaseCheckStatus status,
    QStringView code = {})
{
    const auto& actual =
        check(evidence, id);
    require(
        actual.status == status,
        QStringLiteral(
            "Unexpected status for %1")
            .arg(id));
    if (!code.isEmpty()) {
        require(
            actual.code == code,
            QStringLiteral(
                "Unexpected code for %1: %2")
                .arg(id, actual.code));
    }
}

struct Fixture final {
    QTemporaryDir temporary;
    QByteArray installerBytes =
        QByteArrayLiteral(
            "unsigned synthetic Inno setup");
    QString installerPath;
    QString manifestPath;
    QString stagePath;
    QString outputPath;
    companion::UpdateManifest manifest;
    companion::ReleaseVerifierOptions
        options;
    companion::ReleaseVerifierDependencies
        dependencies;

    Fixture()
    {
        require(
            temporary.isValid(),
            QStringLiteral(
                "Temporary directory is invalid."));
        const QDir root(temporary.path());
        installerPath =
            root.filePath(
                QStringLiteral(
                    "Codex-Companion-0.3.4-34-"
                    "windows-x64.exe"));
        manifestPath =
            root.filePath(
                QStringLiteral(
                    "update-windows-x64.json"));
        stagePath =
            root.filePath(
                QStringLiteral(
                    "portable/Codex Companion"));
        outputPath =
            root.filePath(
                QStringLiteral(
                    "evidence/"
                    "release-verification.json"));

        require(
            writeFile(
                installerPath,
                installerBytes),
            QStringLiteral(
                "Could not write installer fixture."));
        require(
            writeFile(
                manifestPath,
                QByteArrayLiteral(
                    "{\"signed\":\"synthetic\"}\n")),
            QStringLiteral(
                "Could not write manifest fixture."));

        for (const QString& relative :
             companion::ReleaseVerifier::
                 requiredStageFiles()) {
            require(
                writeFile(
                    QDir(stagePath)
                        .filePath(relative),
                    QByteArrayLiteral("stage:")
                        + relative.toUtf8()),
                QStringLiteral(
                    "Could not write stage fixture: %1")
                    .arg(relative));
        }
        for (const QString& relative :
             companion::ReleaseVerifier::
                 requiredStageDirectories()) {
            require(
                writeFile(
                    QDir(stagePath)
                        .filePath(
                            relative
                            + QStringLiteral(
                                "/fixture.qml")),
                    QByteArrayLiteral(
                        "import QtQuick\nItem {}\n")),
                QStringLiteral(
                    "Could not write stage directory fixture."));
        }

        manifest.schemaVersion = 1;
        manifest.version =
            QStringLiteral("0.3.4");
        manifest.build = 34;
        manifest.minimumSystemVersion =
            QStringLiteral("10.0.22000");
        manifest.publishedAt =
            QStringLiteral(
                "2026-07-25T00:00:00Z");
        manifest.downloadUrl =
            QStringLiteral(
                "https://updates.example.test/"
                "Codex-Companion-0.3.4-34-"
                "windows-x64.exe");
        manifest.sha256 =
            QString::fromLatin1(
                sha256(installerBytes)
                    .toHex());
        manifest.size =
            installerBytes.size();
        manifest.signature =
            QStringLiteral(
                "synthetic-signature");

        options.installerPath =
            installerPath;
        options.manifestPath =
            manifestPath;
        options.publicKeyBase64 =
            QStringLiteral(
                "synthetic-public-key");
        options.stagePath =
            stagePath;
        options.metadata = {
            QString(40, QLatin1Char('a')),
            QString(40, QLatin1Char('b')),
            QString(64, QLatin1Char('c')),
            QStringLiteral(
                "MSVC 19.44.35207"),
            QStringLiteral("6.11.1"),
            QStringLiteral("7.0.2"),
            QStringLiteral("10.0.26100"),
            QString(40, QLatin1Char('d')),
            QString(40, QLatin1Char('e')),
            QString(64, QLatin1Char('f')),
        };

        dependencies.manifestValidator =
            [this](
                QByteArrayView,
                QStringView) {
                return companion::Result<
                    companion::UpdateManifest>::
                    success(manifest);
            };
        dependencies
            .windowsVersionProvider =
            [] {
                return companion::Result<
                    companion::WindowsVersion>::
                    success(
                        {
                            10,
                            0,
                            26100,
                            0,
                        });
            };
        dependencies.peInspector =
            [](QStringView) {
                return companion::Result<
                    companion::PeMachine>::
                    success(
                        companion::PeMachine::X86);
            };
        dependencies.metadataInspector =
            [this](QStringView) {
                return companion::Result<
                    companion::
                        InstallerMetadata>::
                    success(
                        {
                            QStringLiteral(
                                "Codex Companion"),
                            QStringLiteral(
                                "cc-update/1|%1|%2|"
                                "w|x64|%3")
                                .arg(
                                    manifest
                                        .version)
                                .arg(
                                    manifest
                                        .build)
                                .arg(
                                    manifest
                                        .minimumSystemVersion),
                            QStringLiteral(
                                "Codex-Companion-%1-%2-"
                                "windows-x64.exe")
                                .arg(
                                    manifest
                                        .version)
                                .arg(
                                    manifest
                                        .build),
                        });
            };
        dependencies.signerInspector =
            [](QStringView) {
                return companion::Result<
                    companion::
                        AuthenticodeIdentity>::
                    success(
                        {
                            QString::fromLatin1(
                                kTrustedSigner),
                            QStringLiteral(
                                "DaSilverFire"),
                        });
            };
        dependencies.iconInspector =
            [](
                QStringView,
                companion::
                    ExecutableIconExpectation) {
                return companion::Result<
                    bool>::success(true);
            };
        dependencies.allowedSignerSha256 = {
            QString::fromLatin1(
                kTrustedSigner),
        };
    }

    companion::ReleaseVerificationEvidence
    verify() const
    {
        return companion::ReleaseVerifier(
                   dependencies)
            .verify(options);
    }
};

void successfulUnsignedFixtureProducesEvidence()
{
    Fixture fixture;
    const auto evidence =
        fixture.verify();

    require(
        evidence.passed,
        QStringLiteral(
            "Synthetic release did not pass."));
    require(
        companion::releaseVerifierExitCode(
            evidence)
            == 0,
        QStringLiteral(
            "Passing evidence did not map to exit 0."));
    requireStatus(
        evidence,
        QStringLiteral(
            "manifest.authentication"),
        companion::ReleaseCheckStatus::
            Passed);
    requireStatus(
        evidence,
        QStringLiteral("installer.pe"),
        companion::ReleaseCheckStatus::
            Passed);
    requireStatus(
        evidence,
        QStringLiteral(
            "installer.compatibility"),
        companion::ReleaseCheckStatus::
            Passed);
    requireStatus(
        evidence,
        QStringLiteral(
            "stage.completeness"),
        companion::ReleaseCheckStatus::
            Passed);

    const auto written =
        companion::
            writeReleaseVerificationEvidence(
                fixture.outputPath,
                evidence);
    require(
        written.hasValue(),
        QStringLiteral(
            "Evidence writer failed."));

    QFile output(fixture.outputPath);
    require(
        output.open(QIODevice::ReadOnly),
        QStringLiteral(
            "Evidence file could not be read."));
    const QByteArray json =
        output.readAll();
    require(
        json.endsWith('\n'),
        QStringLiteral(
            "Evidence lacks final newline."));
    require(
        !json.contains(
            fixture.temporary.path()
                .toUtf8()),
        QStringLiteral(
            "Evidence leaked its workspace path."));
    require(
        !json.toLower().contains(
            QByteArrayLiteral(
                "c:\\users\\")),
        QStringLiteral(
            "Evidence leaked a user path."));
    require(
        !json.toLower().contains(
            QByteArrayLiteral(
                "\"path\"")),
        QStringLiteral(
            "Evidence serialized a path field."));

    const QJsonDocument document =
        QJsonDocument::fromJson(json);
    require(
        document.isObject(),
        QStringLiteral(
            "Evidence is not a JSON object."));
    const QJsonObject object =
        document.object();
    require(
        object.value(
                  QStringLiteral("passed"))
            .toBool(),
        QStringLiteral(
            "Evidence passed field is false."));
    require(
        object.value(
                  QStringLiteral("version"))
                .toString()
            == QStringLiteral("0.3.4"),
        QStringLiteral(
            "Evidence version is incorrect."));
    require(
        object.value(
                  QStringLiteral(
                      "installerSha256"))
                .toString()
            == fixture.manifest.sha256,
        QStringLiteral(
            "Evidence installer hash is incorrect."));
}

void invalidManifestFailsWithoutPathLeakage()
{
    Fixture fixture;
    fixture.dependencies.manifestValidator =
        [](
            QByteArrayView,
            QStringView) {
            return companion::Result<
                companion::UpdateManifest>::
                failure(
                    testError(
                        QStringLiteral(
                            "update.invalid_signature")));
        };

    const auto evidence =
        fixture.verify();
    require(
        !evidence.passed,
        QStringLiteral(
            "Invalid manifest unexpectedly passed."));
    require(
        companion::releaseVerifierExitCode(
            evidence)
            == 2,
        QStringLiteral(
            "Failed verification did not map to exit 2."));
    requireStatus(
        evidence,
        QStringLiteral(
            "manifest.authentication"),
        companion::ReleaseCheckStatus::
            Failed,
        QStringLiteral(
            "update.invalid_signature"));
    requireStatus(
        evidence,
        QStringLiteral("manifest.release"),
        companion::ReleaseCheckStatus::
            Skipped);
    require(
        evidence.version.isEmpty(),
        QStringLiteral(
            "Untrusted manifest data entered evidence."));

    const QByteArray json =
        QJsonDocument(
            companion::
                releaseVerificationEvidenceJson(
                    evidence))
            .toJson();
    require(
        !json.toLower().contains(
            QByteArrayLiteral(
                "c:\\users\\")),
        QStringLiteral(
            "A dependency error path leaked into evidence."));
}

void installerSizeAndDigestAreExact()
{
    {
        Fixture fixture;
        ++fixture.manifest.size;
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "installer.digest"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "update.artifact_size_mismatch"));
    }

    {
        Fixture fixture;
        fixture.manifest.sha256 =
            QString(64, QLatin1Char('0'));
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "installer.digest"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "update.artifact_digest_mismatch"));
    }
}

void installerIdentityFailuresAreSeparated()
{
    {
        Fixture fixture;
        fixture.dependencies.peInspector =
            [](QStringView) {
                return companion::Result<
                    companion::PeMachine>::
                    success(
                        companion::PeMachine::
                            Arm64);
            };
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral("installer.pe"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "update.artifact_architecture_mismatch"));
    }

    {
        Fixture fixture;
        fixture.dependencies
            .metadataInspector =
            [](QStringView) {
                return companion::Result<
                    companion::
                        InstallerMetadata>::
                    success(
                        {
                            QStringLiteral(
                                "Other Product"),
                            QStringLiteral(
                                "wrong"),
                            QStringLiteral(
                                "wrong.exe"),
                        });
            };
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "installer.metadata"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "update.installer_identity_mismatch"));
    }

    {
        Fixture fixture;
        fixture.dependencies.signerInspector =
            [](QStringView) {
                return companion::Result<
                    companion::
                        AuthenticodeIdentity>::
                    failure(
                        testError(
                            QStringLiteral(
                                "update.authenticode_invalid")));
            };
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "installer.authenticode"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "update.authenticode_invalid"));
    }
}

void stageFailuresAreIndependentlyReported()
{
    {
        Fixture fixture;
        require(
            QFile::remove(
                QDir(fixture.stagePath)
                    .filePath(
                        QStringLiteral(
                            "bin/Qt6Network.dll"))),
            QStringLiteral(
                "Could not remove required stage file."));
        require(
            QFile::remove(
                QDir(fixture.stagePath)
                    .filePath(
                        QStringLiteral(
                            "resources/skills/"
                            "companion-pet/SKILL.md"))),
            QStringLiteral(
                "Could not remove required skill fixture."));
        require(
            writeFile(
                QDir(fixture.stagePath)
                    .filePath(
                        QStringLiteral(
                            "debug/companion.pdb")),
                QByteArrayLiteral(
                    "debug symbols")),
            QStringLiteral(
                "Could not write forbidden fixture."));
        fixture.dependencies.iconInspector =
            [](
                QStringView,
                companion::
                    ExecutableIconExpectation) {
                return companion::Result<
                    bool>::success(false);
            };

        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "stage.completeness"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "release.stage_incomplete"));
        requireStatus(
            evidence,
            QStringLiteral("stage.icons"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "release.executable_icon_missing"));
        requireStatus(
            evidence,
            QStringLiteral(
                "stage.forbidden_files"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "release.forbidden_files_present"));
    }

    {
        Fixture fixture;
        require(
            writeFile(
                QDir(fixture.stagePath)
                    .filePath(
                        QStringLiteral(
                            "share/build-note.txt")),
                QByteArrayLiteral(
                    "built from C:\\Users\\secret\\"
                    "Codex Companion Windows "
                    "Worktrees\\native")),
            QStringLiteral(
                "Could not write worktree-path fixture."));
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "stage.forbidden_files"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "release.forbidden_files_present"));
    }

    {
        Fixture fixture;
        require(
            writeFile(
                QDir(fixture.stagePath)
                    .filePath(
                        QStringLiteral(
                            "share/secret-note.txt")),
                QByteArrayLiteral(
                    "-----BEGIN PRIVATE KEY-----\n"
                    "synthetic\n")),
            QStringLiteral(
                "Could not write private-key fixture."));
        const auto evidence =
            fixture.verify();
        requireStatus(
            evidence,
            QStringLiteral(
                "stage.forbidden_files"),
            companion::ReleaseCheckStatus::
                Failed,
            QStringLiteral(
                "release.forbidden_files_present"));
    }
}

void bundledPetArtworkIsRejected()
{
    Fixture fixture;
    require(
        writeFile(
            QDir(fixture.stagePath)
                .filePath(
                    QStringLiteral(
                        "resources/pets/private-pet/"
                        "pet.json")),
            QByteArrayLiteral("{}")),
        QStringLiteral(
            "Could not write bundled pet fixture."));
    const auto evidence =
        fixture.verify();
    requireStatus(
        evidence,
        QStringLiteral(
            "stage.forbidden_files"),
        companion::ReleaseCheckStatus::
            Failed,
        QStringLiteral(
            "release.forbidden_files_present"));
}

void invalidMetadataIsBlankedAndFails()
{
    Fixture fixture;
    fixture.options.metadata.sourceCommit =
        QStringLiteral(
            "C:\\Users\\secret\\source");
    const auto evidence =
        fixture.verify();
    requireStatus(
        evidence,
        QStringLiteral(
            "evidence.metadata"),
        companion::ReleaseCheckStatus::
            Failed,
        QStringLiteral(
            "release.invalid_evidence_metadata"));
    require(
        evidence.sourceCommit.isEmpty(),
        QStringLiteral(
            "Unsafe metadata was not blanked."));

    const QByteArray json =
        QJsonDocument(
            companion::
                releaseVerificationEvidenceJson(
                    evidence))
            .toJson();
    require(
        !json.toLower().contains(
            QByteArrayLiteral(
                "c:\\users\\")),
        QStringLiteral(
            "Unsafe metadata leaked into evidence."));
}

void metadataLoaderAcceptsSafeKnownFieldsOnly()
{
    QTemporaryDir temporary;
    require(
        temporary.isValid(),
        QStringLiteral(
            "Temporary directory is invalid."));
    const QString path =
        temporary.filePath(
            QStringLiteral(
                "release-metadata.json"));
    const QString commit(
        40,
        QLatin1Char('a'));
    const QJsonObject object{
        {
            QStringLiteral("schemaVersion"),
            1,
        },
        {
            QStringLiteral("sourceCommit"),
            commit,
        },
        {
            QStringLiteral("qtVersion"),
            QStringLiteral("6.11.1"),
        },
        {
            QStringLiteral("workspacePath"),
            QStringLiteral(
                "C:\\Users\\secret\\worktree"),
        },
    };
    require(
        writeFile(
            path,
            QJsonDocument(object)
                .toJson()),
        QStringLiteral(
            "Could not write metadata fixture."));

    const auto metadata =
        companion::
            loadReleaseEvidenceMetadata(path);
    require(
        metadata.hasValue(),
        QStringLiteral(
            "Safe release metadata was rejected."));
    require(
        metadata.value().sourceCommit
            == commit,
        QStringLiteral(
            "Source commit was not loaded."));
    require(
        metadata.value().qtVersion
            == QStringLiteral("6.11.1"),
        QStringLiteral(
            "Qt version was not loaded."));

    const QJsonObject invalid{
        {
            QStringLiteral("schemaVersion"),
            1,
        },
        {
            QStringLiteral("sourceCommit"),
            42,
        },
    };
    require(
        writeFile(
            path,
            QJsonDocument(invalid)
                .toJson()),
        QStringLiteral(
            "Could not write invalid metadata fixture."));
    require(
        !companion::
             loadReleaseEvidenceMetadata(
                 path)
             .hasValue(),
        QStringLiteral(
            "Wrong-type metadata was accepted."));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(
        argc,
        argv);

    const QList<std::pair<
        QString,
        std::function<void()>>>
        tests{
            {
                QStringLiteral(
                    "successful unsigned fixture"),
                successfulUnsignedFixtureProducesEvidence,
            },
            {
                QStringLiteral(
                    "invalid manifest"),
                invalidManifestFailsWithoutPathLeakage,
            },
            {
                QStringLiteral(
                    "installer digest"),
                installerSizeAndDigestAreExact,
            },
            {
                QStringLiteral(
                    "installer identity"),
                installerIdentityFailuresAreSeparated,
            },
            {
                QStringLiteral(
                    "stage failures"),
                stageFailuresAreIndependentlyReported,
            },
            {
                QStringLiteral(
                    "bundled pet artwork"),
                bundledPetArtworkIsRejected,
            },
            {
                QStringLiteral(
                    "metadata sanitization"),
                invalidMetadataIsBlankedAndFails,
            },
            {
                QStringLiteral(
                    "metadata loader"),
                metadataLoaderAcceptsSafeKnownFieldsOnly,
            },
        };

    int failures = 0;
    for (const auto& [name, test] :
         tests) {
        try {
            test();
            qInfo().noquote()
                << "PASS" << name;
        } catch (const std::exception&
                 error) {
            ++failures;
            qCritical().noquote()
                << "FAIL" << name << "-"
                << error.what();
        }
    }
    return failures == 0 ? 0 : 1;
}
