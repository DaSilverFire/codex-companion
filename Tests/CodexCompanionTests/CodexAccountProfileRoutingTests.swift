import Foundation
import Testing
@testable import CodexCompanion

@Suite
struct CodexAccountProfileRoutingTests {
    @Test
    func managedStandaloneCodexIsPreferredForProfileDaemonTraffic() {
        let expectedURL = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex", isDirectory: true)
            .appendingPathComponent("packages", isDirectory: true)
            .appendingPathComponent("standalone", isDirectory: true)
            .appendingPathComponent("current", isDirectory: true)
            .appendingPathComponent("codex")

        #expect(WorkspacePaths.codexExecutableURLs.first == expectedURL)
    }

    @Test
    func profileProxyArgumentsPinTheExactDaemonSocket() {
        let socketURL = URL(
            fileURLWithPath: "/tmp/profile/app-server-control/app-server-control.sock"
        )

        #expect(CodexAppServerProxyCommand.arguments(socketURL: socketURL) == [
            "app-server",
            "proxy",
            "--sock",
            socketURL.path,
        ])
        #expect(CodexAppServerProxyCommand.arguments(socketURL: nil) == [
            "app-server",
            "proxy",
        ])
        #expect(CodexAppServerProxyCommand.initializeRequestID(
            excluding: [1, 0, -1]
        ) == -2)
    }

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
    func profileDaemonRestartUsesTheSameIsolatedRuntime() {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let executableURL = URL(fileURLWithPath: "/Applications/ChatGPT.app/Contents/Resources/codex")
        let profileBaseURL = URL(fileURLWithPath: "/tmp/Codex Profiles", isDirectory: true)
        let sharedSQLiteHomeURL = URL(fileURLWithPath: "/tmp/Shared Codex", isDirectory: true)

        let command = CodexAccountProfileDaemonCommandFactory.restart(
            profile: profile,
            executableURL: executableURL,
            profileBaseURL: profileBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL
        )

        #expect(command.arguments == ["app-server", "daemon", "restart"])
        #expect(command.environmentOverrides["CODEX_HOME"] == CodexAccountProfileRuntime
            .daemonHomeURL(for: profile, baseURL: profileBaseURL).path)
        #expect(command.environmentOverrides["CODEX_SQLITE_HOME"] == sharedSQLiteHomeURL.path)
    }

    @Test
    func profileDaemonRuntimeInspectorRequiresBothExpectedHomes() {
        let daemonHomePath = "/tmp/Profile Runtime"
        let sqliteHomePath = "/tmp/Shared Codex"

        #expect(CodexAccountProfileDaemonRuntimeInspector.compatibility(
            environment: [
                "CODEX_HOME": daemonHomePath,
                "CODEX_SQLITE_HOME": sqliteHomePath,
            ],
            expectedDaemonHomePath: daemonHomePath,
            expectedSQLiteHomePath: sqliteHomePath
        ) == .compatible)
        #expect(CodexAccountProfileDaemonRuntimeInspector.compatibility(
            environment: ["CODEX_HOME": daemonHomePath],
            expectedDaemonHomePath: daemonHomePath,
            expectedSQLiteHomePath: sqliteHomePath
        ) == .incompatible)
        #expect(CodexAccountProfileDaemonRuntimeInspector.compatibility(
            environment: [
                "CODEX_HOME": daemonHomePath,
                "CODEX_SQLITE_HOME": "/tmp/Wrong Catalog",
            ],
            expectedDaemonHomePath: daemonHomePath,
            expectedSQLiteHomePath: sqliteHomePath
        ) == .incompatible)
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
    func profileDaemonPreservesRuntimeOwnedDirectoriesWhenPreparingAnExistingHome() throws {
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
        let runtimeOwnedDirectoryNames = ["cache", "mcp-oauth-locks", "shell_snapshots"]
        for name in runtimeOwnedDirectoryNames {
            try FileManager.default.createDirectory(
                at: credentialHomeURL.appendingPathComponent(name, isDirectory: true),
                withIntermediateDirectories: true
            )
        }

        let daemonHomeURL = CodexAccountProfileRuntime.daemonHomeURL(
            for: profile,
            baseURL: daemonBaseURL
        )
        var daemonMarkers: [URL] = []
        for name in runtimeOwnedDirectoryNames {
            let directoryURL = daemonHomeURL.appendingPathComponent(name, isDirectory: true)
            try FileManager.default.createDirectory(
                at: directoryURL,
                withIntermediateDirectories: true
            )
            let markerURL = directoryURL.appendingPathComponent("daemon-owned")
            try Data("keep".utf8).write(to: markerURL)
            daemonMarkers.append(markerURL)
        }

        try CodexAccountProfileRuntime.prepareDaemonHome(
            for: profile,
            daemonBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            managedStandaloneURL: managedStandaloneURL
        )

        for markerURL in daemonMarkers {
            #expect(FileManager.default.fileExists(atPath: markerURL.path))
        }
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
            socketProbe: { $0 == socketURL },
            runtimeProbe: { _ in .compatible }
        )

        let command = try await coordinator.ensureRunning(for: profile)

        #expect(command.socketURL == socketURL)
        #expect(await runner.count == 0)
    }

    @Test
    func profileDaemonRestartsAReachableRuntimeWithTheWrongCatalogEnvironment() async throws {
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

        let runner = DaemonCommandRunnerRecorder(status: 0)
        let runtimeProbe = DaemonRuntimeProbeRecorder(results: [
            .incompatible,
            .compatible,
        ])
        let coordinator = CodexAccountProfileDaemonCoordinator(
            profileBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            managedStandaloneURL: managedStandaloneURL,
            executableProvider: { managedCodexURL },
            commandRunner: { command in
                await runner.run(command)
            },
            socketProbe: { _ in true },
            runtimeProbe: { command in
                await runtimeProbe.probe(command)
            }
        )

        let command = try await coordinator.ensureRunning(for: profile)

        #expect(command.arguments == ["app-server", "daemon", "restart"])
        #expect(command.environmentOverrides["CODEX_HOME"] == CodexAccountProfileRuntime
            .daemonHomeURL(for: profile, baseURL: daemonBaseURL).path)
        #expect(command.environmentOverrides["CODEX_SQLITE_HOME"] == sharedSQLiteHomeURL.path)
        #expect(await runner.commands.map(\.arguments) == [[
            "app-server",
            "daemon",
            "restart",
        ]])
        #expect(await runtimeProbe.count == 2)
    }

    @Test
    func profileDaemonRecoversAStalledRestartBeforeStartingTheProfileAgain() async throws {
        let rootURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: rootURL) }

        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let daemonBaseURL = rootURL.appendingPathComponent("Runtime", isDirectory: true)
        let credentialBaseURL = rootURL.appendingPathComponent("Credentials", isDirectory: true)
        let sharedSQLiteHomeURL = rootURL.appendingPathComponent("Shared", isDirectory: true)
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

        let runner = DaemonCommandSequenceRecorder(statuses: [1, 0])
        let runtimeProbe = DaemonRuntimeProbeRecorder(results: [
            .incompatible,
            .compatible,
        ])
        let recovery = DaemonRecoveryRecorder(result: true)
        let coordinator = CodexAccountProfileDaemonCoordinator(
            profileBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            managedStandaloneURL: managedStandaloneURL,
            executableProvider: { managedCodexURL },
            commandRunner: { command in
                await runner.run(command)
            },
            socketProbe: { _ in true },
            runtimeProbe: { command in
                await runtimeProbe.probe(command)
            },
            stalledRestartRecovery: { command in
                await recovery.recover(command)
            }
        )

        let command = try await coordinator.ensureRunning(for: profile)

        #expect(command.arguments == ["app-server", "daemon", "start"])
        #expect(await runner.commands.map(\.arguments) == [
            ["app-server", "daemon", "restart"],
            ["app-server", "daemon", "start"],
        ])
        #expect(await recovery.count == 1)
    }

    @Test
    func profileDaemonRestartRunsEvenWhenThePreviousSocketIsReachable() async throws {
        let rootURL = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: rootURL) }
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let daemonBaseURL = rootURL.appendingPathComponent("Runtime", isDirectory: true)
        let credentialBaseURL = rootURL.appendingPathComponent("Credentials", isDirectory: true)
        let sharedSQLiteHomeURL = rootURL.appendingPathComponent("Shared", isDirectory: true)
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

        let runner = DaemonCommandRunnerRecorder(status: 0)
        let coordinator = CodexAccountProfileDaemonCoordinator(
            profileBaseURL: daemonBaseURL,
            credentialBaseURL: credentialBaseURL,
            sharedSQLiteHomeURL: sharedSQLiteHomeURL,
            managedStandaloneURL: managedStandaloneURL,
            executableProvider: { managedCodexURL },
            commandRunner: { command in
                await runner.run(command)
            },
            socketProbe: { _ in true },
            runtimeProbe: { _ in .compatible }
        )

        let command = try await coordinator.restart(for: profile)

        #expect(command.arguments == ["app-server", "daemon", "restart"])
        #expect(await runner.count == 1)
        #expect(await runner.commands.map(\.arguments) == [["app-server", "daemon", "restart"]])
    }

    @Test
    func boundLoadedThreadUsesNativeIPCWithoutTouchingProfileTransport() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let verifications = ProfileRuntimeVerificationRecorder(result: true)
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
            profileVerifier: { verifiedProfile in
                await verifications.verify(verifiedProfile)
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
        #expect(await nativeAttempts.count == 1)
        #expect(await profileAttempts.count == 0)
        #expect(await verifications.profileIDs.isEmpty)
    }

    @Test
    func boundUnloadedThreadFallsBackToItsVerifiedProfileTransport() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Main")
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let verifications = ProfileRuntimeVerificationRecorder(result: true)
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
                return .threadNotLoaded
            },
            profileResolver: { threadID in
                threadID == "thread-profile" ? profile : nil
            },
            profileVerifier: { verifiedProfile in
                await verifications.verify(verifiedProfile)
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
            clientMessageID: "message-profile-fallback",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await nativeAttempts.count == 1)
        #expect(await profileAttempts.count == 1)
        #expect(await profileAttempts.profileIDs == [profile.id])
        #expect(await verifications.profileIDs == [profile.id])
    }

    @Test
    func canonicalThreadRoutesRepliesToItsActivePhysicalFork() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let profileAttempts = ProfileRoutingSendRecorder()
        let sender = CodexAppServerSender(
            profileResolver: { threadID in
                threadID == "physical-fork" ? profile : nil
            },
            profileVerifier: { _ in true },
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
            },
            runtimeThreadResolver: { threadID in
                threadID == "canonical-thread" ? "physical-fork" : threadID
            }
        )

        let outcome = await sender.submit(
            prompt: "Continue the same task",
            threadID: "canonical-thread",
            cwd: "/tmp/project",
            action: .steer,
            expectedTurnID: "turn-live",
            clientMessageID: "message-lineage",
            onQueued: {}
        )

        #expect(outcome == .sent)
        #expect(await profileAttempts.threadIDs == ["physical-fork"])
    }

    @Test
    func boundUnloadedThreadRejectsAMismatchedProfileRuntimeBeforeFallback() async {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let nativeAttempts = ProfileRoutingSendRecorder()
        let profileAttempts = ProfileRoutingSendRecorder()
        let verifications = ProfileRuntimeVerificationRecorder(result: false)
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
                return .threadNotLoaded
            },
            profileResolver: { _ in profile },
            profileVerifier: { verifiedProfile in
                await verifications.verify(verifiedProfile)
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
            prompt: "Do not send this through the wrong account",
            threadID: "thread-profile",
            cwd: nil,
            action: .steer,
            expectedTurnID: "turn-live",
            clientMessageID: "message-mismatch",
            onQueued: {}
        )

        #expect(outcome == .failed)
        #expect(await verifications.profileIDs == [profile.id])
        #expect(await nativeAttempts.count == 1)
        #expect(await profileAttempts.count == 0)
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
            profileVerifier: { $0.id == profile.id },
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
            profileVerifier: { $0.id == requestedProfile.id },
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
    func newProfileTaskIsNotCreatedOrBoundWhenRuntimeVerificationFails() async throws {
        let profile = CodexAccountProfile(id: UUID(), label: "Backup")
        let profileAttempts = TaskCreationRoutingRecorder()
        let defaultsName = "CodexAccountProfileRejectedTaskCreationTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: defaultsName))
        defer { defaults.removePersistentDomain(forName: defaultsName) }
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)

        let creator = CodexAppServerTaskCreator(
            selectedProfileProvider: { profile },
            profileVerifier: { _ in false },
            profileTaskCreator: { selectedProfile, request in
                await profileAttempts.record(profile: selectedProfile, request: request)
                return .created(threadID: "thread-wrong-account")
            },
            profileBinder: { threadID, profileID in
                bindings.bind(threadID: threadID, to: profileID)
            }
        )

        let outcome = await creator.create(
            prompt: "Do not create this under the wrong account",
            cwd: nil,
            clientMessageID: "message-new-rejected"
        )

        #expect(outcome == .failed)
        #expect(await profileAttempts.count == 0)
        #expect(bindings.profileID(for: "thread-wrong-account") == nil)
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
    private(set) var threadIDs: [String] = []

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
        _ = action
        _ = clientMessageID
        _ = cwd
        _ = attachments
        count += 1
        threadIDs.append(threadID)
        if let profile {
            profileIDs.append(profile.id)
        }
    }
}

private actor ProfileRuntimeVerificationRecorder {
    private(set) var profileIDs: [UUID] = []
    private let result: Bool

    init(result: Bool) {
        self.result = result
    }

    func verify(_ profile: CodexAccountProfile) -> Bool {
        profileIDs.append(profile.id)
        return result
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
    private(set) var commands: [CodexAccountProfileDaemonCommand] = []
    private let status: Int32

    init(status: Int32) {
        self.status = status
    }

    func run(_ command: CodexAccountProfileDaemonCommand) -> Int32 {
        count += 1
        commands.append(command)
        return status
    }
}

private actor DaemonRuntimeProbeRecorder {
    private(set) var count = 0
    private var results: [CodexAccountProfileDaemonRuntimeCompatibility]

    init(results: [CodexAccountProfileDaemonRuntimeCompatibility]) {
        self.results = results
    }

    func probe(
        _ command: CodexAccountProfileDaemonCommand
    ) -> CodexAccountProfileDaemonRuntimeCompatibility {
        _ = command
        count += 1
        guard !results.isEmpty else { return .unavailable }
        return results.removeFirst()
    }
}

private actor DaemonCommandSequenceRecorder {
    private(set) var commands: [CodexAccountProfileDaemonCommand] = []
    private var statuses: [Int32]

    init(statuses: [Int32]) {
        self.statuses = statuses
    }

    func run(_ command: CodexAccountProfileDaemonCommand) -> Int32 {
        commands.append(command)
        guard !statuses.isEmpty else { return 1 }
        return statuses.removeFirst()
    }
}

private actor DaemonRecoveryRecorder {
    private(set) var count = 0
    private let result: Bool

    init(result: Bool) {
        self.result = result
    }

    func recover(_ command: CodexAccountProfileDaemonCommand) -> Bool {
        _ = command
        count += 1
        return result
    }
}
