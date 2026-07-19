import Foundation

struct CodexMobileToolProjection: Equatable, Sendable {
    var title: String
    var detail: String?
    var omitsWrapper: Bool

    static func project(
        name: String,
        input: String?,
        server: String? = nil
    ) -> CodexMobileToolProjection {
        let normalizedName = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let isExecWrapper = normalizedName == "exec"
        let isJavaScriptWrapper = normalizedName == "js"
            || normalizedName.hasSuffix("__js")
            || normalizedName.hasSuffix(".js")
        let isTransportWrapper = isExecWrapper || isJavaScriptWrapper
        let nestedName = isExecWrapper ? nestedToolName(in: input) : nil
        let effectiveName = nestedName
            ?? (isTransportWrapper && looksLikeToolInventoryPayload(input) ? "tool_search" : normalizedName)
        let title = semanticTitle(name: effectiveName, input: input, server: server)
        let detail = semanticDetail(
            for: title,
            name: effectiveName,
            input: input,
            server: server,
            isWrapper: isTransportWrapper
        )

        return CodexMobileToolProjection(
            title: title,
            detail: detail,
            omitsWrapper: isExecWrapper && effectiveName.hasPrefix("mcp__")
        )
    }

    static func editedFilePaths(fromChanges rawChanges: Any?) -> String? {
        guard let changes = rawChanges as? [String: Any] else { return nil }
        let paths = changes.keys
            .compactMap(nonempty)
            .sorted { $0.localizedStandardCompare($1) == .orderedAscending }
        return paths.isEmpty ? nil : paths.joined(separator: "\n")
    }

    static func editedFilePaths(fromToolOutput rawOutput: String?) -> String? {
        guard let rawOutput = rawOutput?.trimmingCharacters(in: .whitespacesAndNewlines),
              let markerRange = rawOutput.range(
                  of: "Updated the following files:",
                  options: [.caseInsensitive]
              )
        else { return nil }

        var paths: [String] = []
        for rawLine in rawOutput[markerRange.upperBound...].split(
            separator: "\n",
            omittingEmptySubsequences: true
        ) {
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            guard let status = line.first,
                  "MADRC?".contains(status),
                  line.dropFirst().first?.isWhitespace == true
            else { continue }

            let path = line.dropFirst()
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard !path.isEmpty, !paths.contains(path) else { continue }
            paths.append(path)
        }
        return paths.isEmpty ? nil : paths.joined(separator: "\n")
    }

    private static func semanticTitle(
        name: String,
        input: String?,
        server: String?
    ) -> String {
        let normalized = name.lowercased()
        let leaf = normalized.split(separator: "__").last.map(String.init) ?? normalized
        let normalizedServer = server?.lowercased()

        if leaf == "tool_search" || leaf == "load_workspace_dependencies"
            || leaf == "list_mcp_resources" || leaf == "list_mcp_resource_templates" {
            return "Loaded tools"
        }

        if let normalizedServer {
            if normalizedServer == "computer-use" || normalizedServer == "node_repl" {
                return "Inspected an app"
            }
            if normalizedServer == "xcodebuildmcp" {
                if leaf.contains("screenshot") { return "Viewed an image" }
                if leaf.contains("snapshot") || leaf.contains("inspect") { return "Inspected an app" }
                if leaf.contains("build") || leaf.contains("launch") || leaf.contains("test") {
                    return "Tested the app"
                }
                return "Used an integration"
            }
            return "Used an integration"
        }

        if normalized.hasPrefix("mcp__node_repl__")
            || normalized.hasPrefix("mcp__computer_use__")
            || leaf == "computer_use"
            || leaf == "js"
            || ["click", "get_app_state", "list_apps", "set_value", "type_text"].contains(leaf) {
            return "Inspected an app"
        }
        if ["exec", "exec_command", "write_stdin"].contains(leaf) {
            return commandTitle(from: input) ?? "Ran a command"
        }
        if leaf == "view_image" || leaf.contains("screenshot") {
            return "Viewed an image"
        }
        if leaf == "apply_patch" || leaf.contains("patch") {
            return "Edited files"
        }
        if leaf == "find" || leaf == "rg" || leaf.contains("search") {
            return "Searched files"
        }
        if leaf == "spawn_agent" || leaf == "send_input" {
            return "Messaged an agent"
        }
        if leaf == "close_agent" || leaf == "resume_agent" {
            return "Managed an agent"
        }
        if leaf == "wait_agent" || leaf == "wait" {
            return "Wait"
        }
        if leaf.contains("read") && leaf.contains("file") {
            return "Read a file"
        }
        if leaf == "open" && normalized.contains("browser") {
            return "Opened a link"
        }
        if leaf == "update_plan" {
            return "Updated progress"
        }
        if leaf == "get_goal" {
            return "Checked the goal"
        }
        if leaf == "create_goal" || leaf == "update_goal" {
            return "Updated the goal"
        }
        if normalized.contains("image_gen") || leaf == "imagegen" {
            return "Generated an image"
        }
        if normalized.hasPrefix("web__") || leaf == "web_run" {
            return "Searched the web"
        }
        if leaf == "request_user_input" {
            return "Asked a question"
        }
        if leaf.contains("automation") {
            return "Updated an automation"
        }
        if normalized.hasPrefix("mcp__") || leaf == "read_mcp_resource" {
            return "Used an integration"
        }
        return "Used a tool"
    }

    private static func semanticDetail(
        for title: String,
        name: String,
        input: String?,
        server: String?,
        isWrapper: Bool
    ) -> String? {
        guard let input = input?.trimmingCharacters(in: .whitespacesAndNewlines),
              !input.isEmpty
        else { return nil }

        if title == "Messaged an agent" || title == "Managed an agent" {
            return agentDetail(from: input, isWrapper: isWrapper)
        }
        if title == "Edited files", let paths = editedFilePaths(from: input) {
            return paths
        }
        if title == "Inspected an app" {
            // Computer Use and node_repl payloads contain implementation code,
            // coordinates, and transport wrappers. Codex presents the named
            // app action instead of exposing that source to the timeline.
            return appInspectionDetail(from: input)
        }
        if title == "Read files",
           let command = commandText(from: input),
           let paths = readFilePaths(from: command) {
            return paths.joined(separator: "\n")
        }

        let keys: [String]
        switch title {
        case "Ran a command":
            keys = ["cmd"]
        case "Read a file", "Read files":
            keys = ["cmd", "path", "file", "workdir"]
        case "Edited files":
            keys = ["path", "file"]
        case "Searched files":
            keys = ["cmd", "q", "query", "pattern"]
        case "Searched the web":
            keys = ["q", "query", "pattern"]
        case "Viewed an image":
            keys = ["path", "url", "title"]
        case "Inspected an app":
            keys = ["title", "app"]
        case "Built the app", "Tested the app":
            keys = ["cmd", "title", "scheme", "app"]
        case "Opened a link":
            keys = ["url", "ref_id"]
        case "Updated progress":
            keys = ["explanation"]
        case "Checked the goal", "Updated the goal":
            keys = ["objective", "status"]
        case "Generated an image":
            keys = ["prompt"]
        case "Asked a question":
            keys = ["question"]
        case "Loaded tools":
            keys = ["query", "q", "name", "server", "uri"]
        case "Used an integration", "Updated an automation":
            keys = ["title", "q", "query", "prompt", "url", "name"]
        default:
            keys = ["title", "path", "file", "q", "query", "url", "name"]
        }

        let semanticValue = jsonObject(from: input)
            .flatMap { firstStringValue(for: keys, in: $0) }
            ?? firstStringLiteral(for: keys, in: input)

        if title == "Used an integration" {
            return integrationDetail(
                name: name,
                server: server,
                semanticValue: semanticValue
            )
        }
        if let semanticValue { return semanticValue }

        // Transport wrappers and structured argument objects are implementation
        // data. Keep only plain user-facing text when no semantic field exists.
        guard !isWrapper, title != "Edited files" else { return nil }
        return safePlainTextDetail(from: input)
    }

    private static func commandTitle(from input: String?) -> String? {
        guard let command = commandText(from: input) else { return nil }
        let normalized = command
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()

        // Mixed shell scripts should remain commands; assigning one semantic
        // label to several unrelated operations would be misleading.
        guard !normalized.contains("\n"),
              !normalized.contains(" && "),
              !normalized.contains(" || "),
              !normalized.contains(";"),
              !normalized.contains(" | ")
        else { return nil }

        let unwrapped = normalized.replacingOccurrences(
            of: #"^env(?:\s+[A-Za-z_][A-Za-z0-9_]*=(?:'[^']*'|\"[^\"]*\"|\S+))*\s+"#,
            with: "",
            options: .regularExpression
        )

        if matchesCommand(
            unwrapped,
            pattern: #"^(?:/usr/bin/|/bin/)?(?:cat|sed|head|tail|nl|wc|stat|file)\b"#
        ) || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?defaults\s+read\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?plutil\s+-p\b"#) {
            return "Read files"
        }

        if matchesCommand(
            unwrapped,
            pattern: #"^(?:/usr/bin/|/bin/)?(?:rg|grep|find|fd|ls)\b"#
        ) || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?git\s+(?:grep|ls-files)\b"#) {
            return "Searched files"
        }

        if matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?xcodebuild\b.*\btest\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?swift\s+test\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?(?:pytest|cargo\s+test|go\s+test)\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?python(?:3)?\s+-m\s+pytest\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:npm|pnpm|yarn)\s+(?:run\s+)?test\b"#) {
            return "Tested the app"
        }

        if matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?xcodebuild\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?swift\s+build\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:npm|pnpm|yarn)\s+(?:run\s+)?build\b"#)
            || matchesCommand(unwrapped, pattern: #"^(?:/usr/bin/)?cargo\s+build\b"#) {
            return "Built the app"
        }

        return nil
    }

    private static func commandText(from input: String?) -> String? {
        guard let input else { return nil }
        if let object = jsonObject(from: input),
           let command = firstStringValue(for: ["cmd"], in: object) {
            return command
        }
        return firstStringLiteral(for: ["cmd"], in: input)
    }

    private static func readFilePaths(from command: String) -> [String]? {
        let words = shellWords(in: command)
        guard !words.isEmpty else { return nil }

        var commandIndex = 0
        if URL(fileURLWithPath: words[0]).lastPathComponent == "env" {
            commandIndex += 1
            while commandIndex < words.count,
                  words[commandIndex].range(
                      of: #"^[A-Za-z_][A-Za-z0-9_]*="#,
                      options: .regularExpression
                  ) != nil {
                commandIndex += 1
            }
        }
        guard commandIndex < words.count else { return nil }

        let executable = URL(fileURLWithPath: words[commandIndex]).lastPathComponent
        let arguments = Array(words.dropFirst(commandIndex + 1))
        let candidates: [String]

        switch executable {
        case "cat", "nl", "wc", "plutil":
            candidates = positionalArguments(
                in: arguments,
                optionsWithValues: executable == "plutil" ? [] : ["-w"]
            )
        case "head", "tail":
            candidates = positionalArguments(
                in: arguments,
                optionsWithValues: ["-n", "--lines", "-c", "--bytes"]
            )
        case "stat":
            candidates = positionalArguments(
                in: arguments,
                optionsWithValues: ["-f", "--format", "-t"]
            )
        case "file":
            candidates = positionalArguments(
                in: arguments,
                optionsWithValues: ["-e", "--exclude", "-m", "--magic-file"]
            )
        case "sed":
            candidates = sedInputPaths(in: arguments)
        default:
            return nil
        }

        var paths: [String] = []
        for candidate in candidates {
            let trimmed = candidate.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty,
                  trimmed != "-",
                  !trimmed.hasPrefix(">"),
                  !trimmed.hasPrefix("<"),
                  !trimmed.contains(">/dev/null"),
                  !paths.contains(trimmed)
            else { continue }
            paths.append(trimmed)
        }
        return paths.isEmpty ? nil : paths
    }

    private static func sedInputPaths(in arguments: [String]) -> [String] {
        var values: [String] = []
        var index = 0
        var hasExplicitProgram = false
        var consumedImplicitProgram = false

        while index < arguments.count {
            let argument = arguments[index]
            if argument == "-e" || argument == "--expression" {
                hasExplicitProgram = true
                index += 2
                continue
            }
            if argument.hasPrefix("-e") && argument.count > 2 {
                hasExplicitProgram = true
                index += 1
                continue
            }
            if argument == "-f" || argument == "--file" {
                if index + 1 < arguments.count {
                    values.append(arguments[index + 1])
                }
                hasExplicitProgram = true
                index += 2
                continue
            }
            if argument.hasPrefix("-") {
                index += 1
                continue
            }
            if !hasExplicitProgram && !consumedImplicitProgram {
                consumedImplicitProgram = true
            } else {
                values.append(argument)
            }
            index += 1
        }
        return values
    }

    private static func positionalArguments(
        in arguments: [String],
        optionsWithValues: Set<String>
    ) -> [String] {
        var values: [String] = []
        var index = 0
        var acceptsOptions = true

        while index < arguments.count {
            let argument = arguments[index]
            if acceptsOptions && argument == "--" {
                acceptsOptions = false
                index += 1
                continue
            }
            if acceptsOptions && optionsWithValues.contains(argument) {
                index += 2
                continue
            }
            if acceptsOptions && argument.hasPrefix("-") {
                index += 1
                continue
            }
            values.append(argument)
            index += 1
        }
        return values
    }

    private static func shellWords(in source: String) -> [String] {
        var words: [String] = []
        var current = ""
        var quote: Character?
        var isEscaped = false

        func finishWord() {
            guard !current.isEmpty else { return }
            words.append(current)
            current = ""
        }

        for character in source {
            if isEscaped {
                current.append(character)
                isEscaped = false
                continue
            }
            if character == "\\" && quote != "'" {
                isEscaped = true
                continue
            }
            if let activeQuote = quote {
                if character == activeQuote {
                    quote = nil
                } else {
                    current.append(character)
                }
                continue
            }
            if character == "'" || character == "\"" {
                quote = character
            } else if character.isWhitespace {
                finishWord()
            } else {
                current.append(character)
            }
        }
        if isEscaped { current.append("\\") }
        finishWord()
        return words
    }

    private static func matchesCommand(_ command: String, pattern: String) -> Bool {
        command.range(of: pattern, options: .regularExpression) != nil
    }

    private static func editedFilePaths(from input: String) -> String? {
        var candidates = [input]
        if let object = jsonObject(from: input),
           let payload = firstStringValue(for: ["input", "patch"], in: object) {
            candidates.append(payload)
        }
        if let payload = firstStringLiteral(for: ["input", "patch"], in: input) {
            candidates.append(payload)
        }

        let pattern = #"(?m)^\*\*\* (?:Update|Add|Delete) File:\s*(.+?)\s*$"#
        guard let expression = try? NSRegularExpression(pattern: pattern) else { return nil }
        var paths: [String] = []

        for candidate in candidates {
            let matches = expression.matches(
                in: candidate,
                range: NSRange(candidate.startIndex..., in: candidate)
            )
            for match in matches {
                guard let range = Range(match.range(at: 1), in: candidate) else { continue }
                let path = String(candidate[range])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                guard !path.isEmpty, !paths.contains(path) else { continue }
                paths.append(path)
            }
        }

        return paths.isEmpty ? nil : paths.joined(separator: "\n")
    }

    private static func agentDetail(from input: String, isWrapper: Bool) -> String? {
        if jsonObject(from: input) != nil {
            return input
        }
        guard isWrapper else { return input }

        var object: [String: String] = [:]
        for key in ["target", "agent_type", "message", "prompt", "title"] {
            if let value = firstStringLiteral(for: [key], in: input) {
                object[key] = value
            }
        }
        guard !object.isEmpty,
              let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    private static func appInspectionDetail(from input: String) -> String? {
        if let object = jsonObject(from: input),
           let value = firstStringValue(for: ["title", "app"], in: object) {
            return value
        }
        return firstStringLiteral(for: ["title", "app"], in: input)
    }

    private static func integrationDetail(
        name: String,
        server: String?,
        semanticValue: String?
    ) -> String? {
        let components = name.split(separator: "__").map(String.init)
        let inferredServer = components.count >= 3 ? components[1] : nil
        let rawServer = server ?? inferredServer ?? ""
        let serverLabel = nonempty(rawServer).map { humanReadableIdentifier($0) }
        let toolLabel = components.last
            .map { humanReadableIdentifier($0) }
            .flatMap { nonempty($0) }

        var parts: [String] = []
        for candidate in [serverLabel, semanticValue, semanticValue == nil ? toolLabel : nil] {
            guard let candidate = candidate, !parts.contains(candidate) else { continue }
            parts.append(candidate)
        }
        return parts.isEmpty ? nil : parts.joined(separator: " - ")
    }

    private static func humanReadableIdentifier(_ value: String) -> String {
        value
            .replacingOccurrences(of: "_", with: " ")
            .replacingOccurrences(of: "-", with: " ")
            .split(separator: " ")
            .map { $0.prefix(1).uppercased() + $0.dropFirst() }
            .joined(separator: " ")
    }

    private static func safePlainTextDetail(from input: String) -> String? {
        let trimmed = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              !trimmed.hasPrefix("{"),
              !trimmed.hasPrefix("["),
              !trimmed.hasPrefix("const "),
              !trimmed.hasPrefix("let "),
              !trimmed.hasPrefix("var "),
              !trimmed.hasPrefix("await "),
              !trimmed.contains("tools.")
        else { return nil }
        return trimmed
    }

    private static func nestedToolName(in source: String?) -> String? {
        guard let source else { return nil }
        let pattern = #"tools\.([A-Za-z0-9_]+)\s*\("#
        guard let expression = try? NSRegularExpression(pattern: pattern),
              let match = expression.firstMatch(
                  in: source,
                  range: NSRange(source.startIndex..., in: source)
              ),
              let range = Range(match.range(at: 1), in: source)
        else { return nil }
        return String(source[range]).lowercased()
    }

    private static func looksLikeToolInventoryPayload(_ source: String?) -> Bool {
        guard let source else { return false }
        return source.lowercased().contains("all_tools")
    }

    private static func jsonObject(from source: String) -> [String: Any]? {
        guard let data = source.data(using: .utf8) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    private static func firstStringValue(
        for keys: [String],
        in object: [String: Any]
    ) -> String? {
        for key in keys {
            if let value = object[key] as? String,
               let value = nonempty(value) {
                return value
            }
        }
        return nil
    }

    private static func firstStringLiteral(
        for keys: [String],
        in source: String
    ) -> String? {
        for key in keys {
            guard let keyRange = source.range(
                of: #"\b"# + NSRegularExpression.escapedPattern(for: key) + #"\s*:\s*"#,
                options: .regularExpression
            ) else { continue }

            let suffix = source[keyRange.upperBound...]
            guard let quote = suffix.first,
                  quote == "\"" || quote == "'" || quote == "`"
            else { continue }

            var index = suffix.index(after: suffix.startIndex)
            var escaped = false
            while index < suffix.endIndex {
                let character = suffix[index]
                if escaped {
                    escaped = false
                } else if character == "\\" {
                    escaped = true
                } else if character == quote {
                    let rawValue = String(suffix[suffix.index(after: suffix.startIndex)..<index])
                    return nonempty(decodedLiteral(rawValue, quote: quote))
                }
                index = suffix.index(after: index)
            }
        }
        return nil
    }

    private static func decodedLiteral(_ value: String, quote: Character) -> String {
        if quote == "\"",
           let data = "\"\(value)\"".data(using: .utf8),
           let decoded = try? JSONDecoder().decode(String.self, from: data) {
            return decoded
        }
        return value
            .replacingOccurrences(of: "\\n", with: "\n")
            .replacingOccurrences(of: "\\t", with: "\t")
            .replacingOccurrences(of: "\\\(quote)", with: String(quote))
            .replacingOccurrences(of: "\\\\", with: "\\")
    }

    private static func nonempty(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}

enum CodexMobileToolLifecycle {
    static func callStatus(
        from raw: String?
    ) -> CompanionBridgeTimelineItemStatus {
        switch raw?.lowercased() {
        case "failed", "error", "errored": return .failed
        default: return .inProgress
        }
    }

    static func resolvedStatus(
        callStatus: CompanionBridgeTimelineItemStatus,
        outputStatuses: [CompanionBridgeTimelineItemStatus]
    ) -> CompanionBridgeTimelineItemStatus {
        if callStatus == .failed || outputStatuses.contains(.failed) {
            return .failed
        }
        return outputStatuses.isEmpty ? callStatus : .completed
    }
}
