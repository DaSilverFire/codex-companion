import Testing
@testable import CodexCompanion

@Suite
struct ChatGPTModelTests {
    @Test
    func currentCatalogUsesGPT56Tiers() {
        #expect(ChatGPTModel.allCases == [.gpt56Luna, .gpt56Terra, .gpt56Sol])
        #expect(ChatGPTModel.allCases.map(\.title) == [
            "GPT-5.6 Luna",
            "GPT-5.6 Terra",
            "GPT-5.6 Sol",
        ])
        #expect(ChatGPTModel.allCases.map(\.apiModelID) == [
            "gpt-5.6-luna",
            "gpt-5.6-terra",
            "gpt-5.6-sol",
        ])
        #expect(ChatGPTModel.allCases.map(\.reasoningEffort) == ["low", "high", "xhigh"])
    }

    @Test(arguments: [
        ("gpt55", ChatGPTModel.gpt56Luna),
        ("gpt55Thinking", ChatGPTModel.gpt56Terra),
        ("gpt55Pro", ChatGPTModel.gpt56Sol),
        ("gpt56Luna", ChatGPTModel.gpt56Luna),
        ("gpt56Terra", ChatGPTModel.gpt56Terra),
        ("gpt56Sol", ChatGPTModel.gpt56Sol),
    ])
    func persistedSelectionsRestoreToCurrentCatalog(rawValue: String, expected: ChatGPTModel) {
        #expect(ChatGPTModel.restoringPersistedSelection(rawValue) == expected)
    }

    @Test
    func missingOrUnknownSelectionsUseEfficientDefault() {
        #expect(ChatGPTModel.restoringPersistedSelection(nil) == .gpt56Luna)
        #expect(ChatGPTModel.restoringPersistedSelection("unknown") == .gpt56Luna)
    }

    @Test
    func deliveryCatalogRemovesAppHandoffAndMigratesItsSavedPreference() {
        #expect(ChatGPTDeliveryMode.allCases == [.onDevice, .openAIAPI, .lumoAPI])
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("onDevice") == .onDevice)
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("openAIAPI") == .openAIAPI)
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("lumoAPI") == .lumoAPI)
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("appHandoff") == .onDevice)
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection(nil) == .onDevice)
        #expect(ChatGPTDeliveryMode.restoringPersistedSelection("unknown") == .onDevice)
    }
}
