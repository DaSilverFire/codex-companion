import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexCompanionMobileBridgeHistoryTests {
    @Test
    func duplicateHistoryRequestsShareOneArchiveLoadAndPreserveResponseIDs() async throws {
        let fixture = try HistoryBridgeFixture()
        defer { fixture.remove() }

        let archive = CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            sqliteExecutableURL: fixture.countingSQLiteURL,
            readPendingApprovalThreadIDs: { [] }
        )
        let server = CodexCompanionMobileBridgeServer(archive: archive)
        let firstRequest = CompanionBridgeRequest(
            operation: .loadMessages,
            limit: 20,
            threadID: fixture.threadID
        )
        let secondRequest = CompanionBridgeRequest(
            operation: .loadMessages,
            limit: 20,
            threadID: fixture.threadID
        )

        async let first = server.handle(firstRequest)
        async let second = server.handle(secondRequest)
        let responses = await [first, second]

        #expect(responses.allSatisfy { $0.succeeded })
        #expect(responses.map { $0.id } == [firstRequest.id, secondRequest.id])
        #expect(responses.allSatisfy { response in
            response.messages?.map { $0.text } == ["Current response"]
        })
        let cachedResponse = await server.handle(
            CompanionBridgeRequest(
                operation: .loadMessages,
                limit: 20,
                threadID: fixture.threadID
            )
        )
        #expect(cachedResponse.succeeded)
        #expect(cachedResponse.messages?.map { $0.text } == ["Current response"])
        let invocationCount = try fixture.sqliteInvocationCount()
        #expect(invocationCount == 3)
    }
}

private struct HistoryBridgeFixture {
    let threadID = "thread-history"
    let root: URL
    let countingSQLiteURL: URL
    private let countURL: URL

    init() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("CompanionHistoryBridgeTests-\(UUID().uuidString)", isDirectory: true)
        let codexDirectory = root.appendingPathComponent(".codex", isDirectory: true)
        let databaseURL = codexDirectory.appendingPathComponent("state_5.sqlite")
        let rolloutURL = codexDirectory.appendingPathComponent("rollout.jsonl")
        countingSQLiteURL = root.appendingPathComponent("counting-sqlite")
        countURL = root.appendingPathComponent("sqlite-invocations")

        try FileManager.default.createDirectory(at: codexDirectory, withIntermediateDirectories: true)
        _ = FileManager.default.createFile(atPath: databaseURL.path, contents: Data())
        let message = #"{"timestamp":"2026-07-19T00:00:00.000Z","type":"response_item","payload":{"id":"message-current","type":"message","role":"assistant","turn_id":"turn-current","content":[{"type":"output_text","text":"Current response"}]}}"#
        try Data("\(message)\n".utf8).write(to: rolloutURL, options: .atomic)

        let escapedCountPath = Self.shellQuoted(countURL.path)
        let escapedRolloutPath = Self.shellQuoted(rolloutURL.path)
        let script = """
        #!/bin/sh
        printf 'x\\n' >> \(escapedCountPath)
        printf '%s\\036' \(escapedRolloutPath)
        """
        try Data(script.utf8).write(to: countingSQLiteURL, options: .atomic)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755],
            ofItemAtPath: countingSQLiteURL.path
        )
    }

    func sqliteInvocationCount() throws -> Int {
        let text = try String(contentsOf: countURL, encoding: .utf8)
        return text.split(separator: "\n").count
    }

    func remove() {
        try? FileManager.default.removeItem(at: root)
    }

    private static func shellQuoted(_ value: String) -> String {
        "'\(value.replacingOccurrences(of: "'", with: "'\\''"))'"
    }
}
