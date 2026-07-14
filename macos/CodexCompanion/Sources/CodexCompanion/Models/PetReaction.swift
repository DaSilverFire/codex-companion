import Foundation

enum PetReactionEvent: String, CaseIterable, Sendable {
    case response
    case approval
    case completion
    case goalStarted
    case goalCompletion
    case failure
}

struct PetReactionContext: Equatable, Sendable {
    var event: PetReactionEvent
    var processID: String
    var processTitle: String
    var detail: String
    var goalObjective: String?
}

enum PetReactionCopy {
    private static let normalizationLocale = Locale(identifier: "en_US_POSIX")
    private static let rejectedDoubleQuoteScalars: Set<UInt32> = [
        0x0022,
        0x00AB,
        0x00BB,
        0x201C,
        0x201D,
        0x201E,
        0x201F,
        0x301D,
        0x301E,
        0x301F,
        0xFF02
    ]

    private static let candidates: [PetReactionEvent: [String]] = [
        .response: [
            "I found something for you!",
            "Psst, there is a fresh update!",
            "I have something new to show you.",
            "A new answer just came in!",
            "Come see what I found.",
            "I brought you a new update!"
        ],
        .approval: [
            "Psst, I need your approval.",
            "Could you check something for me?",
            "I need your eyes on this one.",
            "A little help here, please?",
            "Can you take a quick look?",
            "This one needs your okay."
        ],
        .completion: [
            "All done! I wrapped that one up.",
            "Good news, I finished it!",
            "Finished! Come see how it went.",
            "That one is ready for you.",
            "I got it done!",
            "Another task neatly finished."
        ],
        .goalStarted: [
            "I am on it! The new goal has started.",
            "New goal, here I go!",
            "I have got the new goal.",
            "Time to chase this goal!",
            "I am starting the new goal now.",
            "The new goal is in my paws."
        ],
        .goalCompletion: [
            "We did it! That goal is complete.",
            "Goal complete! Nice work.",
            "That goal is all wrapped up!",
            "We made it to the finish!",
            "Success! We finished the goal.",
            "All done - that goal is ours!"
        ],
        .failure: [
            "Uh-oh, I hit a snag.",
            "That one did not work out.",
            "I ran into a problem.",
            "Something went wrong over here.",
            "I could not finish that one.",
            "That task needs another look."
        ]
    ]

    static func validated(
        _ candidate: String,
        for context: PetReactionContext,
        excluding recent: [String]
    ) -> String? {
        let line = candidate.trimmingCharacters(in: .whitespacesAndNewlines)
        let lineWords = normalizedWords(line)

        guard !line.isEmpty,
              line.rangeOfCharacter(from: .newlines) == nil,
              (2...14).contains(lineWords.count),
              line.count <= 96,
              !line.unicodeScalars.contains(where: { isRejectedScalar($0) })
        else {
            return nil
        }

        let normalizedLine = lineWords.joined(separator: " ")
        let titleWords = normalizedWords(context.processTitle)
        let titleCharacterCount = titleWords.joined().count
        let containsProcessTitle = titleCharacterCount > 1
            && containsContiguousSequence(titleWords, in: lineWords)
        let containsMetadata = containsMeaningfulMetadata(context.detail, in: lineWords)
            || containsMeaningfulMetadata(context.goalObjective, in: lineWords)
        guard !containsProcessTitle, !containsMetadata else {
            return nil
        }

        let recentSet = Set(recent.map(normalized))
        guard !recentSet.contains(normalizedLine) else {
            return nil
        }

        return line
    }

    static func fallback(for context: PetReactionContext, excluding recent: [String]) -> String {
        let eventCandidates = candidates[context.event] ?? ["I have an update for you!"]
        let recentSet = Set(recent.map(normalized))

        if let unused = eventCandidates.first(where: { !recentSet.contains(normalized($0)) }) {
            return unused
        }

        let immediateLast = recent.last.map(normalized)
        return eventCandidates.first(where: { normalized($0) != immediateLast }) ?? eventCandidates[0]
    }

    private static func normalized(_ value: String) -> String {
        normalizedWords(value).joined(separator: " ")
    }

    private static func normalizedWords(_ value: String) -> [String] {
        value
            .folding(
                options: [.caseInsensitive, .diacriticInsensitive],
                locale: normalizationLocale
            )
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }
    }

    private static func isRejectedScalar(_ scalar: Unicode.Scalar) -> Bool {
        rejectedDoubleQuoteScalars.contains(scalar.value)
            || scalar.properties.isEmojiPresentation
            || CharacterSet.controlCharacters.contains(scalar)
            || scalar.value == 0xFE0F
            || scalar.value == 0x20E3
    }

    private static func containsContiguousSequence(
        _ sequence: [String],
        in words: [String]
    ) -> Bool {
        guard !sequence.isEmpty, sequence.count <= words.count else {
            return false
        }

        for start in 0...(words.count - sequence.count) {
            let end = start + sequence.count
            if words[start..<end].elementsEqual(sequence) {
                return true
            }
        }

        return false
    }

    private static func containsMeaningfulMetadata(
        _ metadata: String?,
        in words: [String]
    ) -> Bool {
        guard let metadata else {
            return false
        }

        let metadataWords = normalizedWords(metadata)
        let alphanumericCount = metadataWords.reduce(0) { $0 + $1.count }
        guard metadataWords.count >= 2, alphanumericCount >= 8 else {
            return false
        }

        return containsContiguousSequence(metadataWords, in: words)
    }
}
