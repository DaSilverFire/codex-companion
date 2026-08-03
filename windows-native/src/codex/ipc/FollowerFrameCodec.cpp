#include "codex/ipc/FollowerFrameCodec.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>

#include <limits>
#include <utility>

namespace companion {

namespace {

CompanionError frameError(
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

} // namespace

Result<QByteArray> FollowerFrameCodec::encode(
    const QJsonObject& message)
{
    const QByteArray payload =
        QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (static_cast<quint64>(payload.size())
        > std::numeric_limits<quint32>::max()) {
        return Result<QByteArray>::failure(frameError(
            QStringLiteral("follower.frame_invalid"),
            QStringLiteral(
                "The Codex follower request is too large.")));
    }

    QByteArray frame(sizeof(quint32), Qt::Uninitialized);
    qToLittleEndian<quint32>(
        static_cast<quint32>(payload.size()),
        reinterpret_cast<uchar*>(frame.data()));
    frame.append(payload);
    return Result<QByteArray>::success(std::move(frame));
}

Result<QVector<QJsonObject>> FollowerFrameCodec::append(
    QByteArrayView bytes)
{
    QVector<QJsonObject> completed;
    qsizetype offset = 0;

    while (offset < bytes.size()) {
        if (drainRemaining_ > 0) {
            const quint64 available =
                static_cast<quint64>(bytes.size() - offset);
            const quint64 consumed =
                qMin(drainRemaining_, available);
            drainRemaining_ -= consumed;
            offset += static_cast<qsizetype>(consumed);
            continue;
        }

        if (!payloadLength_.has_value()) {
            const qsizetype needed =
                static_cast<qsizetype>(sizeof(quint32))
                - header_.size();
            const qsizetype consumed =
                qMin(needed, bytes.size() - offset);
            header_.append(bytes.sliced(offset, consumed));
            offset += consumed;
            if (header_.size()
                < static_cast<qsizetype>(sizeof(quint32))) {
                break;
            }

            const quint32 declared =
                qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar*>(
                        header_.constData()));
            header_.clear();
            if (declared > kFollowerMaximumWireFrameBytes) {
                reset();
                return Result<QVector<QJsonObject>>::failure(
                    frameError(
                        QStringLiteral(
                            "follower.frame_too_large"),
                        QStringLiteral(
                            "The Codex follower frame exceeded the wire size limit."),
                        {
                            {
                                QStringLiteral("declaredBytes"),
                                QVariant::fromValue<qulonglong>(
                                    declared),
                            },
                        }));
            }
            if (declared
                > static_cast<quint32>(
                    kFollowerMaximumParsedFrameBytes)) {
                drainRemaining_ = declared;
                continue;
            }
            payloadLength_ = declared;
            payload_.clear();
        }

        const quint32 declared = *payloadLength_;
        const qsizetype needed =
            static_cast<qsizetype>(declared)
            - payload_.size();
        const qsizetype consumed =
            qMin(needed, bytes.size() - offset);
        payload_.append(bytes.sliced(offset, consumed));
        offset += consumed;
        if (payload_.size()
            < static_cast<qsizetype>(declared)) {
            break;
        }

        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(payload_, &parseError);
        if (parseError.error == QJsonParseError::NoError
            && document.isObject()) {
            completed.push_back(document.object());
        }
        payload_.clear();
        payloadLength_.reset();
    }

    return Result<QVector<QJsonObject>>::success(
        std::move(completed));
}

void FollowerFrameCodec::reset()
{
    header_.clear();
    payload_.clear();
    payloadLength_.reset();
    drainRemaining_ = 0;
}

} // namespace companion
