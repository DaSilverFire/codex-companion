#pragma once

#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QFuture>
#include <QString>

namespace companion {

class NearbyTransport {
public:
    virtual ~NearbyTransport() = default;

    virtual Result<void> start() = 0;
    virtual QFuture<void> stop() = 0;
    virtual bool hasAuthenticatedDevice(
        const QString& deviceId) const = 0;
    virtual QFuture<Result<void>> send(
        const QString& deviceId,
        const BridgeResponse& response) = 0;
};

} // namespace companion
