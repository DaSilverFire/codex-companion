#pragma once

#include "core/Result.h"
#include "mobile/relay/RelayModels.h"
#include "mobile/security/SecurityModels.h"

#include <QByteArray>
#include <QByteArrayView>

namespace companion {

class RelayWireCodec final {
public:
    static Result<QByteArray> encode(
        const RelayWireMessage& message);
    static Result<RelayWireMessage> decode(
        QByteArrayView bytes);

    static Result<QByteArray> encodeEnvelope(
        const EncryptedEnvelope& envelope);
    static Result<EncryptedEnvelope> decodeEnvelope(
        QByteArrayView bytes);
};

} // namespace companion
