#include "codex/attachments/AttachmentStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <utility>

namespace companion {
namespace {

constexpr qint64 kAttachmentRetentionDays = 7;
constexpr qsizetype kMaximumWindowsComponentCodeUnits = 255;
constexpr qsizetype kStoredAttachmentPrefixCodeUnits = 37;

QString uuidText(const QUuid& value)
{
    return value.toString(QUuid::WithoutBraces).toUpper();
}

QString cleanAbsolutePath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool sameWindowsPath(
    const QString& left,
    const QString& right)
{
    return cleanAbsolutePath(left).compare(
               cleanAbsolutePath(right),
               Qt::CaseInsensitive)
        == 0;
}

bool isDirectChild(
    const QString& rootPath,
    const QString& candidatePath)
{
    const QFileInfo candidate(candidatePath);
    return sameWindowsPath(candidate.absolutePath(), rootPath);
}

CompanionError attachmentError(
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

Result<void> atomicWrite(
    const QString& path,
    const QByteArray& data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure(attachmentError(
            QStringLiteral("attachment.write_failed"),
            QStringLiteral(
                "Codex Companion could not stage one attachment.")));
    }

    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return Result<void>::failure(attachmentError(
            QStringLiteral("attachment.write_failed"),
            QStringLiteral(
                "Codex Companion could not stage one attachment.")));
    }

    if (!file.commit()) {
        return Result<void>::failure(attachmentError(
            QStringLiteral("attachment.write_failed"),
            QStringLiteral(
                "Codex Companion could not stage one attachment.")));
    }

    return Result<void>::success();
}

QJsonObject localImageInput(const QString& path)
{
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("localImage"),
        },
        {QStringLiteral("path"), path},
    };
}

QString windowsStorageLabel(const QString& label)
{
    const QString forbidden =
        QStringLiteral("<>:\"/\\|?*");
    QString result;
    result.reserve(label.size());
    for (qsizetype index = 0; index < label.size(); ++index) {
        const QChar character = label.at(index);
        const bool trailingDotOrSpace =
            index == label.size() - 1
            && (character == QLatin1Char('.')
                || character == QLatin1Char(' '));
        const bool mustEncode =
            character.unicode() < 32
            || character == QLatin1Char('~')
            || forbidden.contains(character)
            || trailingDotOrSpace;
        if (!mustEncode) {
            result.append(character);
            continue;
        }
        result.append(
            QStringLiteral("~%1")
                .arg(
                    static_cast<quint16>(character.unicode()),
                    4,
                    16,
                    QLatin1Char('0'))
                .toUpper());
    }
    return result;
}

bool isReservedWindowsDeviceName(
    const QString& component)
{
    QString normalized = component;
    while (normalized.endsWith(QLatin1Char('.'))
           || normalized.endsWith(QLatin1Char(' '))) {
        normalized.chop(1);
    }
    const qsizetype extension =
        normalized.indexOf(QLatin1Char('.'));
    QString stem =
        extension < 0
            ? normalized
            : normalized.left(extension);
    while (stem.endsWith(QLatin1Char('.'))
           || stem.endsWith(QLatin1Char(' '))) {
        stem.chop(1);
    }
    stem = stem.toUpper();

    if (stem == QStringLiteral("CON")
        || stem == QStringLiteral("PRN")
        || stem == QStringLiteral("AUX")
        || stem == QStringLiteral("NUL")
        || stem == QStringLiteral("CLOCK$")
        || stem == QStringLiteral("CONIN$")
        || stem == QStringLiteral("CONOUT$")) {
        return true;
    }
    if (stem.size() != 4) {
        return false;
    }
    const bool numberedDevice =
        stem.startsWith(QStringLiteral("COM"))
        || stem.startsWith(QStringLiteral("LPT"));
    if (!numberedDevice) {
        return false;
    }
    const QChar suffix = stem.back();
    return (suffix >= QLatin1Char('1')
            && suffix <= QLatin1Char('9'))
        || suffix == QChar(0x00B9)
        || suffix == QChar(0x00B2)
        || suffix == QChar(0x00B3);
}

bool isValidWindowsAttachmentLabel(
    const QString& label)
{
    if (label.isEmpty()
        || label == QStringLiteral(".")
        || label == QStringLiteral("..")
        || label.contains(QChar(0))
        || label.contains(QLatin1Char(':'))
        || label.size()
            > kMaximumWindowsComponentCodeUnits
        || isReservedWindowsDeviceName(label)) {
        return false;
    }

    const QString stored =
        windowsStorageLabel(label);
    return stored.size()
        <= kMaximumWindowsComponentCodeUnits
            - kStoredAttachmentPrefixCodeUnits;
}

QString normalizedAbsolutePath(const QString& path)
{
    return QDir::fromNativeSeparators(
        cleanAbsolutePath(path));
}

bool isWithinTree(
    const QString& rootPath,
    const QString& candidatePath)
{
    const QString root = normalizedAbsolutePath(rootPath);
    const QString candidate =
        normalizedAbsolutePath(candidatePath);
    if (candidate.compare(root, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return candidate.startsWith(
        root + QLatin1Char('/'),
        Qt::CaseInsensitive);
}

QFileInfo freshFileInfo(const QString& path)
{
    QFileInfo result(path);
    result.setCaching(false);
    result.refresh();
    return result;
}

bool isUnsafeLink(const QFileInfo& information)
{
    return information.isSymLink()
        || information.isJunction();
}

bool isOwnedEntry(
    const QFileInfo& information,
    const QString& treeRootPath,
    const QString& canonicalTreeRoot)
{
    if (!information.exists()
        || isUnsafeLink(information)
        || !isWithinTree(
            treeRootPath,
            information.absoluteFilePath())) {
        return false;
    }

    const QString canonicalPath =
        information.canonicalFilePath();
    return !canonicalPath.isEmpty()
        && isWithinTree(
            canonicalTreeRoot,
            canonicalPath);
}

bool verifyOwnedTree(const QString& treeRootPath)
{
    const QFileInfo root = freshFileInfo(treeRootPath);
    if (!root.exists()
        || !root.isDir()
        || isUnsafeLink(root)) {
        return false;
    }
    const QString canonicalRoot = root.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        return false;
    }

    QVector<QString> pending{root.absoluteFilePath()};
    while (!pending.isEmpty()) {
        const QString directoryPath = pending.takeLast();
        const QFileInfo directory =
            freshFileInfo(directoryPath);
        if (!directory.isDir()
            || !isOwnedEntry(
                directory,
                treeRootPath,
                canonicalRoot)) {
            return false;
        }

        const QFileInfoList entries =
            QDir(directoryPath).entryInfoList(
                QDir::AllEntries
                    | QDir::NoDotAndDotDot
                    | QDir::Hidden
                    | QDir::System,
                QDir::NoSort);
        for (const QFileInfo& cachedEntry : entries) {
            const QFileInfo entry =
                freshFileInfo(
                    cachedEntry.absoluteFilePath());
            if (!isOwnedEntry(
                    entry,
                    treeRootPath,
                    canonicalRoot)) {
                return false;
            }
            if (entry.isDir()) {
                pending.append(entry.absoluteFilePath());
            }
        }
    }

    return true;
}

struct RemovalEntry final {
    QString path;
    bool expanded = false;
};

bool removeVerifiedTree(const QString& treeRootPath)
{
    const QFileInfo root = freshFileInfo(treeRootPath);
    const QString canonicalRoot = root.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        return false;
    }

    QVector<RemovalEntry> pending{
        {root.absoluteFilePath(), false},
    };
    while (!pending.isEmpty()) {
        const RemovalEntry current = pending.takeLast();
        const QFileInfo information =
            freshFileInfo(current.path);
        if (!information.exists()) {
            continue;
        }
        if (!information.isDir()
            || !isOwnedEntry(
                information,
                treeRootPath,
                canonicalRoot)) {
            return false;
        }
        if (current.expanded) {
            if (!QDir().rmdir(information.absoluteFilePath())) {
                return false;
            }
            continue;
        }

        pending.append({information.absoluteFilePath(), true});
        const QFileInfoList entries =
            QDir(information.absoluteFilePath())
                .entryInfoList(
                    QDir::AllEntries
                        | QDir::NoDotAndDotDot
                        | QDir::Hidden
                        | QDir::System,
                    QDir::NoSort);
        for (const QFileInfo& cachedEntry : entries) {
            const QFileInfo entry =
                freshFileInfo(
                    cachedEntry.absoluteFilePath());
            if (!isOwnedEntry(
                    entry,
                    treeRootPath,
                    canonicalRoot)) {
                return false;
            }
            if (entry.isDir()) {
                pending.append(
                    {entry.absoluteFilePath(), false});
                continue;
            }
            if (!QFile::remove(entry.absoluteFilePath())) {
                return false;
            }
        }
    }

    return true;
}

bool removeOwnedDirectoryAt(
    const QString& rootPath,
    const QString& path)
{
    const QFileInfo candidate = freshFileInfo(path);
    if (isUnsafeLink(candidate)) {
        return false;
    }
    if (!candidate.exists()) {
        return true;
    }
    if (!candidate.isDir()
        || !isDirectChild(rootPath, path)) {
        return false;
    }
    if (!verifyOwnedTree(path)) {
        return false;
    }
    return removeVerifiedTree(path);
}

} // namespace

StagedAttachmentCleanupLease::
StagedAttachmentCleanupLease(
    std::function<void()> cleanup)
    : cleanup_(std::move(cleanup))
{
}

StagedAttachmentCleanupLease::
~StagedAttachmentCleanupLease()
{
    std::function<void()> cleanup;
    {
        const std::scoped_lock lock(mutex_);
        if (retained_) {
            return;
        }
        retained_ = true;
        cleanup = std::move(cleanup_);
    }
    if (cleanup) {
        try {
            cleanup();
        } catch (...) {
        }
    }
}

std::shared_ptr<StagedAttachmentCleanupLease>
StagedAttachmentCleanupLease::retainedInert() noexcept
{
    try {
        auto lease = std::shared_ptr<
            StagedAttachmentCleanupLease>(
            new StagedAttachmentCleanupLease({}));
        lease->retainForCommittedUse();
        return lease;
    } catch (...) {
        return {};
    }
}

void StagedAttachmentCleanupLease::
retainForCommittedUse() noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        retained_ = true;
        cleanup_ = {};
    } catch (...) {
    }
}

QJsonObject StagedAttachment::followerNative() const
{
    return {
        {QStringLiteral("label"), label},
        {QStringLiteral("path"), path},
        {QStringLiteral("fsPath"), fsPath},
    };
}

std::optional<QJsonObject>
StagedAttachment::followerInput() const
{
    if (kind != AttachmentKind::Image) {
        return std::nullopt;
    }
    return localImageInput(path);
}

QJsonObject StagedAttachment::appServerInput() const
{
    if (kind == AttachmentKind::Image) {
        return localImageInput(path);
    }
    return {
        {
            QStringLiteral("type"),
            QStringLiteral("mention"),
        },
        {QStringLiteral("name"), label},
        {QStringLiteral("path"), path},
    };
}

std::optional<QJsonObject>
StagedAttachment::queuedImage() const
{
    if (kind != AttachmentKind::Image) {
        return std::nullopt;
    }

    QJsonObject result{
        {QStringLiteral("id"), uuidText(id)},
        {
            QStringLiteral("src"),
            QUrl::fromLocalFile(path)
                .toString(QUrl::FullyEncoded),
        },
        {QStringLiteral("filename"), label},
        {QStringLiteral("localPath"), path},
        {
            QStringLiteral("uploadStatus"),
            QStringLiteral("uploaded"),
        },
    };
    if (mimeType.has_value() && !mimeType->isEmpty()) {
        result.insert(QStringLiteral("mimeType"), *mimeType);
    }
    return result;
}

std::optional<QJsonObject>
StagedAttachment::queuedFile() const
{
    if (kind != AttachmentKind::File) {
        return std::nullopt;
    }
    return followerNative();
}

QString AttachmentStore::defaultRootPath()
{
    QString localData = qEnvironmentVariable("LOCALAPPDATA");
    if (localData.trimmed().isEmpty()) {
        localData = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation);
    }
    if (localData.trimmed().isEmpty()) {
        localData = QDir::tempPath();
    }

    return cleanAbsolutePath(
        QDir(localData).filePath(
            QStringLiteral(
                "Codex Companion/IncomingAttachments")));
}

Result<void> AttachmentStore::validate(
    const QVector<BridgeAttachment>& attachments)
{
    if (attachments.size() > kMaximumAttachmentCount) {
        return Result<void>::failure(attachmentError(
            QStringLiteral("attachment.too_many"),
            QStringLiteral(
                "You can attach up to 10 items.")));
    }

    qsizetype totalBytes = 0;
    for (const BridgeAttachment& attachment : attachments) {
        const qsizetype itemBytes = attachment.data.size();
        if (itemBytes > kMaximumAttachmentBytes) {
            return Result<void>::failure(attachmentError(
                QStringLiteral("attachment.item_too_large"),
                QStringLiteral(
                    "%1 is larger than the 20 MB attachment limit.")
                    .arg(attachment.filename),
                {
                    {
                        QStringLiteral("filename"),
                        attachment.filename,
                    },
                }));
        }

        totalBytes += itemBytes;
        if (totalBytes > kMaximumAttachmentTotalBytes) {
            return Result<void>::failure(attachmentError(
                QStringLiteral("attachment.total_too_large"),
                QStringLiteral(
                    "The selected attachments are larger than "
                    "the 50 MB total limit.")));
        }
    }

    return Result<void>::success();
}

AttachmentStore::AttachmentStore(
    QString rootPath,
    AttachmentClock clock,
    AttachmentWriter writer)
    : rootPath_(
          cleanAbsolutePath(
              rootPath.trimmed().isEmpty()
                  ? defaultRootPath()
                  : rootPath)),
      clock_(
          clock
              ? std::move(clock)
              : AttachmentClock([] {
                    return QDateTime::currentDateTimeUtc();
                })),
      writer_(
          writer
              ? std::move(writer)
              : AttachmentWriter(atomicWrite))
{
}

Result<QVector<StagedAttachment>> AttachmentStore::stage(
    const QVector<BridgeAttachment>& attachments,
    const QUuid& requestId) const
{
    Result<StagedAttachmentBatch> owned =
        stageOwned(attachments, requestId);
    if (!owned.hasValue()) {
        return Result<
            QVector<StagedAttachment>>::failure(
            owned.error());
    }
    StagedAttachmentBatch batch =
        std::move(owned.value());
    if (batch.cleanupLease != nullptr) {
        batch.cleanupLease
            ->retainForCommittedUse();
    }
    return Result<
        QVector<StagedAttachment>>::success(
        std::move(batch.attachments));
}

Result<StagedAttachmentBatch>
AttachmentStore::stageOwned(
    const QVector<BridgeAttachment>& attachments,
    const QUuid& requestId) const
{
    const Result<void> validation = validate(attachments);
    if (!validation.hasValue()) {
        return Result<StagedAttachmentBatch>::failure(
            validation.error());
    }
    if (attachments.isEmpty()) {
        try {
            return Result<StagedAttachmentBatch>::success({
                {},
                std::shared_ptr<
                    StagedAttachmentCleanupLease>(
                    new StagedAttachmentCleanupLease({})),
            });
        } catch (...) {
            return Result<StagedAttachmentBatch>::failure(
                attachmentError(
                    QStringLiteral(
                        "attachment.storage_failed"),
                    QStringLiteral(
                        "Codex Companion could not retain "
                        "its attachment cleanup state.")));
        }
    }

    QVector<QString> labels;
    labels.reserve(attachments.size());
    for (const BridgeAttachment& attachment : attachments) {
        const Result<QString> label =
            sanitizedFilename(attachment.filename);
        if (!label.hasValue()) {
            return Result<StagedAttachmentBatch>::failure(
                label.error());
        }
        labels.append(label.value());
    }

    if (!QDir().mkpath(rootPath_)) {
        return Result<StagedAttachmentBatch>::failure(
            attachmentError(
                QStringLiteral("attachment.storage_failed"),
                QStringLiteral(
                    "Codex Companion could not create its "
                    "attachment storage.")));
    }

    const Result<void> pruned = pruneExpiredDirectories();
    if (!pruned.hasValue()) {
        return Result<StagedAttachmentBatch>::failure(
            pruned.error());
    }

    const QString requestName = uuidText(requestId);
    const QString partialName =
        requestName + QStringLiteral(".partial");
    const QDir root(rootPath_);
    const QString partialPath = root.filePath(partialName);
    const QString finalPath = root.filePath(requestName);
    if (QFileInfo::exists(partialPath)
        || QFileInfo::exists(finalPath)) {
        return Result<StagedAttachmentBatch>::failure(
            attachmentError(
                QStringLiteral("attachment.request_exists"),
                QStringLiteral(
                    "This attachment request has already been "
                    "staged.")));
    }
    if (!QDir(rootPath_).mkdir(partialName)) {
        return Result<StagedAttachmentBatch>::failure(
            attachmentError(
                QStringLiteral("attachment.storage_failed"),
                QStringLiteral(
                    "Codex Companion could not create an "
                    "attachment request directory.")));
    }

    QVector<StagedAttachment> staged;
    staged.reserve(attachments.size());
    for (qsizetype index = 0;
         index < attachments.size();
         ++index) {
        const BridgeAttachment& attachment =
            attachments.at(index);
        const QString storedFilename =
            uuidText(attachment.id)
            + QLatin1Char('-')
            + windowsStorageLabel(labels.at(index));
        const QString partialFilePath =
            QDir(partialPath).filePath(storedFilename);
        const Result<void> written =
            writer_(partialFilePath, attachment.data);
        if (!written.hasValue()) {
            removeOwnedDirectory(partialPath);
            return Result<StagedAttachmentBatch>::failure(
                written.error());
        }

        const QString publishedPath =
            QDir::toNativeSeparators(
                cleanAbsolutePath(
                    QDir(finalPath).filePath(storedFilename)));
        staged.append({
            attachment.id,
            attachment.kind,
            labels.at(index),
            publishedPath,
            publishedPath,
            attachment.mimeType,
        });
    }

    if (!QDir(rootPath_).rename(partialName, requestName)) {
        removeOwnedDirectory(partialPath);
        return Result<StagedAttachmentBatch>::failure(
            attachmentError(
                QStringLiteral("attachment.publish_failed"),
                QStringLiteral(
                    "Codex Companion could not publish the "
                    "staged attachments.")));
    }

    try {
        const QString ownedRootPath = rootPath_;
        const QString ownedFinalPath = finalPath;
        auto cleanupLease = std::shared_ptr<
            StagedAttachmentCleanupLease>(
            new StagedAttachmentCleanupLease(
                [ownedRootPath, ownedFinalPath] {
                    removeOwnedDirectoryAt(
                        ownedRootPath,
                        ownedFinalPath);
                }));
        return Result<StagedAttachmentBatch>::success({
            std::move(staged),
            std::move(cleanupLease),
        });
    } catch (...) {
        removeOwnedDirectory(finalPath);
        return Result<StagedAttachmentBatch>::failure(
            attachmentError(
                QStringLiteral(
                    "attachment.storage_failed"),
                QStringLiteral(
                    "Codex Companion could not retain its "
                    "attachment cleanup state.")));
    }
}

Result<QString> AttachmentStore::sanitizedFilename(
    const QString& filename) const
{
    const QString trimmed = filename.trimmed();
    if (trimmed.contains(QChar(0))) {
        return Result<QString>::failure(attachmentError(
            QStringLiteral("attachment.invalid_filename"),
            QStringLiteral(
                "One attachment has an invalid filename.")));
    }

    QString normalized = trimmed;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.size() > 1
           && normalized.endsWith(QLatin1Char('/'))) {
        normalized.chop(1);
    }
    const qsizetype separator =
        normalized.lastIndexOf(QLatin1Char('/'));
    const QString lastComponent =
        normalized.mid(separator + 1);
    if (!isValidWindowsAttachmentLabel(
            lastComponent)) {
        return Result<QString>::failure(attachmentError(
            QStringLiteral("attachment.invalid_filename"),
            QStringLiteral(
                "One attachment has an invalid filename.")));
    }

    return Result<QString>::success(lastComponent);
}

Result<void> AttachmentStore::pruneExpiredDirectories() const
{
    const QDateTime expiration =
        clock_().toUTC().addDays(
            -kAttachmentRetentionDays);
    const QFileInfoList candidates =
        QDir(rootPath_).entryInfoList(
            QDir::Dirs
                | QDir::NoDotAndDotDot
                | QDir::Hidden
                | QDir::System,
            QDir::NoSort);

    for (const QFileInfo& candidate : candidates) {
        if (candidate.isSymLink()
            || candidate.isJunction()
            || candidate.isHidden()
            || candidate.fileName().startsWith(
                QLatin1Char('.'))
            || !candidate.lastModified().isValid()
            || candidate.lastModified().toUTC()
                >= expiration) {
            continue;
        }
        removeOwnedDirectory(candidate.absoluteFilePath());
    }

    return Result<void>::success();
}

bool AttachmentStore::removeOwnedDirectory(
    const QString& path) const
{
    return removeOwnedDirectoryAt(rootPath_, path);
}

} // namespace companion
