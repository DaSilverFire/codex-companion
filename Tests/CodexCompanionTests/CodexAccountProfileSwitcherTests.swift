import Foundation
import Testing
@testable import CodexCompanion

@Suite
@MainActor
struct CodexAccountProfileSwitcherTests {
    @Test
    func profileAliasesAreTrimmedDeduplicatedAndPersistedWithoutCredentials() throws {
        let suiteName = "CodexAccountProfileSwitcherTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let store = CodexAccountProfileStore(defaults: defaults)
        guard
            let personal = store.addProfile(label: "  Personal  "),
            let work = store.addProfile(label: "Work")
        else {
            Issue.record("Valid profile labels should be accepted.")
            return
        }

        #expect(store.addProfile(label: "personal") == nil)
        #expect(store.addProfile(label: "   ") == nil)
        #expect(store.profiles.map(\.label) == ["Personal", "Work"])

        store.selectProfile(id: work.id)
        let restored = CodexAccountProfileStore(defaults: defaults)
        #expect(restored.profiles == [personal, work])
        #expect(restored.selectedProfileID == work.id)

        guard let persistedData = defaults.data(forKey: CodexAccountProfileStore.profilesKey) else {
            Issue.record("Profile aliases should be persisted.")
            return
        }
        guard let persistedProfiles = try JSONSerialization.jsonObject(
            with: persistedData
        ) as? [[String: Any]] else {
            Issue.record("Profile aliases should use the expected JSON representation.")
            return
        }
        #expect(persistedProfiles.count == 2)
        #expect(persistedProfiles.allSatisfy { Set($0.keys) == ["id", "label"] })
    }

    @Test
    func officialLoginCommandsAreScopedToTheOpaqueProfileHome() {
        let profile = CodexAccountProfile(id: UUID(), label: "Main Account")
        let baseURL = URL(fileURLWithPath: "/tmp/CodexProfileTests", isDirectory: true)
        let executableURL = URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")

        let signIn = CodexAccountProfileLoginCommandFactory.signIn(
            profile: profile,
            executableURL: executableURL,
            baseURL: baseURL
        )
        let status = CodexAccountProfileLoginCommandFactory.status(
            profile: profile,
            executableURL: executableURL,
            baseURL: baseURL
        )
        let expectedHome = CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: baseURL
        ).path

        #expect(signIn.executableURL == executableURL)
        #expect(signIn.arguments == ["login"])
        #expect(status.arguments == ["login", "status"])
        #expect(signIn.environmentOverrides == [
            "CODEX_HOME": expectedHome,
            "CODEX_SQLITE_HOME": expectedHome,
        ])
        #expect(!signIn.arguments.joined(separator: " ").contains("token"))
        #expect(!signIn.arguments.joined(separator: " ").contains("api-key"))
        #expect(!signIn.environmentOverrides.keys.contains(where: {
            $0.localizedCaseInsensitiveContains("token")
                || $0.localizedCaseInsensitiveContains("key")
        }))
    }

    @Test
    func selectingAndSigningInAProfileUsesOnlyTheManualProfileFlow() async throws {
        let (suiteName, defaults) = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let recorder = ProfileAuthenticationRecorder()
        let switcher = CodexAccountProfileSwitcher(
            defaults: defaults,
            statusChecker: { profile in
                recorder.statusChecks.append(profile.id)
                return .signedOut
            },
            signInHandler: { profile in
                recorder.signIns.append(profile.id)
                return .signedIn
            },
            selectionChanged: {
                recorder.selectionChangeCount += 1
            }
        )
        let profile = try #require(switcher.addProfile(label: "Main"))

        await switcher.refreshSelectedProfileStatus()
        #expect(switcher.authenticationState == .signedOut)
        #expect(recorder.statusChecks == [profile.id])

        await switcher.signInSelectedProfile()
        #expect(switcher.authenticationState == .signedIn)
        #expect(recorder.signIns == [profile.id])
        #expect(recorder.selectionChangeCount == 2)
    }

    @Test
    func selectingAnotherProfileDoesNotSignInOrMoveAnyTaskAutomatically() async throws {
        let (suiteName, defaults) = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let recorder = ProfileAuthenticationRecorder()
        let switcher = CodexAccountProfileSwitcher(
            defaults: defaults,
            statusChecker: { profile in
                recorder.statusChecks.append(profile.id)
                return .signedOut
            },
            signInHandler: { profile in
                recorder.signIns.append(profile.id)
                return .signedIn
            },
            selectionChanged: {
                recorder.selectionChangeCount += 1
            }
        )
        _ = try #require(switcher.addProfile(label: "Backup"))
        let main = try #require(switcher.addProfile(label: "Main"))

        switcher.selectProfile(id: main.id)
        await switcher.refreshSelectedProfileStatus()

        #expect(switcher.selectedProfileID == main.id)
        #expect(recorder.statusChecks == [main.id])
        #expect(recorder.signIns.isEmpty)
        #expect(recorder.selectionChangeCount == 2)
    }

    @Test
    func successfulSignInRefreshesAndVerifiesThePersistentProfileDaemonBeforeReportingSignedIn() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let recorder = ProfileLoginServiceRecorder()
        let suiteName = "CodexAccountProfileLoginServiceTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let service = CodexAccountProfileLoginService(
            baseURL: FileManager.default.temporaryDirectory,
            executableProvider: {
                URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
            },
            commandRunner: { command in
                await recorder.recordLogin(command)
                return 0
            },
            daemonRefresher: { refreshedProfile in
                await recorder.recordRefresh(refreshedProfile.id)
            },
            runtimeIdentityReader: { verifiedProfile in
                await recorder.recordIdentityRead(verifiedProfile.id)
                return CodexAccountProfileIdentity(
                    accountType: "chatgpt",
                    email: "Backup@Example.com",
                    planType: "pro"
                )
            },
            identityStore: identities
        )

        let state = await service.signIn(profile: profile)

        #expect(state == .signedIn)
        #expect(await recorder.refreshedProfileIDs == [profile.id])
        #expect(await recorder.loginCommandCount == 1)
        #expect(await recorder.events == ["login", "refresh", "identity"])
        #expect(identities.identity(for: profile.id)?.email == "backup@example.com")
    }

    @Test
    func daemonRefreshFailureDoesNotReportTheProfileAsSignedIn() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let service = CodexAccountProfileLoginService(
            baseURL: FileManager.default.temporaryDirectory,
            executableProvider: {
                URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
            },
            commandRunner: { _ in 0 },
            daemonRefresher: { _ in
                throw ProfileLoginServiceTestError.refreshFailed
            },
            runtimeIdentityReader: { _ in
                Issue.record("Identity must not be read after daemon refresh fails.")
                throw ProfileLoginServiceTestError.identityReadFailed
            }
        )

        let state = await service.signIn(profile: profile)

        guard case .failed(let message) = state else {
            Issue.record("A stale profile daemon must not be reported as signed in.")
            return
        }
        #expect(message.contains("refresh failed"))
    }

    @Test
    func signInRejectsADaemonAuthenticatedAsAnotherKnownAccount() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let suiteName = "CodexAccountProfileLoginServiceTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: profile.id
        )
        let service = CodexAccountProfileLoginService(
            baseURL: FileManager.default.temporaryDirectory,
            executableProvider: {
                URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
            },
            commandRunner: { _ in 0 },
            daemonRefresher: { _ in },
            runtimeIdentityReader: { _ in
                CodexAccountProfileIdentity(
                    accountType: "chatgpt",
                    email: "wrong@example.com",
                    planType: "pro"
                )
            },
            identityStore: identities
        )

        let state = await service.signIn(profile: profile)

        #expect(state == .failed(
            "The Codex account service is signed in to a different ChatGPT account than this profile."
        ))
        #expect(identities.identity(for: profile.id)?.email == "backup@example.com")
    }

    @Test
    func loginStatusDoesNotTreatAReachableMismatchedDaemonAsSignedIn() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let suiteName = "CodexAccountProfileLoginServiceTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        identities.save(
            CodexAccountProfileIdentity(
                accountType: "chatgpt",
                email: "backup@example.com",
                planType: "pro"
            ),
            for: profile.id
        )
        let service = CodexAccountProfileLoginService(
            baseURL: FileManager.default.temporaryDirectory,
            executableProvider: {
                URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
            },
            commandRunner: { _ in 0 },
            runtimeIdentityReader: { _ in
                CodexAccountProfileIdentity(
                    accountType: "chatgpt",
                    email: "wrong@example.com",
                    planType: "pro"
                )
            },
            identityStore: identities
        )

        let state = await service.status(for: profile)

        #expect(state == .failed(
            "The Codex account service is signed in to a different ChatGPT account than this profile."
        ))
    }

    @Test
    func profileOwningPersistedTasksCannotBeRemoved() throws {
        let (suiteName, defaults) = try makeDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }
        let switcher = CodexAccountProfileSwitcher(
            defaults: defaults,
            statusChecker: { _ in .signedIn },
            signInHandler: { _ in .signedIn },
            selectionChanged: {}
        )
        let profile = try #require(switcher.addProfile(label: "Origin account"))
        CodexThreadAccountProfileBindingStore(defaults: defaults).bind(
            threadID: "thread-origin",
            to: profile.id
        )

        switcher.removeProfile(id: profile.id)

        #expect(switcher.profiles == [profile])
        #expect(switcher.selectedProfileID == profile.id)
        #expect(switcher.profileRemovalErrorMessage?.contains("bound tasks") == true)
    }

    private func makeDefaults() throws -> (String, UserDefaults) {
        let suiteName = "CodexAccountProfileSwitcherTests.\(UUID().uuidString)"
        return (suiteName, try #require(UserDefaults(suiteName: suiteName)))
    }
}

@MainActor
private final class ProfileAuthenticationRecorder {
    var statusChecks: [UUID] = []
    var signIns: [UUID] = []
    var selectionChangeCount = 0
}

private actor ProfileLoginServiceRecorder {
    private(set) var refreshedProfileIDs: [UUID] = []
    private(set) var loginCommandCount = 0
    private(set) var events: [String] = []

    func recordLogin(_ command: CodexAccountProfileLoginCommand) {
        _ = command
        loginCommandCount += 1
        events.append("login")
    }

    func recordRefresh(_ profileID: UUID) {
        refreshedProfileIDs.append(profileID)
        events.append("refresh")
    }

    func recordIdentityRead(_ profileID: UUID) {
        _ = profileID
        events.append("identity")
    }
}

private enum ProfileLoginServiceTestError: LocalizedError {
    case refreshFailed
    case identityReadFailed

    var errorDescription: String? {
        switch self {
        case .refreshFailed:
            return "Profile daemon refresh failed."
        case .identityReadFailed:
            return "Profile identity read failed."
        }
    }
}
