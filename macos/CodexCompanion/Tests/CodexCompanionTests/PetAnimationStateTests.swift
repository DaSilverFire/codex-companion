import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct PetAnimationStateTests {
    @Test
    func companionConversationStatesAppendWithoutMovingExistingRows() {
        #expect(PetAnimationState.goalComplete.rowIndex == 9)
        #expect(PetAnimationState.thinking.rowIndex == 10)
        #expect(PetAnimationState.talking.rowIndex == 11)
        #expect(PetAnimationState.thinking.rawValue == "thinking")
        #expect(PetAnimationState.talking.rawValue == "talking")
    }

    @Test
    func conversationStatesLoopForAsLongAsTheirRuntimeStateIsActive() {
        #expect(PetAnimationState.thinking.loopsContinuously)
        #expect(PetAnimationState.talking.loopsContinuously)
        #expect(!PetAnimationState.waving.loopsContinuously)
    }

    @Test
    func sixteenFrameConversationCyclesRemainReadable() {
        let thinking = PetAnimationState.thinking.frameTiming(frameCount: 16)
        let talking = PetAnimationState.talking.frameTiming(frameCount: 16)

        #expect(thinking.base >= 0.08)
        #expect(thinking.base > talking.base)
        #expect(talking.base >= 0.055)
    }

    @Test
    func conversationStatesUseNativeRowsAndDefaultToSixteenFrames() {
        let pet = Self.pet(rows: 12, frameCounts: [:])

        #expect(pet.resolvedAnimationState(for: .thinking) == .thinking)
        #expect(pet.resolvedAnimationState(for: .talking) == .talking)
        #expect(pet.frameCount(for: .thinking) == 16)
        #expect(pet.frameCount(for: .talking) == 16)
    }

    @Test
    func shorterAtlasesPreserveThePreviousRunningAndReviewFallbacks() {
        let pet = Self.pet(rows: 10, frameCounts: [
            PetAnimationState.running.rawValue: 7,
            PetAnimationState.review.rawValue: 5,
        ])

        #expect(pet.resolvedAnimationState(for: .thinking) == .running)
        #expect(pet.resolvedAnimationState(for: .talking) == .review)
        #expect(pet.frameCount(for: .thinking) == 7)
        #expect(pet.frameCount(for: .talking) == 5)
    }

    @Test
    func processAndAttentionMappingsUseConversationAnimations() {
        #expect(CompanionAppModel.animationState(
            for: CodexProcessItem.Status.running
        ) == .thinking)
        #expect(CompanionAppModel.animationState(
            for: PetAttentionMessage.Kind.response
        ) == .talking)
        #expect(CompanionAppModel.animationState(
            for: PetAttentionMessage.Kind.goal
        ) == .running)
    }

    private static func pet(
        rows: Int,
        frameCounts: [String: Int]
    ) -> PetDefinition {
        PetDefinition(
            id: "test-pet",
            displayName: "Test Pet",
            description: "Test fixture",
            spritesheetURL: URL(fileURLWithPath: "/tmp/test-pet.webp"),
            spriteColumns: 16,
            spriteRows: rows,
            animationFrameCounts: frameCounts,
            source: .builtIn(URL(fileURLWithPath: "/tmp"))
        )
    }
}
