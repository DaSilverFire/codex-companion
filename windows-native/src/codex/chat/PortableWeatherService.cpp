#include "codex/chat/PortableWeatherService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace companion {
namespace {

CompanionError weatherError(
    QString code,
    QString message,
    bool retryable = false,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        std::move(context),
    };
}

CompanionError invalidRequest()
{
    return weatherError(
        QStringLiteral(
            "portable_tool.weather_invalid_request"),
        QStringLiteral(
            "The weather request could not be created."));
}

CompanionError invalidResponse()
{
    return weatherError(
        QStringLiteral(
            "portable_tool.weather_invalid_response"),
        QStringLiteral(
            "The weather service returned an unreadable response."));
}

Result<QJsonObject> responseObject(
    const PortableToolHttpResponse& response)
{
    if (response.statusCode < 200
        || response.statusCode >= 300) {
        return Result<QJsonObject>::failure(
            weatherError(
                QStringLiteral(
                    "portable_tool.weather_request_failed"),
                QStringLiteral(
                    "The weather service returned HTTP %1.")
                    .arg(response.statusCode),
                response.statusCode >= 500,
                {
                    {
                        QStringLiteral(
                            "statusCode"),
                        response.statusCode,
                    },
                }));
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            response.body,
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<QJsonObject>::failure(
            invalidResponse());
    }
    return Result<QJsonObject>::success(
        document.object());
}

std::optional<double> finiteNumber(
    const QJsonObject& object,
    QStringView key)
{
    const QJsonValue value =
        object.value(key.toString());
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number =
        value.toDouble();
    return std::isfinite(number)
        ? std::optional<double>(number)
        : std::nullopt;
}

std::optional<int> integer(
    const QJsonObject& object,
    QStringView key)
{
    const std::optional<double> value =
        finiteNumber(object, key);
    if (!value.has_value()
        || std::trunc(*value) != *value
        || *value
               < static_cast<double>(
                   std::numeric_limits<int>::min())
        || *value
               > static_cast<double>(
                   std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

std::optional<QString> nonEmptyString(
    const QJsonObject& object,
    QStringView key)
{
    const QJsonValue value =
        object.value(key.toString());
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text =
        value.toString().trimmed();
    return text.isEmpty()
        ? std::nullopt
        : std::optional<QString>(text);
}

std::optional<double> firstNumber(
    const QJsonObject& object,
    QStringView key)
{
    const QJsonValue value =
        object.value(key.toString());
    if (!value.isArray()) {
        return std::nullopt;
    }
    const QJsonArray values =
        value.toArray();
    if (values.isEmpty()
        || !values.at(0).isDouble()) {
        return std::nullopt;
    }
    const double number =
        values.at(0).toDouble();
    return std::isfinite(number)
        ? std::optional<double>(number)
        : std::nullopt;
}

std::optional<int> firstInteger(
    const QJsonObject& object,
    QStringView key)
{
    const std::optional<double> value =
        firstNumber(object, key);
    if (!value.has_value()
        || std::trunc(*value) != *value
        || *value
               < static_cast<double>(
                   std::numeric_limits<int>::min())
        || *value
               > static_cast<double>(
                   std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

QString conditionForCode(int code)
{
    switch (code) {
    case 0:
        return QStringLiteral("Clear sky");
    case 1:
        return QStringLiteral("Mainly clear");
    case 2:
        return QStringLiteral("Partly cloudy");
    case 3:
        return QStringLiteral("Overcast");
    case 45:
    case 48:
        return QStringLiteral("Fog");
    case 51:
    case 53:
    case 55:
        return QStringLiteral("Drizzle");
    case 56:
    case 57:
        return QStringLiteral(
            "Freezing drizzle");
    case 61:
    case 63:
    case 65:
        return QStringLiteral("Rain");
    case 66:
    case 67:
        return QStringLiteral(
            "Freezing rain");
    case 71:
    case 73:
    case 75:
        return QStringLiteral("Snow");
    case 77:
        return QStringLiteral("Snow grains");
    case 80:
    case 81:
    case 82:
        return QStringLiteral(
            "Rain showers");
    case 85:
    case 86:
        return QStringLiteral(
            "Snow showers");
    case 95:
        return QStringLiteral(
            "Thunderstorm");
    case 96:
    case 99:
        return QStringLiteral(
            "Thunderstorm with hail");
    default:
        return QStringLiteral(
            "Unknown conditions");
    }
}

QString formatWeatherValue(double value)
{
    return QString::number(
        value,
        'f',
        1);
}

QString temperatureQueryValue(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QStringLiteral("celsius")
        : QStringLiteral("fahrenheit");
}

QString windQueryValue(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QStringLiteral("kmh")
        : QStringLiteral("mph");
}

QString precipitationQueryValue(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QStringLiteral("mm")
        : QStringLiteral("inch");
}

QString temperatureSymbol(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QString(QChar(0x00b0))
              + QLatin1Char('C')
        : QString(QChar(0x00b0))
              + QLatin1Char('F');
}

QString windSymbol(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QStringLiteral("km/h")
        : QStringLiteral("mph");
}

QString precipitationSymbol(
    PortableWeatherUnitSystem units)
{
    return units
            == PortableWeatherUnitSystem::
                Metric
        ? QStringLiteral("mm")
        : QStringLiteral("in");
}

bool validHttpsEndpoint(const QUrl& endpoint)
{
    return endpoint.isValid()
        && endpoint.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive)
            == 0;
}

} // namespace

QString PortableWeatherLocation::displayName()
    const
{
    QStringList components;
    const auto append =
        [&components](
            const std::optional<QString>&
                value) {
            if (!value.has_value()) {
                return;
            }
            const QString trimmed =
                value->trimmed();
            if (trimmed.isEmpty()) {
                return;
            }
            if (!components.isEmpty()
                && components.back().compare(
                       trimmed,
                       Qt::CaseInsensitive)
                    == 0) {
                return;
            }
            components.append(trimmed);
        };
    append(std::optional<QString>(
        name));
    append(region);
    append(country);
    return components.join(
        QStringLiteral(", "));
}

QString PortableWeatherReport::toolSummary()
    const
{
    return QStringLiteral(
        "Live weather from Open-Meteo for %1:\n"
        "Conditions: %2\n"
        "Temperature: %3 %4 (feels like %5 %4)\n"
        "Humidity: %6%\n"
        "Current precipitation: %7 %8\n"
        "Wind: %9 %10\n"
        "Today: high %11 %4, low %12 %4, precipitation chance %13%\n"
        "Observation time: %14 (%15)")
        .arg(
            locationName,
            condition,
            formatWeatherValue(temperature),
            temperatureSymbol(units),
            formatWeatherValue(
                apparentTemperature))
        .arg(humidity)
        .arg(
            formatWeatherValue(
                precipitation),
            precipitationSymbol(units),
            formatWeatherValue(windSpeed),
            windSymbol(units),
            formatWeatherValue(dailyHigh),
            formatWeatherValue(dailyLow))
        .arg(precipitationProbability)
        .arg(observedAt, timeZone);
}

PortableWeatherService::
    PortableWeatherService(
        std::shared_ptr<
            PortableToolHttpTransport>
            transport,
        QUrl geocodingEndpoint,
        QUrl forecastEndpoint)
    : transport_(std::move(transport)),
      geocodingEndpoint_(
          std::move(geocodingEndpoint)),
      forecastEndpoint_(
          std::move(forecastEndpoint))
{
}

PortableWeatherUnitSystem
PortableWeatherService::
    preferredUnitSystem(
        const QLocale& locale)
{
    return locale.measurementSystem()
                == QLocale::MetricSystem
        ? PortableWeatherUnitSystem::Metric
        : PortableWeatherUnitSystem::
              Imperial;
}

Result<PortableWeatherReport>
PortableWeatherService::currentWeather(
    QStringView locationQuery,
    std::stop_token stopToken) const
{
    return currentWeather(
        locationQuery,
        preferredUnitSystem(),
        stopToken);
}

Result<PortableWeatherReport>
PortableWeatherService::currentWeather(
    QStringView locationQuery,
    PortableWeatherUnitSystem units,
    std::stop_token stopToken) const
{
    if (transport_ == nullptr) {
        return Result<
            PortableWeatherReport>::failure(
            weatherError(
                QStringLiteral(
                    "portable_tool.weather_unavailable"),
                QStringLiteral(
                    "The weather transport is unavailable.")));
    }
    const Result<PortableToolHttpRequest>
        geocodingRequest =
            makeGeocodingRequest(
                locationQuery);
    if (!geocodingRequest.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            geocodingRequest.error());
    }
    const Result<
        PortableToolHttpResponse>
        geocodingResponse =
            transport_->get(
                geocodingRequest.value(),
                stopToken);
    if (!geocodingResponse.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            geocodingResponse.error());
    }
    const Result<
        PortableWeatherLocation>
        location = decodeLocation(
            geocodingResponse.value(),
            locationQuery);
    if (!location.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            location.error());
    }
    const Result<PortableToolHttpRequest>
        forecastRequest =
            makeForecastRequest(
                location.value(),
                units);
    if (!forecastRequest.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            forecastRequest.error());
    }
    const Result<
        PortableToolHttpResponse>
        forecastResponse =
            transport_->get(
                forecastRequest.value(),
                stopToken);
    if (!forecastResponse.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            forecastResponse.error());
    }
    return decodeForecast(
        forecastResponse.value(),
        location.value(),
        units);
}

Result<PortableToolHttpRequest>
PortableWeatherService::
    makeGeocodingRequest(
        QStringView locationQuery) const
{
    const QString trimmed =
        locationQuery
            .toString()
            .trimmed();
    if (trimmed.isEmpty()) {
        return Result<
            PortableToolHttpRequest>::failure(
            weatherError(
                QStringLiteral(
                    "portable_tool.weather_empty_location"),
                QStringLiteral(
                    "Enter a city or place for the weather lookup.")));
    }
    if (!validHttpsEndpoint(
            geocodingEndpoint_)) {
        return Result<
            PortableToolHttpRequest>::failure(
            invalidRequest());
    }

    QUrl endpoint =
        geocodingEndpoint_;
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("name"),
        trimmed);
    query.addQueryItem(
        QStringLiteral("count"),
        QStringLiteral("1"));
    query.addQueryItem(
        QStringLiteral("language"),
        QStringLiteral("en"));
    query.addQueryItem(
        QStringLiteral("format"),
        QStringLiteral("json"));
    endpoint.setQuery(query);
    if (!endpoint.isValid()) {
        return Result<
            PortableToolHttpRequest>::failure(
            invalidRequest());
    }
    return Result<
        PortableToolHttpRequest>::success({
        endpoint,
        {
            {
                QByteArray("Accept"),
                QByteArray("application/json"),
            },
        },
        15000,
        256 * 1024,
    });
}

Result<PortableToolHttpRequest>
PortableWeatherService::
    makeForecastRequest(
        const PortableWeatherLocation&
            location,
        PortableWeatherUnitSystem units)
        const
{
    if (!validHttpsEndpoint(
            forecastEndpoint_)
        || !std::isfinite(
            location.latitude)
        || !std::isfinite(
            location.longitude)) {
        return Result<
            PortableToolHttpRequest>::failure(
            invalidRequest());
    }

    QUrl endpoint =
        forecastEndpoint_;
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("latitude"),
        QString::number(
            location.latitude,
            'g',
            15));
    query.addQueryItem(
        QStringLiteral("longitude"),
        QString::number(
            location.longitude,
            'g',
            15));
    query.addQueryItem(
        QStringLiteral("current"),
        QStringLiteral(
            "temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"));
    query.addQueryItem(
        QStringLiteral("daily"),
        QStringLiteral(
            "temperature_2m_max,temperature_2m_min,precipitation_probability_max"));
    query.addQueryItem(
        QStringLiteral(
            "temperature_unit"),
        temperatureQueryValue(units));
    query.addQueryItem(
        QStringLiteral(
            "wind_speed_unit"),
        windQueryValue(units));
    query.addQueryItem(
        QStringLiteral(
            "precipitation_unit"),
        precipitationQueryValue(units));
    query.addQueryItem(
        QStringLiteral("forecast_days"),
        QStringLiteral("2"));
    const QString timeZone =
        location.timeZone.has_value()
            && !location.timeZone
                    ->trimmed()
                    .isEmpty()
        ? location.timeZone->trimmed()
        : QStringLiteral("auto");
    query.addQueryItem(
        QStringLiteral("timezone"),
        timeZone);
    endpoint.setQuery(query);
    if (!endpoint.isValid()) {
        return Result<
            PortableToolHttpRequest>::failure(
            invalidRequest());
    }
    return Result<
        PortableToolHttpRequest>::success({
        endpoint,
        {
            {
                QByteArray("Accept"),
                QByteArray("application/json"),
            },
        },
        15000,
        512 * 1024,
    });
}

Result<PortableWeatherLocation>
PortableWeatherService::decodeLocation(
    const PortableToolHttpResponse&
        response,
    QStringView locationQuery)
{
    const Result<QJsonObject> decoded =
        responseObject(response);
    if (!decoded.hasValue()) {
        return Result<
            PortableWeatherLocation>::failure(
            decoded.error());
    }
    const QJsonValue resultsValue =
        decoded.value().value(
            QStringLiteral("results"));
    if (!resultsValue.isUndefined()
        && !resultsValue.isArray()) {
        return Result<
            PortableWeatherLocation>::failure(
            invalidResponse());
    }
    const QJsonArray results =
        resultsValue.toArray();
    if (results.isEmpty()) {
        return Result<
            PortableWeatherLocation>::failure(
            weatherError(
                QStringLiteral(
                    "portable_tool.weather_location_not_found"),
                QStringLiteral(
                    "No weather location was found for %1.")
                    .arg(
                        locationQuery
                            .toString()
                            .trimmed()),
                false,
                {
                    {
                        QStringLiteral("query"),
                        locationQuery
                            .toString()
                            .trimmed(),
                    },
                }));
    }
    if (!results.at(0).isObject()) {
        return Result<
            PortableWeatherLocation>::failure(
            invalidResponse());
    }
    const QJsonObject object =
        results.at(0).toObject();
    const auto name =
        nonEmptyString(
            object,
            QStringLiteral("name"));
    const auto latitude =
        finiteNumber(
            object,
            QStringLiteral("latitude"));
    const auto longitude =
        finiteNumber(
            object,
            QStringLiteral("longitude"));
    if (!name.has_value()
        || !latitude.has_value()
        || !longitude.has_value()) {
        return Result<
            PortableWeatherLocation>::failure(
            invalidResponse());
    }
    return Result<
        PortableWeatherLocation>::success({
        *name,
        nonEmptyString(
            object,
            QStringLiteral("admin1")),
        nonEmptyString(
            object,
            QStringLiteral("country")),
        *latitude,
        *longitude,
        nonEmptyString(
            object,
            QStringLiteral("timezone")),
    });
}

Result<PortableWeatherReport>
PortableWeatherService::decodeForecast(
    const PortableToolHttpResponse&
        response,
    const PortableWeatherLocation&
        location,
    PortableWeatherUnitSystem units)
{
    const Result<QJsonObject> decoded =
        responseObject(response);
    if (!decoded.hasValue()) {
        return Result<
            PortableWeatherReport>::failure(
            decoded.error());
    }
    const QJsonObject root =
        decoded.value();
    const auto timeZone =
        nonEmptyString(
            root,
            QStringLiteral("timezone"));
    const QJsonValue currentValue =
        root.value(
            QStringLiteral("current"));
    const QJsonValue dailyValue =
        root.value(
            QStringLiteral("daily"));
    if (!timeZone.has_value()
        || !currentValue.isObject()
        || !dailyValue.isObject()) {
        return Result<
            PortableWeatherReport>::failure(
            invalidResponse());
    }
    const QJsonObject current =
        currentValue.toObject();
    const QJsonObject daily =
        dailyValue.toObject();
    const auto observedAt =
        nonEmptyString(
            current,
            QStringLiteral("time"));
    const auto temperature =
        finiteNumber(
            current,
            QStringLiteral(
                "temperature_2m"));
    const auto apparentTemperature =
        finiteNumber(
            current,
            QStringLiteral(
                "apparent_temperature"));
    const auto humidity =
        integer(
            current,
            QStringLiteral(
                "relative_humidity_2m"));
    const auto precipitation =
        finiteNumber(
            current,
            QStringLiteral(
                "precipitation"));
    const auto weatherCode =
        integer(
            current,
            QStringLiteral(
                "weather_code"));
    const auto windSpeed =
        finiteNumber(
            current,
            QStringLiteral(
                "wind_speed_10m"));
    const auto dailyHigh =
        firstNumber(
            daily,
            QStringLiteral(
                "temperature_2m_max"));
    const auto dailyLow =
        firstNumber(
            daily,
            QStringLiteral(
                "temperature_2m_min"));
    const auto precipitationProbability =
        firstInteger(
            daily,
            QStringLiteral(
                "precipitation_probability_max"));
    if (!observedAt.has_value()
        || !temperature.has_value()
        || !apparentTemperature.has_value()
        || !humidity.has_value()
        || !precipitation.has_value()
        || !weatherCode.has_value()
        || !windSpeed.has_value()
        || !dailyHigh.has_value()
        || !dailyLow.has_value()
        || !precipitationProbability
                .has_value()) {
        return Result<
            PortableWeatherReport>::failure(
            invalidResponse());
    }
    return Result<
        PortableWeatherReport>::success({
        location.displayName(),
        conditionForCode(*weatherCode),
        *temperature,
        *apparentTemperature,
        *humidity,
        *precipitation,
        *windSpeed,
        *dailyHigh,
        *dailyLow,
        *precipitationProbability,
        *observedAt,
        *timeZone,
        units,
    });
}

} // namespace companion
