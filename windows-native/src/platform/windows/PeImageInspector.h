#pragma once

#include "core/Result.h"

#include <QStringView>

namespace companion {

enum class PeMachine {
    Unknown,
    X86,
    X64,
    Arm64,
};

class PeImageInspector final {
public:
    Result<PeMachine> machine(
        QStringView path) const;
};

} // namespace companion
