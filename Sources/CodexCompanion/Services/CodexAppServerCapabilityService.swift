import Foundation

struct CodexAppServerCapabilityService: Sendable {
    var client = CodexAppServerRPCClient()

    func load(cwd: String?) throws -> CompanionBridgeCapabilities {
        let resolvedCWD = Self.nonempty(cwd)
            ?? FileManager.default.homeDirectoryForCurrentUser.path
        let responses = try client.perform([
            CodexControlRequestFactory.modelsList(id: 2),
            CodexControlRequestFactory.skillsList(id: 3, cwd: resolvedCWD),
            CodexControlRequestFactory.pluginsList(id: 4, cwd: resolvedCWD),
        ])
        return CompanionBridgeCapabilities(
            models: Self.parseModels(try result(for: 2, in: responses)),
            skills: Self.parseSkills(try result(for: 3, in: responses)),
            plugins: Self.parsePlugins(try result(for: 4, in: responses)),
            chatAgents: CompanionBridgeChatAgent.builtIns,
            chatModels: Self.chatModels(
                hasOpenAIKey: OpenAIAPIKeyStore().hasKey,
                hasLumoKey: LumoAPIKeyStore().hasKey
            )
        )
    }

    static func chatModels(
        hasOpenAIKey: Bool,
        hasLumoKey: Bool
    ) -> [CompanionBridgeChatModel] {
        let openAIModels = ChatGPTModel.allCases.map { model in
            CompanionBridgeChatModel(
                id: "openai:\(model.rawValue)",
                provider: .openAIAPI,
                model: model.rawValue,
                displayName: model.shortTitle,
                description: hasOpenAIKey ? model.costNote : "Add an OpenAI API key on the Mac",
                isDefault: false,
                isAvailable: hasOpenAIKey,
                supportsAttachments: false
            )
        }
        let lumoModels = LumoModel.allCases.map { model in
            CompanionBridgeChatModel(
                id: "lumo:\(model.rawValue)",
                provider: .lumoAPI,
                model: model.rawValue,
                displayName: model.title,
                description: hasLumoKey ? model.capabilityNote : "Add a Lumo API key on the Mac",
                isDefault: false,
                isAvailable: hasLumoKey,
                supportsAttachments: false
            )
        }
        return [.onDeviceDefault] + openAIModels + lumoModels
    }

    private func result(
        for id: Int,
        in responses: [Int: CodexRPCResponse]
    ) throws -> [String: Any] {
        guard let response = responses[id] else {
            throw CodexAppServerControlError.missingResponse(id)
        }
        if let error = response.error {
            throw CodexAppServerControlError.server(error)
        }
        guard let result = response.result else {
            throw CodexAppServerControlError.invalidResponse
        }
        return result
    }

    static func parseModels(_ result: [String: Any]) -> [CompanionBridgeModel] {
        guard let rows = result["data"] as? [[String: Any]] else { return [] }
        return rows.compactMap { row in
            guard
                row["hidden"] as? Bool != true,
                let id = nonempty(row["id"] as? String),
                let model = nonempty(row["model"] as? String)
            else { return nil }
            let efforts = (row["supportedReasoningEfforts"] as? [[String: Any]] ?? []).compactMap {
                option -> CompanionBridgeReasoningEffort? in
                guard let value = nonempty(option["reasoningEffort"] as? String) else { return nil }
                return .init(
                    value: value,
                    description: nonempty(option["description"] as? String) ?? value.capitalized
                )
            }
            return CompanionBridgeModel(
                id: id,
                model: model,
                displayName: nonempty(row["displayName"] as? String) ?? model,
                description: nonempty(row["description"] as? String) ?? "Codex model",
                isDefault: row["isDefault"] as? Bool ?? false,
                defaultReasoningEffort: nonempty(row["defaultReasoningEffort"] as? String)
                    ?? efforts.first?.value
                    ?? "medium",
                supportedReasoningEfforts: efforts
            )
        }
    }

    static func parseSkills(_ result: [String: Any]) -> [CompanionBridgeSkill] {
        guard let entries = result["data"] as? [[String: Any]] else { return [] }
        return entries.flatMap { entry -> [CompanionBridgeSkill] in
            (entry["skills"] as? [[String: Any]] ?? []).compactMap { row in
                guard
                    row["enabled"] as? Bool != false,
                    let name = nonempty(row["name"] as? String),
                    let path = nonempty(row["path"] as? String)
                else { return nil }
                let interface = row["interface"] as? [String: Any]
                return CompanionBridgeSkill(
                    name: name,
                    displayName: nonempty(interface?["displayName"] as? String) ?? name,
                    description: nonempty(interface?["shortDescription"] as? String)
                        ?? nonempty(row["shortDescription"] as? String)
                        ?? nonempty(row["description"] as? String)
                        ?? "Codex skill",
                    path: path,
                    scope: nonempty(row["scope"] as? String) ?? "user",
                    defaultPrompt: nonempty(interface?["defaultPrompt"] as? String)
                )
            }
        }.uniqued(by: \.path).sorted {
            $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }

    static func parsePlugins(_ result: [String: Any]) -> [CompanionBridgePlugin] {
        guard let marketplaces = result["marketplaces"] as? [[String: Any]] else { return [] }
        return marketplaces.flatMap { marketplace -> [CompanionBridgePlugin] in
            (marketplace["plugins"] as? [[String: Any]] ?? []).compactMap { row in
                guard
                    let id = nonempty(row["id"] as? String),
                    let name = nonempty(row["name"] as? String)
                else { return nil }
                let interface = row["interface"] as? [String: Any]
                return CompanionBridgePlugin(
                    id: id,
                    name: name,
                    displayName: nonempty(interface?["displayName"] as? String) ?? name,
                    description: nonempty(interface?["shortDescription"] as? String)
                        ?? nonempty(interface?["longDescription"] as? String)
                        ?? "Codex plugin",
                    enabled: row["enabled"] as? Bool ?? false,
                    installed: row["installed"] as? Bool ?? false
                )
            }
        }.uniqued(by: \.id).filter { $0.installed }.sorted {
            $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }

    private static func nonempty(_ value: String?) -> String? {
        let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return trimmed.isEmpty ? nil : trimmed
    }
}

private extension Array {
    func uniqued<Key: Hashable>(by keyPath: KeyPath<Element, Key>) -> [Element] {
        var seen: Set<Key> = []
        return filter { seen.insert($0[keyPath: keyPath]).inserted }
    }
}
