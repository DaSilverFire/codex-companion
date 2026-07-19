import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CompanionWebLookupTests {
    @Test
    func lookupBuildsBoundedWikipediaRequest() throws {
        let service = CompanionWebLookupService()
        let request = try service.makeRequest(
            query: "Hades II release date",
            maximumResults: 50
        )
        let url = try #require(request.url)
        let components = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
        let query = Dictionary(uniqueKeysWithValues: (components.queryItems ?? []).compactMap { item -> (String, String)? in
            guard let value = item.value else { return nil }
            return (item.name, value)
        })

        #expect(components.scheme == "https")
        #expect(components.host == "en.wikipedia.org")
        #expect(components.path == "/w/api.php")
        #expect(query["generator"] == "search")
        #expect(query["gsrsearch"] == "Hades II release date")
        #expect(query["gsrlimit"] == "5")
        #expect(query["prop"] == "extracts|info")
        #expect(query["explaintext"] == "1")
        #expect(request.value(forHTTPHeaderField: "User-Agent")?.contains("CodexCompanion") == true)
    }

    @Test
    func lookupDecodesRankedReferencesWithCitations() throws {
        let endpoint = try #require(URL(string: "https://en.wikipedia.org/w/api.php"))
        let response = try #require(HTTPURLResponse(
            url: endpoint,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: ["Content-Type": "application/json"]
        ))
        let data = Data(
            """
            {
              "query": {
                "pages": [
                  {
                    "title": "Hades (video game)",
                    "index": 2,
                    "extract": "Hades released in 2020.",
                    "fullurl": "https://en.wikipedia.org/wiki/Hades_(video_game)"
                  },
                  {
                    "title": "Hades II",
                    "index": 1,
                    "extract": "Hades II fully released in September 2025 for PC and Nintendo Switch platforms.",
                    "fullurl": "https://en.wikipedia.org/wiki/Hades_II"
                  }
                ]
              }
            }
            """.utf8
        )

        let result = try CompanionWebLookupService().decode(
            data: data,
            response: response,
            query: "Hades II release date"
        )

        #expect(result.references.map(\.title) == ["Hades II", "Hades (video game)"])
        #expect(result.toolSummary.contains("September 2025"))
        #expect(result.toolSummary.contains("https://en.wikipedia.org/wiki/Hades_II"))
        #expect(result.toolSummary.contains("Cite the source URL"))
    }

    @Test
    func lookupRejectsEmptyQueriesAndEmptyResults() throws {
        let service = CompanionWebLookupService()
        #expect(throws: CompanionWebLookupError.emptyQuery) {
            _ = try service.makeRequest(query: "   ", maximumResults: 4)
        }

        let endpoint = try #require(URL(string: "https://en.wikipedia.org/w/api.php"))
        let response = try #require(HTTPURLResponse(
            url: endpoint,
            statusCode: 200,
            httpVersion: "HTTP/1.1",
            headerFields: nil
        ))
        #expect(throws: CompanionWebLookupError.noResults("missing game")) {
            _ = try service.decode(
                data: Data("{\"batchcomplete\": true}".utf8),
                response: response,
                query: "missing game"
            )
        }
    }
}
