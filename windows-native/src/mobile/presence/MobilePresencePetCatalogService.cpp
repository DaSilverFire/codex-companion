#include "mobile/presence/MobilePresencePetCatalogService.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QReadLocker>
#include <QRegularExpression>
#include <QSet>
#include <QWriteLocker>

namespace companion {
namespace {

constexpr double kMaximumExactJsonInteger =
    9007199254740991.0;

CompanionError presenceError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

CompanionError packageNotFound(
    const QString& packageId)
{
    return presenceError(
        QStringLiteral("presence_pet_not_found"),
        QStringLiteral(
            "That Companion pet package is not available on this PC."),
        {
            {
                QStringLiteral("packageID"),
                packageId,
            },
        });
}

CompanionError stalePackage(
    const QString& packageId)
{
    return presenceError(
        QStringLiteral("stale_presence_pet"),
        QStringLiteral(
            "The Companion pet package changed. Refresh the pet catalog and try again."),
        {
            {
                QStringLiteral("packageID"),
                packageId,
            },
        });
}

CompanionError unsafePath(
    const QString& path)
{
    return presenceError(
        QStringLiteral(
            "unsafe_presence_pet_path"),
        QStringLiteral(
            "The Companion pet package path is unsafe."),
        {
            {
                QStringLiteral("path"),
                path,
            },
        });
}

CompanionError invalidRange()
{
    return presenceError(
        QStringLiteral(
            "invalid_presence_pet_range"),
        QStringLiteral(
            "The requested Companion pet file range is invalid."));
}

CompanionError invalidPackage(
    const MobilePresencePetPackageSource& source)
{
    return presenceError(
        QStringLiteral(
            "invalid_presence_pet_package"),
        QStringLiteral(
            "The Companion pet package failed validation."),
        {
            {
                QStringLiteral("packageID"),
                source.packageId,
            },
            {
                QStringLiteral("petID"),
                source.petId,
            },
            {
                QStringLiteral("path"),
                source.packageDirectory,
            },
        });
}

QString normalizedAbsolutePath(
    const QString& path)
{
    return QDir::fromNativeSeparators(
        QDir::cleanPath(
            QFileInfo(path)
                .absoluteFilePath()));
}

bool isWithinTree(
    const QString& rootPath,
    const QString& candidatePath)
{
    const QString root =
        normalizedAbsolutePath(rootPath);
    const QString candidate =
        normalizedAbsolutePath(candidatePath);
    if (candidate.compare(
            root,
            Qt::CaseInsensitive)
        == 0) {
        return true;
    }
    return candidate.startsWith(
        root + QLatin1Char('/'),
        Qt::CaseInsensitive);
}

QFileInfo freshFileInfo(
    const QString& path)
{
    QFileInfo information(path);
    information.setCaching(false);
    information.refresh();
    return information;
}

bool isUnsafeLink(
    const QFileInfo& information)
{
    return information.isSymLink()
        || information.isJunction();
}

bool isSafeFileName(
    const QString& fileName)
{
    return !fileName.isEmpty()
        && fileName != QStringLiteral(".")
        && fileName != QStringLiteral("..")
        && !fileName.contains(
            QLatin1Char('/'))
        && !fileName.contains(
            QLatin1Char('\\'))
        && !fileName.contains(
            QLatin1Char(':'))
        && !fileName.contains(QChar(0))
        && !QDir::isAbsolutePath(fileName);
}

Result<QString> safeRegularFile(
    const QString& fileName,
    const QString& packageDirectory)
{
    if (!isSafeFileName(fileName)) {
        return Result<QString>::failure(
            unsafePath(fileName));
    }

    const QFileInfo root =
        freshFileInfo(packageDirectory);
    if (!root.exists()
        || !root.isDir()
        || isUnsafeLink(root)) {
        return Result<QString>::failure(
            unsafePath(packageDirectory));
    }
    const QString canonicalRoot =
        root.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        return Result<QString>::failure(
            unsafePath(packageDirectory));
    }

    const QString candidatePath =
        normalizedAbsolutePath(
            QDir(root.absoluteFilePath())
                .filePath(fileName));
    if (!isWithinTree(
            root.absoluteFilePath(),
            candidatePath)) {
        return Result<QString>::failure(
            unsafePath(candidatePath));
    }

    const QFileInfo candidate =
        freshFileInfo(candidatePath);
    if (!candidate.exists()
        || !candidate.isFile()
        || isUnsafeLink(candidate)) {
        return Result<QString>::failure(
            unsafePath(candidatePath));
    }
    const QString canonicalCandidate =
        candidate.canonicalFilePath();
    if (canonicalCandidate.isEmpty()
        || !isWithinTree(
            canonicalRoot,
            canonicalCandidate)) {
        return Result<QString>::failure(
            unsafePath(candidatePath));
    }
    return Result<QString>::success(
        canonicalCandidate);
}

bool requiredString(
    const QJsonObject& object,
    QStringView key,
    QString* value)
{
    const QJsonValue field =
        object.value(key);
    if (!field.isString()) {
        return false;
    }
    *value = field.toString();
    return true;
}

bool requiredNonNegativeInteger(
    const QJsonObject& object,
    QStringView key,
    qint64* value)
{
    const QJsonValue field =
        object.value(key);
    if (!field.isDouble()) {
        return false;
    }
    const double number =
        field.toDouble();
    if (!std::isfinite(number)
        || number < 0.0
        || number > kMaximumExactJsonInteger
        || std::trunc(number) != number) {
        return false;
    }
    *value = static_cast<qint64>(number);
    return true;
}

bool decodeFileRecord(
    const QJsonValue& value,
    BridgePresencePetFile* record)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject object =
        value.toObject();
    return requiredString(
               object,
               u"name",
               &record->name)
        && requiredString(
               object,
               u"sha256",
               &record->sha256)
        && requiredNonNegativeInteger(
               object,
               u"byteCount",
               &record->byteCount);
}

std::optional<BridgePresencePetState>
presenceState(
    const QString& value)
{
    if (value == QStringLiteral("idle")) {
        return BridgePresencePetState::Idle;
    }
    if (value == QStringLiteral("thinking")) {
        return BridgePresencePetState::Thinking;
    }
    if (value == QStringLiteral("talking")) {
        return BridgePresencePetState::Talking;
    }
    return std::nullopt;
}

bool decodeAnimation(
    const QJsonValue& value,
    BridgePresencePetAnimation* animation)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject object =
        value.toObject();
    QString stateText;
    if (!requiredString(
            object,
            u"state",
            &stateText)
        || !requiredNonNegativeInteger(
            object,
            u"row",
            &animation->row)
        || !requiredNonNegativeInteger(
            object,
            u"frameCount",
            &animation->frameCount)
        || !requiredNonNegativeInteger(
            object,
            u"posterFrame",
            &animation->posterFrame)) {
        return false;
    }
    const auto state =
        presenceState(stateText);
    if (!state.has_value()) {
        return false;
    }
    animation->state = *state;

    const QJsonValue durationsValue =
        object.value(
            QStringLiteral(
                "frameDurationsMilliseconds"));
    if (!durationsValue.isArray()) {
        return false;
    }
    for (const QJsonValue duration :
         durationsValue.toArray()) {
        if (!duration.isDouble()) {
            return false;
        }
        const double number =
            duration.toDouble();
        if (!std::isfinite(number)
            || number < 0.0
            || number > kMaximumExactJsonInteger
            || std::trunc(number) != number) {
            return false;
        }
        animation->frameDurationsMilliseconds
            .append(
                static_cast<qint64>(
                    number));
    }
    return true;
}

std::optional<BridgePresencePetManifest>
decodeManifest(
    const QJsonObject& object)
{
    BridgePresencePetManifest manifest;
    if (!requiredNonNegativeInteger(
            object,
            u"schemaVersion",
            &manifest.schemaVersion)
        || !requiredString(
            object,
            u"packageID",
            &manifest.packageId)
        || !requiredString(
            object,
            u"petID",
            &manifest.petId)
        || !requiredString(
            object,
            u"displayName",
            &manifest.displayName)
        || !requiredString(
            object,
            u"assetVersion",
            &manifest.assetVersion)
        || !requiredString(
            object,
            u"contentHash",
            &manifest.contentHash)) {
        return std::nullopt;
    }

    const QJsonValue atlasValue =
        object.value(
            QStringLiteral("atlas"));
    if (!atlasValue.isObject()) {
        return std::nullopt;
    }
    const QJsonObject atlasObject =
        atlasValue.toObject();
    if (!decodeFileRecord(
            atlasObject.value(
                QStringLiteral("file")),
            &manifest.atlas.file)
        || !requiredNonNegativeInteger(
            atlasObject,
            u"cellWidth",
            &manifest.atlas.cellWidth)
        || !requiredNonNegativeInteger(
            atlasObject,
            u"cellHeight",
            &manifest.atlas.cellHeight)
        || !requiredNonNegativeInteger(
            atlasObject,
            u"columns",
            &manifest.atlas.columns)
        || !requiredNonNegativeInteger(
            atlasObject,
            u"rows",
            &manifest.atlas.rows)
        || !decodeFileRecord(
            object.value(
                QStringLiteral("thumbnail")),
            &manifest.thumbnail)) {
        return std::nullopt;
    }

    const QJsonValue animationsValue =
        object.value(
            QStringLiteral("animations"));
    if (!animationsValue.isArray()) {
        return std::nullopt;
    }
    for (const QJsonValue value :
         animationsValue.toArray()) {
        BridgePresencePetAnimation animation;
        if (!decodeAnimation(
                value,
                &animation)) {
            return std::nullopt;
        }
        manifest.animations.append(
            std::move(animation));
    }
    return manifest;
}

QString sha256(
    QByteArrayView bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            bytes,
            QCryptographicHash::Sha256)
            .toHex());
}

bool isLowercaseSha256(
    const QString& value)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            "^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

QString canonicalManifestHash(
    QJsonObject manifest)
{
    manifest.remove(
        QStringLiteral("contentHash"));
    return sha256(
        QJsonDocument(manifest)
            .toJson(
                QJsonDocument::Compact));
}

QString unqualifiedPetId(
    const QString& petId)
{
    const qsizetype separator =
        petId.indexOf(
            QLatin1Char(':'));
    if (separator < 0) {
        return petId;
    }
    return petId.sliced(separator + 1);
}

bool validGeometry(
    const BridgePresencePetManifest& manifest)
{
    const BridgePresencePetAtlas& atlas =
        manifest.atlas;
    if (atlas.cellWidth < 1
        || atlas.cellWidth > 256
        || atlas.cellHeight < 1
        || atlas.cellHeight > 256
        || atlas.columns <= 0
        || atlas.rows != 3
        || atlas.columns
            > 8192 / atlas.cellWidth
        || atlas.rows
            > 8192 / atlas.cellHeight
        || manifest.animations.size() != 3) {
        return false;
    }

    std::array<bool, 3> states{
        false,
        false,
        false,
    };
    QSet<qint64> rows;
    qint64 largestFrameCount = 0;
    for (const BridgePresencePetAnimation&
             animation :
         manifest.animations) {
        const qsizetype stateIndex =
            static_cast<qsizetype>(
                animation.state);
        if (stateIndex < 0
            || stateIndex
                >= static_cast<qsizetype>(
                    states.size())
            || states.at(stateIndex)
            || rows.contains(animation.row)
            || animation.row < 0
            || animation.row >= atlas.rows
            || animation.frameCount < 1
            || animation.frameCount
                > std::min<qint64>(
                    32,
                    atlas.columns)
            || animation
                       .frameDurationsMilliseconds
                       .size()
                != animation.frameCount
            || animation.posterFrame < 0
            || animation.posterFrame
                >= animation.frameCount) {
            return false;
        }
        for (const qint64 duration :
             animation
                 .frameDurationsMilliseconds) {
            if (duration <= 0) {
                return false;
            }
        }
        states.at(stateIndex) = true;
        rows.insert(animation.row);
        largestFrameCount =
            std::max(
                largestFrameCount,
                animation.frameCount);
    }
    return std::all_of(
               states.cbegin(),
               states.cend(),
               [](bool present) {
                   return present;
               })
        && largestFrameCount
            == atlas.columns;
}

Result<QByteArray> readPackageFile(
    const QString& path,
    const MobilePresencePetPackageSource&
        source)
{
    const QFileInfo information =
        freshFileInfo(path);
    if (information.size() < 0
        || information.size()
            > MobilePresencePetCatalogService::
                kMaximumPackageBytes) {
        return Result<QByteArray>::failure(
            invalidPackage(source));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<QByteArray>::failure(
            invalidPackage(source));
    }
    return Result<QByteArray>::success(
        file.readAll());
}

Result<QString> validateFile(
    const BridgePresencePetFile& record,
    const QString& expectedName,
    const QString& packageDirectory,
    const MobilePresencePetPackageSource&
        source)
{
    if (record.name != expectedName
        || !isLowercaseSha256(record.sha256)) {
        return Result<QString>::failure(
            invalidPackage(source));
    }
    const auto path =
        safeRegularFile(
            expectedName,
            packageDirectory);
    if (!path.hasValue()) {
        return path;
    }
    const auto bytes =
        readPackageFile(
            path.value(),
            source);
    if (!bytes.hasValue()) {
        return Result<QString>::failure(
            bytes.error());
    }
    if (bytes.value().size()
            != record.byteCount
        || sha256(bytes.value())
            != record.sha256) {
        return Result<QString>::failure(
            invalidPackage(source));
    }
    return path;
}

bool hasTransparentPixel(
    const QImage& image)
{
    for (int y = 0;
         y < image.height();
         ++y) {
        for (int x = 0;
             x < image.width();
             ++x) {
            if (qAlpha(
                    image.pixel(x, y))
                < 255) {
                return true;
            }
        }
    }
    return false;
}

bool validPng(
    const QString& path,
    qint64 expectedWidth,
    qint64 expectedHeight)
{
    if (QImageReader::imageFormat(path)
            .toLower()
        != QByteArrayLiteral("png")) {
        return false;
    }
    QImageReader reader(path);
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    const QImage image =
        reader.read();
    return !image.isNull()
        && image.width() == expectedWidth
        && image.height() == expectedHeight
        && hasTransparentPixel(image);
}

} // namespace

QVector<CompanionError>
MobilePresencePetCatalogService::replaceSnapshot(
    MobilePresencePetCatalogSnapshot snapshot)
{
    QVector<CompanionError> diagnostics;
    QVector<IndexedPackage> indexed;
    indexed.reserve(snapshot.packages.size());
    for (const MobilePresencePetPackageSource&
             source :
         snapshot.packages) {
        auto result =
            validatePackage(source);
        if (!result.hasValue()) {
            diagnostics.append(
                result.error());
            continue;
        }
        indexed.append(
            std::move(result.value()));
    }

    std::sort(
        indexed.begin(),
        indexed.end(),
        [](const IndexedPackage& left,
           const IndexedPackage& right) {
            const int nameOrder =
                QString::localeAwareCompare(
                    left.manifest.displayName
                        .toCaseFolded(),
                    right.manifest.displayName
                        .toCaseFolded());
            if (nameOrder != 0) {
                return nameOrder < 0;
            }
            return left.manifest.packageId
                < right.manifest.packageId;
        });

    QHash<QString, IndexedPackage>
        packagesById;
    for (const IndexedPackage& package :
         indexed) {
        if (!packagesById.contains(
                package.manifest.packageId)) {
            packagesById.insert(
                package.manifest.packageId,
                package);
        }
    }

    QVector<BridgePresencePetCatalogEntry>
        catalog;
    catalog.reserve(indexed.size());
    for (const IndexedPackage& package :
         indexed) {
        const auto current =
            packagesById.constFind(
                package.manifest.packageId);
        if (current != packagesById.cend()
            && current->manifest.contentHash
                == package.manifest.contentHash) {
            catalog.append({
                package.manifest.packageId,
                package.manifest.petId,
                package.manifest.displayName,
                package.manifest.assetVersion,
                package.manifest.contentHash,
                package.byteCount,
                package.manifest.thumbnail,
            });
        }
    }

    std::optional<QString>
        selectedDesktopPetId;
    if (snapshot.selectedDesktopPetId
            .has_value()) {
        selectedDesktopPetId =
            unqualifiedPetId(
                *snapshot
                     .selectedDesktopPetId);
    }

    {
        QWriteLocker locker(&lock_);
        selectedDesktopPetId_ =
            std::move(selectedDesktopPetId);
        packagesById_ =
            std::move(packagesById);
        catalog_ = std::move(catalog);
    }
    return diagnostics;
}

MobilePresencePetCatalogPresentation
MobilePresencePetCatalogService::presentation()
    const
{
    QReadLocker locker(&lock_);
    return {
        selectedDesktopPetId_,
        catalog_,
    };
}

Result<BridgePresencePetManifest>
MobilePresencePetCatalogService::manifest(
    QString packageId,
    QString contentHash) const
{
    const auto package =
        indexedPackage(
            packageId,
            contentHash);
    if (!package.hasValue()) {
        return Result<
            BridgePresencePetManifest>::
            failure(package.error());
    }
    return Result<
        BridgePresencePetManifest>::success(
        package.value().manifest);
}

Result<BridgePresencePetChunk>
MobilePresencePetCatalogService::chunk(
    QString packageId,
    QString contentHash,
    QString fileName,
    qint64 offset,
    qint64 requestedLength) const
{
    const auto package =
        indexedPackage(
            packageId,
            contentHash);
    if (!package.hasValue()) {
        return Result<
            BridgePresencePetChunk>::failure(
            package.error());
    }
    if (!isSafeFileName(fileName)) {
        return Result<
            BridgePresencePetChunk>::failure(
            unsafePath(fileName));
    }
    const auto indexedFile =
        package.value().files.constFind(
            fileName);
    if (indexedFile
        == package.value().files.cend()) {
        return Result<
            BridgePresencePetChunk>::failure(
            packageNotFound(packageId));
    }
    if (requestedLength <= 0
        || offset < 0) {
        return Result<
            BridgePresencePetChunk>::failure(
            invalidRange());
    }

    const auto currentPath =
        safeRegularFile(
            fileName,
            package.value()
                .packageDirectory);
    if (!currentPath.hasValue()) {
        return Result<
            BridgePresencePetChunk>::failure(
            currentPath.error());
    }
    if (currentPath.value().compare(
            indexedFile.value(),
            Qt::CaseInsensitive)
        != 0) {
        return Result<
            BridgePresencePetChunk>::failure(
            unsafePath(
                currentPath.value()));
    }

    const QFileInfo information =
        freshFileInfo(
            currentPath.value());
    const qint64 fileSize =
        information.size();
    if (fileSize < 0
        || offset > fileSize) {
        return Result<
            BridgePresencePetChunk>::failure(
            invalidRange());
    }

    const qint64 length =
        std::min(
            kMaximumChunkLength,
            requestedLength);
    const qint64 available =
        std::min(
            length,
            fileSize - offset);
    QFile file(currentPath.value());
    if (!file.open(QIODevice::ReadOnly)
        || !file.seek(offset)) {
        return Result<
            BridgePresencePetChunk>::failure(
            unsafePath(
                currentPath.value()));
    }
    const QByteArray data =
        file.read(available);
    if (data.size() != available) {
        return Result<
            BridgePresencePetChunk>::failure(
            unsafePath(
                currentPath.value()));
    }
    const qint64 nextOffset =
        offset + data.size();
    return Result<
        BridgePresencePetChunk>::success({
        std::move(packageId),
        std::move(contentHash),
        std::move(fileName),
        offset,
        data,
        nextOffset,
        nextOffset >= fileSize,
    });
}

Result<
    MobilePresencePetCatalogService::
        IndexedPackage>
MobilePresencePetCatalogService::validatePackage(
    const MobilePresencePetPackageSource& source)
{
    if (source.packageDirectory.isEmpty()
        || !QDir::isAbsolutePath(
            source.packageDirectory)
        || source.packageId.isEmpty()
        || source.packageId
            != source.packageId.trimmed()
        || !isLowercaseSha256(
            source.contentHash)
        || unqualifiedPetId(source.petId)
               .isEmpty()) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }

    const QFileInfo root =
        freshFileInfo(
            source.packageDirectory);
    if (!root.exists()
        || !root.isDir()
        || isUnsafeLink(root)
        || root.canonicalFilePath()
               .isEmpty()) {
        return Result<
            IndexedPackage>::failure(
            unsafePath(
                source.packageDirectory));
    }
    const QString packageDirectory =
        root.canonicalFilePath();

    const auto manifestPath =
        safeRegularFile(
            QStringLiteral(
                "manifest.json"),
            packageDirectory);
    if (!manifestPath.hasValue()) {
        return Result<
            IndexedPackage>::failure(
            manifestPath.error());
    }
    const auto manifestData =
        readPackageFile(
            manifestPath.value(),
            source);
    if (!manifestData.hasValue()) {
        return Result<
            IndexedPackage>::failure(
            manifestData.error());
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            manifestData.value(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }
    const QJsonObject manifestObject =
        document.object();
    const auto decoded =
        decodeManifest(
            manifestObject);
    if (!decoded.has_value()) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }
    const BridgePresencePetManifest&
        manifest = *decoded;
    if (manifest.schemaVersion != 1
        || manifest.packageId
            != source.packageId
        || manifest.petId
            != unqualifiedPetId(
                source.petId)
        || manifest.contentHash
            != source.contentHash
        || manifest.displayName.isEmpty()
        || manifest.assetVersion.isEmpty()
        || !isLowercaseSha256(
            manifest.contentHash)
        || canonicalManifestHash(
               manifestObject)
            != manifest.contentHash
        || !validGeometry(manifest)) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }

    const auto atlasPath =
        validateFile(
            manifest.atlas.file,
            QStringLiteral("atlas.png"),
            packageDirectory,
            source);
    if (!atlasPath.hasValue()) {
        return Result<
            IndexedPackage>::failure(
            atlasPath.error());
    }
    const auto thumbnailPath =
        validateFile(
            manifest.thumbnail,
            QStringLiteral(
                "thumbnail.png"),
            packageDirectory,
            source);
    if (!thumbnailPath.hasValue()) {
        return Result<
            IndexedPackage>::failure(
            thumbnailPath.error());
    }
    if (!validPng(
            atlasPath.value(),
            manifest.atlas.columns
                * manifest.atlas.cellWidth,
            manifest.atlas.rows
                * manifest.atlas.cellHeight)
        || !validPng(
            thumbnailPath.value(),
            manifest.atlas.cellWidth,
            manifest.atlas.cellHeight)) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }

    const qint64 manifestBytes =
        manifestData.value().size();
    if (manifestBytes
            > kMaximumPackageBytes
                - manifest.atlas.file
                      .byteCount
        || manifestBytes
                + manifest.atlas.file
                      .byteCount
            > kMaximumPackageBytes
                - manifest.thumbnail
                      .byteCount) {
        return Result<
            IndexedPackage>::failure(
            invalidPackage(source));
    }
    const qint64 byteCount =
        manifestBytes
        + manifest.atlas.file.byteCount
        + manifest.thumbnail.byteCount;

    return Result<
        IndexedPackage>::success({
        manifest,
        packageDirectory,
        {
            {
                manifest.atlas.file.name,
                atlasPath.value(),
            },
            {
                manifest.thumbnail.name,
                thumbnailPath.value(),
            },
        },
        byteCount,
    });
}

Result<
    MobilePresencePetCatalogService::
        IndexedPackage>
MobilePresencePetCatalogService::indexedPackage(
    const QString& packageId,
    const QString& contentHash) const
{
    QReadLocker locker(&lock_);
    const auto package =
        packagesById_.constFind(
            packageId);
    if (package == packagesById_.cend()) {
        return Result<
            IndexedPackage>::failure(
            packageNotFound(packageId));
    }
    if (package->manifest.contentHash
        != contentHash) {
        return Result<
            IndexedPackage>::failure(
            stalePackage(packageId));
    }
    return Result<
        IndexedPackage>::success(
        *package);
}

} // namespace companion
