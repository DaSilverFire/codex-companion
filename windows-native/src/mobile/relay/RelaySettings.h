#pragma once

#include "core/AppSettings.h"
#include "core/Result.h"

#include <QString>
#include <QStringView>
#include <QUrl>

#include <optional>

namespace companion {

class RelaySettings final {
public:
    explicit RelaySettings(QString bundledUrl);

    static RelaySettings
    fromBundledConfiguration();
    static QString configurationKey();

    Result<std::optional<QUrl>> configuredUrl(
        const AppSettings& settings) const;
    Result<AppSettings> withCustomUrl(
        const AppSettings& settings,
        QStringView value) const;
    AppSettings useAutomatic(
        const AppSettings& settings) const;

    static Result<QUrl> validatedUrl(
        QStringView value);
    static Result<QUrl> withChannel(
        QUrl url,
        QStringView channelId);

    const QString& bundledUrl() const noexcept;

private:
    QString bundledUrl_;
};

} // namespace companion
