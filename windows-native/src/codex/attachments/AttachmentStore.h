#pragma once

#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace companion {

inline constexpr qsizetype kMaximumAttachmentCount = 10;
inline constexpr qsizetype kMaximumAttachmentBytes =
    20 * 1024 * 1024;
inline constexpr qsizetype kMaximumAttachmentTotalBytes =
    50 * 1024 * 1024;

struct StagedAttachment final {
    QUuid id;
    AttachmentKind kind = AttachmentKind::File;
    QString label;
    QString path;
    QString fsPath;
    std::optional<QString> mimeType;

    QJsonObject followerNative() const;
    std::optional<QJsonObject> followerInput() const;
    QJsonObject appServerInput() const;
    std::optional<QJsonObject> queuedImage() const;
    std::optional<QJsonObject> queuedFile() const;

    friend bool operator==(
        const StagedAttachment&,
        const StagedAttachment&) = default;
};

using AttachmentClock = std::function<QDateTime()>;
using AttachmentWriter = std::function<Result<void>(
    const QString&,
    const QByteArray&)>;

class StagedAttachmentCleanupLease final {
public:
    ~StagedAttachmentCleanupLease();

    StagedAttachmentCleanupLease(
        const StagedAttachmentCleanupLease&) = delete;
    StagedAttachmentCleanupLease& operator=(
        const StagedAttachmentCleanupLease&) = delete;
    StagedAttachmentCleanupLease(
        StagedAttachmentCleanupLease&&) = delete;
    StagedAttachmentCleanupLease& operator=(
        StagedAttachmentCleanupLease&&) = delete;

    static std::shared_ptr<
        StagedAttachmentCleanupLease>
    retainedInert() noexcept;

    void retainForCommittedUse() noexcept;

private:
    explicit StagedAttachmentCleanupLease(
        std::function<void()> cleanup);

    friend class AttachmentStore;

    std::mutex mutex_;
    std::function<void()> cleanup_;
    bool retained_ = false;
};

struct StagedAttachmentBatch final {
    QVector<StagedAttachment> attachments;
    std::shared_ptr<
        StagedAttachmentCleanupLease>
        cleanupLease;
};

class AttachmentStore final {
public:
    static QString defaultRootPath();
    static Result<void> validate(
        const QVector<BridgeAttachment>& attachments);

    explicit AttachmentStore(
        QString rootPath = defaultRootPath(),
        AttachmentClock clock = {},
        AttachmentWriter writer = {});

    Result<QVector<StagedAttachment>> stage(
        const QVector<BridgeAttachment>& attachments,
        const QUuid& requestId) const;
    Result<StagedAttachmentBatch> stageOwned(
        const QVector<BridgeAttachment>& attachments,
        const QUuid& requestId) const;

private:
    Result<QString> sanitizedFilename(
        const QString& filename) const;
    Result<void> pruneExpiredDirectories() const;
    bool removeOwnedDirectory(const QString& path) const;

    QString rootPath_;
    AttachmentClock clock_;
    AttachmentWriter writer_;
};

} // namespace companion
