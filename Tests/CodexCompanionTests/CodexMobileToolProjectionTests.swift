import XCTest
@testable import CodexCompanion

final class CodexMobileToolProjectionTests: XCTestCase {
    func testNestedTerminalCommandKeepsOnlyTheCommand() {
        let projection = CodexMobileToolProjection.project(
            name: "exec",
            input: #"const result = await tools.exec_command({cmd:"swift test",workdir:"/tmp/project"});"#
        )

        XCTAssertEqual(projection.title, "Tested the app")
        XCTAssertEqual(projection.detail, "swift test")
        XCTAssertFalse(projection.omitsWrapper)
    }

    func testTerminalPayloadsUseOperationLevelActivityTitles() {
        let read = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"sed -n '1,120p' Sources/App.swift"}"#
        )
        let search = CodexMobileToolProjection.project(
            name: "exec",
            input: #"const result = await tools.exec_command({cmd:"rg -n 'TaskTimeline' Sources"});"#
        )
        let test = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"swift test --filter Timeline"}"#
        )
        let build = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"env DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer xcodebuild -scheme Companion build"}"#
        )

        XCTAssertEqual(read.title, "Read files")
        XCTAssertEqual(read.detail, "Sources/App.swift")
        XCTAssertEqual(search.title, "Searched files")
        XCTAssertEqual(search.detail, "rg -n 'TaskTimeline' Sources")
        XCTAssertEqual(test.title, "Tested the app")
        XCTAssertEqual(test.detail, "swift test --filter Timeline")
        XCTAssertEqual(build.title, "Built the app")
        XCTAssertEqual(
            build.detail,
            "env DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer xcodebuild -scheme Companion build"
        )
    }

    func testShellBackedReadsExposeFilesInsteadOfShellSyntax() {
        let quoted = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"sed -n '1,120p' 'Sources/Task Detail.swift'"}"#
        )
        let multiple = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"cat Sources/App.swift Sources/Timeline.swift"}"#
        )
        let optionValue = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"tail -n 80 /tmp/companion.log"}"#
        )

        XCTAssertEqual(quoted.title, "Read files")
        XCTAssertEqual(quoted.detail, "Sources/Task Detail.swift")
        XCTAssertEqual(multiple.detail, "Sources/App.swift\nSources/Timeline.swift")
        XCTAssertEqual(optionValue.detail, "/tmp/companion.log")
    }

    func testMixedShellScriptsRemainCommands() {
        let projection = CodexMobileToolProjection.project(
            name: "exec_command",
            input: #"{"cmd":"rg -n Task Sources && swift test"}"#
        )

        XCTAssertEqual(projection.title, "Ran a command")
        XCTAssertEqual(projection.detail, "rg -n Task Sources && swift test")
    }

    func testPatchActivityExposesOnlyAffectedFilePaths() {
        let projection = CodexMobileToolProjection.project(
            name: "apply_patch",
            input: """
            *** Begin Patch
            *** Update File: Sources/App.swift
            @@
            -let oldValue = true
            +let oldValue = false
            *** Add File: Sources/Timeline.swift
            +struct Timeline {}
            *** End Patch
            """
        )

        XCTAssertEqual(projection.title, "Edited files")
        XCTAssertEqual(projection.detail, "Sources/App.swift\nSources/Timeline.swift")
        XCTAssertFalse(projection.detail?.contains("oldValue") == true)
    }

    func testPatchCompletionExposesChangeKeysWithoutFileContents() {
        let detail = CodexMobileToolProjection.editedFilePaths(fromChanges: [
            "/tmp/project/Sources/Timeline.swift": [
                "type": "add",
                "content": "struct PrivateImplementation {}",
            ],
            "/tmp/project/Sources/App.swift": [
                "type": "update",
                "content": "let secretValue = true",
            ],
        ])

        XCTAssertEqual(
            detail,
            "/tmp/project/Sources/App.swift\n/tmp/project/Sources/Timeline.swift"
        )
        XCTAssertFalse(detail?.contains("PrivateImplementation") == true)
        XCTAssertFalse(detail?.contains("secretValue") == true)
    }

    func testPatchToolOutputRecoversOnlyAffectedFilePaths() {
        let detail = CodexMobileToolProjection.editedFilePaths(
            fromToolOutput: """
            Success. Updated the following files:
            M Sources/App.swift
            A Sources/Timeline View.swift
            D Tests/LegacyTests.swift
            """
        )

        XCTAssertEqual(
            detail,
            "Sources/App.swift\nSources/Timeline View.swift\nTests/LegacyTests.swift"
        )
        XCTAssertNil(
            CodexMobileToolProjection.editedFilePaths(
                fromToolOutput: "M Sources/App.swift"
            )
        )
    }

    func testNestedComputerUseWrapperDefersToTheMCPEvent() {
        let projection = CodexMobileToolProjection.project(
            name: "exec",
            input: #"const result = await tools.mcp__node_repl__js({title:"Verify rebuilt timeline",code:`await sky.click({x: 10, y: 20})`});"#
        )

        XCTAssertEqual(projection.title, "Inspected an app")
        XCTAssertEqual(projection.detail, "Verify rebuilt timeline")
        XCTAssertTrue(projection.omitsWrapper)
    }

    func testDirectJavaScriptActivityKeepsOnlyItsProductLevelTitle() {
        let titled = CodexMobileToolProjection.project(
            name: "js",
            input: #"const result = await tools.mcp__node_repl__js({title:"Verify mobile text entry",code:`await sky.click({x: 10, y: 20})`});"#
        )
        let untitled = CodexMobileToolProjection.project(
            name: "js",
            input: #"await sky.click({ app: "Device Hub", x: 10, y: 20 })"#
        )

        XCTAssertEqual(titled.title, "Inspected an app")
        XCTAssertEqual(titled.detail, "Verify mobile text entry")
        XCTAssertFalse(titled.detail?.contains("await sky") == true)
        XCTAssertEqual(untitled.title, "Inspected an app")
        XCTAssertEqual(untitled.detail, "Device Hub")
        XCTAssertFalse(untitled.detail?.contains("await sky") == true)
    }

    func testDirectComputerUseToolKeepsItsProductLevelTitle() {
        let projection = CodexMobileToolProjection.project(
            name: "computer_use",
            input: #"{"app":"Device Hub","action":"inspect"}"#
        )

        XCTAssertEqual(projection.title, "Inspected an app")
        XCTAssertEqual(projection.detail, "Device Hub")
    }

    func testMCPServerContextProducesProductLevelActivities() {
        let computerUse = CodexMobileToolProjection.project(
            name: "get_app_state",
            input: #"{"app":"Device Hub"}"#,
            server: "computer-use"
        )
        let simulator = CodexMobileToolProjection.project(
            name: "build_run_sim",
            input: #"{"scheme":"CodexCompanionMobile"}"#,
            server: "xcodebuildmcp"
        )
        let integration = CodexMobileToolProjection.project(
            name: "fetch_record",
            input: #"{"title":"Current task"}"#,
            server: "example-connector"
        )

        XCTAssertEqual(computerUse.title, "Inspected an app")
        XCTAssertEqual(simulator.title, "Tested the app")
        XCTAssertEqual(integration.title, "Used an integration")
    }

    func testFirstPartyActionsUseCodexStyleVocabulary() {
        XCTAssertEqual(
            CodexMobileToolProjection.project(
                name: "exec",
                input: #"const result = await tools.update_plan({explanation:"Timeline rows are semantic",plan:[]});"#
            ),
            CodexMobileToolProjection(
                title: "Updated progress",
                detail: "Timeline rows are semantic",
                omitsWrapper: false
            )
        )
        XCTAssertEqual(
            CodexMobileToolProjection.project(name: "load_workspace_dependencies", input: nil).title,
            "Loaded tools"
        )
        XCTAssertEqual(
            CodexMobileToolProjection.project(name: "image_gen__imagegen", input: #"{"prompt":"Shadow waving"}"#).title,
            "Generated an image"
        )
        XCTAssertEqual(
            CodexMobileToolProjection.project(name: "web__run", input: #"{"q":"SwiftUI animation"}"#).title,
            "Searched the web"
        )
    }

    func testLoadedToolsKeepsOnlyTheNamedCapability() {
        let projection = CodexMobileToolProjection.project(
            name: "tool_search",
            input: #"{"query":"device screenshot tools","privateTransportState":"hidden"}"#
        )

        XCTAssertEqual(projection.title, "Loaded tools")
        XCTAssertEqual(projection.detail, "device screenshot tools")
        XCTAssertFalse(projection.detail?.contains("privateTransportState") == true)
    }

    func testToolInventoryJavaScriptWrapperUsesLoadedToolsWithoutRawSource() {
        let projection = CodexMobileToolProjection.project(
            name: "exec",
            input: """
            const matches = ALL_TOOLS.filter(({ name, description }) =>
                name.includes("device") || description.includes("screenshot")
            );
            text(matches);
            // Warning: truncated output
            // exec tool declaration
            """
        )

        XCTAssertEqual(projection.title, "Loaded tools")
        XCTAssertNil(projection.detail)
        XCTAssertFalse(projection.omitsWrapper)
    }

    func testDirectNodeJavaScriptInventoryUsesLoadedToolsWithoutRawSource() {
        let projection = CodexMobileToolProjection.project(
            name: "mcp__node_repl__js",
            input: #"{"code":"const hits = ALL_TOOLS.filter(tool => tool.name.includes('thread')); text(hits);"}"#
        )

        XCTAssertEqual(projection.title, "Loaded tools")
        XCTAssertNil(projection.detail)
        XCTAssertFalse(projection.omitsWrapper)
    }

    func testDirectNodeJavaScriptInventoryWithServerContextUsesLoadedTools() {
        let projection = CodexMobileToolProjection.project(
            name: "js",
            input: #"{"code":"const hits = ALL_TOOLS.filter(tool => tool.name.includes('thread')); text(hits);"}"#,
            server: "node_repl"
        )

        XCTAssertEqual(projection.title, "Loaded tools")
        XCTAssertNil(projection.detail)
        XCTAssertFalse(projection.omitsWrapper)
    }

    func testIntegrationDetailNamesTheConnectorWithoutRawArguments() {
        let projection = CodexMobileToolProjection.project(
            name: "fetch_record",
            input: #"{"title":"Current task","secretImplementation":"hidden"}"#,
            server: "example-connector"
        )

        XCTAssertEqual(projection.title, "Used an integration")
        XCTAssertEqual(projection.detail, "Example Connector - Current task")
        XCTAssertFalse(projection.detail?.contains("secretImplementation") == true)
    }

    func testUnknownStructuredToolPayloadKeepsOnlySafeSemanticFields() {
        let withPath = CodexMobileToolProjection.project(
            name: "future_tool",
            input: #"{"path":"/tmp/result.json","secretImplementation":"raw"}"#
        )
        let opaque = CodexMobileToolProjection.project(
            name: "future_tool",
            input: #"{"secretImplementation":"raw"}"#
        )

        XCTAssertEqual(withPath.title, "Used a tool")
        XCTAssertEqual(withPath.detail, "/tmp/result.json")
        XCTAssertNil(opaque.detail)
    }

    func testToolCallLifecycleWaitsForItsOutput() {
        XCTAssertEqual(CodexMobileToolLifecycle.callStatus(from: "completed"), .inProgress)
        XCTAssertEqual(CodexMobileToolLifecycle.callStatus(from: "failed"), .failed)
        XCTAssertEqual(
            CodexMobileToolLifecycle.resolvedStatus(
                callStatus: .inProgress,
                outputStatuses: []
            ),
            .inProgress
        )
        XCTAssertEqual(
            CodexMobileToolLifecycle.resolvedStatus(
                callStatus: .inProgress,
                outputStatuses: [.completed]
            ),
            .completed
        )
        XCTAssertEqual(
            CodexMobileToolLifecycle.resolvedStatus(
                callStatus: .inProgress,
                outputStatuses: [.failed]
            ),
            .failed
        )
    }

    func testUnknownNestedWrapperDoesNotExposeJavaScriptSource() {
        let projection = CodexMobileToolProjection.project(
            name: "exec",
            input: #"const result = await tools.future_tool({secretImplementation:"raw wrapper code"});"#
        )

        XCTAssertEqual(projection.title, "Used a tool")
        XCTAssertNil(projection.detail)
    }

    func testNestedAgentMessageKeepsStructuredDelegationMetadata() throws {
        let projection = CodexMobileToolProjection.project(
            name: "exec",
            input: #"const result = await tools.send_input({target:"agent-123",message:"Audit the mobile timeline"});"#
        )

        XCTAssertEqual(projection.title, "Messaged an agent")
        XCTAssertFalse(projection.omitsWrapper)
        let data = try XCTUnwrap(projection.detail?.data(using: .utf8))
        let object = try XCTUnwrap(
            JSONSerialization.jsonObject(with: data) as? [String: String]
        )
        XCTAssertEqual(object["target"], "agent-123")
        XCTAssertEqual(object["message"], "Audit the mobile timeline")
    }

    func testArchiveKeepsReasoningSeparateFromSemanticToolRows() throws {
        let fileManager = FileManager.default
        let home = fileManager.temporaryDirectory
            .appendingPathComponent("CodexMobileTaskArchiveTests-\(UUID().uuidString)", isDirectory: true)
        let codexDirectory = home.appendingPathComponent(".codex", isDirectory: true)
        try fileManager.createDirectory(at: codexDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: home) }

        let rolloutURL = home.appendingPathComponent("rollout.jsonl")
        let records: [[String: Any]] = [
            [
                "timestamp": "2026-07-17T15:00:00.000Z",
                "type": "event_msg",
                "payload": [
                    "type": "agent_reasoning",
                    "text": "**Inspecting source**",
                ],
            ],
            toolCallRecord(
                id: "read-record",
                callID: "read-call",
                input: #"const result = await tools.exec_command({cmd:"sed -n '1,40p' Sources/App.swift"});"#
            ),
            toolOutputRecord(callID: "read-call"),
            toolCallRecord(
                id: "command-record",
                callID: "command-call",
                input: #"const result = await tools.exec_command({cmd:"python3 scripts/audit.py"});"#
            ),
            toolOutputRecord(callID: "command-call"),
        ]
        let rollout = try records.reduce(into: Data()) { data, record in
            data.append(try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys]))
            data.append(0x0A)
        }
        try rollout.write(to: rolloutURL, options: .atomic)

        let databaseURL = codexDirectory.appendingPathComponent("state_5.sqlite")
        let escapedRolloutPath = rolloutURL.path.replacingOccurrences(of: "'", with: "''")
        let sqlite = try CodexSQLiteProcessRunner.run(
            executableURL: URL(fileURLWithPath: "/usr/bin/sqlite3"),
            arguments: [
                databaseURL.path,
                "CREATE TABLE threads (id TEXT PRIMARY KEY, rollout_path TEXT NOT NULL); "
                    + "INSERT INTO threads VALUES ('thread-1', '\(escapedRolloutPath)');",
            ]
        )
        XCTAssertEqual(
            sqlite.terminationStatus,
            0,
            String(decoding: sqlite.standardError, as: UTF8.self)
        )

        let archive = CodexMobileTaskArchive(homeDirectory: home)
        let page = try archive.timeline(threadID: "thread-1", cursor: nil, limit: 20)

        XCTAssertEqual(page.items.map(\.kind), [.reasoning, .tool, .tool])
        XCTAssertEqual(page.items.map(\.title), ["Inspecting source", "Read files", "Ran a command"])
        XCTAssertEqual(page.items.map(\.status), [.completed, .completed, .completed])
        XCTAssertNil(page.items[0].detail)
        XCTAssertEqual(page.items[1].detail, "Sources/App.swift")
        XCTAssertEqual(page.items[2].detail, "python3 scripts/audit.py")
    }

    private func toolCallRecord(id: String, callID: String, input: String) -> [String: Any] {
        [
            "timestamp": "2026-07-17T15:00:01.000Z",
            "type": "response_item",
            "payload": [
                "type": "custom_tool_call",
                "id": id,
                "call_id": callID,
                "name": "exec",
                "status": "completed",
                "input": input,
            ],
        ]
    }

    private func toolOutputRecord(callID: String) -> [String: Any] {
        [
            "timestamp": "2026-07-17T15:00:02.000Z",
            "type": "response_item",
            "payload": [
                "type": "custom_tool_call_output",
                "call_id": callID,
                "output": "",
            ],
        ]
    }
}
