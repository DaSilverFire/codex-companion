#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace companion {

inline constexpr qsizetype kFollowerMaximumParsedFrameBytes =
    4 * 1024 * 1024;
inline constexpr quint64 kFollowerMaximumWireFrameBytes =
    256ULL * 1024ULL * 1024ULL;

class FollowerFrameCodec final {
public:
    static Result<QByteArray> encode(
        const QJsonObject& message);

    Result<QVector<QJsonObject>> append(
        QByteArrayView bytes);

private:
    void reset();

    QByteArray header_;
    QByteArray payload_;
    std::optional<quint32> payloadLength_;
    quint64 drainRemaining_ = 0;
};

} // namespace companion
