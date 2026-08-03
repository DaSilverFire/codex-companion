#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QUuid>
#include <QtTypes>

namespace companion {

enum class NearbyFrameType : quint8 {
    Request = 1,
    AttachmentBegin = 2,
    AttachmentChunk = 3,
    AttachmentCommit = 4,
    TransferCancel = 5,
};

struct NearbyFrame final {
    NearbyFrameType type =
        NearbyFrameType::Request;
    quint16 flags = 0;
    QUuid transferId;
    QUuid attachmentId;
    quint32 chunkIndex = 0;
    quint32 chunkCount = 0;
    QByteArray payload;

    friend bool operator==(
        const NearbyFrame&,
        const NearbyFrame&) = default;
};

class NearbyFrameCodec final {
public:
    static constexpr qsizetype headerBytes = 88;
    static constexpr qsizetype maximumRequestBytes =
        4 * 1024 * 1024;
    static constexpr qsizetype maximumChunkBytes =
        262144;
    static constexpr qsizetype maximumCancelBytes =
        128;

    static Result<QByteArray> encode(
        const NearbyFrame& frame,
        QByteArrayView secret);
    static Result<NearbyFrame> decode(
        QByteArrayView bytes,
        QByteArrayView secret);
};

} // namespace companion
