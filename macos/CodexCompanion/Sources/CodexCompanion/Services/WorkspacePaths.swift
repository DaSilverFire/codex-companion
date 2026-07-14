import Foundation

struct PetRoots: Sendable {
    var companion: URL
    var native: URL

    static var live: PetRoots {
        let support = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        )[0]
        return PetRoots(
            companion: support
                .appendingPathComponent("Codex Companion", isDirectory: true)
                .appendingPathComponent("Pets", isDirectory: true),
            native: FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent(".codex", isDirectory: true)
                .appendingPathComponent("pets", isDirectory: true)
        )
    }
}

enum WorkspacePaths {
    static var builtInPetAssetDirectories: [URL] {
        let cwd = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
        let parent = cwd.deletingLastPathComponent()
        let candidates = [
            cwd.appendingPathComponent("extracted/app-asar-safe/webview/assets", isDirectory: true),
            parent.appendingPathComponent("extracted/app-asar-safe/webview/assets", isDirectory: true),
            parent.appendingPathComponent("extracted/current-running-codex-avatar/webview/assets", isDirectory: true),
        ]

        var seen = Set<String>()
        return candidates.filter { url in
            var isDirectory: ObjCBool = false
            let exists = FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory)
            return exists && isDirectory.boolValue && seen.insert(url.path).inserted
        }
    }

    static var codexAppURLs: [URL] {
        [
            URL(fileURLWithPath: "/Applications/ChatGPT.app", isDirectory: true),
            URL(fileURLWithPath: "/Applications/Codex 2.app", isDirectory: true),
            URL(fileURLWithPath: "/Applications/Codex.app", isDirectory: true),
        ]
    }

    static var codexExecutableURLs: [URL] {
        codexAppURLs.map { $0.appendingPathComponent("Contents/Resources/codex") } + [
            URL(fileURLWithPath: "/opt/homebrew/bin/codex"),
            URL(fileURLWithPath: "/usr/local/bin/codex"),
            URL(fileURLWithPath: "/usr/bin/codex"),
        ]
    }
}
