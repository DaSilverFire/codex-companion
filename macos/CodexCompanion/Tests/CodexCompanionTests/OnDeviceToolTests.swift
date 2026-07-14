import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct OnDeviceToolTests {
    @Test
    func calculatorHonorsOperatorPrecedence() throws {
        #expect(try CompanionMathEvaluator.evaluate("2 + 3 * 4") == 14)
    }

    @Test
    func calculatorSupportsRootsAndPowers() throws {
        let value = try CompanionMathEvaluator.evaluate("sqrt(5092) + 2^3")

        #expect(abs(value - (sqrt(5092) + 8)) < 0.000_000_001)
    }

    @Test
    func calculatorAcceptsCommonMathSymbols() throws {
        #expect(try CompanionMathEvaluator.evaluate("6 × 7 − 2 ÷ 2") == 41)
    }

    @Test
    func calculatorRejectsDivisionByZero() {
        #expect(throws: CompanionMathEvaluator.EvaluationError.divisionByZero) {
            try CompanionMathEvaluator.evaluate("10 / 0")
        }
    }
}
