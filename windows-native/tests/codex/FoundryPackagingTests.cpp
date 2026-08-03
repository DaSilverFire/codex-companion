#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QtTest>

namespace {

QJsonObject readJsonObject(
    const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            file.readAll(),
            &error);
    if (error.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return {};
    }
    return document.object();
}

QByteArray sha256(
    const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

class FoundryPackagingTests final
    : public QObject {
    Q_OBJECT

private slots:
    void manifestsPinDependencies();
    void applicationStagesRuntime();
};

void FoundryPackagingTests::
    manifestsPinDependencies()
{
    const QDir sourceRoot(
        QStringLiteral(
            COMPANION_SOURCE_ROOT));
    const QJsonObject manifest =
        readJsonObject(
            sourceRoot.filePath(
                QStringLiteral(
                    "vcpkg.json")));
    QVERIFY(!manifest.isEmpty());
    QCOMPARE(
        manifest.value(
                    QStringLiteral("name"))
            .toString(),
        QStringLiteral(
            "codex-companion-windows"));
    QCOMPARE(
        manifest.value(
                    QStringLiteral(
                        "version-string"))
            .toString(),
        QStringLiteral("0.3.4"));

    QSet<QString> dependencies;
    for (const QJsonValue value :
         manifest.value(
                     QStringLiteral(
                         "dependencies"))
             .toArray()) {
        dependencies.insert(
            value.toString());
    }
    QCOMPARE(
        dependencies,
        QSet<QString>({
            QStringLiteral("ms-gsl"),
            QStringLiteral(
                "nlohmann-json"),
        }));

    const QString baseline =
        QStringLiteral(
            "a9f0cd0345fb29cd227d802f1fd1917c"
            "28f8e5a3");
    const QJsonObject configuration =
        readJsonObject(
            sourceRoot.filePath(
                QStringLiteral(
                    "vcpkg-configuration.json")));
    QVERIFY(!configuration.isEmpty());
    const QJsonObject registry =
        configuration.value(
                         QStringLiteral(
                             "default-registry"))
            .toObject();
    QCOMPARE(
        registry.value(
                    QStringLiteral("kind"))
            .toString(),
        QStringLiteral("filesystem"));
    QCOMPARE(
        registry.value(
                    QStringLiteral(
                        "baseline"))
            .toString(),
        baseline);
    QCOMPARE(
        registry.value(
                    QStringLiteral("path"))
            .toString(),
        QStringLiteral(
            ".deps/vcpkg/")
            + baseline
            + QStringLiteral("/registry"));

    const QJsonObject foundryStamp =
        readJsonObject(
            sourceRoot.filePath(
                QStringLiteral(
                    ".deps/"
                    "foundry-local/1.2.1/"
                    "bootstrap-stamp.json")));
    QVERIFY(!foundryStamp.isEmpty());
    QCOMPARE(
        foundryStamp.value(
                        QStringLiteral(
                            "foundryVersion"))
            .toString(),
        QStringLiteral("1.2.1"));
    QCOMPARE(
        foundryStamp.value(
                        QStringLiteral(
                            "sourceCommit"))
            .toString(),
        QStringLiteral(
            "e3e134c0f56b18523bec5f2b28b8e921"
            "080dda23"));
    QCOMPARE(
        foundryStamp.value(
                        QStringLiteral(
                            "sourceSha256"))
            .toString(),
        QStringLiteral(
            "9206e71571bb6b80aff296165fd9dfe7b"
            "869dfab1775d4bda8a10ef71db3995b"));

    const QJsonObject vcpkgStamp =
        readJsonObject(
            sourceRoot.filePath(
                QStringLiteral(
                    ".deps/vcpkg/")
                + baseline
                + QStringLiteral(
                    "/bootstrap-stamp.json")));
    QVERIFY(!vcpkgStamp.isEmpty());
    QCOMPARE(
        vcpkgStamp.value(
                      QStringLiteral(
                          "baseline"))
            .toString(),
        baseline);
    QCOMPARE(
        vcpkgStamp.value(
                      QStringLiteral(
                          "triplet"))
            .toString(),
        QStringLiteral(
            "x64-windows-static-md"));
}

void FoundryPackagingTests::
    applicationStagesRuntime()
{
    const QDir applicationDirectory(
        QStringLiteral(
            COMPANION_PACKAGED_APP_DIR));
    QVERIFY2(
        applicationDirectory.exists(
            QStringLiteral(
                "CodexCompanion.exe")),
        qPrintable(
            applicationDirectory
                .absolutePath()));

    const QList<
        std::pair<QString, QByteArray>>
        runtimeFiles = {
            {
                QStringLiteral(
                    "Microsoft.AI.Foundry."
                    "Local.Core.dll"),
                QByteArrayLiteral(
                    "171e368e7e4579e60946bae9a1836d66"
                    "34beb01b7422d69948094b6e49c355c7"),
            },
            {
                QStringLiteral(
                    "onnxruntime.dll"),
                QByteArrayLiteral(
                    "6a4129504501cbd615efddc897345ec9"
                    "557390b408887165ab5faf9812a54b31"),
            },
            {
                QStringLiteral(
                    "onnxruntime_providers_shared."
                    "dll"),
                QByteArrayLiteral(
                    "97fc0ccc43386f8769a0afc43fb1dba"
                    "3a066f718cd1fe0e8f540e24e0ecb61a7"),
            },
            {
                QStringLiteral(
                    "onnxruntime-genai.dll"),
                QByteArrayLiteral(
                    "762a76aa622eb2e7b1ee977752e1a30f"
                    "763669d1037e9be18d036668e0b1ef27"),
            },
        };
    for (const auto& [name, expectedHash] :
         runtimeFiles) {
        const QString path =
            applicationDirectory.filePath(
                name);
        QVERIFY2(
            QFileInfo::exists(path),
            qPrintable(path));
        QCOMPARE(
            sha256(path),
            expectedHash);
    }

    const QDir noticeDirectory(
        applicationDirectory.filePath(
            QStringLiteral(
                "licenses/foundry-local")));
    const QStringList expectedNotices = {
        QStringLiteral(
            "Foundry-Core-LICENSE.txt"),
        QStringLiteral(
            "Foundry-Local-LICENSE"),
        QStringLiteral(
            "ONNX-Runtime-GenAI-LICENSE"),
        QStringLiteral(
            "ONNX-Runtime-GenAI-"
            "ThirdPartyNotices.txt"),
        QStringLiteral(
            "ONNX-Runtime-LICENSE"),
        QStringLiteral(
            "ONNX-Runtime-"
            "ThirdPartyNotices.txt"),
    };
    QCOMPARE(
        noticeDirectory.entryList(
            QDir::Files,
            QDir::Name),
        expectedNotices);
    for (const QString& notice :
         expectedNotices) {
        QVERIFY(
            QFileInfo(
                noticeDirectory.filePath(
                    notice))
                .size()
            > 0);
    }
}

} // namespace

QTEST_MAIN(FoundryPackagingTests)

#include "FoundryPackagingTests.moc"
