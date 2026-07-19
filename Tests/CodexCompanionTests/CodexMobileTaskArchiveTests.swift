import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexMobileTaskArchiveTests {
    @Test
    func stripsAmbientBrowserContextFromVisibleUserMessage() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let storedText = """

        <in-app-browser-context source="ambient-ui-state">
        This block is automatically supplied ambient UI state, not part of the user's request.
        # In app browser:
        - Current URL: http://localhost:3200/?device=fixture-device
        </in-app-browser-context>

        ## My request for Codex:
        approve this request through Companion
        """
        try fixture.writeRollout([
            fixture.messageLine(
                id: "ambient-user-message",
                role: "user",
                text: storedText,
                turnID: "turn-ambient"
            ),
        ])
        try fixture.insertThread(id: "thread-ambient", title: "Ambient context task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .messages(threadID: "thread-ambient", cursor: nil, limit: 20)

        #expect(page.messages.map(\.text) == ["approve this request through Companion"])
    }

    @Test
    func stripsEnvironmentContextEnvelopeWithoutDroppingUserAuthoredText() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let internalContext = """
        <environment_context>
          <current_date>2026-07-16</current_date>
          <timezone>America/Indiana/Indianapolis</timezone>
          <filesystem><workspace_roots><root>/tmp/workspace</root></workspace_roots></filesystem>
          <subagents>- agent-fixture</subagents>
        </environment_context>
        """
        try fixture.writeRollout([
            fixture.messageLine(
                id: "pure-environment-context",
                role: "user",
                text: internalContext,
                turnID: "turn-environment-context"
            ),
            fixture.messageLine(
                id: "prepended-environment-context",
                role: "user",
                text: "\(internalContext)\n\nContinue repairing Companion.",
                turnID: "turn-environment-context"
            ),
            fixture.messageLine(
                id: "appended-environment-context",
                role: "user",
                text: "Keep the task open.\n\n\(internalContext)",
                turnID: "turn-environment-context"
            ),
        ])
        try fixture.insertThread(id: "thread-environment-context", title: "Resumed goal")

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let messages = try archive.messages(
            threadID: "thread-environment-context",
            cursor: nil,
            limit: 20
        )
        let timeline = try archive.timeline(
            threadID: "thread-environment-context",
            cursor: nil,
            limit: 20
        )

        #expect(messages.messages.map(\.id) == [
            "prepended-environment-context",
            "appended-environment-context",
        ])
        #expect(messages.messages.map(\.text) == [
            "Continue repairing Companion.",
            "Keep the task open.",
        ])
        #expect(timeline.items.map(\.id) == [
            "prepended-environment-context",
            "appended-environment-context",
        ])
        #expect(timeline.items.compactMap(\.text) == [
            "Continue repairing Companion.",
            "Keep the task open.",
        ])
    }

    @Test
    func stripsGeneratedAttachmentMetadataBeforeVisibleUserRequest() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let storedText = """
        # Files mentioned by the user:

        ## screenshot.png: /tmp/screenshot.png

        # Applications mentioned by the user:

        <appshot app="Device Hub">generated app state</appshot>

        ## My request for Codex:
        only render this request
        """
        try fixture.writeRollout([
            fixture.messageLine(
                id: "attachment-user-message",
                role: "user",
                text: storedText,
                turnID: "turn-attachment"
            ),
        ])
        try fixture.insertThread(id: "thread-attachment", title: "Attachment context task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .messages(threadID: "thread-attachment", cursor: nil, limit: 20)

        #expect(page.messages.map(\.text) == ["only render this request"])
    }

    @Test
    func timelineStripsRawImageMarkupWhilePreservingRequestAndImageMedia() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let imageBytes = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A])
        let storedText = """
        # Files mentioned by the user:

        ## reference.png: /tmp/reference.png

        # Applications mentioned by the user:

        <appshot app="Device Hub">generated app state</appshot>

        ## My request for Codex:
        Keep sqrt(5092) = √5092 ≈ 71.3583 visible.
        """
        try fixture.writeRollout([
            fixture.messageWithFragmentedImageMarkupLine(
                id: "user-with-image",
                text: storedText,
                imagePath: "/tmp/reference.png",
                imageData: imageBytes,
                mimeType: "image/png",
                turnID: "turn-user-image"
            ),
        ])
        try fixture.insertThread(id: "thread-user-image", title: "User image task")

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let messages = try archive.messages(threadID: "thread-user-image", cursor: nil, limit: 20)
        let timeline = try archive.timeline(threadID: "thread-user-image", cursor: nil, limit: 20)
        let item = try #require(timeline.items.first)

        #expect(messages.messages.map(\.text) == ["Keep sqrt(5092) = √5092 ≈ 71.3583 visible."])
        #expect(item.text == "Keep sqrt(5092) = √5092 ≈ 71.3583 visible.")
        #expect(item.text?.contains("<image") == false)
        #expect(item.media.map(\.data) == [imageBytes])
    }

    @Test
    func omitsGeneratedSubagentNotificationEnvelope() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.messageLine(
                id: "subagent-notification",
                role: "user",
                text: "<subagent_notification>{\"status\":\"completed\"}</subagent_notification>",
                turnID: "turn-subagent"
            ),
            fixture.messageLine(
                id: "assistant-visible",
                role: "assistant",
                text: "Visible Codex update",
                turnID: "turn-subagent"
            ),
        ])
        try fixture.insertThread(id: "thread-subagent", title: "Subagent context task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .messages(threadID: "thread-subagent", cursor: nil, limit: 20)

        #expect(page.messages.map(\.text) == ["Visible Codex update"])
    }

    @Test
    func timelineIncludesReasoningCommandsCommentaryAndCompaction() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.reasoningLine(text: "**Read files**", turnID: "turn-live"),
            fixture.toolCallLine(
                id: "tool-1",
                callID: "call-1",
                name: "exec",
                input: #"{"cmd":"swift test --filter CodexMobileTaskArchiveTests"}"#,
                turnID: "turn-live"
            ),
            fixture.messageLine(
                id: "commentary-1",
                role: "assistant",
                text: "The focused archive test is running now.",
                turnID: "turn-live",
                phase: "commentary"
            ),
            fixture.contextCompactedLine(turnID: "turn-live"),
        ])
        try fixture.insertThread(id: "thread-live", title: "Live task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-live", cursor: nil, limit: 20)

        #expect(page.items.map(\.kind) == [.reasoning, .tool, .message, .compaction])
        #expect(page.items[0].title == "Read files")
        #expect(page.items[0].status == .completed)
        #expect(page.items[0].detail == nil)
        #expect(page.items[1].title == "Tested the app")
        #expect(page.items[1].status == .inProgress)
        #expect(page.items[1].detail?.contains("swift test --filter CodexMobileTaskArchiveTests") == true)
        #expect(page.items[2].text == "The focused archive test is running now.")
        #expect(page.items[2].phase == .commentary)
        #expect(page.items[3].title == "Context compacted")
        #expect(page.revision.isEmpty == false)
    }

    @Test
    func timelineProjectsDelegationAndActiveWorkIntoSemanticRows() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.taskStartedLine(turnID: "turn-semantic"),
            fixture.reasoningLine(
                text: "**Planning release scripts development**",
                turnID: "turn-semantic",
                id: "reason-planning"
            ),
            fixture.functionCallLine(
                id: "delegate-call",
                callID: "call-delegate",
                name: "spawn_agent",
                arguments: #"{"agent_type":"worker","message":"Audit the release scripts without editing unrelated files."}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionOutputLine(
                callID: "call-delegate",
                output: #"{"agent_id":"agent-turing","nickname":"Turing"}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionCallLine(
                id: "wait-call",
                callID: "call-wait",
                name: "wait_agent",
                arguments: #"{"targets":["agent-turing"],"timeout_ms":10000}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionOutputLine(
                callID: "call-wait",
                output: #"{"status":{"agent-turing":"running"},"timed_out":true}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionCallLine(
                id: "delegate-follow-up",
                callID: "call-delegate-follow-up",
                name: "send_input",
                arguments: #"{"target":"agent-turing","message":"Also verify the deterministic audit output."}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionOutputLine(
                callID: "call-delegate-follow-up",
                output: #"{"submission_id":"submission-1"}"#,
                turnID: "turn-semantic"
            ),
            fixture.reasoningLine(
                text: "**Designing deterministic release audit script**",
                turnID: "turn-semantic",
                id: "reason-designing"
            ),
            fixture.toolCallLine(
                id: "command-call",
                callID: "call-command",
                name: "exec",
                input: #"{"cmd":"swift test --filter ReleaseAuditTests"}"#,
                turnID: "turn-semantic"
            ),
            fixture.functionOutputLine(
                callID: "call-command",
                output: "3 release audit tests passed",
                turnID: "turn-semantic"
            ),
        ])
        try fixture.insertThread(id: "thread-semantic", title: "Semantic timeline")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-semantic", cursor: nil, limit: 20)

        #expect(page.items.map(\.id) == [
            "reason-planning",
            "delegate-call",
            "reason-designing",
            "command-call",
        ])
        #expect(page.items.map(\.title) == [
            "Planning release scripts development",
            "Messaged an agent",
            "Designing deterministic release audit script",
            "Tested the app",
        ])
        #expect(page.items[0].status == .completed)
        #expect(page.items[1].detail?.contains("Target: Turing") == true)
        #expect(page.items[1].detail?.contains("Audit the release scripts") == true)
        #expect(page.items[1].detail?.contains("Also verify the deterministic audit output") == true)
        #expect(page.items[1].detail?.contains("timed_out") == false)
        #expect(page.items[2].status == .inProgress)
        #expect(page.items[2].detail == nil)
        #expect(page.items[3].status == .completed)
        let activityDetail = try #require(page.items[3].detail)
        let commandRange = try #require(activityDetail.range(of: "swift test --filter ReleaseAuditTests"))
        let resultRange = try #require(activityDetail.range(of: "3 release audit tests passed"))
        #expect(commandRange.lowerBound < resultRange.lowerBound)
        #expect(!page.items.contains(where: { $0.title == "Wait" || $0.title == "Tool result" }))
    }

    @Test
    func timelineRecoversEditedFilePathsFromPairedToolOutput() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "patch-call",
                callID: "call-patch",
                name: "apply_patch",
                input: "opaque patch payload",
                turnID: "turn-patch"
            ),
            fixture.functionOutputLine(
                callID: "call-patch",
                output: """
                Success. Updated the following files:
                M Sources/App.swift
                A Sources/Timeline View.swift
                """,
                turnID: "turn-patch"
            ),
        ])
        try fixture.insertThread(id: "thread-patch", title: "Patch timeline")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-patch", cursor: nil, limit: 20)

        #expect(page.items.count == 1)
        #expect(page.items[0].title == "Edited files")
        #expect(page.items[0].detail == "Sources/App.swift\nSources/Timeline View.swift")
        #expect(page.items[0].detail?.contains("Success.") == false)
        #expect(page.items[0].detail?.contains("Result") == false)
    }

    @Test
    func timelineReportsLatestContextWindowUsage() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.tokenCountLine(usedTokens: 64_000, contextWindow: 128_000),
            fixture.messageLine(
                id: "assistant-before-usage-update",
                role: "assistant",
                text: "Working through the mobile timeline.",
                turnID: "turn-context"
            ),
            fixture.tokenCountLine(usedTokens: 96_000, contextWindow: 128_000),
        ])
        try fixture.insertThread(id: "thread-context", title: "Context usage")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-context", cursor: nil, limit: 20)
        let usage = try #require(page.contextUsage)

        #expect(usage.usedTokens == 96_000)
        #expect(usage.contextWindow == 128_000)
        #expect(usage.fractionUsed == 0.75)
    }

    @Test
    func timelinePreservesMathSymbolsAndExtractsSmallImages() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let imageBytes = Data([0x89, 0x50, 0x4E, 0x47])
        try fixture.writeRollout([
            fixture.messageWithImageLine(
                id: "message-image",
                role: "assistant",
                text: "sqrt(5092) = √5092 ≈ 71.3583",
                imageData: imageBytes,
                mimeType: "image/png",
                turnID: "turn-image"
            ),
        ])
        try fixture.insertThread(id: "thread-image", title: "Image task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-image", cursor: nil, limit: 20)
        let item = try #require(page.items.first)
        let media = try #require(item.media.first)

        #expect(item.kind == .message)
        #expect(item.text == "sqrt(5092) = √5092 ≈ 71.3583")
        #expect(media.kind == .image)
        #expect(media.mimeType == "image/png")
        #expect(media.data == imageBytes)
    }

    @Test
    func timelineExtractsImagesReturnedByTools() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let imageBytes = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A])
        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "tool-image",
                callID: "call-image",
                name: "view_image",
                input: #"{"path":"/tmp/evidence.png"}"#,
                turnID: "turn-image-output"
            ),
            fixture.toolOutputWithImageLine(
                callID: "call-image",
                imageData: imageBytes,
                mimeType: "image/png",
                turnID: "turn-image-output"
            ),
        ])
        try fixture.insertThread(id: "thread-tool-image", title: "Tool image task")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-tool-image", cursor: nil, limit: 20)

        #expect(page.items.count == 1)
        #expect(page.items[0].title == "Viewed an image")
        #expect(page.items[0].kind == .tool)
        #expect(page.items[0].callID == "call-image")
        #expect(page.items[0].media.first?.mimeType == "image/png")
        #expect(page.items[0].media.first?.data == imageBytes)
    }

    @Test
    func timelineMergesToolErrorsIntoTheRepresentedCall() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "failed-command",
                callID: "call-failed-command",
                name: "exec",
                input: #"{"cmd":"swift test --filter MissingTests"}"#,
                turnID: "turn-failed-command"
            ),
            fixture.functionOutputLine(
                callID: "call-failed-command",
                output: "Error: no tests matched MissingTests",
                turnID: "turn-failed-command"
            ),
        ])
        try fixture.insertThread(id: "thread-failed-command", title: "Failed command")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-failed-command", cursor: nil, limit: 20)
        let item = try #require(page.items.first)

        #expect(page.items.count == 1)
        #expect(item.id == "failed-command")
        #expect(item.title == "Tested the app")
        #expect(item.status == .failed)
        #expect(item.detail?.contains("swift test --filter MissingTests") == true)
        #expect(item.detail?.contains("Error: no tests matched MissingTests") == true)
    }

    @Test
    func timelineUsesCodexStyleTitlesForQualifiedTools() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "javascript-call",
                callID: "call-javascript",
                name: "mcp__node_repl__js",
                input: #"{"code":"nodeRepl.write('ready')"}"#,
                turnID: "turn-javascript"
            ),
            fixture.toolCallLine(
                id: "tool-search-call",
                callID: "call-tool-search",
                name: "tool_search",
                input: #"{"query":"calendar tools"}"#,
                turnID: "turn-javascript"
            ),
            fixture.toolCallLine(
                id: "integration-call",
                callID: "call-integration",
                name: "mcp__calendar__list_events",
                input: #"{"range":"today"}"#,
                turnID: "turn-javascript"
            ),
        ])
        try fixture.insertThread(id: "thread-qualified-tool", title: "Qualified tool")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-qualified-tool", cursor: nil, limit: 20)
        let itemsByID = Dictionary(uniqueKeysWithValues: page.items.map { ($0.id, $0) })
        let computerUse = try #require(itemsByID["javascript-call"])
        let toolSearch = try #require(itemsByID["tool-search-call"])
        let integration = try #require(itemsByID["integration-call"])

        #expect(computerUse.title == "Inspected an app")
        #expect(computerUse.detail == nil)
        #expect(toolSearch.title == "Loaded tools")
        #expect(integration.title == "Used an integration")
    }

    @Test
    func timelineHidesToolInventoryJavaScriptBehindLoadedTools() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "tool-inventory-call",
                callID: "call-tool-inventory",
                name: "exec",
                input: """
                const matches = ALL_TOOLS.filter(({ name, description }) =>
                    name.includes("device") || description.includes("screenshot")
                );
                text(matches);
                // Warning: truncated output
                // exec tool declaration
                """,
                turnID: "turn-tool-inventory"
            ),
        ])
        try fixture.insertThread(id: "thread-tool-inventory", title: "Tool inventory")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-tool-inventory", cursor: nil, limit: 20)
        let item = try #require(page.items.first)

        #expect(page.items.count == 1)
        #expect(item.title == "Loaded tools")
        #expect(item.detail == nil)
        #expect(item.text == nil)
    }

    @Test
    func timelineHidesDirectNodeInventoryBehindLoadedTools() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.toolCallLine(
                id: "direct-tool-inventory-call",
                callID: "call-direct-tool-inventory",
                name: "mcp__node_repl__js",
                input: #"{"code":"const hits = ALL_TOOLS.filter(tool => tool.name.includes('thread')); text(hits);"}"#,
                turnID: "turn-direct-tool-inventory"
            ),
        ])
        try fixture.insertThread(id: "thread-direct-tool-inventory", title: "Direct tool inventory")

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-direct-tool-inventory", cursor: nil, limit: 20)
        let item = try #require(page.items.first)

        #expect(page.items.count == 1)
        #expect(item.title == "Loaded tools")
        #expect(item.detail == nil)
        #expect(item.text == nil)
    }

    @Test
    func timelineHidesServerQualifiedNodeInventoryBehindLoadedTools() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.writeRollout([
            fixture.mcpToolCallEndLine(
                id: "server-tool-inventory-call",
                callID: "call-server-tool-inventory",
                server: "node_repl",
                tool: "js",
                arguments: [
                    "code": "const hits = ALL_TOOLS.filter(tool => tool.name.includes('thread')); text(hits);",
                ],
                turnID: "turn-server-tool-inventory"
            ),
        ])
        try fixture.insertThread(
            id: "thread-server-tool-inventory",
            title: "Server tool inventory"
        )

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .timeline(threadID: "thread-server-tool-inventory", cursor: nil, limit: 20)
        let item = try #require(page.items.first)

        #expect(page.items.count == 1)
        #expect(item.title == "Loaded tools")
        #expect(item.detail == nil)
        #expect(item.text == nil)
    }

    @Test
    func semanticTimelinePaginationKeepsStableGroupIDsWithoutDuplicates() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        var lines = [fixture.taskStartedLine(turnID: "turn-pagination")]
        for index in 1...3 {
            lines.append(fixture.reasoningLine(
                text: "**Activity \(index)**",
                turnID: "turn-pagination",
                id: "activity-\(index)"
            ))
            lines.append(fixture.toolCallLine(
                id: "command-\(index)",
                callID: "call-command-\(index)",
                name: "exec",
                input: #"{"cmd":"command \#(index)"}"#,
                turnID: "turn-pagination"
            ))
            lines.append(fixture.functionOutputLine(
                callID: "call-command-\(index)",
                output: "result \(index)",
                turnID: "turn-pagination"
            ))
        }
        try fixture.writeRollout(lines)
        try fixture.insertThread(id: "thread-semantic-pages", title: "Semantic pages")

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let newest = try archive.timeline(threadID: "thread-semantic-pages", cursor: nil, limit: 2)
        let olderCursor = try #require(newest.nextCursor)
        let older = try archive.timeline(
            threadID: "thread-semantic-pages",
            cursor: olderCursor,
            limit: 2
        )
        let oldestCursor = try #require(older.nextCursor)
        let oldest = try archive.timeline(
            threadID: "thread-semantic-pages",
            cursor: oldestCursor,
            limit: 2
        )

        #expect(newest.items.map(\.id) == ["activity-3", "command-3"])
        #expect(older.items.map(\.id) == ["activity-2", "command-2"])
        #expect(oldest.items.map(\.id) == ["activity-1", "command-1"])
        #expect(oldest.nextCursor == nil)
        #expect(Set((oldest.items + older.items + newest.items).map(\.id)).count == 6)
    }

    @Test
    func subagentsAreLoadedFromTheSelectedParentInRecentOrder() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        try fixture.insertThread(id: "parent", title: "Parent task")
        try fixture.insertThread(
            id: "agent-old",
            title: "Inspect transport",
            source: fixture.subagentSource(parentID: "parent", nickname: "Curie", role: "explorer"),
            recency: 100
        )
        try fixture.insertThread(
            id: "agent-new",
            title: "Build mobile timeline",
            source: fixture.subagentSource(parentID: "parent", nickname: "Noether", role: "worker"),
            recency: 200
        )
        try fixture.insertThread(
            id: "agent-other",
            title: "Unrelated agent",
            source: fixture.subagentSource(parentID: "other", nickname: "Hume", role: "explorer"),
            recency: 300
        )

        let agents = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .subagents(parentThreadID: "parent", limit: 8)

        #expect(agents.map(\.id) == ["agent-new", "agent-old"])
        #expect(agents.map(\.name) == ["Noether", "Curie"])
        #expect(agents.map(\.role) == ["worker", "explorer"])
        #expect(agents.map(\.title) == ["Build mobile timeline", "Inspect transport"])
    }

    @Test
    func pagesLongHistoryBackwardWithoutDuplicatesOrInternalContext() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let visibleMessages = (1...7).map { index in
            fixture.messageLine(
                id: "message-\(index)",
                role: index.isMultiple(of: 2) ? "assistant" : "user",
                text: "Visible message \(index)",
                turnID: "turn-\(index)"
            )
        }
        let oversizedIgnoredEvent = try #require(String(
            data: JSONSerialization.data(withJSONObject: [
                "type": "event_msg",
                "payload": ["type": "token_count", "padding": String(repeating: "x", count: 600_000)],
            ]),
            encoding: .utf8
        ))
        let internalContext = fixture.messageLine(
            id: "internal",
            role: "user",
            text: "<codex_internal_context>hidden</codex_internal_context>",
            turnID: "turn-hidden"
        )
        try fixture.writeRollout(
            visibleMessages.prefix(3)
                + [oversizedIgnoredEvent, internalContext]
                + visibleMessages.suffix(4)
        )
        try fixture.insertThread(id: "thread-paged", title: "Paged task")

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let newest = try archive.messages(threadID: "thread-paged", cursor: nil, limit: 3)
        let middleCursor = try #require(newest.nextCursor)
        let middle = try archive.messages(
            threadID: "thread-paged",
            cursor: middleCursor,
            limit: 3
        )
        let oldestCursor = try #require(middle.nextCursor)
        let oldest = try archive.messages(
            threadID: "thread-paged",
            cursor: oldestCursor,
            limit: 3
        )

        #expect(newest.messages.map(\.text) == ["Visible message 5", "Visible message 6", "Visible message 7"])
        #expect(middle.messages.map(\.text) == ["Visible message 2", "Visible message 3", "Visible message 4"])
        #expect(oldest.messages.map(\.text) == ["Visible message 1"])
        #expect(oldest.nextCursor == nil)

        let allIDs = (oldest.messages + middle.messages + newest.messages).map(\.id)
        #expect(Set(allIDs).count == 7)
        #expect(!allIDs.contains("internal"))
    }

    @Test
    func skipsAnOversizedNonMessageRecordWhilePagingOlderHistory() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }

        let oversizedIgnoredEvent = try #require(String(
            data: JSONSerialization.data(withJSONObject: [
                "type": "event_msg",
                "payload": [
                    "type": "token_count",
                    "padding": String(repeating: "x", count: 3_200_000),
                ],
            ]),
            encoding: .utf8
        ))
        try fixture.writeRollout([
            fixture.messageLine(id: "oldest", role: "user", text: "Oldest visible", turnID: "turn-old"),
            oversizedIgnoredEvent,
            fixture.messageLine(id: "newer", role: "assistant", text: "Newer visible", turnID: "turn-new"),
            fixture.messageLine(id: "newest", role: "assistant", text: "Newest visible", turnID: "turn-new"),
        ])
        try fixture.insertThread(id: "thread-oversized", title: "Oversized history")

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let newest = try archive.messages(threadID: "thread-oversized", cursor: nil, limit: 2)
        let olderCursor = try #require(newest.nextCursor)
        let older = try archive.messages(
            threadID: "thread-oversized",
            cursor: olderCursor,
            limit: 2
        )

        #expect(newest.messages.map(\.text) == ["Newer visible", "Newest visible"])
        #expect(older.messages.map(\.text) == ["Oldest visible"])
        #expect(older.nextCursor == nil)
    }

    @Test
    func taskPageUsesStoredChatTitleAndLatestAssistantPreview() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([
            fixture.taskStartedLine(turnID: "turn-a"),
            fixture.messageLine(id: "user-1", role: "user", text: "Initial request", turnID: "turn-a"),
            fixture.messageLine(id: "assistant-1", role: "assistant", text: "Latest Codex update", turnID: "turn-a"),
        ])
        try fixture.insertThread(
            id: "thread-title",
            title: "Mobile Companion work",
            firstMessage: "Initial request",
            cwd: "/tmp/unrelated-folder"
        )

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root).tasks(cursor: nil, limit: 20)
        let task = try #require(page.tasks.first)

        #expect(task.id == "thread-title")
        #expect(task.title == "Mobile Companion work")
        #expect(task.preview == "Latest Codex update")
        #expect(task.status == .running)
        #expect(task.activeTurnID == "turn-a")
        #expect(page.nextCursor == nil)
    }

    @Test
    func taskPageUsesCompletedTurnLifecycleWithoutWaitingForTheRecencyHeuristic() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([
            fixture.taskStartedLine(turnID: "turn-complete"),
            fixture.messageLine(
                id: "assistant-complete",
                role: "assistant",
                text: "The response is finished.",
                turnID: "turn-complete",
                phase: "final"
            ),
            fixture.taskLifecycleLine(type: "task_complete", turnID: "turn-complete"),
        ])
        try fixture.insertThread(id: "thread-complete", title: "Completed response")

        let task = try #require(
            CodexMobileTaskArchive(homeDirectory: fixture.root)
                .tasks(cursor: nil, limit: 20)
                .tasks.first
        )

        #expect(task.status == .completed)
        #expect(task.activeTurnID == nil)
        #expect(task.preview == "The response is finished.")
    }

    @Test
    func taskPageExposesFailedTurnLifecycleForMobileAlerts() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([
            fixture.taskStartedLine(turnID: "turn-failed"),
            fixture.messageLine(
                id: "assistant-before-failure",
                role: "assistant",
                text: "The command failed.",
                turnID: "turn-failed"
            ),
            fixture.taskLifecycleLine(type: "task_aborted", turnID: "turn-failed"),
        ])
        try fixture.insertThread(id: "thread-failed", title: "Failed response")

        let task = try #require(
            CodexMobileTaskArchive(homeDirectory: fixture.root)
                .tasks(cursor: nil, limit: 20)
                .tasks.first
        )

        #expect(task.status == .failed)
        #expect(task.activeTurnID == nil)
    }

    @Test
    func taskPagePrefersTheSessionIndexConversationName() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([])
        try fixture.insertThread(
            id: "thread-renamed",
            title: "This is the original first prompt and not the visible chat name",
            firstMessage: "This is the original first prompt and not the visible chat name",
            cwd: "/tmp/Sample Project"
        )
        try fixture.writeSessionName(
            id: "thread-renamed",
            name: "Sample project task"
        )

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root).tasks(cursor: nil, limit: 20)

        #expect(page.tasks.first?.title == "Sample project task")
    }

    @Test
    func taskPageUsesSidebarTaskAndNamedProjectMetadata() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([])
        try fixture.insertThread(
            id: "loose-task",
            title: "Companion task",
            cwd: "/Users/test/Documents/Codex/2026-07-13/implementation"
        )
        try fixture.insertThread(
            id: "project-task",
            title: "Project task",
            cwd: "/Users/test/Documents/Sample Project/work"
        )
        try fixture.writeSidebarState(
            pinnedThreadIDs: [],
            projectOrder: ["/Users/test/Documents/Sample Project"],
            projectlessThreadIDs: ["loose-task"],
            threadWorkspaceRootHints: [
                "loose-task": "/Users/test/Documents/Codex",
                "project-task": "/Users/test/Documents/Sample Project",
            ],
            workspaceRootLabels: [
                "/Users/test/Documents/Sample Project": "Sample Project",
            ]
        )

        let page = try CodexMobileTaskArchive(homeDirectory: fixture.root)
            .tasks(cursor: nil, limit: 20)
        let looseTask = try #require(page.tasks.first(where: { $0.id == "loose-task" }))
        let projectTask = try #require(page.tasks.first(where: { $0.id == "project-task" }))

        #expect(looseTask.taskGroup?.kind == .chats)
        #expect(looseTask.taskGroup?.title == "Chats")
        #expect(looseTask.taskGroup?.path == nil)
        #expect(projectTask.taskGroup?.kind == .project)
        #expect(projectTask.taskGroup?.title == "Sample Project")
        #expect(projectTask.taskGroup?.path == "/Users/test/Documents/Sample Project")
    }

    @Test
    func taskPaginationCapsRowsAndExcludesSubagentsAndArchivedTasks() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([])
        try fixture.insertThread(id: "thread-1", title: "One", recency: 30)
        try fixture.insertThread(id: "thread-2", title: "Two", recency: 20)
        try fixture.insertThread(id: "thread-3", title: "Three", recency: 10)
        try fixture.insertThread(id: "archived", title: "Archived", archived: true, recency: 40)
        try fixture.insertThread(
            id: "subagent",
            title: "Subagent",
            source: #"{"subagent":"worker"}"#,
            recency: 50
        )

        let archive = CodexMobileTaskArchive(homeDirectory: fixture.root)
        let first = try archive.tasks(cursor: nil, limit: 2)
        let second = try archive.tasks(cursor: first.nextCursor, limit: 2)

        #expect(first.tasks.map(\.id) == ["thread-1", "thread-2"])
        #expect(first.nextCursor == "2")
        #expect(second.tasks.map(\.id) == ["thread-3"])
        #expect(second.nextCursor == nil)
    }

    @Test
    func taskPageMatchesPinnedThenProjectThenLooseChatOrdering() throws {
        let fixture = try ArchiveFixture()
        defer { fixture.remove() }
        try fixture.writeRollout([])
        try fixture.insertThread(
            id: "loose-chat",
            title: "Loose chat",
            cwd: "/tmp/loose",
            recency: 300
        )
        try fixture.insertThread(
            id: "project-task",
            title: "Project task",
            cwd: "/Users/test/Documents/Sample Project",
            recency: 200
        )
        try fixture.insertThread(
            id: "pinned-old",
            title: "Pinned old task",
            cwd: "/tmp/pinned",
            recency: 100
        )
        try fixture.writeSidebarState(
            pinnedThreadIDs: ["pinned-old"],
            projectOrder: ["/Users/test/Documents/Sample Project"],
            projectlessThreadIDs: ["loose-chat", "pinned-old"]
        )

        let page = try CodexMobileTaskArchive(
            homeDirectory: fixture.root,
            approvalPromotionTracker: CodexApprovalPromotionTracker(),
            readPendingApprovalThreadIDs: { [] }
        ).tasks(cursor: nil, limit: 20)

        #expect(page.tasks.map(\.id) == ["pinned-old", "project-task", "loose-chat"])
    }

    @Test
    func clearedApprovalRemainsPromotedForTenSeconds() {
        let tracker = CodexApprovalPromotionTracker(holdDuration: 10)
        let start = Date(timeIntervalSince1970: 100)

        #expect(tracker.promotedThreadIDs(
            pendingThreadIDs: ["approval"],
            now: start
        ) == ["approval"])
        #expect(tracker.promotedThreadIDs(
            pendingThreadIDs: [],
            now: start.addingTimeInterval(1)
        ) == ["approval"])
        #expect(tracker.promotedThreadIDs(
            pendingThreadIDs: [],
            now: start.addingTimeInterval(10.9)
        ) == ["approval"])
        #expect(tracker.promotedThreadIDs(
            pendingThreadIDs: [],
            now: start.addingTimeInterval(11.1)
        ).isEmpty)
    }
}

private struct ArchiveFixture {
    let root: URL
    let codexDirectory: URL
    let databaseURL: URL
    let rolloutURL: URL

    init() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("CodexMobileArchiveTests-\(UUID().uuidString)", isDirectory: true)
        codexDirectory = root.appendingPathComponent(".codex", isDirectory: true)
        databaseURL = codexDirectory.appendingPathComponent("state_5.sqlite")
        rolloutURL = codexDirectory.appendingPathComponent("rollout.jsonl")
        try FileManager.default.createDirectory(at: codexDirectory, withIntermediateDirectories: true)
        try runSQLite("""
        create table threads (
            id text primary key,
            title text,
            cwd text,
            updated_at_ms integer,
            updated_at integer,
            first_user_message text,
            rollout_path text,
            preview text,
            model text,
            reasoning_effort text,
            archived integer,
            source text,
            recency_at_ms integer
        );
        """)
    }

    func remove() {
        try? FileManager.default.removeItem(at: root)
    }

    func writeRollout<S: Sequence>(_ lines: S) throws where S.Element == String {
        let collected = Array(lines)
        let content = collected.joined(separator: "\n") + (collected.isEmpty ? "" : "\n")
        try Data(content.utf8).write(to: rolloutURL, options: .atomic)
    }

    func insertThread(
        id: String,
        title: String,
        firstMessage: String = "First message",
        cwd: String = "/tmp/workspace",
        archived: Bool = false,
        source: String = "user",
        recency: Int = 100
    ) throws {
        let updatedMilliseconds = Int(Date().timeIntervalSince1970 * 1_000)
        try runSQLite("""
        insert into threads values (
            '\(sql(id))', '\(sql(title))', '\(sql(cwd))', \(updatedMilliseconds),
            \(updatedMilliseconds / 1_000), '\(sql(firstMessage))', '\(sql(rolloutURL.path))',
            'Stored preview', 'gpt-test', 'high', \(archived ? 1 : 0), '\(sql(source))', \(recency)
        );
        """)
    }

    func messageLine(
        id: String,
        role: String,
        text: String,
        turnID: String,
        phase: String? = nil
    ) -> String {
        var payload: [String: Any] = [
            "id": id,
            "type": "message",
            "role": role,
            "turn_id": turnID,
            "content": [[
                "type": role == "assistant" ? "output_text" : "input_text",
                "text": text,
            ]],
        ]
        if let phase {
            payload["phase"] = phase
        }
        let object: [String: Any] = [
            "timestamp": "2026-07-13T03:00:00.000Z",
            "type": "response_item",
            "payload": payload,
        ]
        let data = try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }

    func subagentSource(parentID: String, nickname: String, role: String) -> String {
        jsonLine([
            "subagent": [
                "thread_spawn": [
                    "parent_thread_id": parentID,
                    "depth": 1,
                    "agent_nickname": nickname,
                    "agent_role": role,
                ],
            ],
        ])
    }

    func messageWithImageLine(
        id: String,
        role: String,
        text: String,
        imageData: Data,
        mimeType: String,
        turnID: String
    ) -> String {
        let object: [String: Any] = [
            "timestamp": "2026-07-13T03:00:00.000Z",
            "type": "response_item",
            "payload": [
                "id": id,
                "type": "message",
                "role": role,
                "turn_id": turnID,
                "content": [
                    [
                        "type": role == "assistant" ? "output_text" : "input_text",
                        "text": text,
                    ],
                    [
                        "type": "input_image",
                        "image_url": "data:\(mimeType);base64,\(imageData.base64EncodedString())",
                        "detail": "auto",
                    ],
                ],
            ],
        ]
        let data = try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }

    func reasoningLine(text: String, turnID: String, id: String? = nil) -> String {
        var payload: [String: Any] = [
            "type": "agent_reasoning",
            "text": text,
            "turn_id": turnID,
        ]
        if let id {
            payload["id"] = id
        }
        return jsonLine([
            "timestamp": "2026-07-13T03:00:00.000Z",
            "type": "event_msg",
            "payload": payload,
        ])
    }

    func taskStartedLine(turnID: String) -> String {
        taskLifecycleLine(type: "task_started", turnID: turnID)
    }

    func taskLifecycleLine(type: String, turnID: String) -> String {
        jsonLine([
            "timestamp": "2026-07-13T02:59:59.000Z",
            "type": "event_msg",
            "payload": [
                "type": type,
                "turn_id": turnID,
            ],
        ])
    }

    func functionCallLine(
        id: String,
        callID: String,
        name: String,
        arguments: String,
        turnID: String
    ) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:01.000Z",
            "type": "response_item",
            "payload": [
                "id": id,
                "type": "function_call",
                "call_id": callID,
                "name": name,
                "namespace": "multi_agent_v1",
                "arguments": arguments,
                "status": "completed",
                "internal_chat_message_metadata_passthrough": ["turn_id": turnID],
            ],
        ])
    }

    func functionOutputLine(callID: String, output: String, turnID: String) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:02.000Z",
            "type": "response_item",
            "payload": [
                "type": "function_call_output",
                "call_id": callID,
                "output": output,
                "internal_chat_message_metadata_passthrough": ["turn_id": turnID],
            ],
        ])
    }

    func messageWithFragmentedImageMarkupLine(
        id: String,
        text: String,
        imagePath: String,
        imageData: Data,
        mimeType: String,
        turnID: String
    ) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:00.000Z",
            "type": "response_item",
            "payload": [
                "id": id,
                "type": "message",
                "role": "user",
                "turn_id": turnID,
                "content": [
                    ["type": "input_text", "text": text],
                    ["type": "input_text", "text": "<image name=[Image #1] path=\"\(imagePath)\">"],
                    [
                        "type": "input_image",
                        "image_url": "data:\(mimeType);base64,\(imageData.base64EncodedString())",
                        "detail": "original",
                    ],
                    ["type": "input_text", "text": "</image>"],
                ],
            ],
        ])
    }

    func toolCallLine(
        id: String,
        callID: String,
        name: String,
        input: String,
        turnID: String
    ) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:01.000Z",
            "type": "response_item",
            "payload": [
                "id": id,
                "type": "custom_tool_call",
                "call_id": callID,
                "name": name,
                "input": input,
                "status": "completed",
                "internal_chat_message_metadata_passthrough": ["turn_id": turnID],
            ],
        ])
    }

    func toolOutputWithImageLine(
        callID: String,
        imageData: Data,
        mimeType: String,
        turnID: String
    ) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:02.000Z",
            "type": "response_item",
            "payload": [
                "type": "function_call_output",
                "call_id": callID,
                "output": [[
                    "type": "input_image",
                    "image_url": "data:\(mimeType);base64,\(imageData.base64EncodedString())",
                    "detail": "original",
                ]],
                "internal_chat_message_metadata_passthrough": ["turn_id": turnID],
            ],
        ])
    }

    func contextCompactedLine(turnID: String) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:03.000Z",
            "type": "event_msg",
            "payload": [
                "type": "context_compacted",
                "turn_id": turnID,
            ],
        ])
    }

    func mcpToolCallEndLine(
        id: String,
        callID: String,
        server: String,
        tool: String,
        arguments: [String: Any],
        turnID: String
    ) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:02.000Z",
            "type": "event_msg",
            "payload": [
                "id": id,
                "type": "mcp_tool_call_end",
                "call_id": callID,
                "turn_id": turnID,
                "invocation": [
                    "server": server,
                    "tool": tool,
                    "arguments": arguments,
                ],
            ],
        ])
    }

    func tokenCountLine(usedTokens: Int, contextWindow: Int) -> String {
        jsonLine([
            "timestamp": "2026-07-13T03:00:04.000Z",
            "type": "event_msg",
            "payload": [
                "type": "token_count",
                "info": [
                    "last_token_usage": ["total_tokens": usedTokens],
                    "model_context_window": contextWindow,
                ],
            ],
        ])
    }

    private func jsonLine(_ object: [String: Any]) -> String {
        let data = try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }

    func writeSessionName(id: String, name: String) throws {
        let object: [String: Any] = [
            "id": id,
            "thread_name": name,
            "updated_at": "2026-07-13T03:00:00.000Z",
        ]
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        try data.appendingNewline().write(
            to: codexDirectory.appendingPathComponent("session_index.jsonl"),
            options: .atomic
        )
    }

    func writeSidebarState(
        pinnedThreadIDs: [String],
        projectOrder: [String],
        projectlessThreadIDs: [String],
        threadWorkspaceRootHints: [String: String] = [:],
        workspaceRootLabels: [String: String] = [:]
    ) throws {
        let object: [String: Any] = [
            "pinned-thread-ids": pinnedThreadIDs,
            "project-order": projectOrder,
            "projectless-thread-ids": projectlessThreadIDs,
            "thread-workspace-root-hints": threadWorkspaceRootHints,
            "electron-workspace-root-labels": workspaceRootLabels,
        ]
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        try data.write(
            to: codexDirectory.appendingPathComponent(".codex-global-state.json"),
            options: .atomic
        )
    }

    private func runSQLite(_ query: String) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")
        process.arguments = [databaseURL.path, query]
        let errors = Pipe()
        process.standardError = errors
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let message = String(decoding: errors.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            throw ArchiveFixtureError.sqliteFailed(message)
        }
    }

    private func sql(_ value: String) -> String {
        value.replacingOccurrences(of: "'", with: "''")
    }
}

private extension Data {
    func appendingNewline() -> Data {
        var result = self
        result.append(0x0A)
        return result
    }
}

private enum ArchiveFixtureError: Error {
    case sqliteFailed(String)
}
