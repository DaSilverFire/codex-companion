import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexSharedThreadRuntimeTests {
    @Test
    func parsesApprovalAndUserInputFlagsBeforeGenericActiveState() throws {
        let message: [String: Any] = [
            "result": [
                "data": [
                    [
                        "id": "approval",
                        "status": [
                            "type": "active",
                            "activeFlags": ["waitingOnApproval"],
                        ],
                    ],
                    [
                        "id": "input",
                        "status": [
                            "type": "active",
                            "activeFlags": ["waitingOnUserInput"],
                        ],
                    ],
                    [
                        "id": "running",
                        "status": [
                            "type": "active",
                            "activeFlags": [],
                        ],
                    ],
                ],
            ],
        ]

        let statuses = try #require(CodexSharedThreadStatusParser.statuses(from: message))

        #expect(statuses["approval"] == .waitingOnApproval)
        #expect(statuses["input"] == .waitingOnUserInput)
        #expect(statuses["running"] == .active)
    }
}
