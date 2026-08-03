#pragma once

#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QByteArrayView>

namespace companion {

class BridgeJsonCodec final {
public:
    static Result<BridgeRequest> decodeRequest(
        QByteArrayView bytes,
        BridgeWireProfile profile);
    static Result<QByteArray> encodeRequest(
        const BridgeRequest& request,
        BridgeWireProfile profile);
    static Result<BridgeResponse> decodeResponse(
        QByteArrayView bytes,
        BridgeWireProfile profile);
    static Result<QByteArray> encodeResponse(
        const BridgeResponse& response,
        BridgeWireProfile profile);
};

} // namespace companion
