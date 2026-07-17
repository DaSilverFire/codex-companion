import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct PetStoreTests {
    @Test
    func loadsCompanionAndNativePetsWithoutSharingRoots() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)
        try makePet(id: "shadow-native-v2", columns: 8, rows: 11, in: environment.native)

        let store = makeStore(environment)
        let ids = Set(store.pets.map(\.id))

        #expect(ids.contains("custom:shadow-16"))
        #expect(ids.contains("custom:shadow-native-v2"))
    }

    @Test
    func duplicateCustomIDFavorsCompanionPath() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shared-shadow", columns: 16, rows: 10, in: environment.companion)
        try makePet(id: "shared-shadow", columns: 8, rows: 11, in: environment.native)

        let store = makeStore(environment)
        let matchingPets = store.pets.filter { $0.id == "custom:shared-shadow" }
        let pet = try #require(matchingPets.first)
        guard case let .custom(sourceDirectory) = pet.source else {
            Issue.record("Expected a custom pet source")
            return
        }

        #expect(matchingPets.count == 1)
        #expect(Array(sourceDirectory.pathComponents.suffix(2)) == ["companion", "shared-shadow"])
        #expect(pet.spriteColumns == 16)
        #expect(pet.spriteRows == 10)
    }

    @Test
    func sameRootDuplicateIDUsesAlphabeticallyFirstDirectory() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(
            id: "same-id",
            columns: 8,
            rows: 9,
            directoryName: "z-package",
            in: environment.companion
        )
        try makePet(
            id: "same-id",
            columns: 16,
            rows: 10,
            directoryName: "a-package",
            in: environment.companion
        )

        let store = makeStore(environment)
        let matchingPets = store.pets.filter { $0.id == "custom:same-id" }
        let pet = try #require(matchingPets.first)
        guard case let .custom(sourceDirectory) = pet.source else {
            Issue.record("Expected a custom pet source")
            return
        }

        #expect(matchingPets.count == 1)
        #expect(sourceDirectory.lastPathComponent == "a-package")
        #expect(pet.spriteColumns == 16)
        #expect(pet.spriteRows == 10)
    }

    @Test
    func sameDisplayNameCustomPetsSortByID() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(
            id: "zeta",
            displayName: "Shared Name",
            columns: 8,
            rows: 9,
            in: environment.companion
        )
        try makePet(
            id: "alpha",
            displayName: "Shared Name",
            columns: 8,
            rows: 9,
            in: environment.companion
        )

        let store = makeStore(environment)

        #expect(store.pets.map(\.id) == ["custom:alpha", "custom:zeta"])
    }

    @Test
    func migratesLegacyShadowSelectionOnce() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)
        try makePet(id: "shadow-32-real", columns: 32, rows: 10, in: environment.native)
        environment.defaults.set("custom:shadow-32-real", forKey: "selectedPetID")

        let store = makeStore(environment)

        #expect(store.selectedPetID == "custom:shadow-16")
        #expect(environment.defaults.string(forKey: "selectedPetID") == "custom:shadow-16")
        #expect(environment.defaults.integer(forKey: "shadow16PetMigrationVersion") == 1)
    }

    @Test
    func doesNotMigrateLegacySelectionWhenTargetIsMissing() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-32-real", columns: 32, rows: 10, in: environment.native)
        environment.defaults.set("custom:shadow-32-real", forKey: "selectedPetID")

        let store = makeStore(environment)

        #expect(store.selectedPetID == "custom:shadow-32-real")
        #expect(environment.defaults.string(forKey: "selectedPetID") == "custom:shadow-32-real")
        #expect(environment.defaults.object(forKey: "shadow16PetMigrationVersion") == nil)
    }

    @Test
    func versionOneInitializationPreservesValidLegacySelection() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)
        try makePet(id: "shadow-32-real", columns: 32, rows: 10, in: environment.native)
        environment.defaults.set("custom:shadow-32-real", forKey: "selectedPetID")
        environment.defaults.set(1, forKey: "shadow16PetMigrationVersion")

        let store = makeStore(environment)

        #expect(store.selectedPetID == "custom:shadow-32-real")
        #expect(environment.defaults.string(forKey: "selectedPetID") == "custom:shadow-32-real")
        #expect(environment.defaults.integer(forKey: "shadow16PetMigrationVersion") == 1)
    }

    @Test
    func considersMigrationOnceWhenCurrentSelectionIsNotLegacy() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)
        try makePet(id: "shadow-32-real", columns: 32, rows: 10, in: environment.native)
        try makePet(id: "other-pet", columns: 8, rows: 9, in: environment.native)
        environment.defaults.set("custom:other-pet", forKey: "selectedPetID")

        let firstStore = makeStore(environment)

        #expect(firstStore.selectedPetID == "custom:other-pet")
        #expect(environment.defaults.integer(forKey: "shadow16PetMigrationVersion") == 1)

        firstStore.selectedPetID = "custom:shadow-32-real"
        let secondStore = makeStore(environment)

        #expect(secondStore.selectedPetID == "custom:shadow-32-real")
        #expect(environment.defaults.integer(forKey: "shadow16PetMigrationVersion") == 1)
    }

    @Test
    func readsAndWritesInjectedDefaults() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "alpha", columns: 8, rows: 9, in: environment.companion)
        try makePet(id: "zeta", columns: 8, rows: 9, in: environment.native)
        environment.defaults.set("custom:zeta", forKey: "selectedPetID")

        let store = makeStore(environment)
        #expect(store.selectedPetID == "custom:zeta")

        store.selectedPetID = "custom:alpha"
        #expect(environment.defaults.string(forKey: "selectedPetID") == "custom:alpha")

        store.selectedPetID = nil
        #expect(environment.defaults.object(forKey: "selectedPetID") == nil)
    }

    @Test
    func shadow16IsThePreferredDefault() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "alpha", columns: 8, rows: 9, in: environment.native)
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)

        let store = makeStore(environment)

        #expect(store.selectedPetID == "custom:shadow-16")
    }

    @Test
    func loadsInjectedBuiltInsAfterCustomPets() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        let builtIns = environment.temporary.appendingPathComponent("built-ins", isDirectory: true)
        try makePet(id: "custom-pet", columns: 8, rows: 9, in: environment.companion)
        try makeBuiltInPet(id: "bundled-pet", in: builtIns)

        let store = PetStore(
            roots: PetRoots(companion: environment.companion, native: environment.native),
            builtInDirectories: [builtIns],
            defaults: environment.defaults
        )

        #expect(store.pets.map(\.id) == ["custom:custom-pet", "built-in:bundled-pet"])
    }

    @Test
    func reloadReevaluatesBuiltInDirectoryProvider() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        let builtIns = environment.temporary.appendingPathComponent("built-ins", isDirectory: true)
        try makeBuiltInPet(id: "late-bundled-pet", in: builtIns)
        var providedDirectories: [URL] = []
        let store = PetStore(
            roots: PetRoots(companion: environment.companion, native: environment.native),
            builtInDirectoryProvider: { providedDirectories },
            defaults: environment.defaults
        )

        #expect(!store.pets.map(\.id).contains("built-in:late-bundled-pet"))

        providedDirectories = [builtIns]
        store.reload()

        #expect(store.pets.map(\.id).contains("built-in:late-bundled-pet"))
    }

    @Test
    func liveRootsUseCompanionSupportAndNativeCodexLocations() {
        let support = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        )[0]
        let expectedCompanion = support
            .appendingPathComponent("Codex Companion", isDirectory: true)
            .appendingPathComponent("Pets", isDirectory: true)
        let expectedNative = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("pets", isDirectory: true)

        #expect(PetRoots.live.companion == expectedCompanion)
        #expect(PetRoots.live.native == expectedNative)
    }

    @Test
    func chatGPTIsFirstCodexAppCandidate() {
        #expect(
            WorkspacePaths.codexAppURLs.map(\.path) == [
                "/Applications/ChatGPT.app",
                "/Applications/Codex 2.app",
                "/Applications/Codex.app",
            ]
        )
    }

    @Test
    func loadsDirectionalLookSheetOnlyWhenItsContractIsValid() throws {
        let environment = try TestEnvironment()
        defer { environment.cleanup() }
        try makePet(id: "shadow-16", columns: 16, rows: 10, in: environment.companion)
        let directory = environment.companion.appendingPathComponent("shadow-16", isDirectory: true)
        let manifestURL = directory.appendingPathComponent("pet.json")
        var manifest = try #require(
            JSONSerialization.jsonObject(with: Data(contentsOf: manifestURL)) as? [String: Any]
        )
        manifest["lookSpritesheetPath"] = "look-spritesheet.webp"
        manifest["lookSpriteColumns"] = 8
        manifest["lookSpriteRows"] = 11
        manifest["lookFrameStartRow"] = 9
        try JSONSerialization.data(withJSONObject: manifest).write(to: manifestURL)
        try Data([0]).write(to: directory.appendingPathComponent("look-spritesheet.webp"))

        let pet = try #require(makeStore(environment).pets.first)
        let lookFrames = try #require(pet.directionalLookFrames)

        #expect(lookFrames.spriteColumns == 8)
        #expect(lookFrames.spriteRows == 11)
        #expect(lookFrames.startRow == 9)
        #expect(lookFrames.spritesheetURL.lastPathComponent == "look-spritesheet.webp")
    }

    private func makeStore(_ environment: TestEnvironment) -> PetStore {
        PetStore(
            roots: PetRoots(companion: environment.companion, native: environment.native),
            builtInDirectories: [],
            defaults: environment.defaults
        )
    }
}

private struct TestEnvironment {
    let temporary: URL
    let companion: URL
    let native: URL
    let suiteName: String
    let defaults: UserDefaults

    init() throws {
        temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        companion = temporary.appendingPathComponent("companion", isDirectory: true)
        native = temporary.appendingPathComponent("native", isDirectory: true)
        suiteName = "PetStoreTests.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suiteName) else {
            throw TestEnvironmentError.unavailableDefaultsSuite
        }
        self.defaults = defaults

        try FileManager.default.createDirectory(
            at: temporary,
            withIntermediateDirectories: true
        )
        defaults.removePersistentDomain(forName: suiteName)
    }

    func cleanup() {
        try? FileManager.default.removeItem(at: temporary)
        defaults.removePersistentDomain(forName: suiteName)
    }
}

private enum TestEnvironmentError: Error {
    case unavailableDefaultsSuite
}

private func makePet(
    id: String,
    displayName: String? = nil,
    columns: Int,
    rows: Int,
    directoryName: String? = nil,
    in root: URL
) throws {
    let directory = root.appendingPathComponent(directoryName ?? id, isDirectory: true)
    try FileManager.default.createDirectory(
        at: directory,
        withIntermediateDirectories: true
    )
    let manifest: [String: Any] = [
        "id": id,
        "displayName": displayName ?? id,
        "spritesheetPath": "spritesheet.webp",
        "spriteColumns": columns,
        "spriteRows": rows,
        "animationFrameCounts": ["idle": columns],
    ]
    let data = try JSONSerialization.data(withJSONObject: manifest)
    try data.write(to: directory.appendingPathComponent("pet.json"))
    try Data([0]).write(to: directory.appendingPathComponent("spritesheet.webp"))
}

private func makeBuiltInPet(id: String, in root: URL) throws {
    try FileManager.default.createDirectory(
        at: root,
        withIntermediateDirectories: true
    )
    try Data([0]).write(
        to: root.appendingPathComponent("\(id)-spritesheet-v4-test.webp")
    )
}
