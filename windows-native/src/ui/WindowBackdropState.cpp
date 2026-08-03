#include "ui/WindowBackdropState.h"

namespace {

QString backdropModeName(companion::BackdropMode mode)
{
    switch (mode) {
    case companion::BackdropMode::Mica:
        return QStringLiteral("mica");
    case companion::BackdropMode::WindowsGlass:
        return QStringLiteral("windows-glass");
    case companion::BackdropMode::SolidBlack:
        return QStringLiteral("solid-black");
    }
    return QStringLiteral("solid-black");
}

} // namespace

namespace companion {

WindowBackdropState::WindowBackdropState(QObject* parent)
    : QObject(parent)
{
}

QString WindowBackdropState::settingsEffectiveMode() const
{
    return backdropModeName(settingsMode_);
}

QString WindowBackdropState::companionMenuEffectiveMode() const
{
    return backdropModeName(companionMenuMode_);
}

QString WindowBackdropState::modelPickerEffectiveMode() const
{
    return backdropModeName(modelPickerMode_);
}

QString WindowBackdropState::goalEffectiveMode() const
{
    return backdropModeName(goalMode_);
}

QString WindowBackdropState::usageEffectiveMode() const
{
    return backdropModeName(usageMode_);
}

QString WindowBackdropState::attentionEffectiveMode() const
{
    return backdropModeName(attentionMode_);
}

void WindowBackdropState::setEffectiveMode(WindowRole role, BackdropMode mode)
{
    BackdropMode* destination = nullptr;
    switch (role) {
    case WindowRole::Settings:
        destination = &settingsMode_;
        break;
    case WindowRole::CompanionMenu:
        destination = &companionMenuMode_;
        break;
    case WindowRole::ModelPicker:
        destination = &modelPickerMode_;
        break;
    case WindowRole::Goal:
        destination = &goalMode_;
        break;
    case WindowRole::Usage:
        destination = &usageMode_;
        break;
    case WindowRole::Attention:
        destination = &attentionMode_;
        break;
    case WindowRole::Pet:
        return;
    }

    if (*destination == mode) {
        return;
    }
    *destination = mode;
    emit effectiveBackdropModesChanged();
}

} // namespace companion
