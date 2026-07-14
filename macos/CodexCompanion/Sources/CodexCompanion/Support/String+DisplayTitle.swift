import Foundation

extension String {
    var displayTitle: String {
        split(separator: "-")
            .map { part in
                part.prefix(1).uppercased() + part.dropFirst()
            }
            .joined(separator: " ")
    }

    func clipped(_ limit: Int) -> String {
        guard count > limit else { return self }
        let end = index(startIndex, offsetBy: max(0, limit - 1))
        return String(self[..<end]) + "…"
    }
}
