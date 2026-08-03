#pragma once

#include "codex/runtime/CodexRuntime.h"
#include "core/Result.h"
#include "mobile/MobileRequestDispatcher.h"

namespace companion {

struct CompanionMobileRuntimeBindings final {
    MobileRequestReadDependencies reads;
    MobileRequestMutationDependencies
        mutations;
};

class CompanionMobileRuntimeAdapter final {
public:
    static Result<
        CompanionMobileRuntimeBindings>
    create(
        CodexRuntimeDependencies
            dependencies);
};

} // namespace companion
