#pragma once

#include "core/AppSettings.h"
#include "core/Result.h"

#include <QString>
#include <functional>

namespace companion {

class SettingsRepository final {
public:
    using SettingsMutation =
        std::function<void(AppSettings&)>;

    explicit SettingsRepository(QString filePath);

    Result<AppSettings> load() const;
    Result<void> save(const AppSettings& settings) const;
    Result<AppSettings> update(
        SettingsMutation mutation) const;
    const QString& filePath() const noexcept;

private:
    QString filePath_;
};

} // namespace companion
