#pragma once

#include "codex/models/BridgeModels.h"
#include "core/Result.h"
#include "mobile/nearby/NearbyFrameCodec.h"

#include <QString>

#include <memory>
#include <optional>

namespace companion {

enum class NearbyAssemblyDisposition {
    AwaitingFrames,
    Dispatch,
    Cancelled,
};

struct NearbyAssemblyResult final {
    NearbyAssemblyDisposition disposition =
        NearbyAssemblyDisposition::
            AwaitingFrames;
    std::optional<BridgeRequest> request;
    std::optional<QString> cancelCode;
};

class NearbyTransferAssembler final {
public:
    static QString defaultRootPath();

    explicit NearbyTransferAssembler(
        QString rootPath =
            defaultRootPath());
    ~NearbyTransferAssembler();

    NearbyTransferAssembler(
        const NearbyTransferAssembler&) =
        delete;
    NearbyTransferAssembler& operator=(
        const NearbyTransferAssembler&) =
        delete;

    Result<NearbyAssemblyResult> consume(
        const NearbyFrame& frame);
    void cancelAll() noexcept;
    qsizetype activeTransferCount() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation>
        implementation_;
};

} // namespace companion
