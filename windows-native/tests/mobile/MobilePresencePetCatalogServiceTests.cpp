#include "mobile/presence/MobilePresencePetCatalogService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace companion;

namespace {

const QString kPackageId =
    QStringLiteral("fixture-pet-mobile-presence-v1");
const QString kContentHash =
    QStringLiteral(
        "20f53b7f5c377426581f06c213c6f238ed3bbcaec70c468813295c9c96132804");
const QString kAtlasHash =
    QStringLiteral(
        "0edc426f78aa5211741d90e28a5e4810c555779b6c47ef01c4932e69e96f4342");
const QString kThumbnailHash =
    QStringLiteral(
        "e3062dacaf5a040ede4f496bab235440a010615ff710b4cbc5c25ab791be078b");

QString fixtureDirectory()
{
    return QDir::cleanPath(
        QStringLiteral(
            COMPANION_MOBILE_PRESENCE_FIXTURE_ROOT));
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool writeFile(
    const QString& path,
    const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

bool copyFixture(
    const QString& destination)
{
    if (!QDir().mkpath(destination)) {
        return false;
    }
    for (const QString& fileName :
         {
             QStringLiteral("manifest.json"),
             QStringLiteral("atlas.png"),
             QStringLiteral("thumbnail.png"),
         }) {
        const QString source =
            QDir(fixtureDirectory())
                .filePath(fileName);
        const QString target =
            QDir(destination)
                .filePath(fileName);
        if (!QFile::copy(source, target)) {
            return false;
        }
    }
    return true;
}

QJsonObject manifestObject(
    const QString& packageDirectory)
{
    return QJsonDocument::fromJson(
               readFile(
                   QDir(packageDirectory)
                       .filePath(
                           QStringLiteral(
                               "manifest.json"))))
        .object();
}

QString canonicalContentHash(
    QJsonObject manifest)
{
    manifest.remove(
        QStringLiteral("contentHash"));
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(manifest)
                .toJson(
                    QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}

QString writeManifest(
    const QString& packageDirectory,
    QJsonObject manifest,
    qsizetype leadingWhitespace = 0)
{
    const QString contentHash =
        canonicalContentHash(manifest);
    manifest.insert(
        QStringLiteral("contentHash"),
        contentHash);
    QByteArray bytes(
        leadingWhitespace,
        ' ');
    bytes.append(
        QJsonDocument(manifest)
            .toJson(
                QJsonDocument::Indented));
    if (!writeFile(
            QDir(packageDirectory)
                .filePath(
                    QStringLiteral(
                        "manifest.json")),
            bytes)) {
        return {};
    }
    return contentHash;
}

void synchronizeFileRecord(
    QJsonObject& manifest,
    const QString& packageDirectory,
    bool atlas)
{
    const QString fileName =
        atlas
        ? QStringLiteral("atlas.png")
        : QStringLiteral("thumbnail.png");
    const QByteArray bytes =
        readFile(
            QDir(packageDirectory)
                .filePath(fileName));
    QJsonObject record;
    if (atlas) {
        QJsonObject atlasObject =
            manifest.value(
                QStringLiteral("atlas"))
                .toObject();
        record = atlasObject
                     .value(
                         QStringLiteral("file"))
                     .toObject();
        record.insert(
            QStringLiteral("byteCount"),
            bytes.size());
        record.insert(
            QStringLiteral("sha256"),
            QString::fromLatin1(
                QCryptographicHash::hash(
                    bytes,
                    QCryptographicHash::Sha256)
                    .toHex()));
        atlasObject.insert(
            QStringLiteral("file"),
            record);
        manifest.insert(
            QStringLiteral("atlas"),
            atlasObject);
        return;
    }

    record = manifest
                 .value(
                     QStringLiteral("thumbnail"))
                 .toObject();
    record.insert(
        QStringLiteral("byteCount"),
        bytes.size());
    record.insert(
        QStringLiteral("sha256"),
        QString::fromLatin1(
            QCryptographicHash::hash(
                bytes,
                QCryptographicHash::Sha256)
                .toHex()));
    manifest.insert(
        QStringLiteral("thumbnail"),
        record);
}

MobilePresencePetPackageSource packageSource(
    const QString& packageDirectory,
    QString contentHash = kContentHash)
{
    return {
        QStringLiteral("fixture-pet"),
        QStringLiteral("Fixture Pet"),
        packageDirectory,
        kPackageId,
        std::move(contentHash),
    };
}

MobilePresencePetCatalogSnapshot snapshot(
    const QString& packageDirectory,
    QString contentHash = kContentHash,
    QString selectedPetId =
        QStringLiteral("fixture-pet"))
{
    return {
        std::move(selectedPetId),
        {
            packageSource(
                packageDirectory,
                std::move(contentHash)),
        },
    };
}

bool createOpaquePng(
    const QString& path,
    int width,
    int height)
{
    QImage image(
        width,
        height,
        QImage::Format_ARGB32);
    image.fill(qRgba(24, 40, 72, 255));
    return image.save(path, "PNG");
}

bool createTransparentPng(
    const QString& path,
    int width,
    int height)
{
    QImage image(
        width,
        height,
        QImage::Format_ARGB32);
    image.fill(qRgba(0, 0, 0, 0));
    return image.save(path, "PNG");
}

bool createDirectoryJunction(
    const QString& junctionPath,
    const QString& targetPath,
    QString* errorMessage)
{
    QProcess process;
    process.setProgram(
        qEnvironmentVariable(
            "COMSPEC",
            QStringLiteral(
                "C:\\Windows\\System32\\cmd.exe")));
    process.setNativeArguments(
        QStringLiteral(
            "/d /c mklink /J \"%1\" \"%2\"")
            .arg(
                QDir::toNativeSeparators(
                    junctionPath),
                QDir::toNativeSeparators(
                    targetPath)));
    process.setProcessChannelMode(
        QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000)
        || !process.waitForFinished(5000)
        || process.exitStatus()
            != QProcess::NormalExit
        || process.exitCode() != 0) {
        if (errorMessage != nullptr) {
            *errorMessage =
                QString::fromLocal8Bit(
                    process.readAll())
                    .trimmed();
        }
        return false;
    }
    return true;
}

void verifyRejectedPackage(
    const QString& packageDirectory,
    const QString& contentHash,
    const QString& expectedCode)
{
    MobilePresencePetCatalogService service;
    const QVector<CompanionError> diagnostics =
        service.replaceSnapshot(
            snapshot(
                packageDirectory,
                contentHash));
    QCOMPARE(diagnostics.size(), 1);
    QCOMPARE(
        diagnostics.front().code,
        expectedCode);
    QCOMPARE(
        service.presentation().catalog.size(),
        0);
}

} // namespace

class MobilePresencePetCatalogServiceTests final
    : public QObject {
    Q_OBJECT

private slots:
    void indexesValidFixtureAndReturnsExactManifest()
    {
        MobilePresencePetCatalogService service;
        const QVector<CompanionError> diagnostics =
            service.replaceSnapshot(
                snapshot(fixtureDirectory()));

        QVERIFY(diagnostics.isEmpty());
        const auto presentation =
            service.presentation();
        QVERIFY(
            presentation.selectedDesktopPetId
                .has_value());
        QCOMPARE(
            *presentation.selectedDesktopPetId,
            QStringLiteral("fixture-pet"));
        QCOMPARE(
            presentation.catalog.size(),
            1);
        const BridgePresencePetCatalogEntry&
            entry = presentation.catalog.front();
        QCOMPARE(entry.packageId, kPackageId);
        QCOMPARE(
            entry.petId,
            QStringLiteral("fixture-pet"));
        QCOMPARE(
            entry.displayName,
            QStringLiteral("Fixture Pet"));
        QCOMPARE(
            entry.assetVersion,
            QStringLiteral("1"));
        QCOMPARE(entry.contentHash, kContentHash);
        QCOMPARE(entry.byteCount, qint64(750129));
        QCOMPARE(
            entry.thumbnail.name,
            QStringLiteral("thumbnail.png"));
        QCOMPARE(
            entry.thumbnail.sha256,
            kThumbnailHash);
        QCOMPARE(
            entry.thumbnail.byteCount,
            qint64(57752));

        const auto result =
            service.manifest(
                kPackageId,
                kContentHash);
        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error().message));
        const BridgePresencePetManifest&
            manifest = result.value();
        QCOMPARE(manifest.schemaVersion, qint64(1));
        QCOMPARE(manifest.packageId, kPackageId);
        QCOMPARE(
            manifest.petId,
            QStringLiteral("fixture-pet"));
        QCOMPARE(
            manifest.displayName,
            QStringLiteral("Fixture Pet"));
        QCOMPARE(
            manifest.assetVersion,
            QStringLiteral("1"));
        QCOMPARE(manifest.contentHash, kContentHash);
        QCOMPARE(
            manifest.atlas.file.name,
            QStringLiteral("atlas.png"));
        QCOMPARE(
            manifest.atlas.file.sha256,
            kAtlasHash);
        QCOMPARE(
            manifest.atlas.file.byteCount,
            qint64(691112));
        QCOMPARE(
            manifest.atlas.cellWidth,
            qint64(128));
        QCOMPARE(
            manifest.atlas.cellHeight,
            qint64(128));
        QCOMPARE(
            manifest.atlas.columns,
            qint64(4));
        QCOMPARE(
            manifest.atlas.rows,
            qint64(3));
        QCOMPARE(manifest.animations.size(), 3);
        QCOMPARE(
            manifest.animations.at(0).state,
            BridgePresencePetState::Idle);
        QCOMPARE(
            manifest.animations.at(0)
                .frameDurationsMilliseconds,
            QVector<qint64>({
                400, 160, 160, 520,
            }));
        QCOMPARE(
            manifest.animations.at(1).state,
            BridgePresencePetState::Thinking);
        QCOMPARE(
            manifest.animations.at(2).state,
            BridgePresencePetState::Talking);
    }

    void returnsBoundedResumableChunks()
    {
        MobilePresencePetCatalogService service;
        QVERIFY(
            service.replaceSnapshot(
                       snapshot(
                           fixtureDirectory()))
                .isEmpty());
        const QByteArray atlas =
            readFile(
                QDir(fixtureDirectory())
                    .filePath(
                        QStringLiteral(
                            "atlas.png")));
        QCOMPARE(atlas.size(), 691112);

        const auto first = service.chunk(
            kPackageId,
            kContentHash,
            QStringLiteral("atlas.png"),
            0,
            999999);
        QVERIFY(first.hasValue());
        QCOMPARE(
            first.value().data.size(),
            qsizetype(
                MobilePresencePetCatalogService::
                    kMaximumChunkLength));
        QCOMPARE(
            first.value().data,
            atlas.first(
                MobilePresencePetCatalogService::
                    kMaximumChunkLength));
        QCOMPARE(
            first.value().nextOffset,
            MobilePresencePetCatalogService::
                kMaximumChunkLength);
        QVERIFY(!first.value().isComplete);

        const qint64 finalOffset =
            MobilePresencePetCatalogService::
                kMaximumChunkLength
            * 2;
        const auto finalChunk = service.chunk(
            kPackageId,
            kContentHash,
            QStringLiteral("atlas.png"),
            finalOffset,
            999999);
        QVERIFY(finalChunk.hasValue());
        QCOMPARE(
            finalChunk.value().data,
            atlas.mid(finalOffset));
        QCOMPARE(
            finalChunk.value().nextOffset,
            qint64(691112));
        QVERIFY(finalChunk.value().isComplete);

        const auto atEnd = service.chunk(
            kPackageId,
            kContentHash,
            QStringLiteral("atlas.png"),
            atlas.size(),
            1);
        QVERIFY(atEnd.hasValue());
        QVERIFY(atEnd.value().data.isEmpty());
        QCOMPARE(
            atEnd.value().nextOffset,
            qint64(691112));
        QVERIFY(atEnd.value().isComplete);
    }

    void rejectsStaleHashUnknownPackageAndUnknownFile()
    {
        MobilePresencePetCatalogService service;
        QVERIFY(
            service.replaceSnapshot(
                       snapshot(
                           fixtureDirectory()))
                .isEmpty());

        const auto unknownManifest =
            service.manifest(
                QStringLiteral("unknown-package"),
                kContentHash);
        QVERIFY(!unknownManifest.hasValue());
        QCOMPARE(
            unknownManifest.error().code,
            QStringLiteral(
                "presence_pet_not_found"));

        const auto staleManifest =
            service.manifest(
                kPackageId,
                QString(64, QLatin1Char('0')));
        QVERIFY(!staleManifest.hasValue());
        QCOMPARE(
            staleManifest.error().code,
            QStringLiteral(
                "stale_presence_pet"));

        const auto unknownFile = service.chunk(
            kPackageId,
            kContentHash,
            QStringLiteral("poster.png"),
            0,
            1);
        QVERIFY(!unknownFile.hasValue());
        QCOMPARE(
            unknownFile.error().code,
            QStringLiteral(
                "presence_pet_not_found"));

        const auto traversal = service.chunk(
            kPackageId,
            kContentHash,
            QStringLiteral("../atlas.png"),
            0,
            1);
        QVERIFY(!traversal.hasValue());
        QCOMPARE(
            traversal.error().code,
            QStringLiteral(
                "unsafe_presence_pet_path"));

        for (const auto [offset, length] :
             {
                 std::pair<qint64, qint64>{-1, 1},
                 std::pair<qint64, qint64>{0, 0},
                 std::pair<qint64, qint64>{691113, 1},
             }) {
            const auto invalidRange =
                service.chunk(
                    kPackageId,
                    kContentHash,
                    QStringLiteral("atlas.png"),
                    offset,
                    length);
            QVERIFY(!invalidRange.hasValue());
            QCOMPARE(
                invalidRange.error().code,
                QStringLiteral(
                    "invalid_presence_pet_range"));
        }
    }

    void rejectsTraversalSymlinkHashByteCountAndGeometryFailures()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        const QString external =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral("external"));
        QVERIFY(copyFixture(external));
        const QString junction =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "package-junction"));
        QString junctionError;
        QVERIFY2(
            createDirectoryJunction(
                junction,
                external,
                &junctionError),
            qPrintable(junctionError));
        QVERIFY(
            QFileInfo(junction)
                .isJunction());
        verifyRejectedPackage(
            junction,
            kContentHash,
            QStringLiteral(
                "unsafe_presence_pet_path"));
        QVERIFY(
            QDir(temporary.path())
                .rmdir(
                    QStringLiteral(
                        "package-junction")));

        const QString hashFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "hash-failure"));
        QVERIFY(copyFixture(hashFailure));
        QFile changedAtlas(
            QDir(hashFailure)
                .filePath(
                    QStringLiteral(
                        "atlas.png")));
        QVERIFY(
            changedAtlas.open(
                QIODevice::ReadWrite));
        QVERIFY(
            changedAtlas.seek(
                changedAtlas.size() - 1));
        const QByteArray lastByte =
            changedAtlas.read(1);
        QCOMPARE(lastByte.size(), 1);
        QVERIFY(
            changedAtlas.seek(
                changedAtlas.size() - 1));
        const char changed =
            static_cast<char>(
                lastByte.front() ^ 0x01);
        QCOMPARE(
            changedAtlas.write(
                &changed,
                1),
            qint64(1));
        changedAtlas.close();
        verifyRejectedPackage(
            hashFailure,
            kContentHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString byteCountFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "byte-count-failure"));
        QVERIFY(copyFixture(byteCountFailure));
        QJsonObject byteCountManifest =
            manifestObject(byteCountFailure);
        QJsonObject thumbnail =
            byteCountManifest
                .value(
                    QStringLiteral(
                        "thumbnail"))
                .toObject();
        thumbnail.insert(
            QStringLiteral("byteCount"),
            thumbnail.value(
                         QStringLiteral(
                             "byteCount"))
                    .toInteger()
                + 1);
        byteCountManifest.insert(
            QStringLiteral("thumbnail"),
            thumbnail);
        const QString byteCountHash =
            writeManifest(
                byteCountFailure,
                byteCountManifest);
        QVERIFY(!byteCountHash.isEmpty());
        verifyRejectedPackage(
            byteCountFailure,
            byteCountHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString geometryFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "geometry-failure"));
        QVERIFY(copyFixture(geometryFailure));
        QJsonObject geometryManifest =
            manifestObject(geometryFailure);
        QJsonObject atlas =
            geometryManifest
                .value(
                    QStringLiteral("atlas"))
                .toObject();
        atlas.insert(
            QStringLiteral("columns"),
            0);
        geometryManifest.insert(
            QStringLiteral("atlas"),
            atlas);
        const QString geometryHash =
            writeManifest(
                geometryFailure,
                geometryManifest);
        verifyRejectedPackage(
            geometryFailure,
            geometryHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString formatFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "format-failure"));
        QVERIFY(copyFixture(formatFailure));
        QVERIFY(
            writeFile(
                QDir(formatFailure)
                    .filePath(
                        QStringLiteral(
                            "thumbnail.png")),
                QByteArray("not a png")));
        QJsonObject formatManifest =
            manifestObject(formatFailure);
        synchronizeFileRecord(
            formatManifest,
            formatFailure,
            false);
        const QString formatHash =
            writeManifest(
                formatFailure,
                formatManifest);
        verifyRejectedPackage(
            formatFailure,
            formatHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString dimensionFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "dimension-failure"));
        QVERIFY(copyFixture(dimensionFailure));
        QVERIFY(
            createTransparentPng(
                QDir(dimensionFailure)
                    .filePath(
                        QStringLiteral(
                            "thumbnail.png")),
                1,
                1));
        QJsonObject dimensionManifest =
            manifestObject(dimensionFailure);
        synchronizeFileRecord(
            dimensionManifest,
            dimensionFailure,
            false);
        const QString dimensionHash =
            writeManifest(
                dimensionFailure,
                dimensionManifest);
        verifyRejectedPackage(
            dimensionFailure,
            dimensionHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString transparencyFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "transparency-failure"));
        QVERIFY(copyFixture(transparencyFailure));
        QVERIFY(
            createOpaquePng(
                QDir(transparencyFailure)
                    .filePath(
                        QStringLiteral(
                            "thumbnail.png")),
                128,
                128));
        QJsonObject transparencyManifest =
            manifestObject(
                transparencyFailure);
        synchronizeFileRecord(
            transparencyManifest,
            transparencyFailure,
            false);
        const QString transparencyHash =
            writeManifest(
                transparencyFailure,
                transparencyManifest);
        verifyRejectedPackage(
            transparencyFailure,
            transparencyHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString canonicalHashFailure =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "canonical-hash-failure"));
        QVERIFY(copyFixture(canonicalHashFailure));
        QJsonObject changedManifest =
            manifestObject(
                canonicalHashFailure);
        changedManifest.insert(
            QStringLiteral("displayName"),
            QStringLiteral(
                "Changed Fixture"));
        QVERIFY(
            writeFile(
                QDir(canonicalHashFailure)
                    .filePath(
                        QStringLiteral(
                            "manifest.json")),
                QJsonDocument(changedManifest)
                    .toJson(
                        QJsonDocument::Indented)));
        verifyRejectedPackage(
            canonicalHashFailure,
            kContentHash,
            QStringLiteral(
                "invalid_presence_pet_package"));

        const QString oversizedPackage =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "oversized-package"));
        QVERIFY(copyFixture(oversizedPackage));
        QJsonObject oversizedManifest =
            manifestObject(
                oversizedPackage);
        const QString oversizedHash =
            writeManifest(
                oversizedPackage,
                oversizedManifest,
                MobilePresencePetCatalogService::
                        kMaximumPackageBytes
                    + 1);
        verifyRejectedPackage(
            oversizedPackage,
            oversizedHash,
            QStringLiteral(
                "invalid_presence_pet_package"));
    }

    void invalidMobilePackageDoesNotRemoveDesktopSelection()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString invalidPackage =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "invalid-package"));
        QVERIFY(copyFixture(invalidPackage));
        QVERIFY(
            writeFile(
                QDir(invalidPackage)
                    .filePath(
                        QStringLiteral(
                            "manifest.json")),
                QByteArray("{}")));

        MobilePresencePetCatalogService service;
        QVERIFY(
            service.replaceSnapshot(
                       snapshot(
                           fixtureDirectory()))
                .isEmpty());
        QCOMPARE(
            service.presentation()
                .catalog.size(),
            1);

        const QVector<CompanionError> diagnostics =
            service.replaceSnapshot(
                snapshot(
                    invalidPackage,
                    kContentHash,
                    QStringLiteral(
                        "custom:fixture-pet")));

        QCOMPARE(diagnostics.size(), 1);
        QCOMPARE(
            diagnostics.front().code,
            QStringLiteral(
                "invalid_presence_pet_package"));
        const auto presentation =
            service.presentation();
        QVERIFY(
            presentation.selectedDesktopPetId
                .has_value());
        QCOMPARE(
            *presentation.selectedDesktopPetId,
            QStringLiteral("fixture-pet"));
        QVERIFY(presentation.catalog.isEmpty());
        const auto priorManifest =
            service.manifest(
                kPackageId,
                kContentHash);
        QVERIFY(!priorManifest.hasValue());
        QCOMPARE(
            priorManifest.error().code,
            QStringLiteral(
                "presence_pet_not_found"));
    }
};

QTEST_GUILESS_MAIN(
    MobilePresencePetCatalogServiceTests)

#include "MobilePresencePetCatalogServiceTests.moc"
