#include "mobile/relay/RelaySettings.h"

#include <QUrlQuery>

#include <windows.h>

#include <vector>

namespace companion {
namespace {

constexpr auto kConfigurationKey =
    "CompanionRelayURL";

CompanionError invalidUrl(QString message)
{
    return {
        QStringLiteral("relay.invalid_url"),
        std::move(message),
        false,
        {},
    };
}

QString versionStringValue(
    QStringView key)
{
    std::vector<wchar_t> executablePath(
        32'768);
    const DWORD pathLength =
        GetModuleFileNameW(
            nullptr,
            executablePath.data(),
            DWORD(executablePath.size()));
    if (pathLength == 0
        || pathLength
            >= executablePath.size()) {
        return {};
    }

    DWORD ignoredHandle = 0;
    const DWORD versionBytes =
        GetFileVersionInfoSizeW(
            executablePath.data(),
            &ignoredHandle);
    if (versionBytes == 0) {
        return {};
    }

    std::vector<BYTE> versionInfo(
        versionBytes);
    if (!GetFileVersionInfoW(
            executablePath.data(),
            0,
            versionBytes,
            versionInfo.data())) {
        return {};
    }

    struct Translation final {
        WORD language;
        WORD codePage;
    };

    void* rawTranslations = nullptr;
    UINT translationBytes = 0;
    QVector<Translation> translations;
    if (VerQueryValueW(
            versionInfo.data(),
            L"\\VarFileInfo\\Translation",
            &rawTranslations,
            &translationBytes)
        && rawTranslations != nullptr
        && translationBytes
            >= sizeof(Translation)) {
        const auto* values =
            static_cast<const Translation*>(
                rawTranslations);
        const UINT count =
            translationBytes
            / sizeof(Translation);
        translations.reserve(int(count));
        for (UINT index = 0;
             index < count;
             ++index) {
            translations.append(
                values[index]);
        }
    }
    if (translations.isEmpty()) {
        translations.append(
            {0x0409, 1200});
    }

    for (const Translation translation :
         translations) {
        const QString query =
            QStringLiteral(
                "\\StringFileInfo\\%1%2\\%3")
                .arg(
                    translation.language,
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(
                    translation.codePage,
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(key);
        const std::wstring nativeQuery =
            query.toStdWString();
        void* rawValue = nullptr;
        UINT valueCharacters = 0;
        if (!VerQueryValueW(
                versionInfo.data(),
                nativeQuery.c_str(),
                &rawValue,
                &valueCharacters)
            || rawValue == nullptr
            || valueCharacters == 0) {
            continue;
        }
        return QString::fromWCharArray(
            static_cast<const wchar_t*>(
                rawValue),
            int(valueCharacters - 1));
    }
    return {};
}

} // namespace

RelaySettings::RelaySettings(
    QString bundledUrl)
    : bundledUrl_(
          std::move(bundledUrl))
{
}

RelaySettings
RelaySettings::fromBundledConfiguration()
{
    return RelaySettings(
        versionStringValue(
            configurationKey()));
}

QString RelaySettings::configurationKey()
{
    return QString::fromLatin1(
        kConfigurationKey);
}

Result<std::optional<QUrl>>
RelaySettings::configuredUrl(
    const AppSettings& settings) const
{
    if (settings.relayMode
        == RelayMode::Disabled) {
        return Result<std::optional<QUrl>>
            ::success(std::nullopt);
    }

    const QString source =
        settings.relayMode
                == RelayMode::Custom
            ? settings.customRelayUrl
            : bundledUrl_;
    const auto validated =
        validatedUrl(source);
    if (!validated.hasValue()) {
        return Result<std::optional<QUrl>>
            ::failure(validated.error());
    }
    return Result<std::optional<QUrl>>
        ::success(validated.value());
}

Result<AppSettings>
RelaySettings::withCustomUrl(
    const AppSettings& settings,
    QStringView value) const
{
    const auto validated =
        validatedUrl(value);
    if (!validated.hasValue()) {
        return Result<AppSettings>::failure(
            validated.error());
    }

    AppSettings candidate = settings;
    candidate.relayMode =
        RelayMode::Custom;
    candidate.customRelayUrl =
        validated.value().toString(
            QUrl::FullyEncoded);
    return Result<AppSettings>::success(
        std::move(candidate));
}

AppSettings RelaySettings::useAutomatic(
    const AppSettings& settings) const
{
    AppSettings candidate = settings;
    candidate.relayMode =
        RelayMode::Automatic;
    candidate.customRelayUrl.clear();
    return candidate;
}

Result<QUrl> RelaySettings::validatedUrl(
    QStringView value)
{
    const QString trimmed =
        value.trimmed().toString();
    const QUrl url(
        trimmed,
        QUrl::StrictMode);
    const QString scheme =
        url.scheme().toLower();
    const QString host =
        url.host().toLower();
    if (trimmed.isEmpty()
        || !url.isValid()
        || host.isEmpty()) {
        return Result<QUrl>::failure(
            invalidUrl(
                QStringLiteral(
                    "Relay URL is missing or malformed.")));
    }
    if (scheme == QStringLiteral("wss")) {
        QUrl normalized = url;
        normalized.setScheme(scheme);
        return Result<QUrl>::success(
            std::move(normalized));
    }
    const bool localWebSocket =
        scheme == QStringLiteral("ws")
        && (host == QStringLiteral("localhost")
            || host == QStringLiteral("127.0.0.1")
            || host == QStringLiteral("::1"));
    if (!localWebSocket) {
        return Result<QUrl>::failure(
            invalidUrl(
                QStringLiteral(
                    "Relay URL must use wss://, except for a localhost ws:// endpoint.")));
    }
    QUrl normalized = url;
    normalized.setScheme(scheme);
    return Result<QUrl>::success(
        std::move(normalized));
}

Result<QUrl> RelaySettings::withChannel(
    QUrl url,
    QStringView channelId)
{
    const QString channel =
        channelId.toString();
    if (channel.isEmpty()) {
        return Result<QUrl>::failure(
            invalidUrl(
                QStringLiteral(
                    "Relay channel identifier must not be empty.")));
    }
    const auto validated =
        validatedUrl(
            url.toString(
                QUrl::FullyEncoded));
    if (!validated.hasValue()) {
        return validated;
    }
    url = validated.value();

    QUrlQuery query(url);
    query.removeAllQueryItems(
        QStringLiteral("channel"));
    query.addQueryItem(
        QStringLiteral("channel"),
        channel);
    url.setQuery(query);
    return Result<QUrl>::success(
        std::move(url));
}

const QString&
RelaySettings::bundledUrl() const noexcept
{
    return bundledUrl_;
}

} // namespace companion
