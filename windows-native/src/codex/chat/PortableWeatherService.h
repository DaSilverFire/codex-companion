#pragma once

#include "codex/chat/PortableToolHttpTransport.h"
#include "core/Result.h"

#include <QLocale>
#include <QString>
#include <QStringView>
#include <QUrl>

#include <memory>
#include <optional>
#include <stop_token>

namespace companion {

enum class PortableWeatherUnitSystem {
    Imperial,
    Metric,
};

struct PortableWeatherLocation final {
    QString name;
    std::optional<QString> region;
    std::optional<QString> country;
    double latitude = 0.0;
    double longitude = 0.0;
    std::optional<QString> timeZone;

    QString displayName() const;
};

struct PortableWeatherReport final {
    QString locationName;
    QString condition;
    double temperature = 0.0;
    double apparentTemperature = 0.0;
    int humidity = 0;
    double precipitation = 0.0;
    double windSpeed = 0.0;
    double dailyHigh = 0.0;
    double dailyLow = 0.0;
    int precipitationProbability = 0;
    QString observedAt;
    QString timeZone;
    PortableWeatherUnitSystem units =
        PortableWeatherUnitSystem::Imperial;

    QString toolSummary() const;
};

class PortableWeatherService final {
public:
    explicit PortableWeatherService(
        std::shared_ptr<
            PortableToolHttpTransport>
            transport =
                createDefaultPortableToolHttpTransport(),
        QUrl geocodingEndpoint =
            QUrl(QStringLiteral(
                "https://geocoding-api.open-meteo.com/v1/search")),
        QUrl forecastEndpoint =
            QUrl(QStringLiteral(
                "https://api.open-meteo.com/v1/forecast")));

    static PortableWeatherUnitSystem
    preferredUnitSystem(
        const QLocale& locale =
            QLocale::system());

    Result<PortableWeatherReport>
    currentWeather(
        QStringView locationQuery,
        std::stop_token stopToken = {})
        const;
    Result<PortableWeatherReport>
    currentWeather(
        QStringView locationQuery,
        PortableWeatherUnitSystem units,
        std::stop_token stopToken = {})
        const;

private:
    Result<PortableToolHttpRequest>
    makeGeocodingRequest(
        QStringView locationQuery) const;
    Result<PortableToolHttpRequest>
    makeForecastRequest(
        const PortableWeatherLocation&
            location,
        PortableWeatherUnitSystem units)
        const;
    static Result<
        PortableWeatherLocation>
    decodeLocation(
        const PortableToolHttpResponse&
            response,
        QStringView locationQuery);
    static Result<PortableWeatherReport>
    decodeForecast(
        const PortableToolHttpResponse&
            response,
        const PortableWeatherLocation&
            location,
        PortableWeatherUnitSystem units);

    std::shared_ptr<
        PortableToolHttpTransport>
        transport_;
    QUrl geocodingEndpoint_;
    QUrl forecastEndpoint_;
};

} // namespace companion
