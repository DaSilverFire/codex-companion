import Combine
import Foundation

struct CodexAccountProfile: Codable, Equatable, Identifiable, Sendable {
    var id: UUID
    var label: String
}

final class CodexAccountProfileStore {
    static let profilesKey = "com.silverfire.codexcompanion.codex-account-profile-labels"
    static let selectedProfileKey = "com.silverfire.codexcompanion.selected-codex-account-profile"

    private(set) var profiles: [CodexAccountProfile]
    private(set) var selectedProfileID: UUID?

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        profiles = defaults.data(forKey: Self.profilesKey)
            .flatMap { try? JSONDecoder().decode([CodexAccountProfile].self, from: $0) }
            ?? []
        selectedProfileID = defaults.string(forKey: Self.selectedProfileKey)
            .flatMap(UUID.init(uuidString:))

        if let selectedProfileID, !profiles.contains(where: { $0.id == selectedProfileID }) {
            self.selectedProfileID = nil
            defaults.removeObject(forKey: Self.selectedProfileKey)
        }
    }

    @discardableResult
    func addProfile(label: String) -> CodexAccountProfile? {
        let normalizedLabel = label.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedLabel.isEmpty else { return nil }
        guard !profiles.contains(where: {
            $0.label.compare(normalizedLabel, options: [.caseInsensitive, .diacriticInsensitive])
                == .orderedSame
        }) else { return nil }

        let profile = CodexAccountProfile(id: UUID(), label: normalizedLabel)
        profiles.append(profile)
        if selectedProfileID == nil {
            selectedProfileID = profile.id
        }
        persist()
        return profile
    }

    func selectProfile(id: UUID?) {
        guard id == nil || profiles.contains(where: { $0.id == id }) else { return }
        selectedProfileID = id
        persistSelection()
    }

    func removeProfile(id: UUID) {
        profiles.removeAll { $0.id == id }
        if selectedProfileID == id {
            selectedProfileID = profiles.first?.id
        }
        persist()
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(profiles) {
            defaults.set(data, forKey: Self.profilesKey)
        }
        persistSelection()
    }

    private func persistSelection() {
        if let selectedProfileID {
            defaults.set(selectedProfileID.uuidString, forKey: Self.selectedProfileKey)
        } else {
            defaults.removeObject(forKey: Self.selectedProfileKey)
        }
    }
}

struct CodexAccountProfileLoginCommand: Equatable, Sendable {
    var executableURL: URL
    var arguments: [String]
    var environmentOverrides: [String: String]
}

enum CodexAccountProfileLoginCommandFactory {
    static func signIn(
        profile: CodexAccountProfile,
        executableURL: URL,
        baseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    ) -> CodexAccountProfileLoginCommand {
        command(
            profile: profile,
            executableURL: executableURL,
            arguments: ["login"],
            baseURL: baseURL
        )
    }

    static func status(
        profile: CodexAccountProfile,
        executableURL: URL,
        baseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    ) -> CodexAccountProfileLoginCommand {
        command(
            profile: profile,
            executableURL: executableURL,
            arguments: ["login", "status"],
            baseURL: baseURL
        )
    }

    private static func command(
        profile: CodexAccountProfile,
        executableURL: URL,
        arguments: [String],
        baseURL: URL
    ) -> CodexAccountProfileLoginCommand {
        let homeURL = CodexAccountProfileRuntime.homeURL(for: profile, baseURL: baseURL)
        return CodexAccountProfileLoginCommand(
            executableURL: executableURL,
            arguments: arguments,
            environmentOverrides: [
                "CODEX_HOME": homeURL.path,
                "CODEX_SQLITE_HOME": homeURL.path,
            ]
        )
    }
}

enum CodexAccountProfileAuthenticationState: Equatable, Sendable {
    case unchecked
    case checking
    case signedOut
    case signingIn
    case signedIn
    case failed(String)
}

enum CodexAccountProfileLoginError: LocalizedError {
    case missingExecutable
    case launchFailed(String)
    case signInFailed(Int32)

    var errorDescription: String? {
        switch self {
        case .missingExecutable:
            return "The official Codex executable could not be found."
        case .launchFailed(let message):
            return "Codex sign-in could not start: \(message)"
        case .signInFailed(let status):
            return "Codex sign-in ended with status \(status)."
        }
    }
}

struct CodexAccountProfileLoginService: Sendable {
    typealias ExecutableProvider = @Sendable () -> URL?
    typealias CommandRunner = @Sendable (CodexAccountProfileLoginCommand) async throws -> Int32

    var baseURL: URL = CodexAccountProfileRuntime.defaultBaseURL
    var executableProvider: ExecutableProvider = {
        WorkspacePaths.codexExecutableURLs.first(where: {
            FileManager.default.isExecutableFile(atPath: $0.path)
        })
    }
    var commandRunner: CommandRunner = { command in
        try await Task.detached(priority: .userInitiated) {
            let process = Process()
            process.executableURL = command.executableURL
            process.arguments = command.arguments
            var environment = ProcessInfo.processInfo.environment
            command.environmentOverrides.forEach { environment[$0.key] = $0.value }
            process.environment = environment
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            do {
                try process.run()
            } catch {
                throw CodexAccountProfileLoginError.launchFailed(error.localizedDescription)
            }
            process.waitUntilExit()
            return process.terminationStatus
        }.value
    }

    func status(for profile: CodexAccountProfile) async -> CodexAccountProfileAuthenticationState {
        do {
            let executableURL = try resolvedExecutableURL()
            try CodexAccountProfileRuntime.prepareHome(for: profile, baseURL: baseURL)
            let exitStatus = try await commandRunner(
                CodexAccountProfileLoginCommandFactory.status(
                    profile: profile,
                    executableURL: executableURL,
                    baseURL: baseURL
                )
            )
            return exitStatus == 0 ? .signedIn : .signedOut
        } catch {
            return .failed(Self.errorText(error))
        }
    }

    func signIn(profile: CodexAccountProfile) async -> CodexAccountProfileAuthenticationState {
        do {
            let executableURL = try resolvedExecutableURL()
            try CodexAccountProfileRuntime.prepareHome(for: profile, baseURL: baseURL)
            let exitStatus = try await commandRunner(
                CodexAccountProfileLoginCommandFactory.signIn(
                    profile: profile,
                    executableURL: executableURL,
                    baseURL: baseURL
                )
            )
            guard exitStatus == 0 else {
                throw CodexAccountProfileLoginError.signInFailed(exitStatus)
            }
            return .signedIn
        } catch {
            return .failed(Self.errorText(error))
        }
    }

    private func resolvedExecutableURL() throws -> URL {
        guard let executableURL = executableProvider() else {
            throw CodexAccountProfileLoginError.missingExecutable
        }
        return executableURL
    }

    private static func errorText(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    }
}

@MainActor
final class CodexAccountProfileSwitcher: ObservableObject {
    typealias StatusChecker = @MainActor (CodexAccountProfile) async
        -> CodexAccountProfileAuthenticationState
    typealias SignInHandler = @MainActor (CodexAccountProfile) async
        -> CodexAccountProfileAuthenticationState
    typealias SelectionChanged = @MainActor () -> Void

    @Published private(set) var profiles: [CodexAccountProfile]
    @Published private(set) var selectedProfileID: UUID?
    @Published private(set) var authenticationState: CodexAccountProfileAuthenticationState = .unchecked
    @Published private(set) var profileRemovalErrorMessage: String?

    private let store: CodexAccountProfileStore
    private let bindingStore: CodexThreadAccountProfileBindingStore
    private let statusChecker: StatusChecker
    private let signInHandler: SignInHandler
    private let selectionChanged: SelectionChanged

    convenience init(
        defaults: UserDefaults = .standard,
        selectionChanged: @escaping SelectionChanged
    ) {
        let loginService = CodexAccountProfileLoginService()
        self.init(
            defaults: defaults,
            statusChecker: { profile in
                await loginService.status(for: profile)
            },
            signInHandler: { profile in
                await loginService.signIn(profile: profile)
            },
            selectionChanged: selectionChanged
        )
    }

    init(
        defaults: UserDefaults,
        statusChecker: @escaping StatusChecker,
        signInHandler: @escaping SignInHandler,
        selectionChanged: @escaping SelectionChanged
    ) {
        let store = CodexAccountProfileStore(defaults: defaults)
        self.store = store
        bindingStore = CodexThreadAccountProfileBindingStore(defaults: defaults)
        profiles = store.profiles
        selectedProfileID = store.selectedProfileID
        self.statusChecker = statusChecker
        self.signInHandler = signInHandler
        self.selectionChanged = selectionChanged
    }

    var selectedProfile: CodexAccountProfile? {
        profiles.first { $0.id == selectedProfileID }
    }

    @discardableResult
    func addProfile(label: String) -> CodexAccountProfile? {
        let previousSelection = selectedProfileID
        guard let profile = store.addProfile(label: label) else { return nil }
        syncFromStore()
        selectionDidChange(from: previousSelection)
        return profile
    }

    func selectProfile(id: UUID?) {
        let previousSelection = selectedProfileID
        store.selectProfile(id: id)
        syncFromStore()
        selectionDidChange(from: previousSelection)
    }

    @discardableResult
    func removeProfile(id: UUID) -> Bool {
        guard !bindingStore.hasBindings(to: id) else {
            profileRemovalErrorMessage =
                "This profile still owns bound tasks. Remove or hand off its bound tasks first."
            return false
        }
        let previousSelection = selectedProfileID
        store.removeProfile(id: id)
        syncFromStore()
        selectionDidChange(from: previousSelection)
        profileRemovalErrorMessage = nil
        return true
    }

    func canRemoveProfile(id: UUID) -> Bool {
        !bindingStore.hasBindings(to: id)
    }

    func refreshSelectedProfileStatus() async {
        guard let profile = selectedProfile else {
            authenticationState = .unchecked
            return
        }
        authenticationState = .checking
        let state = await statusChecker(profile)
        guard selectedProfileID == profile.id else { return }
        authenticationState = state
    }

    func signInSelectedProfile() async {
        guard let profile = selectedProfile, authenticationState != .signingIn else { return }
        authenticationState = .signingIn
        let state = await signInHandler(profile)
        guard selectedProfileID == profile.id else { return }
        authenticationState = state
        if state == .signedIn {
            selectionChanged()
        }
    }

    private func selectionDidChange(from previousSelection: UUID?) {
        guard previousSelection != selectedProfileID else { return }
        authenticationState = .unchecked
        selectionChanged()
    }

    private func syncFromStore() {
        profiles = store.profiles
        selectedProfileID = store.selectedProfileID
    }
}
