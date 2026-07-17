import Foundation

final class PetStore: ObservableObject {
    @Published private(set) var pets: [PetDefinition] = []
    @Published var selectedPetID: String? {
        didSet {
            if let selectedPetID {
                defaults.set(selectedPetID, forKey: Self.selectedPetIDKey)
            } else {
                defaults.removeObject(forKey: Self.selectedPetIDKey)
            }
        }
    }

    private let roots: PetRoots
    private let builtInDirectoryProvider: () -> [URL]
    private let defaults: UserDefaults

    convenience init() {
        self.init(
            roots: PetRoots.live,
            builtInDirectoryProvider: { WorkspacePaths.builtInPetAssetDirectories },
            defaults: UserDefaults.standard
        )
    }

    convenience init(roots: PetRoots, builtInDirectories: [URL], defaults: UserDefaults) {
        self.init(
            roots: roots,
            builtInDirectoryProvider: { builtInDirectories },
            defaults: defaults
        )
    }

    init(
        roots: PetRoots,
        builtInDirectoryProvider: @escaping () -> [URL],
        defaults: UserDefaults
    ) {
        self.roots = roots
        self.builtInDirectoryProvider = builtInDirectoryProvider
        self.defaults = defaults
        selectedPetID = defaults.string(forKey: Self.selectedPetIDKey)
        reload()
    }

    var selectedPet: PetDefinition? {
        guard let selectedPetID else { return defaultPet(from: pets) }
        return pets.first { $0.id == selectedPetID }
    }

    func reload() {
        var loaded = loadCustomPets()
        loaded.append(contentsOf: loadBuiltInPets())
        loaded.sort { lhs, rhs in
            let lhsSourceRank = sourceSortRank(lhs.source)
            let rhsSourceRank = sourceSortRank(rhs.source)
            if lhsSourceRank != rhsSourceRank {
                return lhsSourceRank < rhsSourceRank
            }

            let displayNameOrder = lhs.displayName.localizedCaseInsensitiveCompare(rhs.displayName)
            if displayNameOrder != .orderedSame {
                return displayNameOrder == .orderedAscending
            }
            if lhs.id != rhs.id {
                return lhs.id < rhs.id
            }
            return sourcePath(lhs.source) < sourcePath(rhs.source)
        }

        pets = loaded
        selectedPetID = selectedPetIDAfterReload(in: loaded)
    }

    private func loadCustomPets() -> [PetDefinition] {
        var seen = Set<String>()
        var pets: [PetDefinition] = []

        for root in [roots.companion, roots.native] {
            for pet in loadCustomPets(from: root) where seen.insert(pet.id).inserted {
                pets.append(pet)
            }
        }

        return pets
    }

    private func loadCustomPets(from root: URL) -> [PetDefinition] {
        guard let directories = try? FileManager.default.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        return directories.sorted { $0.path < $1.path }.compactMap { directory in
            let manifestURL = directory.appendingPathComponent("pet.json")
            guard
                let data = try? Data(contentsOf: manifestURL),
                let manifest = try? JSONDecoder().decode(PetManifest.self, from: data)
            else {
                return nil
            }

            let id = manifest.id ?? directory.lastPathComponent
            let sheetName = manifest.spritesheetPath ?? "spritesheet.webp"
            let sheetURL = directory.appendingPathComponent(sheetName)
            guard FileManager.default.fileExists(atPath: sheetURL.path) else { return nil }
            let directionalLookFrames: PetDefinition.DirectionalLookFrames?
            if let lookSheetName = manifest.lookSpritesheetPath {
                let lookSheetURL = directory.appendingPathComponent(lookSheetName)
                let lookColumns = max(1, manifest.lookSpriteColumns ?? 8)
                let lookRows = max(1, manifest.lookSpriteRows ?? 11)
                let startRow = max(0, manifest.lookFrameStartRow ?? 9)
                if FileManager.default.fileExists(atPath: lookSheetURL.path),
                   lookColumns >= 8,
                   startRow + 1 < lookRows
                {
                    directionalLookFrames = PetDefinition.DirectionalLookFrames(
                        spritesheetURL: lookSheetURL,
                        spriteColumns: lookColumns,
                        spriteRows: lookRows,
                        startRow: startRow
                    )
                } else {
                    directionalLookFrames = nil
                }
            } else {
                directionalLookFrames = nil
            }

            return PetDefinition(
                id: "custom:\(id)",
                displayName: manifest.displayName ?? id.displayTitle,
                description: manifest.description ?? "Custom Codex pet",
                spritesheetURL: sheetURL,
                spriteColumns: max(1, min(32, manifest.spriteColumns ?? 8)),
                spriteRows: max(1, manifest.spriteRows ?? 9),
                animationFrameCounts: manifest.animationFrameCounts ?? [:],
                directionalLookFrames: directionalLookFrames,
                source: .custom(directory)
            )
        }
    }

    private func loadBuiltInPets() -> [PetDefinition] {
        var seen = Set<String>()
        var pets: [PetDefinition] = []

        for directory in builtInDirectoryProvider() {
            guard let files = try? FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: nil,
                options: [.skipsHiddenFiles]
            ) else {
                continue
            }

            for file in files where file.lastPathComponent.contains("-spritesheet-v4-") && file.pathExtension == "webp" {
                guard let slug = file.lastPathComponent.components(separatedBy: "-spritesheet").first else { continue }
                guard seen.insert(slug).inserted else { continue }

                pets.append(
                    PetDefinition(
                        id: "built-in:\(slug)",
                        displayName: builtInDisplayName(for: slug),
                        description: "Built-in Codex pet",
                        spritesheetURL: file,
                        spriteColumns: 8,
                        spriteRows: 9,
                        animationFrameCounts: Self.builtInFrameCounts,
                        source: .builtIn(file)
                    )
                )
            }
        }

        return pets
    }

    private func builtInDisplayName(for slug: String) -> String {
        switch slug {
        case "bsod": "BSOD"
        case "null-signal": "Null Signal"
        default: slug.displayTitle
        }
    }

    private func sourceSortRank(_ source: PetSource) -> Int {
        switch source {
        case .custom: 0
        case .builtIn: 1
        }
    }

    private func sourcePath(_ source: PetSource) -> String {
        switch source {
        case let .custom(directory): directory.path
        case let .builtIn(file): file.path
        }
    }

    private static let builtInFrameCounts: [String: Int] = [
        "idle": 6,
        "running-right": 8,
        "running-left": 8,
        "waving": 4,
        "jumping": 5,
        "failed": 8,
        "waiting": 6,
        "running": 6,
        "review": 6,
        "goal-complete": 8,
    ]

    private func selectedPetIDAfterReload(in loaded: [PetDefinition]) -> String? {
        if defaults.integer(forKey: Self.migrationVersionKey) < Self.currentMigrationVersion,
           loaded.contains(where: { $0.id == Self.preferredDefaultPetID }) {
            defaults.set(Self.currentMigrationVersion, forKey: Self.migrationVersionKey)
            if defaults.string(forKey: Self.selectedPetIDKey) == Self.legacyShadowPetID {
                return Self.preferredDefaultPetID
            }
        }

        return resolvedSelectedPetID(in: loaded)
    }

    private func resolvedSelectedPetID(in loaded: [PetDefinition]) -> String? {
        if let selectedPetID, loaded.contains(where: { $0.id == selectedPetID }) {
            return selectedPetID
        }
        return defaultPet(from: loaded)?.id
    }

    private func defaultPet(from loaded: [PetDefinition]) -> PetDefinition? {
        loaded.first { $0.id == Self.preferredDefaultPetID } ?? loaded.first
    }

    private static let preferredDefaultPetID = "custom:shadow-16"
    private static let legacyShadowPetID = "custom:shadow-32-real"
    private static let selectedPetIDKey = "selectedPetID"
    private static let migrationVersionKey = "shadow16PetMigrationVersion"
    private static let currentMigrationVersion = 1
}
