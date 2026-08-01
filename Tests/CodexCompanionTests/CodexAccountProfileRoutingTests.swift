import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAccountProfileRoutingTests {
    @Test
    func selectedProfileControlsNewWorkWhileBindingsControlExistingThreads() throws {
        let defaultsName = "CodexAccountProfileRoutingTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }

        let first = CodexAccountProfile(id: UUID(), label: "First")
        let second = CodexAccountProfile(id: UUID(), label: "Second")
        let profileStore = CodexAccountProfileStore(defaults: defaults)
        _ = profileStore.addProfile(label: first.label)
        _ = profileStore.addProfile(label: second.label)

        let storedProfiles = profileStore.profiles
        let storedFirst = try #require(storedProfiles.first { $0.label == first.label })
        let storedSecond = try #require(storedProfiles.first { $0.label == second.label })
        profileStore.selectProfile(id: storedSecond.id)

        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        bindings.bind(threadID: "thread-first", to: storedFirst.id)
        let resolver = CodexAccountProfileRouteResolver(defaults: defaults)

        #expect(resolver.selectedProfile() == storedSecond)
        #expect(resolver.profile(for: "thread-first") == storedFirst)
        #expect(resolver.profile(for: "thread-unbound") == nil)
    }

    @Test
    func profileDaemonUsesAnIsolatedCredentialHomeAndTheSharedThreadDatabase() {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let executableURL = URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
        let profileBaseURL = URL(fileURLWithPath: "/tmp/Codex Profiles", isDirectory: true)
        let sharedSQLiteHomeURL = URL(fileURLWithPath: "/tmp/Shared Codex", isDirectory: true)

        let command = CodexAccountProfileDaemonCommandFactory.start(
            profile: profile,
            executableURL: executableURL,
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )

        let expectedProfileHome = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: profileBaseURL
        )
        #expect(command.executableURL == executableURL)
        #expect(command.arguments == ["app-server", "daemon", "start"])
        #expect(command.environmentOverrides["CODEX_HOME"] == expectedProfileHome.path)
        #expect(command.environmentOverrides["CODEX_SQLITE_HOME"] == sharedSQLiteHomeURL.path)
        #expect(command.socketURL == expectedProfileHome
            .appendingPathComponent("app-server-control", isDirectory: true)
            .appendingPathComponent("app-server-control.sock"))
    }

    @Test
    func profileHomeLinksTheExistingManagedStandaloneRuntime() throws {
        let rootURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: rootURL) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let profileBaseURL = rootURL.appendingPathComponent("Profiles", isDirectory: true)
        let managedStandaloneURL = rootURL
            .appendingPathComponent("Shared Codex", isDirectory: true)
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
        let managedCodexURL = managedStandaloneURL
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")
        try FileManager.default.createDirectory(
            at: managedCodexURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data("managed codex".utf8).write(to: managedCodexURL)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755],
            ofItemAtPath: managedCodexURL.path
        )

        try CodexAccountProfileRuntime.prepareHome(
            for: profile,
            baseURL: profileBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )

        let profileStandaloneURL = CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: profileBaseURL
        )
        .appendingPathComponent("packages", isDirectory: true)
        .appendingPathComponent("standalone", isDirectory: true)
        let attributes = try FileManager.default.attributesOfItem(
            atPath: profileStandaloneURL.path
        )

        #expect(attributes[.type] as? FileAttributeType == .typeSymbolicLink)
        #expect(
            try FileManager.default.destinationOfSymbolicLink(
                atPath: profileStandaloneURL.path
            ) == managedStandaloneURL.path
        )
        #expect(FileManager.default.fileExists(
            atPath: profileStandaloneURL
                .appendingPathComponent("current", isDirectory: true)
                .appendingPathComponent("codex")
                .path
        ))
    }

    @Test
    func profileDaemonUsesAShortRuntimeHomeWithLinkedCredentials() throws {
        let rootURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: rootURL) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let daemonBaseURL = rootURL.appendingPathComponent("Runtime", isDirectory: true)
        let credentialBaseURL = rootURL.appendingPathComponent(
            "Credential Profiles",
            isDirectory: true
        )
        let managedStandaloneURL = rootURL
            .appendingPathComponent("Shared Codex", isDirectory: true)
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
        let managedCodexURL = managedStandaloneURL
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")
        try FileManager.default.createDirectory(
            at: managedCodexURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data("managed codex".utf8).write(to: managedCodexURL)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755],
            ofItemAtPath: managedCodexURL.path
        )

        let credentialHomeURL = CodexAccountProfileRuntime.homeURL(
            for: profile,
            baseURL: credentialBaseURL
        )
        try FileManager.default.createDirectory(
            at: credentialHomeURL,
            withIntermediateDirectories: true
        )
        let credentialAuthURL = credentialHomeURL.appendingPathComponent("auth.json")
        try Data("profile credentials".utf8).write(to: credentialAuthURL)
        try FileManager.default.createDirectory(
            at: credentialHomeURL.appendingPathComponent(
                "app-server-daemon",
                isDirectory: true
            ),
            withIntermediateDirectories: true
        )

        try CodexAccountProfileRuntime.prepareDaemonHome(
            for: profile,
            daemonBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )

        let daemonHomeURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: daemonBaseURL
        )
        let attributes = try FileManager.default.attributesOfItem(
            atPath: daemonHomeURL.path
        )

        #expect(attributes[.type] as? FileAttributeType == .typeDirectory)
        #expect(
            daemonHomeURL.resolvingSymlinksInPath().standardizedFileURL
                == daemonHomeURL.standardizedFileURL
        )
        let daemonAuthURL = daemonHomeURL.appendingPathComponent("auth.json")
        let authAttributes = try FileManager.default.attributesOfItem(
            atPath: daemonAuthURL.path
        )
        #expect(authAttributes[.type] as? FileAttributeType == .typeSymbolicLink)
        #expect(
            try FileManager.default.destinationOfSymbolicLink(
                atPath: daemonAuthURL.path
            ) == credentialAuthURL.path
        )
        #expect(!FileManager.default.fileExists(
            atPath: daemonHomeURL.appendingPathComponent(
                "app-server-daemon",
                isDirectory: true
            ).path
        ))
        #expect(FileManager.default.isExecutableFile(
            atPath: daemonHomeURL
                .appendingPathComponent("packages", isDirectory: true)
                .appendingPathComponent("standalone", isDirectory: true)
                .appendingPathComponent("current", isDirectory: true)
                .appendingPathComponent("codex")
                .path
        ))
    }

    @Test
    func profileDaemonReusesAnInheritedSocketAfterCoordinatorRestart() async throws {
        let rootURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: rootURL) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let daemonBaseURL = rootURL.appendingPathComponent("Runtime", isDirectory: true)
        let credentialBaseURL = rootURL.appendingPathComponent(
            "Credential Profiles",
            isDirectory: true
        )
        let sharedSQLiteHomeURL = rootURL.appendingPathComponent(
            "Shared Codex",
            isDirectory: true
        )
        let managedStandaloneURL = sharedSQLiteHomeURL
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
        let managedCodexURL = managedStandaloneURL
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")
        try FileManager.default.createDirectory(
            at: managedCodexURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data("managed codex".utf8).write(to: managedCodexURL)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755],
            ofItemAtPath: managedCodexURL.path
        )

        try CodexAccountProfileRuntime.prepareDaemonHome(
            for: profile,
            daemonBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )
        let socketURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: daemonBaseURL
        )
        .appendingPathComponent("app-server-control", isDirectory: true)
        .appendingPathComponent("app-server-control.sock")
        try FileManager.default.createDirectory(
            at: socketURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data().write(to: socketURL)

        let runner = DaemonCommandRunnerRecorder(status: 1)
        let coordinator = CodexAccountProfileDaemonCoordinator(
            profileBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            managedStandaloneURL: managedStandaloneURL,
            executableProvider: { managedCodexURL },
            commandRunner: { command in
                await runner.run(command)
            },
            socketProbe: { $0 == socketURL }
        )

        let command = try await coordinator.ensureRunning(for: profile)

        #expect(command.socketURL == socketURL)
        #expect(await runner.count == 0)
    }

    @Test
    func boundThreadUsesItsProfileTransportWithoutTouchingNativeIPC() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd, attachments in
                await nativeAttempts.record(
                    profile: nil,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            },
            profileResolver: { threadID in
                threadID == "thread-profile" ? profile : nil
            },
            profileSubmitter: {
                selectedProfile, prompt, threadID, cwd, action, expectedTurnID,
                clientMessageID, queuedNotification, attachments in
                _ = expectedTurnID
                _ = queuedNotification
                await profileAttempts.record(
                    profile: selectedProfile,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            }
        )

        let outcome = await sender.submit(
            prompt: "Continue with this account",
            threadID: "thread-profile",
            cwd: "/tmp/project",
            action: .steer,
            expectedTurnID: "turn-live",
            clientMessageID: "message-profile",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await nativeAttempts.count == 0)
        #expect(await profileAttempts.count == 1)
        #expect(await profileAttempts.profileIDs == [profile.id])
    }

    @Test
    func unboundThreadKeepsTheExistingNativeTransport() async {
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd, attachments in
                await nativeAttempts.record(
                    profile: nil,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            },
            profileResolver: { _ in nil },
            profileSubmitter: {
                profile, prompt, threadID, cwd, action, expectedTurnID,
                clientMessageID, queuedNotification, attachments in
                _ = expectedTurnID
                _ = queuedNotification
                await profileAttempts.record(
                    profile: profile,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            }
        )

        let outcome = await sender.submit(
            prompt: "Keep native routing",
            threadID: "thread-native",
            cwd: nil,
            action: .steer,
            expectedTurnID: "turn-native",
            clientMessageID: "message-native",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await nativeAttempts.count == 1)
        #expect(await profileAttempts.count == 0)
    }

    @Test
    func missingBoundProfileFailsWithoutTouchingAnotherAccountTransport() async {
        let missingProfileID = UUID()
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let sender = CodexAppServerSender(
            submitter: { prompt, threadID, action, clientMessageID, cwd, attachments in
                await nativeAttempts.record(
                    profile: nil,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            },
            profileResolver: { _ in nil },
            profileBindingResolver: { _ in missingProfileID },
            profileSubmitter: {
                profile, prompt, threadID, cwd, action, expectedTurnID,
                clientMessageID, queuedNotification, attachments in
                _ = expectedTurnID
                _ = queuedNotification
                await profileAttempts.record(
                    profile: profile,
                    prompt: prompt,
                    threadID: threadID,
                    action: action,
                    clientMessageID: clientMessageID,
                    cwd: cwd,
                    attachments: attachments
                )
                return .sent
            }
        )

        let outcome = await sender.submit(
            prompt: "Continue without changing accounts",
            threadID: "thread-missing-profile",
            cwd: nil,
            action: .steer,
            expectedTurnID: "turn-origin",
            clientMessageID: "message-missing-profile",
            onQueued: {}
        )

        #expect(outcome == .failed)
        #expect(await nativeAttempts.count == 0)
        #expect(await profileAttempts.count == 0)
    }

    @Test
    func selectedProfileCreatesNewWorkThroughItsDaemonAndPersistsTheBinding() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Second account")
        let sharedAttempts = TaskCreationRoutingRecorder()
        let profileAttempts = TaskCreationRoutingRecorder()
        let defaultsName = "CodexAccountProfileTaskCreationTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)

        let creator = CodexAppServerTaskCreator(
            selectedProfileProvider: { profile },
            sharedTaskCreator: { request in
                await sharedAttempts.record(profile: nil, request: request)
                return .created(threadID: "thread-shared")
            },
            profileTaskCreator: { selectedProfile, request in
                await profileAttempts.record(profile: selectedProfile, request: request)
                return .created(threadID: "thread-profile")
            },
            profileBinder: { threadID, profileID in
                bindings.bind(threadID: threadID, to: profileID)
            }
        )

        let outcome = await creator.create(
            prompt: "Start on this account",
            cwd: "/tmp/project",
            clientMessageID: "message-new-profile"
        )

        #expect(outcome == .created(threadID: "thread-profile"))
        #expect(await sharedAttempts.count == 0)
        #expect(await profileAttempts.count == 1)
        #expect(await profileAttempts.profileIDs == [profile.id])
        #expect(bindings.profileID(for: "thread-profile") == profile.id)
    }

    @Test
    func explicitProfileCreatesNewWorkWithoutChangingTheMacSelection() async throws {
        let selectedProfile = CodexAccountProfile(id: UUID(), label: "Main")
        let requestedProfile = CodexAccountProfile(id: UUID(), label: "Account 3")
        let sharedAttempts = TaskCreationRoutingRecorder()
        let profileAttempts = TaskCreationRoutingRecorder()
        let defaultsName = "CodexAccountProfileExplicitTaskCreationTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)

        let creator = CodexAppServerTaskCreator(
            selectedProfileProvider: { selectedProfile },
            profileProvider: { profileID in
                profileID == requestedProfile.id ? requestedProfile : nil
            },
            sharedTaskCreator: { request in
                await sharedAttempts.record(profile: nil, request: request)
                return .created(threadID: "thread-shared")
            },
            profileTaskCreator: { profile, request in
                await profileAttempts.record(profile: profile, request: request)
                return .created(threadID: "thread-explicit")
            },
            profileBinder: { threadID, profileID in
                bindings.bind(threadID: threadID, to: profileID)
            }
        )

        let outcome = await creator.create(
            prompt: "Start on Account 3",
            cwd: "/tmp/project",
            accountProfileID: requestedProfile.id,
            clientMessageID: "message-new-explicit-profile"
        )

        #expect(outcome == .created(threadID: "thread-explicit"))
        #expect(await sharedAttempts.count == 0)
        #expect(await profileAttempts.profileIDs == [requestedProfile.id])
        #expect(bindings.profileID(for: "thread-explicit") == requestedProfile.id)
    }

    @Test
    func noSelectedProfileKeepsNewWorkOnTheSharedTransport() async {
        let sharedAttempts = TaskCreationRoutingRecorder()
        let profileAttempts = TaskCreationRoutingRecorder()
        let creator = CodexAppServerTaskCreator(
            selectedProfileProvider: { nil },
            sharedTaskCreator: { request in
                await sharedAttempts.record(profile: nil, request: request)
                return .created(threadID: "thread-shared")
            },
            profileTaskCreator: { profile, request in
                await profileAttempts.record(profile: profile, request: request)
                return .created(threadID: "thread-profile")
            },
            profileBinder: { _, _ in }
        )

        let outcome = await creator.create(
            prompt: "Use the normal account",
            cwd: nil,
            clientMessageID: "message-new-shared"
        )

        #expect(outcome == .created(threadID: "thread-shared"))
        #expect(await sharedAttempts.count == 1)
        #expect(await profileAttempts.count == 0)
    }
}

private actor ProfileRoutingSendRecorder {
    private(set) var count = 0
    private(set) var profileIDs: [UUID] = []

    func record(
        profile: CodexAccountProfile?,
        prompt: String,
        threadID: String,
        action: CodexSendAction,
        clientMessageID: String,
        cwd: String?,
        attachments: [CodexFollowerAttachment]
    ) {
        _ = prompt
        _ = threadID
        _ = action
        _ = clientMessageID
        _ = cwd
        _ = attachments
        count += 1
        if let profile {
            profileIDs.append(profile.id)
        }
    }
}

private actor TaskCreationRoutingRecorder {
    private(set) var count = 0
    private(set) var profileIDs: [UUID] = []

    func record(
        profile: CodexAccountProfile?,
        request: CodexAppServerTaskCreationRequest
    ) {
        _ = request
        count += 1
        if let profile {
            profileIDs.append(profile.id)
        }
    }
}

private actor DaemonCommandRunnerRecorder {
    private(set) var count = 0
    private let status: Int32

    init(status: Int32) {
        self.status = status
    }

    func run(_ command: CodexAccountProfileDaemonCommand) -> Int32 {
        _ = command
        count += 1
        return status
    }
}
