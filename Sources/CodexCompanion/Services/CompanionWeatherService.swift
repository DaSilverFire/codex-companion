import Foundation

enum CompanionWeatherUnitSystem: Equatable, Sendable {
    case imperial
    case metric

    static var preferred: CompanionWeatherUnitSystem {
        Locale.current.measurementSystem == .metric ? .metric : .imperial
    }

    var temperatureQueryValue: String {
        switch self {
        case .imperial: return "fahrenheit"
        case .metric: return "celsius"
        }
    }

    var windSpeedQueryValue: String {
        switch self {
        case .imperial: return "mph"
        case .metric: return "kmh"
        }
    }

    var precipitationQueryValue: String {
        switch self {
        case .imperial: return "inch"
        case .metric: return "mm"
        }
    }

    var temperatureSymbol: String {
        switch self {
        case .imperial: return "°F"
        case .metric: return "°C"
        }
    }

    var windSpeedSymbol: String {
        switch self {
        case .imperial: return "mph"
        case .metric: return "km/h"
        }
    }

    var precipitationSymbol: String {
        switch self {
        case .imperial: return "in"
        case .metric: return "mm"
        }
    }
}

struct CompanionWeatherLocation: Equatable, Sendable {
    let name: String
    let region: String?
    let country: String?
    let latitude: Double
    let longitude: Double
    let timeZone: String?

    var displayName: String {
        let components: [String?] = [name, region, country]
        return components
            .compactMap { value -> String? in
                guard let value else { return nil }
                let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed.isEmpty ? nil : trimmed
            }
            .reduce(into: [String]()) { result, component in
                if result.last?.localizedCaseInsensitiveCompare(component) != .orderedSame {
                    result.append(component)
                }
            }
            .joined(separator: ", ")
    }
}

struct CompanionWeatherReport: Equatable, Sendable {
    let locationName: String
    let condition: String
    let temperature: Double
    let apparentTemperature: Double
    let humidity: Int
    let precipitation: Double
    let windSpeed: Double
    let dailyHigh: Double
    let dailyLow: Double
    let precipitationProbability: Int
    let observedAt: String
    let timeZone: String
    let units: CompanionWeatherUnitSystem

    var toolSummary: String {
        """
        Live weather from Open-Meteo for \(locationName):
        Conditions: \(condition)
        Temperature: \(format(temperature)) \(units.temperatureSymbol) (feels like \(format(apparentTemperature)) \(units.temperatureSymbol))
        Humidity: \(humidity)%
        Current precipitation: \(format(precipitation)) \(units.precipitationSymbol)
        Wind: \(format(windSpeed)) \(units.windSpeedSymbol)
        Today: high \(format(dailyHigh)) \(units.temperatureSymbol), low \(format(dailyLow)) \(units.temperatureSymbol), precipitation chance \(precipitationProbability)%
        Observation time: \(observedAt) (\(timeZone))
        """
    }

    private func format(_ value: Double) -> String {
        String(format: "%.1f", locale: Locale(identifier: "en_US_POSIX"), value)
    }
}

enum CompanionWeatherError: LocalizedError, Equatable {
    case emptyLocation
    case locationNotFound(String)
    case invalidRequest
    case invalidResponse
    case requestFailed(statusCode: Int)

    var errorDescription: String? {
        switch self {
        case .emptyLocation:
            return "Enter a city or place for the weather lookup."
        case .locationNotFound(let location):
            return "No weather location was found for \(location)."
        case .invalidRequest:
            return "The weather request could not be created."
        case .invalidResponse:
            return "The weather service returned an unreadable response."
        case .requestFailed(let statusCode):
            return "The weather service returned HTTP \(statusCode)."
        }
    }
}

struct CompanionWeatherService: @unchecked Sendable {
    private static let defaultGeocodingEndpoint = URL(string: "https://geocoding-api.open-meteo.com/v1/search")!
    private static let defaultForecastEndpoint = URL(string: "https://api.open-meteo.com/v1/forecast")!

    private let session: URLSession
    private let geocodingEndpoint: URL
    private let forecastEndpoint: URL

    init(
        session: URLSession = .shared,
        geocodingEndpoint: URL = CompanionWeatherService.defaultGeocodingEndpoint,
        forecastEndpoint: URL = CompanionWeatherService.defaultForecastEndpoint
    ) {
        self.session = session
        self.geocodingEndpoint = geocodingEndpoint
        self.forecastEndpoint = forecastEndpoint
    }

    func currentWeather(
        for locationQuery: String,
        units: CompanionWeatherUnitSystem = .preferred
    ) async throws -> CompanionWeatherReport {
        let geocodingRequest = try makeGeocodingRequest(location: locationQuery)
        let (geocodingData, geocodingResponse) = try await session.data(for: geocodingRequest)
        let location = try decodeLocation(
            data: geocodingData,
            response: geocodingResponse,
            query: locationQuery
        )

        return try await currentWeather(at: location, units: units)
    }

    func currentWeather(
        at location: CompanionWeatherLocation,
        units: CompanionWeatherUnitSystem = .preferred
    ) async throws -> CompanionWeatherReport {
        let forecastRequest = try makeForecastRequest(location: location, units: units)
        let (forecastData, forecastResponse) = try await session.data(for: forecastRequest)
        return try decodeForecast(
            data: forecastData,
            response: forecastResponse,
            location: location,
            units: units
        )
    }

    func makeGeocodingRequest(location: String) throws -> URLRequest {
        let trimmed = location.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw CompanionWeatherError.emptyLocation }
        guard var components = URLComponents(url: geocodingEndpoint, resolvingAgainstBaseURL: false) else {
            throw CompanionWeatherError.invalidRequest
        }
        components.queryItems = [
            URLQueryItem(name: "name", value: trimmed),
            URLQueryItem(name: "count", value: "1"),
            URLQueryItem(name: "language", value: "en"),
            URLQueryItem(name: "format", value: "json"),
        ]
        guard let url = components.url else { throw CompanionWeatherError.invalidRequest }
        var request = URLRequest(url: url)
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        return request
    }

    func makeForecastRequest(
        location: CompanionWeatherLocation,
        units: CompanionWeatherUnitSystem
    ) throws -> URLRequest {
        guard var components = URLComponents(url: forecastEndpoint, resolvingAgainstBaseURL: false) else {
            throw CompanionWeatherError.invalidRequest
        }
        components.queryItems = [
            URLQueryItem(name: "latitude", value: String(location.latitude)),
            URLQueryItem(name: "longitude", value: String(location.longitude)),
            URLQueryItem(
                name: "current",
                value: "temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m"
            ),
            URLQueryItem(
                name: "daily",
                value: "temperature_2m_max,temperature_2m_min,precipitation_probability_max"
            ),
            URLQueryItem(name: "temperature_unit", value: units.temperatureQueryValue),
            URLQueryItem(name: "wind_speed_unit", value: units.windSpeedQueryValue),
            URLQueryItem(name: "precipitation_unit", value: units.precipitationQueryValue),
            URLQueryItem(name: "forecast_days", value: "2"),
            URLQueryItem(name: "timezone", value: location.timeZone ?? "auto"),
        ]
        guard let url = components.url else { throw CompanionWeatherError.invalidRequest }
        var request = URLRequest(url: url)
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        return request
    }

    func decodeLocation(
        data: Data,
        response: URLResponse,
        query: String
    ) throws -> CompanionWeatherLocation {
        try validate(response)
        guard let payload = try? JSONDecoder().decode(GeocodingResponse.self, from: data) else {
            throw CompanionWeatherError.invalidResponse
        }
        guard let result = payload.results?.first else {
            throw CompanionWeatherError.locationNotFound(query)
        }
        return CompanionWeatherLocation(
            name: result.name,
            region: result.admin1,
            country: result.country,
            latitude: result.latitude,
            longitude: result.longitude,
            timeZone: result.timezone
        )
    }

    func decodeForecast(
        data: Data,
        response: URLResponse,
        location: CompanionWeatherLocation,
        units: CompanionWeatherUnitSystem
    ) throws -> CompanionWeatherReport {
        try validate(response)
        guard
            let payload = try? JSONDecoder().decode(ForecastResponse.self, from: data),
            let high = payload.daily.temperature2mMax.first,
            let low = payload.daily.temperature2mMin.first,
            let precipitationProbability = payload.daily.precipitationProbabilityMax.first
        else {
            throw CompanionWeatherError.invalidResponse
        }
        return CompanionWeatherReport(
            locationName: location.displayName,
            condition: Self.condition(for: payload.current.weatherCode),
            temperature: payload.current.temperature2m,
            apparentTemperature: payload.current.apparentTemperature,
            humidity: payload.current.relativeHumidity2m,
            precipitation: payload.current.precipitation,
            windSpeed: payload.current.windSpeed10m,
            dailyHigh: high,
            dailyLow: low,
            precipitationProbability: precipitationProbability,
            observedAt: payload.current.time,
            timeZone: payload.timezone,
            units: units
        )
    }

    private func validate(_ response: URLResponse) throws {
        guard let response = response as? HTTPURLResponse else {
            throw CompanionWeatherError.invalidResponse
        }
        guard (200..<300).contains(response.statusCode) else {
            throw CompanionWeatherError.requestFailed(statusCode: response.statusCode)
        }
    }

    private static func condition(for code: Int) -> String {
        switch code {
        case 0: return "Clear sky"
        case 1: return "Mainly clear"
        case 2: return "Partly cloudy"
        case 3: return "Overcast"
        case 45, 48: return "Fog"
        case 51, 53, 55: return "Drizzle"
        case 56, 57: return "Freezing drizzle"
        case 61, 63, 65: return "Rain"
        case 66, 67: return "Freezing rain"
        case 71, 73, 75: return "Snow"
        case 77: return "Snow grains"
        case 80, 81, 82: return "Rain showers"
        case 85, 86: return "Snow showers"
        case 95: return "Thunderstorm"
        case 96, 99: return "Thunderstorm with hail"
        default: return "Unknown conditions"
        }
    }

    private struct GeocodingResponse: Decodable {
        let results: [GeocodingResult]?
    }

    private struct GeocodingResult: Decodable {
        let name: String
        let latitude: Double
        let longitude: Double
        let country: String?
        let admin1: String?
        let timezone: String?
    }

    private struct ForecastResponse: Decodable {
        let timezone: String
        let current: CurrentConditions
        let daily: DailyForecast
    }

    private struct CurrentConditions: Decodable {
        let time: String
        let temperature2m: Double
        let apparentTemperature: Double
        let relativeHumidity2m: Int
        let precipitation: Double
        let weatherCode: Int
        let windSpeed10m: Double

        enum CodingKeys: String, CodingKey {
            case time
            case temperature2m = "temperature_2m"
            case apparentTemperature = "apparent_temperature"
            case relativeHumidity2m = "relative_humidity_2m"
            case precipitation
            case weatherCode = "weather_code"
            case windSpeed10m = "wind_speed_10m"
        }
    }

    private struct DailyForecast: Decodable {
        let temperature2mMax: [Double]
        let temperature2mMin: [Double]
        let precipitationProbabilityMax: [Int]

        enum CodingKeys: String, CodingKey {
            case temperature2mMax = "temperature_2m_max"
            case temperature2mMin = "temperature_2m_min"
            case precipitationProbabilityMax = "precipitation_probability_max"
        }
    }
}
