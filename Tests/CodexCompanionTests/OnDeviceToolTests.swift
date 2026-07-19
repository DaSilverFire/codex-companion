import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct OnDeviceToolTests {
    @Test
    func calculatorHonorsOperatorPrecedence() throws {
        #expect(try CompanionMathEvaluator.evaluate("2 + 3 * 4") == 14)
    }

    @Test
    func calculatorSupportsRootsAndPowers() throws {
        let value = try CompanionMathEvaluator.evaluate("sqrt(5092) + 2^3")

        #expect(abs(value - (sqrt(5092) + 8)) < 0.000_000_001)
    }

    @Test
    func calculatorAcceptsCommonMathSymbols() throws {
        #expect(try CompanionMathEvaluator.evaluate("6 × 7 − 2 ÷ 2") == 41)
    }

    @Test
    func calculatorRejectsDivisionByZero() {
        #expect(throws: CompanionMathEvaluator.EvaluationError.divisionByZero) {
            try CompanionMathEvaluator.evaluate("10 / 0")
        }
    }

    @Test
    func attachmentContextIncludesReadableDocuments() throws {
        let attachment = CompanionBridgeAttachment(
            kind: .file,
            filename: "notes.md",
            mimeType: "text/markdown",
            data: Data("Shadow should wave with one front paw.".utf8)
        )

        let context = try OnDeviceChatAttachmentContext.prepare(
            prompt: "Summarize this file.",
            attachments: [attachment]
        )

        #expect(context.prompt.contains("Summarize this file."))
        #expect(context.prompt.contains("File: notes.md"))
        #expect(context.prompt.contains("Shadow should wave with one front paw."))
        #expect(context.images.isEmpty)
    }

    @Test
    func attachmentOnlyImageGetsAUsefulPrompt() throws {
        let attachment = CompanionBridgeAttachment(
            kind: .image,
            filename: "shadow.png",
            mimeType: "image/png",
            data: Data([0x89, 0x50, 0x4E, 0x47])
        )

        let context = try OnDeviceChatAttachmentContext.prepare(
            prompt: "",
            attachments: [attachment]
        )

        #expect(context.prompt.contains("Describe the attached content"))
        #expect(context.prompt.contains("Attached images: shadow.png"))
        #expect(context.images == [attachment])
    }

    @Test
    func attachmentContextRejectsUnknownBinaryFiles() {
        let attachment = CompanionBridgeAttachment(
            kind: .file,
            filename: "archive.bin",
            mimeType: "application/octet-stream",
            data: Data([0x00, 0xFF, 0x01])
        )

        #expect(throws: OnDeviceChatError.self) {
            _ = try OnDeviceChatAttachmentContext.prepare(
                prompt: "Inspect this.",
                attachments: [attachment]
            )
        }
    }

    @Test
    func attachmentContextMarksLaterDocumentsOmittedAfterTheTotalLimit() throws {
        let documents = (1 ... 4).map { index in
            CompanionBridgeAttachment(
                kind: .file,
                filename: "notes-\(index).txt",
                mimeType: "text/plain",
                data: Data(String(repeating: "x", count: 12_000).utf8)
            )
        }

        let context = try OnDeviceChatAttachmentContext.prepare(
            prompt: "Summarize these files.",
            attachments: documents
        )

        #expect(context.prompt.contains("File: notes-4.txt"))
        #expect(context.prompt.contains("document context limit was reached"))
    }

    @Test
    func weatherLookupBuildsDocumentedGeocodingRequest() throws {
        let service = CompanionWeatherService()
        let request = try service.makeGeocodingRequest(location: "Indianapolis, IN")
        let url = try #require(request.url)
        let components = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
        let query = Dictionary(uniqueKeysWithValues: (components.queryItems ?? []).compactMap { item -> (String, String)? in
            guard let value = item.value else { return nil }
            return (item.name, value)
        })

        #expect(components.scheme == "https")
        #expect(components.host == "geocoding-api.open-meteo.com")
        #expect(components.path == "/v1/search")
        #expect(query["name"] == "Indianapolis, IN")
        #expect(query["count"] == "1")
        #expect(query["format"] == "json")
    }

    @Test
    func weatherLookupBuildsCurrentForecastRequest() throws {
        let service = CompanionWeatherService()
        let location = CompanionWeatherLocation(
            name: "Indianapolis",
            region: "Indiana",
            country: "United States",
            latitude: 39.7684,
            longitude: -86.1581,
            timeZone: "America/Indiana/Indianapolis"
        )
        let request = try service.makeForecastRequest(location: location, units: .imperial)
        let url = try #require(request.url)
        let components = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
        let query = Dictionary(uniqueKeysWithValues: (components.queryItems ?? []).compactMap { item -> (String, String)? in
            guard let value = item.value else { return nil }
            return (item.name, value)
        })

        #expect(components.host == "api.open-meteo.com")
        #expect(components.path == "/v1/forecast")
        #expect(query["latitude"] == "39.7684")
        #expect(query["longitude"] == "-86.1581")
        #expect(query["temperature_unit"] == "fahrenheit")
        #expect(query["wind_speed_unit"] == "mph")
        #expect(query["timezone"] == "America/Indiana/Indianapolis")
        #expect(query["current"]?.contains("temperature_2m") == true)
        #expect(query["daily"]?.contains("temperature_2m_max") == true)
    }

    @Test
    func weatherLookupDecodesLiveConditionsForTheModel() throws {
        let endpoint = try #require(URL(string: "https://api.open-meteo.com/v1/forecast"))
        let response = try #require(HTTPURLResponse(
            url: endpoint,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        ))
        let location = CompanionWeatherLocation(
            name: "Indianapolis",
            region: "Indiana",
            country: "United States",
            latitude: 39.7684,
            longitude: -86.1581,
            timeZone: "America/Indiana/Indianapolis"
        )
        let data = Data(
            """
            {
              "timezone": "America/Indiana/Indianapolis",
              "current": {
                "time": "2026-07-16T14:15",
                "temperature_2m": 83.1,
                "apparent_temperature": 87.0,
                "relative_humidity_2m": 62,
                "precipitation": 0.0,
                "weather_code": 1,
                "wind_speed_10m": 8.2
              },
              "daily": {
                "time": ["2026-07-16"],
                "temperature_2m_max": [88.0],
                "temperature_2m_min": [69.0],
                "precipitation_probability_max": [20]
              }
            }
            """.utf8
        )

        let report = try CompanionWeatherService().decodeForecast(
            data: data,
            response: response,
            location: location,
            units: .imperial
        )

        #expect(report.locationName == "Indianapolis, Indiana, United States")
        #expect(report.condition == "Mainly clear")
        #expect(report.temperature == 83.1)
        #expect(report.apparentTemperature == 87.0)
        #expect(report.dailyHigh == 88.0)
        #expect(report.dailyLow == 69.0)
        #expect(report.toolSummary.contains("Live weather from Open-Meteo"))
        #expect(report.toolSummary.contains("20%"))
    }
}
