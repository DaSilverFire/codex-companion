#pragma once

#include <QPoint>
#include <QString>
#include <optional>

namespace companion {

enum class BackdropMode {
    Mica,
    WindowsGlass,
    SolidBlack,
};

enum class RelayMode {
    Automatic,
    Disabled,
    Custom,
};

struct AppSettings final {
    BackdropMode backdrop = BackdropMode::Mica;
    QString selectedPetId;
    QString selectedChatModelId =
        QStringLiteral("on-device");
    double animationSpeedScale = 1.15;
    bool petVisible = true;
    std::optional<QPoint> petWindowPosition;
    bool hideControlsUntilHover = false;
    bool allowAutonomousMovement = true;
    bool mobileEnabled = true;
    bool keepAvailableWhileDisplayOff = true;
    bool allowNearbyOnPublicNetworks = false;
    bool automaticallyContinuesAcrossCodexAccounts = false;
    RelayMode relayMode = RelayMode::Automatic;
    QString customRelayUrl;

    friend bool operator==(const AppSettings&, const AppSettings&) = default;
};

} // namespace companion
