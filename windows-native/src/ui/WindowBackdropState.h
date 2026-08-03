#pragma once

#include "core/AppSettings.h"
#include "platform/windows/WindowCoordinator.h"

#include <QObject>
#include <QString>

namespace companion {

class WindowBackdropState final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString settingsEffectiveMode READ settingsEffectiveMode NOTIFY effectiveBackdropModesChanged)
    Q_PROPERTY(QString companionMenuEffectiveMode READ companionMenuEffectiveMode NOTIFY effectiveBackdropModesChanged)
    Q_PROPERTY(QString modelPickerEffectiveMode READ modelPickerEffectiveMode NOTIFY effectiveBackdropModesChanged)
    Q_PROPERTY(QString goalEffectiveMode READ goalEffectiveMode NOTIFY effectiveBackdropModesChanged)
    Q_PROPERTY(QString usageEffectiveMode READ usageEffectiveMode NOTIFY effectiveBackdropModesChanged)
    Q_PROPERTY(QString attentionEffectiveMode READ attentionEffectiveMode NOTIFY effectiveBackdropModesChanged)

public:
    explicit WindowBackdropState(QObject* parent = nullptr);

    QString settingsEffectiveMode() const;
    QString companionMenuEffectiveMode() const;
    QString modelPickerEffectiveMode() const;
    QString goalEffectiveMode() const;
    QString usageEffectiveMode() const;
    QString attentionEffectiveMode() const;

    void setEffectiveMode(WindowRole role, BackdropMode mode);

signals:
    void effectiveBackdropModesChanged();

private:
    BackdropMode settingsMode_ = BackdropMode::Mica;
    BackdropMode companionMenuMode_ = BackdropMode::Mica;
    BackdropMode modelPickerMode_ = BackdropMode::Mica;
    BackdropMode goalMode_ = BackdropMode::Mica;
    BackdropMode usageMode_ = BackdropMode::Mica;
    BackdropMode attentionMode_ = BackdropMode::Mica;
};

} // namespace companion
