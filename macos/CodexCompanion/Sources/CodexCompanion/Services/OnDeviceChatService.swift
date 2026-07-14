import Foundation

#if canImport(FoundationModels)
import FoundationModels
#endif

protocol OnDeviceChatServing: Sendable {
    func prewarm() async
    func send(prompt: String) async throws -> String
}

enum OnDeviceChatError: LocalizedError {
    case unavailable
    case emptyResponse
    case busy

    var errorDescription: String? {
        switch self {
        case .unavailable:
            return "The on-device Apple model is not available on this Mac right now."
        case .emptyResponse:
            return "The on-device model returned an empty response."
        case .busy:
            return "The on-device model is already answering another message."
        }
    }
}

struct UnavailableOnDeviceChatService: OnDeviceChatServing {
    func prewarm() async {}

    func send(prompt: String) async throws -> String {
        throw OnDeviceChatError.unavailable
    }
}

enum OnDeviceChatServiceFactory {
    static func make() -> any OnDeviceChatServing {
        #if canImport(FoundationModels)
        if #available(macOS 26.0, *) {
            return AppleOnDeviceChatService()
        }
        #endif
        return UnavailableOnDeviceChatService()
    }
}

enum CompanionMathEvaluator {
    enum EvaluationError: LocalizedError, Equatable {
        case expressionTooLong
        case invalidExpression
        case divisionByZero
        case unsupportedFunction(String)
        case nonFiniteResult

        var errorDescription: String? {
            switch self {
            case .expressionTooLong:
                return "The expression is too long."
            case .invalidExpression:
                return "The expression is not valid."
            case .divisionByZero:
                return "The expression divides by zero."
            case .unsupportedFunction(let name):
                return "The function \(name) is not supported."
            case .nonFiniteResult:
                return "The expression does not have a finite result."
            }
        }
    }

    static func evaluate(_ expression: String) throws -> Double {
        let normalized = expression
            .replacingOccurrences(of: "×", with: "*")
            .replacingOccurrences(of: "÷", with: "/")
            .replacingOccurrences(of: "−", with: "-")
            .replacingOccurrences(of: "√(", with: "sqrt(")
        guard normalized.utf8.count <= 256 else {
            throw EvaluationError.expressionTooLong
        }

        var parser = Parser(characters: Array(normalized))
        let result = try parser.parseExpression()
        parser.skipWhitespace()
        guard parser.isAtEnd else {
            throw EvaluationError.invalidExpression
        }
        guard result.isFinite else {
            throw EvaluationError.nonFiniteResult
        }
        return result
    }

    static func formatted(_ value: Double) -> String {
        let normalized = abs(value) < 1e-12 ? 0 : value
        return String(format: "%.12g", locale: Locale(identifier: "en_US_POSIX"), normalized)
    }

    private struct Parser {
        var characters: [Character]
        var index = 0

        var isAtEnd: Bool { index >= characters.count }

        mutating func parseExpression() throws -> Double {
            var value = try parseTerm()
            while true {
                if consume("+") {
                    value += try parseTerm()
                } else if consume("-") {
                    value -= try parseTerm()
                } else {
                    return value
                }
            }
        }

        mutating func parseTerm() throws -> Double {
            var value = try parsePower()
            while true {
                if consume("*") {
                    value *= try parsePower()
                } else if consume("/") {
                    let divisor = try parsePower()
                    guard divisor != 0 else { throw EvaluationError.divisionByZero }
                    value /= divisor
                } else if consume("%") {
                    let divisor = try parsePower()
                    guard divisor != 0 else { throw EvaluationError.divisionByZero }
                    value.formTruncatingRemainder(dividingBy: divisor)
                } else {
                    return value
                }
            }
        }

        mutating func parsePower() throws -> Double {
            let base = try parseUnary()
            if consume("^") {
                return pow(base, try parsePower())
            }
            return base
        }

        mutating func parseUnary() throws -> Double {
            if consume("+") { return try parseUnary() }
            if consume("-") { return try -parseUnary() }
            return try parsePrimary()
        }

        mutating func parsePrimary() throws -> Double {
            skipWhitespace()
            if consume("(") {
                let value = try parseExpression()
                guard consume(")") else { throw EvaluationError.invalidExpression }
                return value
            }

            if let next = peek(), next.isLetter {
                let identifier = parseIdentifier().lowercased()
                if identifier == "pi" { return .pi }
                if identifier == "e" { return M_E }
                guard consume("(") else { throw EvaluationError.invalidExpression }
                let argument = try parseExpression()
                guard consume(")") else { throw EvaluationError.invalidExpression }
                return try apply(function: identifier, to: argument)
            }

            return try parseNumber()
        }

        mutating func parseNumber() throws -> Double {
            skipWhitespace()
            let start = index
            var sawDecimalPoint = false
            while let character = peek() {
                if character.isNumber {
                    index += 1
                } else if character == ".", !sawDecimalPoint {
                    sawDecimalPoint = true
                    index += 1
                } else {
                    break
                }
            }

            if let exponent = peek(), exponent == "e" || exponent == "E" {
                index += 1
                if let sign = peek(), sign == "+" || sign == "-" {
                    index += 1
                }
                let exponentStart = index
                while let character = peek(), character.isNumber {
                    index += 1
                }
                guard index > exponentStart else { throw EvaluationError.invalidExpression }
            }

            guard index > start else { throw EvaluationError.invalidExpression }
            let token = String(characters[start..<index])
            guard let value = Double(token) else { throw EvaluationError.invalidExpression }
            return value
        }

        mutating func parseIdentifier() -> String {
            skipWhitespace()
            let start = index
            while let character = peek(), character.isLetter || character.isNumber || character == "_" {
                index += 1
            }
            return String(characters[start..<index])
        }

        mutating func apply(function: String, to value: Double) throws -> Double {
            switch function {
            case "sqrt":
                guard value >= 0 else { throw EvaluationError.nonFiniteResult }
                return sqrt(value)
            case "abs": return abs(value)
            case "sin": return sin(value)
            case "cos": return cos(value)
            case "tan": return tan(value)
            case "ln":
                guard value > 0 else { throw EvaluationError.nonFiniteResult }
                return log(value)
            case "log", "log10":
                guard value > 0 else { throw EvaluationError.nonFiniteResult }
                return log10(value)
            case "exp": return exp(value)
            case "floor": return floor(value)
            case "ceil": return ceil(value)
            case "round": return value.rounded()
            default: throw EvaluationError.unsupportedFunction(function)
            }
        }

        mutating func consume(_ expected: Character) -> Bool {
            skipWhitespace()
            guard peek() == expected else { return false }
            index += 1
            return true
        }

        mutating func skipWhitespace() {
            while let character = peek(), character.isWhitespace {
                index += 1
            }
        }

        func peek() -> Character? {
            guard !isAtEnd else { return nil }
            return characters[index]
        }
    }
}

#if canImport(FoundationModels)
@available(macOS 26.0, *)
private struct CompanionCalculatorTool: Tool {
    let name = "calculate"
    let description = "Calculate an exact numeric result for arithmetic, roots, powers, percentages, or common functions."

    @Generable
    struct Arguments {
        @Guide(description: "A math expression using numbers, parentheses, +, -, *, /, %, ^, or functions such as sqrt, abs, sin, cos, tan, ln, log10, exp, floor, ceil, and round.")
        var expression: String
    }

    func call(arguments: Arguments) async throws -> String {
        let value = try CompanionMathEvaluator.evaluate(arguments.expression)
        return "Exact calculator result: \(CompanionMathEvaluator.formatted(value))"
    }
}

@available(macOS 26.0, *)
private struct CompanionCurrentContextTool: Tool {
    let name = "current_context"
    let description = "Get the current date, time, time zone, locale, and macOS version from this Mac."

    @Generable
    struct Arguments {
        @Guide(description: "Use 'local' unless the user explicitly requests an IANA time zone such as America/New_York.")
        var timeZoneIdentifier: String
    }

    func call(arguments: Arguments) async throws -> String {
        let requested = arguments.timeZoneIdentifier.trimmingCharacters(in: .whitespacesAndNewlines)
        let timeZone = requested.isEmpty || requested.lowercased() == "local"
            ? TimeZone.current
            : (TimeZone(identifier: requested) ?? TimeZone.current)
        let formatter = DateFormatter()
        formatter.locale = Locale.current
        formatter.timeZone = timeZone
        formatter.dateStyle = .full
        formatter.timeStyle = .long
        return """
        Current date and time: \(formatter.string(from: Date()))
        Time zone: \(timeZone.identifier)
        Locale: \(Locale.current.identifier)
        Operating system: \(ProcessInfo.processInfo.operatingSystemVersionString)
        """
    }
}

@available(macOS 26.0, *)
actor AppleOnDeviceChatService: OnDeviceChatServing {
    private static let instructions = """
    You are the concise local assistant inside Codex Companion.
    Answer the user's question directly and accurately.
    Use plain text or lightweight Markdown when it improves readability.
    Use the calculate tool for arithmetic instead of estimating a result.
    Use the current_context tool for current date, time, time zone, locale, or operating-system questions.
    Do not claim to have used Codex, ChatGPT, the internet, files, or tools unless the prompt provides that result.
    Do not invent actions or system state.
    """

    private static let options = GenerationOptions(
        sampling: .greedy,
        temperature: 0,
        maximumResponseTokens: 768
    )
    private static let sessionGenerationLimit = 8

    private var session: LanguageModelSession?
    private var successfulGenerationCount = 0

    func prewarm() async {
        guard SystemLanguageModel.default.availability == .available else { return }
        let activeSession = session ?? makeSession()
        session = activeSession
        activeSession.prewarm()
    }

    func send(prompt: String) async throws -> String {
        guard SystemLanguageModel.default.availability == .available else {
            throw OnDeviceChatError.unavailable
        }

        let activeSession = session ?? makeSession()
        session = activeSession
        guard !activeSession.isResponding else {
            throw OnDeviceChatError.busy
        }

        do {
            let response = try await activeSession.respond(
                to: prompt,
                options: Self.options
            )
            let text = response.content.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty else {
                throw OnDeviceChatError.emptyResponse
            }
            recordSuccessfulGeneration(using: activeSession)
            return text
        } catch let error as LanguageModelSession.GenerationError {
            if case .exceededContextWindowSize = error {
                replaceAndPrewarmSession(ifCurrent: activeSession)
            }
            throw error
        }
    }

    private func makeSession() -> LanguageModelSession {
        let tools: [any Tool] = [
            CompanionCalculatorTool(),
            CompanionCurrentContextTool(),
        ]
        return LanguageModelSession(
            tools: tools,
            instructions: Self.instructions
        )
    }

    private func recordSuccessfulGeneration(using completedSession: LanguageModelSession) {
        guard session === completedSession else { return }
        successfulGenerationCount += 1
        if successfulGenerationCount >= Self.sessionGenerationLimit {
            replaceAndPrewarmSession(ifCurrent: completedSession)
        }
    }

    private func replaceAndPrewarmSession(ifCurrent expectedSession: LanguageModelSession) {
        guard session === expectedSession else { return }
        let replacement = makeSession()
        session = replacement
        successfulGenerationCount = 0
        replacement.prewarm()
    }
}
#endif
