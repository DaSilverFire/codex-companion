import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexControlModelsTests {
    @Test
    func decodesEveryGoalStatusPublishedByAppServer() throws {
        let statuses: [CodexGoalStatus] = [
            .active,
            .paused,
            .blocked,
            .usageLimited,
            .budgetLimited,
            .complete,
        ]

        for status in statuses {
            let json = """
            {
              "threadId": "thread-1",
              "objective": "Finish the feature",
              "status": "\(status.rawValue)",
              "tokenBudget": null,
              "tokensUsed": 42,
              "timeUsedSeconds": 90,
              "createdAt": 100,
              "updatedAt": 200
            }
            """
            let goal = try JSONDecoder().decode(
                CodexGoalSnapshot.self,
                from: Data(json.utf8)
            )

            #expect(goal.status == status)
            #expect(goal.threadID == "thread-1")
            #expect(goal.objective == "Finish the feature")
        }
    }

    @Test
    func decodesDetailedAndCountOnlyResetSummaries() throws {
        let detailedJSON = """
        {
          "availableCount": 2,
          "credits": [
            {
              "id": "reset-credit-2",
              "resetType": "codexRateLimits",
              "status": "available",
              "grantedAt": 100,
              "expiresAt": 200,
              "title": "Referral reset",
              "description": "Resets Codex rate limits"
            }
          ]
        }
        """
        let detailed = try JSONDecoder().decode(
            CodexRateLimitResetCreditsSummary.self,
            from: Data(detailedJSON.utf8)
        )

        #expect(detailed.availableCount == 2)
        #expect(detailed.credits?.first?.id == "reset-credit-2")
        #expect(detailed.credits?.first?.resetType == .codexRateLimits)
        #expect(detailed.credits?.first?.status == .available)
        #expect(detailed.credits?.first?.title == "Referral reset")

        let countOnly = try JSONDecoder().decode(
            CodexRateLimitResetCreditsSummary.self,
            from: Data(#"{"availableCount":3,"credits":null}"#.utf8)
        )
        #expect(countOnly.availableCount == 3)
        #expect(countOnly.credits == nil)
    }

    @Test
    func buildsGoalGetAndResumeRequestsWithoutChangingObjective() throws {
        let get = CodexControlRequestFactory.goalGet(id: 2, threadID: "thread-2")
        #expect(get.method == "thread/goal/get")
        #expect(get.params["threadId"] as? String == "thread-2")

        let resume = CodexControlRequestFactory.goalSet(
            id: 3,
            threadID: "thread-2",
            objective: nil,
            status: .active,
            tokenBudget: nil
        )
        #expect(resume.method == "thread/goal/set")
        #expect(resume.params["threadId"] as? String == "thread-2")
        #expect(resume.params["status"] as? String == "active")
        #expect(resume.params["objective"] == nil)
        #expect(resume.params["tokenBudget"] == nil)
    }

    @Test
    func objectiveEditMutationDoesNotOverwriteStatusOrBudget() throws {
        let mutation = CodexGoalMutation.editObjective("Updated objective")
        let request = CodexControlRequestFactory.goalSet(
            id: 7,
            threadID: "thread-7",
            objective: mutation.objective,
            status: mutation.status,
            tokenBudget: mutation.tokenBudget
        )

        #expect(request.params["objective"] as? String == "Updated objective")
        #expect(request.params["status"] == nil)
        #expect(request.params["tokenBudget"] == nil)
    }

    @Test
    func resetConsumeRequestRequiresSelectedCreditAndIdempotencyKey() throws {
        let request = CodexControlRequestFactory.consumeReset(
            id: 9,
            creditID: "chosen-credit",
            idempotencyKey: "attempt-uuid"
        )

        #expect(request.method == "account/rateLimitResetCredit/consume")
        #expect(request.params["creditId"] as? String == "chosen-credit")
        #expect(request.params["idempotencyKey"] as? String == "attempt-uuid")
    }

    @Test
    func capabilityRequestsUseCurrentAppServerMethodsAndWorkspace() {
        let models = CodexControlRequestFactory.modelsList(id: 2)
        #expect(models.method == "model/list")
        #expect(models.params["includeHidden"] as? Bool == false)

        let skills = CodexControlRequestFactory.skillsList(id: 3, cwd: "/workspace")
        #expect(skills.method == "skills/list")
        #expect(skills.params["cwds"] as? [String] == ["/workspace"])

        let plugins = CodexControlRequestFactory.pluginsList(id: 4, cwd: "/workspace")
        #expect(plugins.method == "plugin/list")
        #expect(plugins.params["cwds"] as? [String] == ["/workspace"])
        #expect(plugins.params["marketplaceKinds"] as? [String] == ["local"])
    }

    @Test
    func capabilityParsersKeepVisibleEnabledInstalledEntries() throws {
        let models = CodexAppServerCapabilityService.parseModels([
            "data": [
                [
                    "id": "gpt-current",
                    "model": "gpt-current",
                    "displayName": "GPT Current",
                    "description": "Current model",
                    "hidden": false,
                    "isDefault": true,
                    "defaultReasoningEffort": "high",
                    "supportedReasoningEfforts": [
                        ["reasoningEffort": "high", "description": "Deep reasoning"],
                    ],
                ],
                [
                    "id": "hidden",
                    "model": "hidden",
                    "hidden": true,
                ],
            ],
        ])
        #expect(models.map(\.id) == ["gpt-current"])
        #expect(models.first?.supportedReasoningEfforts.map(\.value) == ["high"])

        let skills = CodexAppServerCapabilityService.parseSkills([
            "data": [[
                "skills": [
                    [
                        "name": "design-and-build",
                        "description": "Build interfaces",
                        "path": "/skills/design-and-build/SKILL.md",
                        "scope": "user",
                        "enabled": true,
                        "interface": ["displayName": "Design and Build"],
                    ],
                    [
                        "name": "disabled",
                        "path": "/skills/disabled/SKILL.md",
                        "enabled": false,
                    ],
                ],
            ]],
        ])
        #expect(skills.map(\.name) == ["design-and-build"])

        let plugins = CodexAppServerCapabilityService.parsePlugins([
            "marketplaces": [[
                "plugins": [
                    [
                        "id": "companion",
                        "name": "codex-companion",
                        "enabled": true,
                        "installed": true,
                        "interface": ["displayName": "Codex Companion"],
                    ],
                    [
                        "id": "available",
                        "name": "available",
                        "enabled": false,
                        "installed": false,
                    ],
                ],
            ]],
        ])
        #expect(plugins.map(\.id) == ["companion"])
        #expect(plugins.first?.enabled == true)
    }
}
