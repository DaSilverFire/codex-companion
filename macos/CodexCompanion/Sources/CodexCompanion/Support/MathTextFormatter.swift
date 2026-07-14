import Foundation

enum MathTextFormatter {
    static func displayString(from rawText: String) -> String {
        var text = rawText

        let pairedDelimiters = [
            "\\(": "",
            "\\)": "",
            "\\[": "",
            "\\]": "",
        ]
        for (source, replacement) in pairedDelimiters {
            text = text.replacingOccurrences(of: source, with: replacement)
        }

        text = text.replacingOccurrences(of: "$$", with: "")
        text = text.replacingOccurrences(of: "$", with: "")
        text = replaceRegex(#"\\(?:dfrac|tfrac|frac)\{([^{}]+)\}\{([^{}]+)\}"#, in: text, with: "($1)/($2)")
        text = replaceRegex(#"\\sqrt\{([^{}]+)\}"#, in: text, with: "√$1")

        for command in ["text", "mathrm", "mathbf", "boxed"] {
            text = unwrapLatexCommand(command, in: text)
        }

        let replacements: [(String, String)] = [
            ("\\Rightarrow", "⇒"),
            ("\\rightarrow", "→"),
            ("\\leftarrow", "←"),
            ("\\approx", "≈"),
            ("\\simeq", "≈"),
            ("\\neq", "≠"),
            ("\\ne", "≠"),
            ("\\leq", "≤"),
            ("\\geq", "≥"),
            ("\\le", "≤"),
            ("\\ge", "≥"),
            ("\\times", "×"),
            ("\\cdot", "·"),
            ("\\div", "÷"),
            ("\\pm", "±"),
            ("\\mp", "∓"),
            ("\\infty", "∞"),
            ("\\pi", "π"),
            ("\\theta", "θ"),
            ("\\lambda", "λ"),
            ("\\alpha", "α"),
            ("\\beta", "β"),
            ("\\gamma", "γ"),
            ("\\delta", "δ"),
            ("\\Delta", "Δ"),
            ("\\mu", "μ"),
            ("\\sigma", "σ"),
            ("\\omega", "ω"),
            ("\\to", "→"),
            ("\\left", ""),
            ("\\right", ""),
            ("\\,", " "),
            ("\\!", ""),
            ("\\%", "%"),
        ]

        for (source, replacement) in replacements {
            text = text.replacingOccurrences(of: source, with: replacement)
        }

        text = convertScripts(in: text, marker: "^", map: superscripts)
        text = convertScripts(in: text, marker: "_", map: subscripts)
        return text
            .replacingOccurrences(of: "\\ ", with: " ")
            .replacingOccurrences(of: "\\\\", with: "\n")
    }

    private static let superscripts: [Character: Character] = [
        "0": "⁰", "1": "¹", "2": "²", "3": "³", "4": "⁴",
        "5": "⁵", "6": "⁶", "7": "⁷", "8": "⁸", "9": "⁹",
        "+": "⁺", "-": "⁻", "=": "⁼", "(": "⁽", ")": "⁾",
        "n": "ⁿ", "i": "ⁱ",
    ]

    private static let subscripts: [Character: Character] = [
        "0": "₀", "1": "₁", "2": "₂", "3": "₃", "4": "₄",
        "5": "₅", "6": "₆", "7": "₇", "8": "₈", "9": "₉",
        "+": "₊", "-": "₋", "=": "₌", "(": "₍", ")": "₎",
        "a": "ₐ", "e": "ₑ", "h": "ₕ", "i": "ᵢ", "j": "ⱼ",
        "k": "ₖ", "l": "ₗ", "m": "ₘ", "n": "ₙ", "o": "ₒ",
        "p": "ₚ", "r": "ᵣ", "s": "ₛ", "t": "ₜ", "u": "ᵤ",
        "v": "ᵥ", "x": "ₓ",
    ]

    private static func replaceRegex(_ pattern: String, in text: String, with template: String) -> String {
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return text
        }
        let range = NSRange(text.startIndex..<text.endIndex, in: text)
        return regex.stringByReplacingMatches(in: text, range: range, withTemplate: template)
    }

    private static func unwrapLatexCommand(_ command: String, in text: String) -> String {
        replaceRegex(#"\\"# + command + #"\{([^{}]+)\}"#, in: text, with: "$1")
    }

    private static func convertScripts(
        in text: String,
        marker: Character,
        map: [Character: Character]
    ) -> String {
        var output = ""
        var index = text.startIndex

        while index < text.endIndex {
            let character = text[index]
            guard character == marker else {
                output.append(character)
                index = text.index(after: index)
                continue
            }

            let nextIndex = text.index(after: index)
            guard nextIndex < text.endIndex else {
                output.append(character)
                index = nextIndex
                continue
            }

            if text[nextIndex] == "{" {
                guard let closeIndex = text[nextIndex...].firstIndex(of: "}") else {
                    output.append(character)
                    index = nextIndex
                    continue
                }
                let contentStart = text.index(after: nextIndex)
                let content = String(text[contentStart..<closeIndex])
                if let converted = convertedCharacters(content, map: map) {
                    output.append(converted)
                } else {
                    output.append(marker)
                    output.append(contentsOf: content)
                }
                index = text.index(after: closeIndex)
            } else {
                let value = text[nextIndex]
                if let converted = map[value] {
                    output.append(converted)
                } else {
                    output.append(marker)
                    output.append(value)
                }
                index = text.index(after: nextIndex)
            }
        }

        return output
    }

    private static func convertedCharacters(_ content: String, map: [Character: Character]) -> String? {
        var converted = ""
        for character in content {
            guard let mapped = map[character] else {
                return nil
            }
            converted.append(mapped)
        }
        return converted
    }
}
