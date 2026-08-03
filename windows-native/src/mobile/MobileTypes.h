#pragma once

namespace companion {

enum class MobileTransportRoute {
    Nearby,
    Relay,
    Unavailable,
};

enum class MobileConnectionState {
    Stopped,
    Starting,
    Pairing,
    NearbyConnected,
    RelayConnecting,
    RelayConnected,
    Unavailable,
    Failed,
};

} // namespace companion
