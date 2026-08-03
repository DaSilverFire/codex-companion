#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/UpdateCompatibility.h"

#include <array>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#define NOMINMAX
#include <windows.h>

namespace {

constexpr auto kTrustedSigner =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";

companion::CompanionError testError(
    QString code)
{
    return {
        std::move(code),
        QStringLiteral("test failure"),
    };
}

companion::UpdateManifest validManifest()
{
    companion::UpdateManifest manifest;
    manifest.schemaVersion = 1;
    manifest.version =
        QStringLiteral("0.3.4");
    manifest.build = 34;
    manifest.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    manifest.downloadUrl =
        QStringLiteral(
            "https://updates.example.test/"
            "Codex-Companion-0.3.4-34-windows-x64.exe");
    manifest.sha256 =
        QString::fromLatin1(
            QByteArray(32, char(0x5a))
                .toHex());
    manifest.size = 42'000;
    return manifest;
}

companion::ArtifactFacts validFacts()
{
    companion::ArtifactFacts facts;
    facts.path =
        QStringLiteral("C:\\Updates\\installer.exe");
    facts.exists = true;
    facts.regularFile = true;
    facts.size = 42'000;
    facts.sha256 =
        QByteArray(32, char(0x5a));
    facts.machine =
        companion::PeMachine::X64;
    facts.metadata = {
        QStringLiteral("Codex Companion"),
        QStringLiteral(
            "cc-update/1|0.3.4|34|w|x64|10.0.22000"),
        QStringLiteral(
            "Codex-Companion-0.3.4-34-windows-x64.exe"),
    };
    facts.signer = {
        QString::fromLatin1(kTrustedSigner),
        QStringLiteral("DaSilverFire"),
    };
    return facts;
}

QString validationCode(
    const companion::UpdateCompatibility&
        compatibility,
    const companion::UpdateManifest& manifest,
    const companion::ArtifactFacts& facts)
{
    const auto result =
        compatibility.validate(
            manifest,
            facts);
    return result.hasValue()
        ? QStringLiteral("<success>")
        : result.error().code;
}

QByteArray peFixture(
    quint16 machine)
{
    QByteArray bytes(0x86, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    qToLittleEndian<quint32>(
        0x80,
        reinterpret_cast<uchar*>(
            bytes.data() + 0x3c));
    bytes.replace(
        0x80,
        4,
        QByteArrayLiteral("PE\0\0"));
    qToLittleEndian<quint16>(
        machine,
        reinterpret_cast<uchar*>(
            bytes.data() + 0x84));
    return bytes;
}

QString writeFixture(
    QTemporaryDir* directory,
    QStringView name,
    const QByteArray& bytes)
{
    const QString path =
        directory->filePath(
            name.toString());
    QFile file(path);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)
        || file.write(bytes)
            != bytes.size()) {
        qFatal("could not write PE fixture");
    }
    return path;
}

} // namespace

class UpdateCompatibilityTests final
    : public QObject {
    Q_OBJECT

private slots:
    void parsesWindowsVersions_data();
    void parsesWindowsVersions();
    void comparesWindowsMinimums();
    void versionProviderUsesInjectedQuery();
    void productionVersionProviderReturnsWindowsVersion();
    void readsPeMachines_data();
    void readsPeMachines();
    void rejectsMalformedPeFiles();
    void readsInstallerVersionMetadata();
    void authenticodePolicyNormalizesAndMatches();
    void authenticodePolicyRejectsMissingOrWrongSigner();
    void authenticodeVerifierPropagatesNativeFailure();
    void compatibilityAcceptsMatchingArtifact();
    void compatibilityRejectsInSecurityOrder();
    void compatibilityRejectsMetadataDrift_data();
    void compatibilityRejectsMetadataDrift();
};

void UpdateCompatibilityTests::
    parsesWindowsVersions_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<quint32>("major");
    QTest::addColumn<quint32>("minor");
    QTest::addColumn<quint32>("build");
    QTest::addColumn<quint32>("revision");

    QTest::newRow("two-components")
        << QStringLiteral("10.0")
        << true
        << quint32(10)
        << quint32(0)
        << quint32(0)
        << quint32(0);
    QTest::newRow("three-components")
        << QStringLiteral("10.0.22000")
        << true
        << quint32(10)
        << quint32(0)
        << quint32(22000)
        << quint32(0);
    QTest::newRow("four-components")
        << QStringLiteral("10.0.22000.7")
        << true
        << quint32(10)
        << quint32(0)
        << quint32(22000)
        << quint32(7);
    QTest::newRow("one-component")
        << QStringLiteral("10")
        << false
        << quint32(0)
        << quint32(0)
        << quint32(0)
        << quint32(0);
    QTest::newRow("five-components")
        << QStringLiteral("10.0.22000.0.1")
        << false
        << quint32(0)
        << quint32(0)
        << quint32(0)
        << quint32(0);
    QTest::newRow("nonnumeric")
        << QStringLiteral("10.x.22000")
        << false
        << quint32(0)
        << quint32(0)
        << quint32(0)
        << quint32(0);
    QTest::newRow("empty-component")
        << QStringLiteral("10..22000")
        << false
        << quint32(0)
        << quint32(0)
        << quint32(0)
        << quint32(0);
    QTest::newRow("signed-component")
        << QStringLiteral("10.0.+22000")
        << false
        << quint32(0)
        << quint32(0)
        << quint32(0)
        << quint32(0);
}

void UpdateCompatibilityTests::
    parsesWindowsVersions()
{
    QFETCH(QString, value);
    QFETCH(bool, valid);
    QFETCH(quint32, major);
    QFETCH(quint32, minor);
    QFETCH(quint32, build);
    QFETCH(quint32, revision);

    const auto parsed =
        companion::WindowsVersion::parse(value);
    QCOMPARE(parsed.has_value(), valid);
    if (!parsed) {
        return;
    }
    QCOMPARE(parsed->major, major);
    QCOMPARE(parsed->minor, minor);
    QCOMPARE(parsed->build, build);
    QCOMPARE(parsed->revision, revision);
}

void UpdateCompatibilityTests::
    comparesWindowsMinimums()
{
    const auto minimum =
        companion::WindowsVersion::parse(
            QStringLiteral("10.0.22000"));
    QVERIFY(minimum.has_value());

    const companion::WindowsVersion equal{
        10,
        0,
        22000,
        0,
    };
    const companion::WindowsVersion newerBuild{
        10,
        0,
        26100,
        0,
    };
    const companion::WindowsVersion olderBuild{
        10,
        0,
        21999,
        0,
    };
    const companion::WindowsVersion missingBuild{
        10,
        0,
        0,
        0,
    };
    const companion::WindowsVersion newerMajor{
        11,
        0,
        0,
        0,
    };

    QVERIFY(equal >= *minimum);
    QVERIFY(newerBuild >= *minimum);
    QVERIFY(olderBuild < *minimum);
    QVERIFY(missingBuild < *minimum);
    QVERIFY(newerMajor >= *minimum);
}

void UpdateCompatibilityTests::
    versionProviderUsesInjectedQuery()
{
    int calls = 0;
    const companion::WindowsVersion expected{
        10,
        0,
        26100,
        0,
    };
    const companion::WindowsVersionProvider
        provider(
            [&calls, expected] {
                ++calls;
                return companion::Result<
                    companion::WindowsVersion>::
                    success(expected);
            });

    const auto current =
        provider.current();
    QVERIFY(current.hasValue());
    QCOMPARE(current.value(), expected);
    QCOMPARE(calls, 1);
}

void UpdateCompatibilityTests::
    productionVersionProviderReturnsWindowsVersion()
{
    const companion::WindowsVersionProvider
        provider;
    const auto current =
        provider.current();
    QVERIFY2(
        current.hasValue(),
        qPrintable(
            current.hasValue()
                ? QString()
                : current.error().message));
    QVERIFY(current.value().major >= 10);
    QVERIFY(current.value().build > 0);
}

void UpdateCompatibilityTests::
    readsPeMachines_data()
{
    QTest::addColumn<quint16>("rawMachine");
    QTest::addColumn<companion::PeMachine>(
        "expected");

    QTest::newRow("x64")
        << quint16(IMAGE_FILE_MACHINE_AMD64)
        << companion::PeMachine::X64;
    QTest::newRow("x86")
        << quint16(IMAGE_FILE_MACHINE_I386)
        << companion::PeMachine::X86;
    QTest::newRow("arm64")
        << quint16(IMAGE_FILE_MACHINE_ARM64)
        << companion::PeMachine::Arm64;
    QTest::newRow("unknown")
        << quint16(0x1234)
        << companion::PeMachine::Unknown;
}

void UpdateCompatibilityTests::
    readsPeMachines()
{
    QFETCH(quint16, rawMachine);
    QFETCH(companion::PeMachine, expected);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path =
        writeFixture(
            &directory,
            QStringLiteral("fixture.exe"),
            peFixture(rawMachine));
    const companion::PeImageInspector
        inspector;
    const auto machine =
        inspector.machine(path);
    QVERIFY(machine.hasValue());
    QCOMPARE(machine.value(), expected);
}

void UpdateCompatibilityTests::
    rejectsMalformedPeFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const companion::PeImageInspector
        inspector;

    const QString truncated =
        writeFixture(
            &directory,
            QStringLiteral("truncated.exe"),
            QByteArrayLiteral("MZ"));
    const auto truncatedResult =
        inspector.machine(truncated);
    QVERIFY(!truncatedResult.hasValue());
    QCOMPARE(
        truncatedResult.error().code,
        QStringLiteral("update.invalid_pe"));

    QByteArray invalidOffset =
        peFixture(
            IMAGE_FILE_MACHINE_AMD64);
    qToLittleEndian<quint32>(
        0xfffffff0U,
        reinterpret_cast<uchar*>(
            invalidOffset.data() + 0x3c));
    const auto offsetResult =
        inspector.machine(
            writeFixture(
                &directory,
                QStringLiteral(
                    "invalid-offset.exe"),
                invalidOffset));
    QVERIFY(!offsetResult.hasValue());
    QCOMPARE(
        offsetResult.error().code,
        QStringLiteral("update.invalid_pe"));
}

void UpdateCompatibilityTests::
    readsInstallerVersionMetadata()
{
    const companion::InstallerMetadataReader
        reader;
    const auto metadata =
        reader.read(
            QCoreApplication::
                applicationFilePath());
    QVERIFY2(
        metadata.hasValue(),
        qPrintable(
            metadata.hasValue()
                ? QString()
                : metadata.error().message));
    QCOMPARE(
        metadata.value().productName,
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        metadata.value()
            .productVersionMarker,
        QStringLiteral(
            "cc-update/1|0.3.4|34|w|x64|10.0.22000"));
    QCOMPARE(
        metadata.value().originalFilename,
        QStringLiteral(
            "Codex-Companion-0.3.4-34-windows-x64.exe"));
}

void UpdateCompatibilityTests::
    authenticodePolicyNormalizesAndMatches()
{
    bool cacheOnly = true;
    const companion::AuthenticodeVerifier
        verifier(
            [&cacheOnly](
                QStringView,
                bool requestedCacheOnly) {
                cacheOnly =
                    requestedCacheOnly;
                return companion::Result<
                    companion::
                        AuthenticodeIdentity>::
                    success(
                        {
                            QStringLiteral(
                                "01:23:45:67:89:ab:cd:ef:"
                                "01:23:45:67:89:ab:cd:ef:"
                                "01:23:45:67:89:ab:cd:ef:"
                                "01:23:45:67:89:ab:cd:ef"),
                            QStringLiteral(
                                "DaSilverFire"),
                        });
            });
    companion::AuthenticodePolicy policy;
    policy.allowedSignerSha256 = {
        QString::fromLatin1(
            kTrustedSigner),
    };

    const auto result =
        verifier.verify(
            QStringLiteral(
                "C:\\Updates\\installer.exe"),
            policy);
    QVERIFY(result.hasValue());
    QCOMPARE(
        result.value().sha256Thumbprint,
        QString::fromLatin1(
            kTrustedSigner));
    QVERIFY(!cacheOnly);
}

void UpdateCompatibilityTests::
    authenticodePolicyRejectsMissingOrWrongSigner()
{
    const companion::AuthenticodeVerifier
        verifier(
            [](
                QStringView,
                bool) {
                return companion::Result<
                    companion::
                        AuthenticodeIdentity>::
                    success(
                        {
                            QString(64,
                                    QLatin1Char('A')),
                            QStringLiteral(
                                "Other Publisher"),
                        });
            });

    const auto missingPolicy =
        verifier.verify(
            QStringLiteral(
                "C:\\Updates\\installer.exe"),
            {});
    QVERIFY(!missingPolicy.hasValue());
    QCOMPARE(
        missingPolicy.error().code,
        QStringLiteral(
            "update.signer_policy_unconfigured"));

    companion::AuthenticodePolicy policy;
    policy.allowedSignerSha256 = {
        QString::fromLatin1(
            kTrustedSigner),
    };
    const auto wrongSigner =
        verifier.verify(
            QStringLiteral(
                "C:\\Updates\\installer.exe"),
            policy);
    QVERIFY(!wrongSigner.hasValue());
    QCOMPARE(
        wrongSigner.error().code,
        QStringLiteral(
            "update.untrusted_signer"));
}

void UpdateCompatibilityTests::
    authenticodeVerifierPropagatesNativeFailure()
{
    const companion::AuthenticodeVerifier
        verifier(
            [](
                QStringView,
                bool) {
                return companion::Result<
                    companion::
                        AuthenticodeIdentity>::
                    failure(
                        testError(
                            QStringLiteral(
                                "update.authenticode_invalid")));
            });
    companion::AuthenticodePolicy policy;
    policy.allowedSignerSha256 = {
        QString::fromLatin1(
            kTrustedSigner),
    };

    const auto result =
        verifier.verify(
            QStringLiteral(
                "C:\\Updates\\installer.exe"),
            policy);
    QVERIFY(!result.hasValue());
    QCOMPARE(
        result.error().code,
        QStringLiteral(
            "update.authenticode_invalid"));
}

void UpdateCompatibilityTests::
    compatibilityAcceptsMatchingArtifact()
{
    const companion::UpdateCompatibility
        compatibility(
            {
                10,
                0,
                26100,
                0,
            },
            {
                QString::fromLatin1(
                    kTrustedSigner),
            });
    QCOMPARE(
        validationCode(
            compatibility,
            validManifest(),
            validFacts()),
        QStringLiteral("<success>"));

    companion::ArtifactFacts innoFacts =
        validFacts();
    innoFacts.machine =
        companion::PeMachine::X86;
    QCOMPARE(
        validationCode(
            compatibility,
            validManifest(),
            innoFacts),
        QStringLiteral("<success>"));
}

void UpdateCompatibilityTests::
    compatibilityRejectsInSecurityOrder()
{
    const companion::UpdateManifest manifest =
        validManifest();
    companion::ArtifactFacts facts =
        validFacts();

    const companion::UpdateCompatibility
        oldWindows(
            {
                10,
                0,
                21999,
                0,
            },
            {});
    facts.exists = false;
    QCOMPARE(
        validationCode(
            oldWindows,
            manifest,
            facts),
        QStringLiteral(
            "update.unsupported_windows_version"));

    const companion::UpdateCompatibility
        compatibility(
            {
                10,
                0,
                26100,
                0,
            },
            {
                QString::fromLatin1(
                    kTrustedSigner),
            });
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_missing"));

    facts = validFacts();
    facts.regularFile = false;
    facts.reparsePoint = true;
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_not_regular_file"));

    facts = validFacts();
    facts.reparsePoint = true;
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_reparse_point"));

    facts = validFacts();
    facts.size += 1;
    facts.sha256.clear();
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_size_mismatch"));

    facts = validFacts();
    facts.sha256[0] = char(0);
    facts.machine =
        companion::PeMachine::X86;
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_digest_mismatch"));

    facts = validFacts();
    facts.machine =
        companion::PeMachine::Arm64;
    facts.metadata.productName =
        QStringLiteral("Other");
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.artifact_architecture_mismatch"));

    facts = validFacts();
    facts.metadata.productName =
        QStringLiteral("Other");
    facts.signer.sha256Thumbprint =
        QString(64, QLatin1Char('A'));
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.installer_identity_mismatch"));

    facts = validFacts();
    facts.signer.sha256Thumbprint =
        QString(64, QLatin1Char('A'));
    QCOMPARE(
        validationCode(
            compatibility,
            manifest,
            facts),
        QStringLiteral(
            "update.untrusted_signer"));
}

void UpdateCompatibilityTests::
    compatibilityRejectsMetadataDrift_data()
{
    QTest::addColumn<QString>("field");
    QTest::newRow("product-name")
        << QStringLiteral("productName");
    QTest::newRow("marker")
        << QStringLiteral(
            "productVersionMarker");
    QTest::newRow("filename")
        << QStringLiteral(
            "originalFilename");
}

void UpdateCompatibilityTests::
    compatibilityRejectsMetadataDrift()
{
    QFETCH(QString, field);
    companion::ArtifactFacts facts =
        validFacts();
    if (field
        == QStringLiteral("productName")) {
        facts.metadata.productName =
            QStringLiteral(
                "Codex Companion Preview");
    } else if (field
               == QStringLiteral(
                   "productVersionMarker")) {
        facts.metadata.productVersionMarker =
            QStringLiteral(
                "cc-update/1|0.3.4|35|w|x64|10.0.22000");
    } else {
        facts.metadata.originalFilename =
            QStringLiteral(
                "Codex-Companion-0.3.4-35-windows-x64.exe");
    }

    const companion::UpdateCompatibility
        compatibility(
            {
                10,
                0,
                26100,
                0,
            },
            {
                QString::fromLatin1(
                    kTrustedSigner),
            });
    QCOMPARE(
        validationCode(
            compatibility,
            validManifest(),
            facts),
        QStringLiteral(
            "update.installer_identity_mismatch"));
}

QTEST_GUILESS_MAIN(UpdateCompatibilityTests)
#include "UpdateCompatibilityTests.moc"
