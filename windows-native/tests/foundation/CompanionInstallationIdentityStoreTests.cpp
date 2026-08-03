#include "core/CompanionInstallationIdentityStore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace companion;

class CompanionInstallationIdentityStoreTests final
    : public QObject {
    Q_OBJECT

private slots:
    void persistsNormalizesAndRepairsTheIdentity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "Security/installation-id"));

        CompanionInstallationIdentityStore
            firstStore(path);
        const auto first =
            firstStore.loadOrCreate();
        QVERIFY(first.hasValue());
        const QUuid firstId(first.value());
        QVERIFY(!firstId.isNull());
        QCOMPARE(
            first.value(),
            firstId.toString(
                       QUuid::WithoutBraces)
                .toUpper());

        CompanionInstallationIdentityStore
            secondStore(path);
        const auto second =
            secondStore.loadOrCreate();
        QVERIFY(second.hasValue());
        QCOMPARE(second.value(), first.value());

        QFile invalid(path);
        QVERIFY(
            invalid.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate));
        QCOMPARE(
            invalid.write("invalid"),
            qint64(7));
        invalid.close();

        CompanionInstallationIdentityStore
            repairStore(path);
        const auto repaired =
            repairStore.loadOrCreate();
        QVERIFY(repaired.hasValue());
        QVERIFY(
            !QUuid(repaired.value())
                 .isNull());
        QVERIFY(
            repaired.value()
            != QStringLiteral("invalid"));

        CompanionInstallationIdentityStore
            reloadedStore(path);
        const auto reloaded =
            reloadedStore.loadOrCreate();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(
            reloaded.value(),
            repaired.value());
    }

    void cachesTheLoadedIdentityWithinOneStore()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(
                QStringLiteral(
                    "installation-id"));
        CompanionInstallationIdentityStore
            store(path);
        const auto first =
            store.loadOrCreate();
        QVERIFY(first.hasValue());

        QFile replacement(path);
        QVERIFY(
            replacement.open(
                QIODevice::WriteOnly
                | QIODevice::Truncate));
        const QByteArray other(
            "11111111-2222-3333-4444-555555555555");
        QCOMPARE(
            replacement.write(other),
            qint64(other.size()));
        replacement.close();

        const auto cached =
            store.loadOrCreate();
        QVERIFY(cached.hasValue());
        QCOMPARE(cached.value(), first.value());
    }

    void reportsAnAtomicWriteFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString blockingPath =
            directory.filePath(
                QStringLiteral("Security"));
        QFile blockingFile(blockingPath);
        QVERIFY(
            blockingFile.open(
                QIODevice::WriteOnly));
        blockingFile.close();

        CompanionInstallationIdentityStore
            store(
                QDir(blockingPath)
                    .filePath(
                        QStringLiteral(
                            "installation-id")));
        const auto result =
            store.loadOrCreate();
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "mobile.installation_identity_unavailable"));
    }

    void defaultPathUsesWindowsApplicationData()
    {
        const QString expectedRoot =
            QStandardPaths::writableLocation(
                QStandardPaths::
                    AppDataLocation);
        const QString path =
            CompanionInstallationIdentityStore::
                defaultFilePath();
        QVERIFY(!path.isEmpty());
        QVERIFY(
            QDir::cleanPath(path)
                .startsWith(
                    QDir::cleanPath(
                        expectedRoot),
                    Qt::CaseInsensitive));
        QVERIFY(
            QDir::fromNativeSeparators(path)
                .endsWith(
                    QStringLiteral(
                        "/Security/installation-id")));
    }
};

QTEST_GUILESS_MAIN(
    CompanionInstallationIdentityStoreTests)
#include "CompanionInstallationIdentityStoreTests.moc"
