#include "mobile/nearby/NearbyTransferAssembler.h"

#include "codex/attachments/AttachmentStore.h"
#include "codex/models/BridgeJsonCodec.h"
#include "platform/windows/security/WindowsCrypto.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace companion {
namespace {

constexpr qsizetype kMaximumActiveTransfers = 8;
constexpr qsizetype kSha256HexBytes = 64;

CompanionError assemblyError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

QString uuidText(const QUuid& value)
{
    return value.toString(
        QUuid::WithoutBraces)
        .toUpper();
}

QString cleanAbsolutePath(
    const QString& path)
{
    return QDir::cleanPath(
        QFileInfo(path)
            .absoluteFilePath());
}

bool isLowercaseHex(
    QByteArrayView bytes)
{
    if (bytes.size() != kSha256HexBytes) {
        return false;
    }
    for (const char character : bytes) {
        const bool valid =
            (character >= '0'
             && character <= '9')
            || (character >= 'a'
                && character <= 'f');
        if (!valid) {
            return false;
        }
    }
    return true;
}

std::optional<AttachmentKind> attachmentKind(
    const QString& value)
{
    if (value == QStringLiteral("file")) {
        return AttachmentKind::File;
    }
    if (value == QStringLiteral("image")) {
        return AttachmentKind::Image;
    }
    return std::nullopt;
}

quint32 expectedChunkCount(
    qint64 byteCount)
{
    if (byteCount <= 0) {
        return 0;
    }
    const qint64 chunks =
        (byteCount
         + NearbyFrameCodec::
             maximumChunkBytes
         - 1)
        / NearbyFrameCodec::
            maximumChunkBytes;
    return static_cast<quint32>(chunks);
}

qsizetype expectedChunkBytes(
    qint64 byteCount,
    quint32 index)
{
    const qint64 offset =
        static_cast<qint64>(index)
        * NearbyFrameCodec::
            maximumChunkBytes;
    return static_cast<qsizetype>(
        std::min<qint64>(
            NearbyFrameCodec::
                maximumChunkBytes,
            byteCount - offset));
}

Result<void> removeTransferDirectory(
    const QString& rootPath,
    const QString& directoryPath)
{
    const QFileInfo candidate(
        directoryPath);
    if (!candidate.exists()) {
        return Result<void>::success();
    }
    if (!candidate.isDir()
        || candidate.isSymLink()
        || candidate.isJunction()
        || cleanAbsolutePath(
               candidate.absolutePath())
            .compare(
                cleanAbsolutePath(rootPath),
                Qt::CaseInsensitive)
            != 0) {
        return Result<void>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.cleanup_rejected"),
                QStringLiteral(
                    "Codex Companion rejected unsafe nearby transfer cleanup.")));
    }
    if (!QDir(directoryPath)
             .removeRecursively()) {
        return Result<void>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.cleanup_failed"),
                QStringLiteral(
                    "Codex Companion could not remove an incomplete nearby transfer.")));
    }
    return Result<void>::success();
}

struct ParsedBegin final {
    QUuid id;
    AttachmentKind kind =
        AttachmentKind::File;
    QString filename;
    std::optional<QString> mimeType;
    qint64 byteCount = 0;
    QByteArray digest;
};

Result<ParsedBegin> parseBegin(
    QByteArrayView bytes)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            bytes.toByteArray(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<ParsedBegin>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.invalid_begin"),
                QStringLiteral(
                    "The nearby attachment metadata is invalid.")));
    }
    const QJsonObject object =
        document.object();
    const QSet<QString> allowed{
        QStringLiteral("id"),
        QStringLiteral("kind"),
        QStringLiteral("filename"),
        QStringLiteral("mimeType"),
        QStringLiteral("byteCount"),
        QStringLiteral("sha256"),
    };
    for (auto iterator = object.begin();
         iterator != object.end();
         ++iterator) {
        if (!allowed.contains(iterator.key())) {
            return Result<ParsedBegin>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_begin"),
                    QStringLiteral(
                        "The nearby attachment metadata is invalid.")));
        }
    }
    const auto idValue =
        object.value(QStringLiteral("id"));
    const auto kindValue =
        object.value(QStringLiteral("kind"));
    const auto filenameValue =
        object.value(
            QStringLiteral("filename"));
    const auto byteCountValue =
        object.value(
            QStringLiteral("byteCount"));
    const auto digestValue =
        object.value(
            QStringLiteral("sha256"));
    if (!idValue.isString()
        || !kindValue.isString()
        || !filenameValue.isString()
        || !byteCountValue.isDouble()
        || !digestValue.isString()) {
        return Result<ParsedBegin>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.invalid_begin"),
                QStringLiteral(
                    "The nearby attachment metadata is invalid.")));
    }
    const QUuid id(idValue.toString());
    const auto kind =
        attachmentKind(
            kindValue.toString());
    const double byteCountNumber =
        byteCountValue.toDouble();
    const bool exactByteCount =
        std::isfinite(byteCountNumber)
        && std::trunc(byteCountNumber)
            == byteCountNumber
        && byteCountNumber >= 0
        && byteCountNumber
            <= static_cast<double>(
                kMaximumAttachmentBytes);
    const qint64 byteCount =
        exactByteCount
        ? static_cast<qint64>(
              byteCountNumber)
        : -1;
    const QByteArray digest =
        digestValue.toString()
            .toLatin1();
    if (id.isNull()
        || !kind.has_value()
        || filenameValue.toString()
               .isEmpty()
        || byteCount < 0
        || !isLowercaseHex(digest)) {
        return Result<ParsedBegin>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.invalid_begin"),
                QStringLiteral(
                    "The nearby attachment metadata is invalid.")));
    }

    std::optional<QString> mimeType;
    const auto mimeValue =
        object.constFind(
            QStringLiteral("mimeType"));
    if (mimeValue != object.constEnd()) {
        if (!mimeValue->isString()) {
            return Result<ParsedBegin>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_begin"),
                    QStringLiteral(
                        "The nearby attachment metadata is invalid.")));
        }
        mimeType = mimeValue->toString();
    }
    return Result<ParsedBegin>::success({
        id,
        *kind,
        filenameValue.toString(),
        std::move(mimeType),
        byteCount,
        QByteArray::fromHex(digest),
    });
}

} // namespace

struct NearbyTransferAssembler::
Implementation final {
    struct AttachmentState final {
        BridgeAttachment metadata;
        qint64 byteCount = 0;
        QByteArray expectedDigest;
        quint32 expectedChunks = 0;
        quint32 nextChunk = 0;
        qint64 writtenBytes = 0;
        QFile file;
        QCryptographicHash hash{
            QCryptographicHash::Sha256};
        bool committed = false;
    };

    struct TransferState final {
        BridgeRequest request;
        QString directoryPath;
        QSet<QUuid> streamedAttachmentIds;
        QHash<
            QUuid,
            std::shared_ptr<
                AttachmentState>>
            attachments;
        qint64 declaredBytes = 0;
    };

    explicit Implementation(
        QString requestedRootPath)
        : rootPath(
              cleanAbsolutePath(
                  requestedRootPath
                          .trimmed()
                          .isEmpty()
                      ? NearbyTransferAssembler::
                            defaultRootPath()
                      : std::move(
                            requestedRootPath)))
    {
    }

    ~Implementation()
    {
        cancelAll();
    }

    void closeFiles(
        const std::shared_ptr<
            TransferState>& transfer)
    {
        if (transfer == nullptr) {
            return;
        }
        for (auto iterator =
                 transfer->attachments.begin();
             iterator
             != transfer->attachments.end();
             ++iterator) {
            if (iterator.value() != nullptr
                && iterator.value()
                       ->file.isOpen()) {
                iterator.value()
                    ->file.close();
            }
        }
    }

    void removeTransfer(
        const QUuid& transferId)
    {
        const auto iterator =
            transfers.find(transferId);
        if (iterator == transfers.end()) {
            return;
        }
        const auto transfer =
            iterator.value();
        transfers.erase(iterator);
        closeFiles(transfer);
        if (transfer != nullptr) {
            removeTransferDirectory(
                rootPath,
                transfer->directoryPath);
        }
    }

    Result<NearbyAssemblyResult>
    failTransfer(
        const QUuid& transferId,
        CompanionError error)
    {
        removeTransfer(transferId);
        return Result<
            NearbyAssemblyResult>::failure(
            std::move(error));
    }

    Result<NearbyAssemblyResult>
    consumeRequest(
        const NearbyFrame& frame)
    {
        if (transfers.contains(
                frame.transferId)) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.duplicate_transfer"),
                    QStringLiteral(
                        "The nearby transfer already exists.")));
        }
        const auto decoded =
            BridgeJsonCodec::decodeRequest(
                frame.payload,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        if (!decoded.hasValue()) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_request"),
                    QStringLiteral(
                        "The nearby request payload is invalid.")));
        }
        BridgeRequest request =
            decoded.value();
        const QVector<BridgeAttachment>
            requestAttachments =
                request.attachments
                    .value_or(
                        QVector<
                            BridgeAttachment>{});
        const auto validation =
            AttachmentStore::validate(
                requestAttachments);
        if (!validation.hasValue()) {
            return Result<
                NearbyAssemblyResult>::failure(
                validation.error());
        }

        QSet<QUuid> attachmentIds;
        QSet<QUuid> streamedIds;
        qint64 inlineBytes = 0;
        for (const auto& attachment :
             requestAttachments) {
            if (attachment.id.isNull()
                || attachmentIds.contains(
                    attachment.id)) {
                return Result<
                    NearbyAssemblyResult>::failure(
                    assemblyError(
                        QStringLiteral(
                            "nearby.invalid_attachment_id"),
                        QStringLiteral(
                            "The nearby request attachment identifier is invalid.")));
            }
            attachmentIds.insert(
                attachment.id);
            if (attachment.data.isEmpty()) {
                streamedIds.insert(
                    attachment.id);
            } else {
                inlineBytes +=
                    attachment.data.size();
            }
        }
        if (streamedIds.isEmpty()) {
            return Result<
                NearbyAssemblyResult>::success({
                NearbyAssemblyDisposition::
                    Dispatch,
                std::move(request),
                std::nullopt,
            });
        }
        if (transfers.size()
            >= kMaximumActiveTransfers) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.too_many_transfers"),
                    QStringLiteral(
                        "Too many nearby transfers are active.")));
        }
        if (!QDir().mkpath(rootPath)) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.storage_failed"),
                    QStringLiteral(
                        "Codex Companion could not create nearby transfer storage.")));
        }
        const QString directoryPath =
            QDir(rootPath).filePath(
                uuidText(frame.transferId)
                + QStringLiteral(".partial"));
        if (QFileInfo::exists(
                directoryPath)
            || !QDir(rootPath).mkdir(
                QFileInfo(directoryPath)
                    .fileName())) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.storage_failed"),
                    QStringLiteral(
                        "Codex Companion could not create the nearby transfer directory.")));
        }

        auto transfer =
            std::make_shared<
                TransferState>();
        transfer->request =
            std::move(request);
        transfer->directoryPath =
            directoryPath;
        transfer->streamedAttachmentIds =
            std::move(streamedIds);
        transfer->declaredBytes =
            inlineBytes;
        transfers.insert(
            frame.transferId,
            std::move(transfer));
        return Result<
            NearbyAssemblyResult>::success({
            NearbyAssemblyDisposition::
                AwaitingFrames,
            std::nullopt,
            std::nullopt,
        });
    }

    Result<NearbyAssemblyResult>
    consumeBegin(
        const NearbyFrame& frame)
    {
        const auto iterator =
            transfers.find(frame.transferId);
        if (iterator == transfers.end()) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_transfer"),
                    QStringLiteral(
                        "The nearby transfer is unknown.")));
        }
        const auto transfer =
            iterator.value();
        const auto parsed =
            parseBegin(frame.payload);
        if (!parsed.hasValue()) {
            return failTransfer(
                frame.transferId,
                parsed.error());
        }
        const ParsedBegin& begin =
            parsed.value();
        if (begin.id != frame.attachmentId
            || !transfer
                    ->streamedAttachmentIds
                    .contains(begin.id)
            || transfer->attachments
                   .contains(begin.id)
            || frame.chunkCount
                != expectedChunkCount(
                    begin.byteCount)) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_begin"),
                    QStringLiteral(
                        "The nearby attachment metadata does not match its request.")));
        }

        const auto requestAttachments =
            transfer->request.attachments;
        if (!requestAttachments.has_value()) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_begin"),
                    QStringLiteral(
                        "The nearby attachment metadata does not match its request.")));
        }
        const auto placeholder =
            std::find_if(
                requestAttachments->cbegin(),
                requestAttachments->cend(),
                [&begin](
                    const BridgeAttachment&
                        attachment) {
                    return attachment.id
                        == begin.id;
                });
        if (placeholder
                == requestAttachments->cend()
            || placeholder->kind
                != begin.kind
            || placeholder->filename
                != begin.filename
            || placeholder->mimeType
                != begin.mimeType) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_begin"),
                    QStringLiteral(
                        "The nearby attachment metadata does not match its request.")));
        }
        if (begin.byteCount
            > kMaximumAttachmentBytes
            || transfer->declaredBytes
                    + begin.byteCount
                > kMaximumAttachmentTotalBytes) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.attachment_too_large"),
                    QStringLiteral(
                        "The nearby attachment payload exceeds its limit.")));
        }

        auto attachment =
            std::make_shared<
                AttachmentState>();
        attachment->metadata =
            *placeholder;
        attachment->byteCount =
            begin.byteCount;
        attachment->expectedDigest =
            begin.digest;
        attachment->expectedChunks =
            frame.chunkCount;
        attachment->file.setFileName(
            QDir(transfer->directoryPath)
                .filePath(
                    uuidText(begin.id)
                    + QStringLiteral(
                        ".partial")));
        if (!attachment->file.open(
                QIODevice::WriteOnly
                | QIODevice::NewOnly)) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.storage_failed"),
                    QStringLiteral(
                        "Codex Companion could not create a nearby attachment file.")));
        }
        transfer->declaredBytes +=
            begin.byteCount;
        transfer->attachments.insert(
            begin.id,
            std::move(attachment));
        return Result<
            NearbyAssemblyResult>::success({
            NearbyAssemblyDisposition::
                AwaitingFrames,
            std::nullopt,
            std::nullopt,
        });
    }

    Result<NearbyAssemblyResult>
    consumeChunk(
        const NearbyFrame& frame)
    {
        const auto transferIterator =
            transfers.find(frame.transferId);
        if (transferIterator
            == transfers.end()) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_transfer"),
                    QStringLiteral(
                        "The nearby transfer is unknown.")));
        }
        const auto transfer =
            transferIterator.value();
        const auto attachmentIterator =
            transfer->attachments.find(
                frame.attachmentId);
        if (attachmentIterator
                == transfer
                       ->attachments.end()
            || attachmentIterator.value()
                == nullptr) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_attachment"),
                    QStringLiteral(
                        "The nearby attachment transfer is unknown.")));
        }
        const auto attachment =
            attachmentIterator.value();
        const qsizetype expectedBytes =
            attachment->nextChunk
                    < attachment
                          ->expectedChunks
                ? expectedChunkBytes(
                      attachment->byteCount,
                      attachment->nextChunk)
                : -1;
        if (attachment->committed
            || frame.chunkCount
                != attachment
                       ->expectedChunks
            || frame.chunkIndex
                != attachment->nextChunk
            || expectedBytes < 0
            || frame.payload.size()
                != expectedBytes) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_chunk"),
                    QStringLiteral(
                        "The nearby attachment chunk is out of sequence.")));
        }
        if (attachment->file.write(
                frame.payload)
            != frame.payload.size()) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.storage_failed"),
                    QStringLiteral(
                        "Codex Companion could not write a nearby attachment chunk.")));
        }
        attachment->hash.addData(
            frame.payload);
        attachment->writtenBytes +=
            frame.payload.size();
        ++attachment->nextChunk;
        return Result<
            NearbyAssemblyResult>::success({
            NearbyAssemblyDisposition::
                AwaitingFrames,
            std::nullopt,
            std::nullopt,
        });
    }

    Result<NearbyAssemblyResult>
    consumeCommit(
        const NearbyFrame& frame)
    {
        const auto transferIterator =
            transfers.find(frame.transferId);
        if (transferIterator
            == transfers.end()) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_transfer"),
                    QStringLiteral(
                        "The nearby transfer is unknown.")));
        }
        const auto transfer =
            transferIterator.value();
        const auto attachmentIterator =
            transfer->attachments.find(
                frame.attachmentId);
        if (attachmentIterator
                == transfer
                       ->attachments.end()
            || attachmentIterator.value()
                == nullptr) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_attachment"),
                    QStringLiteral(
                        "The nearby attachment transfer is unknown.")));
        }
        const auto attachment =
            attachmentIterator.value();
        if (attachment->committed
            || frame.chunkCount
                != attachment
                       ->expectedChunks
            || attachment->nextChunk
                != attachment
                       ->expectedChunks
            || attachment->writtenBytes
                != attachment->byteCount) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_commit"),
                    QStringLiteral(
                        "The nearby attachment is incomplete.")));
        }
        attachment->file.close();
        if (!WindowsCrypto::constantTimeEquals(
                attachment->hash.result(),
                attachment
                    ->expectedDigest)) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.hash_mismatch"),
                    QStringLiteral(
                        "The nearby attachment hash does not match.")));
        }
        attachment->committed = true;
        for (const QUuid& expected :
             transfer
                 ->streamedAttachmentIds) {
            const auto current =
                transfer->attachments
                    .constFind(expected);
            if (current
                    == transfer
                           ->attachments
                           .constEnd()
                || current.value()
                    == nullptr
                || !current.value()
                        ->committed) {
                return Result<
                    NearbyAssemblyResult>::success({
                    NearbyAssemblyDisposition::
                        AwaitingFrames,
                    std::nullopt,
                    std::nullopt,
                });
            }
        }

        if (!transfer->request.attachments
                 .has_value()) {
            return failTransfer(
                frame.transferId,
                assemblyError(
                    QStringLiteral(
                        "nearby.invalid_request"),
                    QStringLiteral(
                        "The nearby request attachment list is missing.")));
        }
        BridgeRequest request =
            transfer->request;
        for (BridgeAttachment& item :
             *request.attachments) {
            if (!item.data.isEmpty()) {
                continue;
            }
            const auto state =
                transfer->attachments
                    .value(item.id);
            if (state == nullptr
                || !state->committed) {
                return failTransfer(
                    frame.transferId,
                    assemblyError(
                        QStringLiteral(
                            "nearby.invalid_request"),
                        QStringLiteral(
                            "The nearby request attachment is incomplete.")));
            }
            QFile file(
                state->file.fileName());
            if (!file.open(
                    QIODevice::ReadOnly)
                || file.size()
                    != state->byteCount) {
                return failTransfer(
                    frame.transferId,
                    assemblyError(
                        QStringLiteral(
                            "nearby.storage_failed"),
                        QStringLiteral(
                            "Codex Companion could not read a completed nearby attachment.")));
            }
            item.data = file.readAll();
            if (item.data.size()
                != state->byteCount) {
                return failTransfer(
                    frame.transferId,
                    assemblyError(
                        QStringLiteral(
                            "nearby.storage_failed"),
                        QStringLiteral(
                            "Codex Companion could not read a completed nearby attachment.")));
            }
        }
        const auto validation =
            AttachmentStore::validate(
                *request.attachments);
        if (!validation.hasValue()) {
            return failTransfer(
                frame.transferId,
                validation.error());
        }

        removeTransfer(
            frame.transferId);
        return Result<
            NearbyAssemblyResult>::success({
            NearbyAssemblyDisposition::
                Dispatch,
            std::move(request),
            std::nullopt,
        });
    }

    Result<NearbyAssemblyResult>
    consumeCancel(
        const NearbyFrame& frame)
    {
        if (!transfers.contains(
                frame.transferId)) {
            return Result<
                NearbyAssemblyResult>::failure(
                assemblyError(
                    QStringLiteral(
                        "nearby.unknown_transfer"),
                    QStringLiteral(
                        "The nearby transfer is unknown.")));
        }
        const QString code =
            QString::fromLatin1(
                frame.payload);
        removeTransfer(frame.transferId);
        return Result<
            NearbyAssemblyResult>::success({
            NearbyAssemblyDisposition::
                Cancelled,
            std::nullopt,
            code,
        });
    }

    Result<NearbyAssemblyResult> consume(
        const NearbyFrame& frame)
    {
        switch (frame.type) {
        case NearbyFrameType::Request:
            return consumeRequest(frame);
        case NearbyFrameType::AttachmentBegin:
            return consumeBegin(frame);
        case NearbyFrameType::AttachmentChunk:
            return consumeChunk(frame);
        case NearbyFrameType::AttachmentCommit:
            return consumeCommit(frame);
        case NearbyFrameType::TransferCancel:
            return consumeCancel(frame);
        }
        return Result<
            NearbyAssemblyResult>::failure(
            assemblyError(
                QStringLiteral(
                    "nearby.invalid_type"),
                QStringLiteral(
                    "The nearby frame type is unsupported.")));
    }

    void cancelAll() noexcept
    {
        try {
            const QList<QUuid> ids =
                transfers.keys();
            for (const QUuid& id : ids) {
                removeTransfer(id);
            }
        } catch (...) {
        }
    }

    QString rootPath;
    QHash<
        QUuid,
        std::shared_ptr<
            TransferState>>
        transfers;
};

QString NearbyTransferAssembler::
defaultRootPath()
{
    QString root =
        qEnvironmentVariable(
            "LOCALAPPDATA");
    if (root.trimmed().isEmpty()) {
        root =
            QStandardPaths::writableLocation(
                QStandardPaths::
                    GenericDataLocation);
    }
    if (root.trimmed().isEmpty()) {
        root = QDir::tempPath();
    }
    return cleanAbsolutePath(
        QDir(root).filePath(
            QStringLiteral(
                "Codex Companion/NearbyTransfers")));
}

NearbyTransferAssembler::
NearbyTransferAssembler(
    QString rootPath)
    : implementation_(
          std::make_unique<
              Implementation>(
              std::move(rootPath)))
{
}

NearbyTransferAssembler::
~NearbyTransferAssembler() = default;

Result<NearbyAssemblyResult>
NearbyTransferAssembler::consume(
    const NearbyFrame& frame)
{
    return implementation_->consume(frame);
}

void NearbyTransferAssembler::cancelAll() noexcept
{
    implementation_->cancelAll();
}

qsizetype
NearbyTransferAssembler::
activeTransferCount() const
{
    return implementation_
        ->transfers.size();
}

} // namespace companion
