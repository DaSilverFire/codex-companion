#include "mobile/MobileTypes.h"

namespace companion {

static_assert(
    MobileTransportRoute::Nearby
    != MobileTransportRoute::Unavailable);
static_assert(
    MobileConnectionState::Starting
    != MobileConnectionState::Stopped);

} // namespace companion
