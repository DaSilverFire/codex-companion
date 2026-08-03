#include "codex/models/BridgeJsonCodec.h"
#include "mobile/nearby/NearbyFrameCodec.h"
#include "mobile/nearby/NearbyTransferAssembler.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace companion;

namespace {

QByteArray secret()
{
    return QByteArray(32, '\x53');
}

QUuid transferId()
{
    return QUuid(
        QStringLiteral(
            "10000000-0000-0000-0000-000000000001"));
}

QUuid firstAttachmentId()
{
    return QUuid(
        QStringLiteral(
            "20000000-0000-0000-0000-000000000002"));
}

QUuid secondAttachmentId()
{
    return QUuid(
        QStringLiteral(
            "30000000-0000-0000-0000-000000000003"));
}

quint32 bigEndian32(
    const QByteArray& bytes,
    qsizetype offset)
{
    const auto* data =
        reinterpret_cast<
            const unsigned char*>(
            bytes.constData() + offset);
    return (quint32(data[0]) << 24)
        | (quint32(data[1]) << 16)
        | (quint32(data[2]) << 8)
        | quint32(data[3]);
}

NearbyFrame requestFrame(
    BridgeRequest request,
    const QUuid& id = transferId())
{
    const auto encoded =
        BridgeJsonCodec::encodeRequest(
            request,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    Q_ASSERT(encoded.hasValue());
    return {
        NearbyFrameType::Request,
        0,
        id,
        {},
        0,
        0,
        encoded.value(),
    };
}

NearbyFrame beginFrame(
    const QUuid& transfer,
    const BridgeAttachment& attachment,
    const QByteArray& data,
    quint32 chunkCount,
    QByteArray digest = {})
{
    if (digest.isEmpty()) {
        digest =
            QCryptographicHash::hash(
                data,
                QCryptographicHash::Sha256)
                .toHex();
    }
    QJsonObject payload{
        {
            QStringLiteral("id"),
            attachment.id.toString(
                QUuid::WithoutBraces)
                .toUpper(),
        },
        {
            QStringLiteral("kind"),
            attachment.kind
                    == AttachmentKind::Image
                ? QStringLiteral("image")
                : QStringLiteral("file"),
        },
        {
            QStringLiteral("filename"),
            attachment.filename,
        },
        {
            QStringLiteral("byteCount"),
            static_cast<qint64>(
                data.size()),
        },
        {
            QStringLiteral("sha256"),
            QString::fromLatin1(digest),
        },
    };
    if (attachment.mimeType.has_value()) {
        payload.insert(
            QStringLiteral("mimeType"),
            *attachment.mimeType);
    }
    return {
        NearbyFrameType::AttachmentBegin,
        0,
        transfer,
        attachment.id,
        0,
        chunkCount,
        QJsonDocument(payload).toJson(
            QJsonDocument::Compact),
    };
}

NearbyFrame chunkFrame(
    const QUuid& transfer,
    const QUuid& attachment,
    quint32 index,
    quint32 count,
    QByteArray payload)
{
    return {
        NearbyFrameType::AttachmentChunk,
        0,
        transfer,
        attachment,
        index,
        count,
        std::move(payload),
    };
}

NearbyFrame commitFrame(
    const QUuid& transfer,
    const QUuid& attachment,
    quint32 count)
{
    return {
        NearbyFrameType::AttachmentCommit,
        0,
        transfer,
        attachment,
        0,
        count,
        {},
    };
}

BridgeAttachment placeholder(
    QUuid id,
    AttachmentKind kind,
    QString filename,
    std::optional<QString> mimeType =
        std::nullopt)
{
    return {
        id,
        kind,
        std::move(filename),
        std::move(mimeType),
        {},
    };
}

BridgeRequest streamedRequest(
    QVector<BridgeAttachment> attachments)
{
    BridgeRequest request;
    request.id = QUuid(
        QStringLiteral(
            "40000000-0000-0000-0000-000000000004"));
    request.operation =
        BridgeOperation::SendMessage;
    request.threadId =
        QStringLiteral("thread-1");
    request.text =
        QStringLiteral("Review these");
    request.attachments =
        std::move(attachments);
    return request;
}

NearbyAssemblyResult accepted(
    NearbyTransferAssembler& assembler,
    const NearbyFrame& frame)
{
    const auto result =
        assembler.consume(frame);
    Q_ASSERT(result.hasValue());
    return result.value();
}

} // namespace

class NearbyFrameCodecTests final : public QObject {
    Q_OBJECT

private slots:
    void exactHeaderRoundTripsWithAuthenticatedPayload()
    {
        const NearbyFrame frame{
            NearbyFrameType::Request,
            0,
            transferId(),
            {},
            0,
            0,
            QByteArray("{\"operation\":\"handshake\"}"),
        };

        const auto encoded =
            NearbyFrameCodec::encode(
                frame,
                secret());

        QVERIFY(encoded.hasValue());
        QCOMPARE(
            encoded.value().toHex(),
            QByteArray(
                "43434e310101000010000000000000000000000000000001"
                "000000000000000000000000000000000000000000000000"
                "000000190000000012299b100ed3bbf0bb28e64fd4503373"
                "290f1464aff6adac80d5a67b0964767e7b226f7065726174"
                "696f6e223a2268616e647368616b65227d"));
        QCOMPARE(
            encoded.value().size(),
            NearbyFrameCodec::headerBytes
                + frame.payload.size());
        QCOMPARE(
            encoded.value().left(4),
            QByteArray("CCN1"));
        QCOMPARE(
            static_cast<quint8>(
                encoded.value().at(4)),
            quint8(1));
        QCOMPARE(
            static_cast<quint8>(
                encoded.value().at(5)),
            quint8(1));
        QCOMPARE(
            encoded.value().mid(8, 16),
            transferId().toRfc4122());
        QCOMPARE(
            encoded.value().mid(24, 16),
            QByteArray(16, '\0'));
        QCOMPARE(bigEndian32(encoded.value(), 40), quint32(0));
        QCOMPARE(bigEndian32(encoded.value(), 44), quint32(0));
        QCOMPARE(
            bigEndian32(encoded.value(), 48),
            static_cast<quint32>(
                frame.payload.size()));
        QCOMPARE(bigEndian32(encoded.value(), 52), quint32(0));
        QVERIFY(
            encoded.value().mid(56, 32)
            != QByteArray(32, '\0'));
        QCOMPARE(
            encoded.value().mid(
                NearbyFrameCodec::headerBytes),
            frame.payload);

        const auto decoded =
            NearbyFrameCodec::decode(
                encoded.value(),
                secret());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value(), frame);
    }

    void rejectsTamperingMalformedLengthsAndInvalidFields()
    {
        const NearbyFrame valid{
            NearbyFrameType::AttachmentChunk,
            0,
            transferId(),
            firstAttachmentId(),
            0,
            1,
            QByteArray("payload"),
        };
        const auto encoded =
            NearbyFrameCodec::encode(
                valid,
                secret());
        QVERIFY(encoded.hasValue());

        QByteArray tampered = encoded.value();
        tampered[tampered.size() - 1] ^= 0x01;
        QVERIFY(
            !NearbyFrameCodec::decode(
                 tampered,
                 secret())
                 .hasValue());

        QByteArray wrongSecret = secret();
        wrongSecret[0] ^= 0x01;
        QVERIFY(
            !NearbyFrameCodec::decode(
                 encoded.value(),
                 wrongSecret)
                 .hasValue());

        QByteArray truncated = encoded.value();
        truncated.chop(1);
        QVERIFY(
            !NearbyFrameCodec::decode(
                 truncated,
                 secret())
                 .hasValue());

        NearbyFrame invalidTransfer = valid;
        invalidTransfer.transferId = {};
        QVERIFY(
            !NearbyFrameCodec::encode(
                 invalidTransfer,
                 secret())
                 .hasValue());

        NearbyFrame invalidAttachment = valid;
        invalidAttachment.attachmentId = {};
        QVERIFY(
            !NearbyFrameCodec::encode(
                 invalidAttachment,
                 secret())
                 .hasValue());

        NearbyFrame invalidIndex = valid;
        invalidIndex.chunkIndex = 1;
        QVERIFY(
            !NearbyFrameCodec::encode(
                 invalidIndex,
                 secret())
                 .hasValue());

        NearbyFrame oversized = valid;
        oversized.payload =
            QByteArray(
                NearbyFrameCodec::maximumChunkBytes
                    + 1,
                '\x7f');
        QVERIFY(
            !NearbyFrameCodec::encode(
                 oversized,
                 secret())
                 .hasValue());

        NearbyFrame invalidCancel{
            NearbyFrameType::TransferCancel,
            0,
            transferId(),
            {},
            0,
            0,
            QByteArray(129, 'x'),
        };
        QVERIFY(
            !NearbyFrameCodec::encode(
                 invalidCancel,
                 secret())
                 .hasValue());
        invalidCancel.payload =
            QByteArray::fromHex("ff");
        QVERIFY(
            !NearbyFrameCodec::encode(
                 invalidCancel,
                 secret())
                 .hasValue());
    }

    void inlineRequestDispatchesWithoutCreatingStorage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral("transfers"));
        NearbyTransferAssembler assembler(root);
        BridgeAttachment inlineFile{
            firstAttachmentId(),
            AttachmentKind::File,
            QStringLiteral("notes.txt"),
            QStringLiteral("text/plain"),
            QByteArray("inline"),
        };
        BridgeRequest request =
            streamedRequest({inlineFile});

        const auto result =
            assembler.consume(
                requestFrame(request));

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value().disposition,
            NearbyAssemblyDisposition::Dispatch);
        QVERIFY(result.value().request.has_value());
        QCOMPARE(*result.value().request, request);
        QVERIFY(!QFileInfo::exists(root));
    }

    void rejectsFractionalAttachmentByteCounts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NearbyTransferAssembler assembler(
            directory.filePath(
                QStringLiteral("transfers")));
        const BridgeAttachment item =
            placeholder(
                firstAttachmentId(),
                AttachmentKind::File,
                QStringLiteral("file.bin"));
        QVERIFY(
            assembler.consume(
                requestFrame(
                    streamedRequest({item})))
                .hasValue());

        NearbyFrame begin =
            beginFrame(
                transferId(),
                item,
                QByteArray("payload"),
                1);
        QJsonObject metadata =
            QJsonDocument::fromJson(
                begin.payload)
                .object();
        metadata.insert(
            QStringLiteral("byteCount"),
            1.5);
        begin.payload =
            QJsonDocument(metadata).toJson(
                QJsonDocument::Compact);

        const auto rejected =
            assembler.consume(begin);
        QVERIFY(!rejected.hasValue());
        QCOMPARE(
            rejected.error().code,
            QStringLiteral(
                "nearby.invalid_begin"));
    }

    void streamedAttachmentsWriteChunksAndDispatchInRequestOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral("transfers"));
        NearbyTransferAssembler assembler(root);
        const QByteArray firstData(
            NearbyFrameCodec::maximumChunkBytes
                + 7,
            '\x41');
        const QByteArray secondData(
            13,
            '\x42');
        const BridgeAttachment first =
            placeholder(
                firstAttachmentId(),
                AttachmentKind::File,
                QStringLiteral("../first.txt"),
                QStringLiteral("text/plain"));
        const BridgeAttachment second =
            placeholder(
                secondAttachmentId(),
                AttachmentKind::Image,
                QStringLiteral(
                    "C:\\temp\\second.png"),
                QStringLiteral("image/png"));
        const BridgeRequest request =
            streamedRequest({first, second});

        QCOMPARE(
            accepted(
                assembler,
                requestFrame(request))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        QCOMPARE(
            accepted(
                assembler,
                beginFrame(
                    transferId(),
                    second,
                    secondData,
                    1))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        QCOMPARE(
            accepted(
                assembler,
                beginFrame(
                    transferId(),
                    first,
                    firstData,
                    2))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        QCOMPARE(
            accepted(
                assembler,
                chunkFrame(
                    transferId(),
                    first.id,
                    0,
                    2,
                    firstData.left(
                        NearbyFrameCodec::
                            maximumChunkBytes)))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);

        const QString partialRoot =
            QDir(root).filePath(
                transferId().toString(
                    QUuid::WithoutBraces)
                    .toUpper()
                + QStringLiteral(".partial"));
        QVERIFY(QFileInfo::exists(partialRoot));
        const QFileInfoList partialFiles =
            QDir(partialRoot).entryInfoList(
                QDir::Files | QDir::NoDotAndDotDot);
        QCOMPARE(partialFiles.size(), 2);

        QCOMPARE(
            accepted(
                assembler,
                chunkFrame(
                    transferId(),
                    first.id,
                    1,
                    2,
                    firstData.mid(
                        NearbyFrameCodec::
                            maximumChunkBytes)))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        QCOMPARE(
            accepted(
                assembler,
                commitFrame(
                    transferId(),
                    first.id,
                    2))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        QCOMPARE(
            accepted(
                assembler,
                chunkFrame(
                    transferId(),
                    second.id,
                    0,
                    1,
                    secondData))
                .disposition,
            NearbyAssemblyDisposition::AwaitingFrames);
        const NearbyAssemblyResult completed =
            accepted(
                assembler,
                commitFrame(
                    transferId(),
                    second.id,
                    1));

        QCOMPARE(
            completed.disposition,
            NearbyAssemblyDisposition::Dispatch);
        QVERIFY(completed.request.has_value());
        QVERIFY(
            completed.request->attachments
                .has_value());
        QCOMPARE(
            completed.request->attachments->size(),
            2);
        QCOMPARE(
            completed.request
                ->attachments->at(0)
                .id,
            first.id);
        QCOMPARE(
            completed.request
                ->attachments->at(0)
                .data,
            firstData);
        QCOMPARE(
            completed.request
                ->attachments->at(1)
                .id,
            second.id);
        QCOMPARE(
            completed.request
                ->attachments->at(1)
                .data,
            secondData);
        QVERIFY(!QFileInfo::exists(partialRoot));
    }

    void rejectsDuplicateWrongCountUnknownAndHashMismatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const BridgeAttachment item =
            placeholder(
                firstAttachmentId(),
                AttachmentKind::File,
                QStringLiteral("file.bin"));
        const QByteArray data("payload");

        {
            NearbyTransferAssembler assembler(
                directory.filePath(
                    QStringLiteral("duplicate-begin")));
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            const NearbyFrame begin =
                beginFrame(
                    transferId(),
                    item,
                    data,
                    1);
            QVERIFY(
                assembler.consume(begin)
                    .hasValue());
            QVERIFY(
                !assembler.consume(begin)
                     .hasValue());
        }

        {
            NearbyTransferAssembler assembler(
                directory.filePath(
                    QStringLiteral("wrong-count")));
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            QVERIFY(
                !assembler.consume(
                     beginFrame(
                         transferId(),
                         item,
                         data,
                         2))
                     .hasValue());
        }

        {
            NearbyTransferAssembler assembler(
                directory.filePath(
                    QStringLiteral("unknown")));
            QVERIFY(
                !assembler.consume(
                     chunkFrame(
                         transferId(),
                         item.id,
                         0,
                         1,
                         data))
                     .hasValue());
        }

        {
            NearbyTransferAssembler assembler(
                directory.filePath(
                    QStringLiteral("duplicate-chunk")));
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    beginFrame(
                        transferId(),
                        item,
                        data,
                        1))
                    .hasValue());
            const NearbyFrame chunk =
                chunkFrame(
                    transferId(),
                    item.id,
                    0,
                    1,
                    data);
            QVERIFY(
                assembler.consume(chunk)
                    .hasValue());
            QVERIFY(
                !assembler.consume(chunk)
                     .hasValue());
        }

        {
            NearbyTransferAssembler assembler(
                directory.filePath(
                    QStringLiteral("hash")));
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    beginFrame(
                        transferId(),
                        item,
                        data,
                        1,
                        QByteArray(64, '0')))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    chunkFrame(
                        transferId(),
                        item.id,
                        0,
                        1,
                        data))
                    .hasValue());
            QVERIFY(
                !assembler.consume(
                     commitFrame(
                         transferId(),
                         item.id,
                         1))
                     .hasValue());
        }
    }

    void cancelAndDestructionRemoveIncompleteTransfers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root =
            directory.filePath(
                QStringLiteral("transfers"));
        const BridgeAttachment item =
            placeholder(
                firstAttachmentId(),
                AttachmentKind::File,
                QStringLiteral("file.bin"));
        const QByteArray data("payload");
        const QString partialRoot =
            QDir(root).filePath(
                transferId().toString(
                    QUuid::WithoutBraces)
                    .toUpper()
                + QStringLiteral(".partial"));

        {
            NearbyTransferAssembler assembler(root);
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    beginFrame(
                        transferId(),
                        item,
                        data,
                        1))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    chunkFrame(
                        transferId(),
                        item.id,
                        0,
                        1,
                        data))
                    .hasValue());
            QVERIFY(QFileInfo::exists(partialRoot));

            const auto canceled =
                assembler.consume({
                    NearbyFrameType::TransferCancel,
                    0,
                    transferId(),
                    {},
                    0,
                    0,
                    QByteArray("client_cancelled"),
                });
            QVERIFY(canceled.hasValue());
            QCOMPARE(
                canceled.value().disposition,
                NearbyAssemblyDisposition::Cancelled);
            QVERIFY(!QFileInfo::exists(partialRoot));
        }

        {
            NearbyTransferAssembler assembler(root);
            QVERIFY(
                assembler.consume(
                    requestFrame(
                        streamedRequest({item})))
                    .hasValue());
            QVERIFY(
                assembler.consume(
                    beginFrame(
                        transferId(),
                        item,
                        data,
                        1))
                    .hasValue());
            QVERIFY(QFileInfo::exists(partialRoot));
        }
        QVERIFY(!QFileInfo::exists(partialRoot));
    }
};

QTEST_GUILESS_MAIN(NearbyFrameCodecTests)

#include "NearbyFrameCodecTests.moc"
