#include "ui/pet/PetDefinition.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using companion::PetAnimation;
using companion::PetFrame;

constexpr qint64 kMaximumManifestBytes =
    1024 * 1024;
constexpr int kMaximumAtlasDimension = 64;

companion::CompanionError petError(
    QString code,
    QString message,
    const QString& sourceDirectory)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {
                QStringLiteral("sourceDirectory"),
                sourceDirectory,
            },
        },
    };
}

QString normalizedAbsolutePath(const QString& path)
{
    return QDir::fromNativeSeparators(
        QDir::cleanPath(
            QFileInfo(path).absoluteFilePath()));
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

QFileInfo freshFileInfo(const QString& path)
{
    QFileInfo information(path);
    information.setCaching(false);
    information.refresh();
    return information;
}

bool isUnsafeLink(const QFileInfo& information)
{
    return information.isSymLink()
        || information.isJunction();
}

std::optional<companion::PetMobilePresence>
mobilePresenceFromManifest(
    const QJsonObject& manifest,
    const QString& sourceDirectory,
    std::optional<companion::CompanionError>&
        diagnostic)
{
    const QString fieldName =
        QStringLiteral("mobilePresence");
    if (!manifest.contains(fieldName)) {
        return std::nullopt;
    }

    const QJsonValue value =
        manifest.value(fieldName);
    if (!value.isObject()) {
        diagnostic = petError(
            QStringLiteral(
                "pet.mobile-presence-invalid"),
            QStringLiteral(
                "The mobile presence metadata must be an object."),
            sourceDirectory);
        return std::nullopt;
    }

    const QJsonObject metadata =
        value.toObject();
    const QString directory =
        metadata.value(
            QStringLiteral("directory"))
            .toString();
    const QString packageId =
        metadata.value(
            QStringLiteral("packageID"))
            .toString();
    const QString contentHash =
        metadata.value(
            QStringLiteral("contentHash"))
            .toString();
    static const QRegularExpression
        sha256Pattern(
            QStringLiteral(
                "^[0-9a-f]{64}$"));
    const bool metadataIsValid =
        directory
            == QStringLiteral(
                "mobile-presence")
        && !QDir::isAbsolutePath(directory)
        && !directory.contains(
            QLatin1Char('/'))
        && !directory.contains(
            QLatin1Char('\\'))
        && !packageId.isEmpty()
        && packageId
               == packageId.trimmed()
        && !packageId.contains(QChar(0))
        && sha256Pattern
               .match(contentHash)
               .hasMatch();
    if (!metadataIsValid) {
        diagnostic = petError(
            QStringLiteral(
                "pet.mobile-presence-invalid"),
            QStringLiteral(
                "The mobile presence metadata is invalid."),
            sourceDirectory);
        return std::nullopt;
    }

    const QString packageDirectory =
        normalizedAbsolutePath(
            QDir(sourceDirectory)
                .filePath(directory));
    const QFileInfo directoryInformation =
        freshFileInfo(packageDirectory);
    const QFileInfo rootInformation =
        freshFileInfo(sourceDirectory);
    const QString canonicalRoot =
        rootInformation.canonicalFilePath();
    const QString canonicalDirectory =
        directoryInformation
            .canonicalFilePath();
    if (!directoryInformation.exists()
        || !directoryInformation.isDir()
        || isUnsafeLink(directoryInformation)
        || canonicalRoot.isEmpty()
        || canonicalDirectory.isEmpty()
        || !isWithinTree(
            canonicalRoot,
            canonicalDirectory)) {
        diagnostic = petError(
            QStringLiteral(
                "pet.mobile-presence-invalid"),
            QStringLiteral(
                "The mobile presence directory is not a safe package directory."),
            sourceDirectory);
        return std::nullopt;
    }

    return companion::PetMobilePresence{
        directory,
        packageId,
        contentHash,
    };
}

companion::Result<QString> containedAssetPath(
    const QString& sourceDirectory,
    const QString& relativePath)
{
    const QString trimmed = relativePath.trimmed();
    if (trimmed.isEmpty()
        || QDir::isAbsolutePath(trimmed)
        || trimmed.contains(QChar(0))) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "Pet asset paths must be non-empty relative paths."),
                sourceDirectory));
    }

    const QString cleanRelative =
        QDir::cleanPath(
            QDir::fromNativeSeparators(trimmed));
    if (cleanRelative == QStringLiteral("..")
        || cleanRelative.startsWith(
            QStringLiteral("../"))
        || cleanRelative == QStringLiteral(".")) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "A pet asset path leaves its package."),
                sourceDirectory));
    }

    const QString rootPath =
        normalizedAbsolutePath(sourceDirectory);
    const QString candidatePath =
        normalizedAbsolutePath(
            QDir(rootPath).filePath(cleanRelative));
    if (!isWithinTree(rootPath, candidatePath)) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "A pet asset path leaves its package."),
                sourceDirectory));
    }

    const QFileInfo root = freshFileInfo(rootPath);
    if (!root.exists()
        || !root.isDir()
        || isUnsafeLink(root)) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "The pet package directory is not a safe local directory."),
                sourceDirectory));
    }

    QString currentPath = rootPath;
    const QStringList components =
        cleanRelative.split(
            QLatin1Char('/'),
            Qt::SkipEmptyParts);
    for (const QString& component : components) {
        currentPath =
            QDir(currentPath).filePath(component);
        const QFileInfo information =
            freshFileInfo(currentPath);
        if (information.exists()
            && isUnsafeLink(information)) {
            return companion::Result<QString>::failure(
                petError(
                    QStringLiteral("pet.unsafe-path"),
                    QStringLiteral(
                        "A pet asset path crosses a reparse point."),
                    sourceDirectory));
        }
    }

    const QFileInfo asset =
        freshFileInfo(candidatePath);
    if (!asset.exists() || !asset.isFile()) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.asset-missing"),
                QStringLiteral(
                    "The pet spritesheet does not exist."),
                sourceDirectory));
    }

    const QString canonicalRoot =
        root.canonicalFilePath();
    const QString canonicalAsset =
        asset.canonicalFilePath();
    if (canonicalRoot.isEmpty()
        || canonicalAsset.isEmpty()
        || !isWithinTree(
            canonicalRoot,
            canonicalAsset)) {
        return companion::Result<QString>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "The pet spritesheet resolves outside its package."),
                sourceDirectory));
    }

    return companion::Result<QString>::success(
        candidatePath);
}

int animationRow(PetAnimation animation)
{
    switch (animation) {
    case PetAnimation::Idle:
        return 0;
    case PetAnimation::RunningRight:
        return 1;
    case PetAnimation::RunningLeft:
        return 2;
    case PetAnimation::Waving:
        return 3;
    case PetAnimation::Jumping:
        return 4;
    case PetAnimation::Failed:
        return 5;
    case PetAnimation::Waiting:
        return 6;
    case PetAnimation::Running:
        return 7;
    case PetAnimation::Review:
        return 8;
    case PetAnimation::GoalComplete:
        return 9;
    case PetAnimation::Thinking:
        return 10;
    case PetAnimation::Talking:
        return 11;
    }
    return 0;
}

int defaultFrameCount(PetAnimation animation)
{
    switch (animation) {
    case PetAnimation::Idle:
        return 6;
    case PetAnimation::RunningRight:
    case PetAnimation::RunningLeft:
        return 8;
    case PetAnimation::Waving:
        return 4;
    case PetAnimation::Jumping:
        return 5;
    case PetAnimation::Failed:
        return 8;
    case PetAnimation::Waiting:
    case PetAnimation::Running:
    case PetAnimation::Review:
        return 6;
    case PetAnimation::GoalComplete:
        return 8;
    case PetAnimation::Thinking:
    case PetAnimation::Talking:
        return 16;
    }
    return 1;
}

struct FrameTiming final {
    double baseMilliseconds;
    double finalMilliseconds;
};

struct AnimationTiming final {
    double previewMilliseconds;
    double finalMilliseconds;
    double targetCycleMilliseconds;
    double minimumMilliseconds;
    bool loopsContinuously;
};

AnimationTiming timingFor(PetAnimation animation)
{
    switch (animation) {
    case PetAnimation::Idle:
        return {160, 240, 5200, 80, true};
    case PetAnimation::RunningRight:
    case PetAnimation::RunningLeft:
    case PetAnimation::Running:
        return {80, 80, 2650, 55, true};
    case PetAnimation::Waving:
        return {120, 200, 3600, 80, false};
    case PetAnimation::Jumping:
        return {90, 200, 2750, 65, false};
    case PetAnimation::Failed:
        return {150, 240, 4800, 80, false};
    case PetAnimation::Waiting:
        return {140, 240, 5000, 80, false};
    case PetAnimation::Review:
        return {140, 240, 4400, 80, false};
    case PetAnimation::GoalComplete:
        return {100, 180, 3900, 65, false};
    case PetAnimation::Thinking:
        return {130, 180, 3600, 80, true};
    case PetAnimation::Talking:
        return {75, 80, 1800, 55, true};
    }
    return {160, 240, 5200, 80, true};
}

FrameTiming frameTiming(
    PetAnimation animation,
    int frameCount,
    bool preserveCycleDuration)
{
    const AnimationTiming timing =
        timingFor(animation);
    if (frameCount <= 12
        && !preserveCycleDuration) {
        return {
            timing.previewMilliseconds,
            timing.finalMilliseconds,
        };
    }

    const double finalHold =
        std::max(
            timing.previewMilliseconds,
            timing.finalMilliseconds);
    const double bodyDuration =
        std::max(
            0.0,
            timing.targetCycleMilliseconds
                - finalHold);
    return {
        std::max(
            timing.minimumMilliseconds,
            bodyDuration
                / static_cast<double>(
                    std::max(1, frameCount - 1))),
        finalHold,
    };
}

double normalizedSpeedScale(double value)
{
    if (std::isnan(value)) {
        return 1.0;
    }
    if (std::isinf(value)) {
        return value < 0.0 ? 0.4 : 3.0;
    }
    return std::clamp(value, 0.4, 3.0);
}

QVector<PetFrame> makeFrames(
    int row,
    int count,
    FrameTiming timing,
    double speedScale)
{
    const int frameCount =
        std::max(1, count);
    QVector<PetFrame> frames;
    frames.reserve(frameCount);
    for (int column = 0;
         column < frameCount;
         ++column) {
        const double duration =
            column == frameCount - 1
            ? timing.finalMilliseconds
            : timing.baseMilliseconds;
        frames.append({
            row,
            column,
            qMax(
                40,
                qRound(
                    duration
                    * speedScale)),
        });
    }
    return frames;
}

int safeRow(
    PetAnimation animation,
    int rows)
{
    if (rows <= 1) {
        return 0;
    }
    const int row = animationRow(animation);
    if (row < rows) {
        return row;
    }
    if (animation == PetAnimation::GoalComplete
        && animationRow(PetAnimation::Review)
            < rows) {
        return animationRow(PetAnimation::Review);
    }
    return std::clamp(row, 0, rows - 1);
}

} // namespace

namespace companion {

Result<PetDefinition> PetDefinition::load(
    const QString& manifestPath,
    PetSourceKind source)
{
    const QFileInfo manifestInformation =
        freshFileInfo(manifestPath);
    const QString sourceDirectory =
        normalizedAbsolutePath(
            manifestInformation.absolutePath());
    if (!manifestInformation.exists()
        || !manifestInformation.isFile()) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.manifest-missing"),
                QStringLiteral(
                    "The pet package has no pet.json manifest."),
                sourceDirectory));
    }
    if (isUnsafeLink(manifestInformation)) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral("pet.unsafe-path"),
                QStringLiteral(
                    "The pet manifest is a reparse point."),
                sourceDirectory));
    }

    QFile manifestFile(
        manifestInformation.absoluteFilePath());
    if (!manifestFile.open(QIODevice::ReadOnly)
        || manifestFile.size()
            > kMaximumManifestBytes) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral("pet.manifest-io"),
                QStringLiteral(
                    "The pet manifest could not be read."),
                sourceDirectory));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            manifestFile.readAll(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.manifest-invalid"),
                QStringLiteral(
                    "The pet manifest is not valid JSON."),
                sourceDirectory));
    }

    const QJsonObject manifest =
        document.object();
    const QString directoryName =
        QFileInfo(sourceDirectory).fileName();
    const QString id =
        manifest.contains(QStringLiteral("id"))
        ? manifest.value(QStringLiteral("id"))
              .toString()
              .trimmed()
        : directoryName;
    const QString displayName =
        manifest.contains(
            QStringLiteral("displayName"))
        ? manifest.value(
              QStringLiteral("displayName"))
              .toString()
              .trimmed()
        : id;
    if (id.isEmpty()
        || displayName.isEmpty()
        || id.contains(QChar(0))) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.identity-invalid"),
                QStringLiteral(
                    "The pet manifest requires a valid ID and display name."),
                sourceDirectory));
    }

    const QString relativeSpritesheet =
        manifest.value(
            QStringLiteral("spritesheetPath"))
            .toString(
                QStringLiteral("spritesheet.webp"));
    const auto resolvedSpritesheet =
        containedAssetPath(
            sourceDirectory,
            relativeSpritesheet);
    if (!resolvedSpritesheet.hasValue()) {
        return Result<PetDefinition>::failure(
            resolvedSpritesheet.error());
    }

    const int columns =
        manifest.value(
            QStringLiteral("spriteColumns"))
            .toInt(8);
    const int rows =
        manifest.value(
            QStringLiteral("spriteRows"))
            .toInt(9);
    if (columns < 1
        || rows < 1
        || columns > kMaximumAtlasDimension
        || rows > kMaximumAtlasDimension) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.geometry-invalid"),
                QStringLiteral(
                    "The pet atlas grid dimensions are invalid."),
                sourceDirectory));
    }

    QImageReader reader(
        resolvedSpritesheet.value());
    reader.setAutoTransform(false);
    const QImage image = reader.read();
    if (image.isNull()) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.atlas-invalid"),
                QStringLiteral(
                    "The pet spritesheet could not be decoded."),
                sourceDirectory));
    }
    if (image.width() % columns != 0
        || image.height() % rows != 0) {
        return Result<PetDefinition>::failure(
            petError(
                QStringLiteral(
                    "pet.atlas-geometry"),
                QStringLiteral(
                    "The pet spritesheet size does not divide evenly into its grid."),
                sourceDirectory));
    }

    QHash<QString, int> frameCounts;
    const QJsonObject counts =
        manifest.value(
            QStringLiteral(
                "animationFrameCounts"))
            .toObject();
    for (auto iterator = counts.constBegin();
         iterator != counts.constEnd();
         ++iterator) {
        const QString name =
            iterator.key().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        frameCounts.insert(
            name,
            std::clamp(
                iterator.value().toInt(1),
                1,
                columns));
    }

    PetDefinition pet;
    pet.id = id;
    pet.displayName = displayName;
    pet.description =
        manifest.value(
            QStringLiteral("description"))
            .toString(
                source
                        == PetSourceKind::Custom
                    ? QStringLiteral(
                          "Custom Codex pet")
                    : QStringLiteral(
                          "Built-in Codex pet"))
            .trimmed();
    pet.spritesheetPath =
        resolvedSpritesheet.value();
    pet.spriteColumns = columns;
    pet.spriteRows = rows;
    pet.animationFrameCounts =
        std::move(frameCounts);
    pet.source = source;
    pet.sourceDirectory =
        sourceDirectory;
    pet.sourceFrameSize = {
        image.width() / columns,
        image.height() / rows,
    };
    pet.mobilePresence =
        mobilePresenceFromManifest(
            manifest,
            sourceDirectory,
            pet.mobilePresenceDiagnostic);
    return Result<PetDefinition>::success(
        std::move(pet));
}

QString PetDefinition::sourceTitle() const
{
    return source == PetSourceKind::Custom
        ? QStringLiteral("Custom")
        : QStringLiteral("Built-in");
}

QUrl PetDefinition::spriteSheetUrl() const
{
    return QUrl::fromLocalFile(
        spritesheetPath);
}

bool PetDefinition::hasNativeRow(
    PetAnimation animation) const noexcept
{
    return animationRow(animation)
        < spriteRows;
}

PetAnimation PetDefinition::resolvedAnimation(
    PetAnimation animation) const noexcept
{
    if (hasNativeRow(animation)) {
        return animation;
    }

    switch (animation) {
    case PetAnimation::Thinking:
        return hasNativeRow(
                   PetAnimation::Running)
            ? PetAnimation::Running
            : PetAnimation::Idle;
    case PetAnimation::Talking:
        return hasNativeRow(
                   PetAnimation::Review)
            ? PetAnimation::Review
            : PetAnimation::Idle;
    default:
        return animation;
    }
}

int PetDefinition::frameCount(
    PetAnimation animation) const noexcept
{
    const PetAnimation resolved =
        resolvedAnimation(animation);
    const auto requested =
        animationFrameCounts.constFind(
            petAnimationName(animation));
    if (requested
        != animationFrameCounts.constEnd()) {
        return std::clamp(
            requested.value(),
            1,
            spriteColumns);
    }
    const auto fallback =
        animationFrameCounts.constFind(
            petAnimationName(resolved));
    const int count =
        fallback
            != animationFrameCounts.constEnd()
        ? fallback.value()
        : defaultFrameCount(resolved);
    return std::clamp(
        count,
        1,
        spriteColumns);
}

PetAnimationSequence
PetDefinition::animationSequence(
    PetAnimation animation,
    double speedScale) const
{
    const double scale =
        normalizedSpeedScale(speedScale);
    const int idleCount =
        frameCount(PetAnimation::Idle);
    const QVector<PetFrame> idleFrames =
        makeFrames(
            safeRow(
                PetAnimation::Idle,
                spriteRows),
            idleCount,
            frameTiming(
                PetAnimation::Idle,
                idleCount,
                usesShadowStyle()
                    && timingFor(
                           PetAnimation::Idle)
                           .loopsContinuously),
            scale);

    if (animation
        == PetAnimation::GoalComplete) {
        if (hasNativeRow(
                PetAnimation::GoalComplete)) {
            const int nativeCount =
                frameCount(
                    PetAnimation::GoalComplete);
            const QVector<PetFrame> nativeFrames =
                makeFrames(
                    animationRow(
                        PetAnimation::GoalComplete),
                    nativeCount,
                    frameTiming(
                        PetAnimation::GoalComplete,
                        nativeCount,
                        false),
                    scale);
            QVector<PetFrame> frames;
            frames.reserve(
                nativeFrames.size()
                * (nativeCount > 16 ? 1 : 2)
                + idleFrames.size());
            frames.append(nativeFrames);
            if (nativeCount <= 16) {
                frames.append(nativeFrames);
            }
            const int loopStartIndex =
                frames.size();
            frames.append(idleFrames);
            return {
                std::move(frames),
                loopStartIndex,
            };
        }

        const bool shadowStyle =
            usesShadowStyle();
        const int jumpCount =
            std::min(
                shadowStyle ? 12 : 5,
                frameCount(
                    PetAnimation::Jumping));
        const int waveCount =
            std::min(
                shadowStyle ? 18 : 4,
                frameCount(
                    PetAnimation::Waving));
        const int reviewCount =
            std::min(
                shadowStyle ? 10 : 6,
                frameCount(
                    PetAnimation::Review));
        const QVector<PetFrame> jumpFrames =
            makeFrames(
                safeRow(
                    PetAnimation::Jumping,
                    spriteRows),
                jumpCount,
                {
                    shadowStyle ? 100.0 : 120.0,
                    shadowStyle ? 140.0 : 160.0,
                },
                scale);
        const QVector<PetFrame> waveFrames =
            makeFrames(
                safeRow(
                    PetAnimation::Waving,
                    spriteRows),
                waveCount,
                {
                    shadowStyle ? 120.0 : 140.0,
                    shadowStyle ? 180.0 : 200.0,
                },
                scale);
        const QVector<PetFrame> reviewFrames =
            makeFrames(
                safeRow(
                    PetAnimation::Review,
                    spriteRows),
                reviewCount,
                {
                    shadowStyle ? 120.0 : 150.0,
                    shadowStyle ? 200.0 : 220.0,
                },
                scale);

        QVector<PetFrame> prelude;
        if (shadowStyle) {
            prelude.append(jumpFrames);
            for (int index =
                     jumpFrames.size() - 2;
                 index >= 0;
                 --index) {
                prelude.append(
                    jumpFrames.at(index));
            }
            prelude.append(waveFrames);
            for (int index =
                     waveFrames.size() - 2;
                 index >= 0;
                 --index) {
                prelude.append(
                    waveFrames.at(index));
            }
            const qsizetype settleCount =
                std::min<qsizetype>(
                    reviewFrames.size(),
                    10);
            for (qsizetype index = 0;
                 index < settleCount;
                 ++index) {
                prelude.append(
                    reviewFrames.at(index));
            }
        } else {
            prelude.append(jumpFrames);
            prelude.append(waveFrames);
            prelude.append(reviewFrames);
        }
        const int loopStartIndex =
            prelude.size();
        prelude.append(idleFrames);
        return {
            std::move(prelude),
            loopStartIndex,
        };
    }

    const PetAnimation resolved =
        resolvedAnimation(animation);
    const int actionCount =
        frameCount(animation);
    const QVector<PetFrame> actionFrames =
        makeFrames(
            safeRow(resolved, spriteRows),
            actionCount,
            frameTiming(
                animation,
                actionCount,
                usesShadowStyle()
                    && timingFor(animation)
                           .loopsContinuously),
            scale);
    if (animation == PetAnimation::Idle
        || timingFor(animation)
               .loopsContinuously) {
        return {actionFrames, 0};
    }

    const int repeatCount =
        actionCount > 16 ? 1 : 3;
    QVector<PetFrame> frames;
    frames.reserve(
        actionFrames.size() * repeatCount
        + idleFrames.size());
    for (int repeat = 0;
         repeat < repeatCount;
         ++repeat) {
        frames.append(actionFrames);
    }
    const int loopStartIndex =
        frames.size();
    frames.append(idleFrames);
    return {
        std::move(frames),
        loopStartIndex,
    };
}

bool PetDefinition::usesShadowStyle() const
{
    return id.contains(
               QStringLiteral("shadow"),
               Qt::CaseInsensitive)
        || displayName.contains(
            QStringLiteral("shadow"),
            Qt::CaseInsensitive);
}

} // namespace companion
