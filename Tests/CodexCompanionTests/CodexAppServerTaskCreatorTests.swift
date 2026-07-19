import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAppServerTaskCreatorTests {
    @Test
    func threadStartUsesARealPersistentThreadAndOptionalWorkspace() throws {
        let request = CodexAppServerTaskRequestFactory.threadStart(
            id: 2,
            cwd: "  /tmp/CodexCompanionProject  ",
            model: "  gpt-5.6-sol  "
        )
        let params = try #require(request["params"] as? [String: Any])

        #expect(request["id"] as? Int == 2)
        #expect(request["method"] as? String == "thread/start")
        #expect(params["ephemeral"] as? Bool == false)
        #expect(params["serviceName"] as? String == "codex-companion-mobile")
        #expect(params["cwd"] as? String == "/tmp/CodexCompanionProject")
        #expect(params["model"] as? String == "gpt-5.6-sol")
        #expect(params["approvalPolicy"] == nil)
    }

    @Test
    func blankWorkspaceIsOmittedInsteadOfSendingAnInvalidPath() throws {
        let request = CodexAppServerTaskRequestFactory.threadStart(id: 2, cwd: "  ")
        let params = try #require(request["params"] as? [String: Any])

        #expect(params["cwd"] == nil)
    }

    @Test
    func turnStartCarriesOnlyTheNewTaskPromptAndStableClientMessageID() throws {
        let request = CodexAppServerTaskRequestFactory.turnStart(
            id: 3,
            threadID: "thread-new",
            prompt: "Build the mobile bridge",
            clientMessageID: "message-stable"
        )
        let params = try #require(request["params"] as? [String: Any])
        let input = try #require(params["input"] as? [[String: Any]])
        let text = try #require(input.first)

        #expect(request["method"] as? String == "turn/start")
        #expect(params["threadId"] as? String == "thread-new")
        #expect(params["clientUserMessageId"] as? String == "message-stable")
        #expect(text["type"] as? String == "text")
        #expect(text["text"] as? String == "Build the mobile bridge")
    }

    @Test
    func turnStartCarriesNativeModelEffortAndSkillInputs() throws {
        let request = CodexAppServerTaskRequestFactory.turnStart(
            id: 3,
            threadID: "thread-new",
            prompt: "Build the mobile bridge",
            clientMessageID: "message-stable",
            model: "gpt-5.6-sol",
            reasoningEffort: "high",
            skillName: "design-and-build",
            skillPath: "/tmp/.codex/skills/design-and-build/SKILL.md"
        )
        let params = try #require(request["params"] as? [String: Any])
        let input = try #require(params["input"] as? [[String: Any]])
        let skill = try #require(input.last)

        #expect(params["model"] as? String == "gpt-5.6-sol")
        #expect(params["effort"] as? String == "high")
        #expect(skill["type"] as? String == "skill")
        #expect(skill["name"] as? String == "design-and-build")
        #expect(skill["path"] as? String == "/tmp/.codex/skills/design-and-build/SKILL.md")
    }

    @Test
    func turnStartCarriesImageAndFileAttachmentsAsNativeUserInputs() throws {
        let image = CodexFollowerAttachment(
            id: UUID(),
            kind: .image,
            label: "reference.png",
            path: "/tmp/reference.png",
            fsPath: "/tmp/reference.png",
            mimeType: "image/png"
        )
        let file = CodexFollowerAttachment(
            id: UUID(),
            kind: .file,
            label: "design.md",
            path: "/tmp/design.md",
            fsPath: "/tmp/design.md",
            mimeType: "text/markdown"
        )

        let request = CodexAppServerTaskRequestFactory.turnStart(
            id: 3,
            threadID: "thread-new",
            prompt: "Use these references",
            clientMessageID: "message-with-attachments",
            attachments: [image, file]
        )
        let params = try #require(request["params"] as? [String: Any])
        let input = try #require(params["input"] as? [[String: Any]])

        #expect(input.count == 3)
        #expect(input[1]["type"] as? String == "localImage")
        #expect(input[1]["path"] as? String == "/tmp/reference.png")
        #expect(input[2]["type"] as? String == "mention")
        #expect(input[2]["name"] as? String == "design.md")
        #expect(input[2]["path"] as? String == "/tmp/design.md")
        #expect(params["attachments"] == nil)
    }

    @Test
    func extractsCreatedThreadIDFromNativeResponse() {
        let response: [String: Any] = [
            "id": 2,
            "result": [
                "thread": ["id": "thread-created"],
            ],
        ]

        #expect(CodexAppServerTaskResponseParser.threadID(from: response) == "thread-created")
        #expect(CodexAppServerTaskResponseParser.threadID(from: ["result": [:]]) == nil)
    }
}
