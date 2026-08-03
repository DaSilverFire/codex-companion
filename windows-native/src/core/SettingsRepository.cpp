#include "core/SettingsRepository.h"

#include <cmath>
#include <QSettings>

#include <algorithm>

namespace {

using companion::AppSettings;
using companion::BackdropMode;
using companion::RelayMode;

constexpr double kMinimumAnimationSpeedScale = 0.75;
constexpr double kMaximumAnimationSpeedScale = 2.5;
constexpr double kDefaultAnimationSpeedScale = 1.15;
constexpr int kCurrentAnimationSpeedTimingVersion = 2;

QString backdropName(BackdropMode mode)
{
    switch (mode) {
    case BackdropMode::Mica:
        return QStringLiteral("mica");
    case BackdropMode::WindowsGlass:
        return QStringLiteral("windows-glass");
    case BackdropMode::SolidBlack:
        return QStringLiteral("solid-black");
    }
    return QStringLiteral("solid-black");
}

BackdropMode parseBackdrop(const QString& value)
{
    if (value == QStringLiteral("mica")) {
        return BackdropMode::Mica;
    }
    if (value == QStringLiteral("windows-glass")) {
        return BackdropMode::WindowsGlass;
    }
    return BackdropMode::SolidBlack;
}

QString relayModeName(RelayMode mode)
{
    switch (mode) {
    case RelayMode::Automatic:
        return QStringLiteral("automatic");
    case RelayMode::Disabled:
        return QStringLiteral("disabled");
    case RelayMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("automatic");
}

RelayMode parseRelayMode(const QString& value)
{
    if (value == QStringLiteral("disabled")) {
        return RelayMode::Disabled;
    }
    if (value == QStringLiteral("custom")) {
        return RelayMode::Custom;
    }
    return RelayMode::Automatic;
}

double clampAnimationSpeedScale(double value)
{
    return std::clamp(
        value, kMinimumAnimationSpeedScale, kMaximumAnimationSpeedScale);
}

double normalizeAnimationSpeedScale(double value, double fallback)
{
    if (std::isnan(value)) {
        return fallback;
    }
    if (std::isinf(value)) {
        return value < 0.0 ? kMinimumAnimationSpeedScale
                           : kMaximumAnimationSpeedScale;
    }
    return clampAnimationSpeedScale(value);
}

double parseAnimationSpeedScale(const QVariant& value, double fallback)
{
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    if (!ok) {
        return fallback;
    }
    return normalizeAnimationSpeedScale(parsed, fallback);
}

companion::CompanionError settingsError(
    QString code,
    QString message,
    const QString& path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {{QStringLiteral("path"), path}},
    };
}

} // namespace

namespace companion {

SettingsRepository::SettingsRepository(QString filePath)
    : filePath_(std::move(filePath))
{
}

Result<AppSettings> SettingsRepository::load() const
{
    QSettings settings(filePath_, QSettings::IniFormat);

    AppSettings loaded;
    loaded.backdrop = parseBackdrop(
        settings.value(QStringLiteral("appearance/backdrop"),
                       backdropName(loaded.backdrop))
            .toString());
    loaded.selectedPetId =
        settings.value(QStringLiteral("pet/selectedId"), loaded.selectedPetId)
            .toString();
    loaded.selectedChatModelId =
        settings
            .value(
                QStringLiteral(
                    "chat/selectedModelId"),
                loaded.selectedChatModelId)
            .toString();
    const bool needsAnimationTimingMigration =
        settings
            .value(
                QStringLiteral(
                    "pet/animationSpeedTimingVersion"),
                0)
            .toInt()
        < kCurrentAnimationSpeedTimingVersion;
    loaded.animationSpeedScale =
        needsAnimationTimingMigration
        ? kDefaultAnimationSpeedScale
        : parseAnimationSpeedScale(
              settings.value(
                  QStringLiteral(
                      "pet/animationSpeedScale"),
                  loaded.animationSpeedScale),
              loaded.animationSpeedScale);
    loaded.petVisible =
        settings.value(QStringLiteral("pet/visible"), loaded.petVisible)
            .toBool();
    if (settings.contains(QStringLiteral("pet/windowPosition"))) {
        const QVariant storedPosition =
            settings.value(QStringLiteral("pet/windowPosition"));
        if (storedPosition.canConvert<QPoint>()) {
            loaded.petWindowPosition =
                storedPosition.toPoint();
        }
    }
    loaded.hideControlsUntilHover =
        settings
            .value(QStringLiteral("pet/hideControlsUntilHover"),
                   loaded.hideControlsUntilHover)
            .toBool();
    loaded.allowAutonomousMovement =
        settings
            .value(QStringLiteral("pet/allowAutonomousMovement"),
                   loaded.allowAutonomousMovement)
            .toBool();
    loaded.mobileEnabled =
        settings.value(QStringLiteral("mobile/enabled"), loaded.mobileEnabled)
            .toBool();
    loaded.keepAvailableWhileDisplayOff =
        settings
            .value(QStringLiteral("mobile/keepAvailableWhileDisplayOff"),
                   loaded.keepAvailableWhileDisplayOff)
            .toBool();
    loaded.allowNearbyOnPublicNetworks =
        settings
            .value(QStringLiteral("mobile/allowNearbyOnPublicNetworks"),
                   loaded.allowNearbyOnPublicNetworks)
            .toBool();
    loaded.automaticallyContinuesAcrossCodexAccounts =
        settings
            .value(
                QStringLiteral(
                    "accounts/automaticallyContinuesAcrossCodexAccounts"),
                loaded
                    .automaticallyContinuesAcrossCodexAccounts)
            .toBool();
    loaded.relayMode = parseRelayMode(
        settings.value(QStringLiteral("mobile/relayMode"),
                       relayModeName(loaded.relayMode))
            .toString());
    loaded.customRelayUrl =
        settings
            .value(QStringLiteral("mobile/customRelayUrl"),
                   loaded.customRelayUrl)
            .toString();

    if (settings.status() != QSettings::NoError) {
        return Result<AppSettings>::failure(settingsError(
            QStringLiteral("settings.read-failed"),
            QStringLiteral("Failed to read settings."),
            filePath_));
    }

    if (needsAnimationTimingMigration) {
        settings.setValue(
            QStringLiteral("pet/animationSpeedScale"),
            kDefaultAnimationSpeedScale);
        settings.setValue(
            QStringLiteral(
                "pet/animationSpeedTimingVersion"),
            kCurrentAnimationSpeedTimingVersion);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            return Result<AppSettings>::failure(
                settingsError(
                    QStringLiteral(
                        "settings.write-failed"),
                    QStringLiteral(
                        "Failed to migrate animation settings."),
                    filePath_));
        }
    }

    return Result<AppSettings>::success(std::move(loaded));
}

Result<void> SettingsRepository::save(const AppSettings& settings) const
{
    QSettings persisted(filePath_, QSettings::IniFormat);

    persisted.setValue(QStringLiteral("appearance/backdrop"),
                       backdropName(settings.backdrop));
    persisted.setValue(QStringLiteral("pet/selectedId"), settings.selectedPetId);
    persisted.setValue(
        QStringLiteral("chat/selectedModelId"),
        settings.selectedChatModelId);
    persisted.setValue(QStringLiteral("pet/animationSpeedScale"),
                       normalizeAnimationSpeedScale(settings.animationSpeedScale,
                                                    AppSettings{}.animationSpeedScale));
    persisted.setValue(
        QStringLiteral("pet/animationSpeedTimingVersion"),
        kCurrentAnimationSpeedTimingVersion);
    persisted.setValue(QStringLiteral("pet/visible"),
                       settings.petVisible);
    if (settings.petWindowPosition.has_value()) {
        persisted.setValue(
            QStringLiteral("pet/windowPosition"),
            *settings.petWindowPosition);
    } else {
        persisted.remove(
            QStringLiteral("pet/windowPosition"));
    }
    persisted.setValue(QStringLiteral("pet/hideControlsUntilHover"),
                       settings.hideControlsUntilHover);
    persisted.setValue(QStringLiteral("pet/allowAutonomousMovement"),
                       settings.allowAutonomousMovement);
    persisted.setValue(QStringLiteral("mobile/enabled"), settings.mobileEnabled);
    persisted.setValue(QStringLiteral("mobile/keepAvailableWhileDisplayOff"),
                       settings.keepAvailableWhileDisplayOff);
    persisted.setValue(QStringLiteral("mobile/allowNearbyOnPublicNetworks"),
                       settings.allowNearbyOnPublicNetworks);
    persisted.setValue(
        QStringLiteral(
            "accounts/automaticallyContinuesAcrossCodexAccounts"),
        settings
            .automaticallyContinuesAcrossCodexAccounts);
    persisted.setValue(QStringLiteral("mobile/relayMode"),
                       relayModeName(settings.relayMode));
    persisted.setValue(QStringLiteral("mobile/customRelayUrl"),
                       settings.customRelayUrl);
    persisted.sync();

    if (persisted.status() != QSettings::NoError) {
        return Result<void>::failure(settingsError(
            QStringLiteral("settings.write-failed"),
            QStringLiteral("Failed to write settings."),
            filePath_));
    }

    return Result<void>::success();
}

Result<AppSettings> SettingsRepository::update(
    SettingsMutation mutation) const
{
    auto loaded = load();
    if (!loaded.hasValue()) {
        return Result<AppSettings>::failure(
            loaded.error());
    }

    AppSettings candidate = loaded.value();
    mutation(candidate);

    const auto saved = save(candidate);
    if (!saved.hasValue()) {
        return Result<AppSettings>::failure(
            saved.error());
    }

    return load();
}

const QString& SettingsRepository::filePath() const noexcept
{
    return filePath_;
}

} // namespace companion
