import Foundation
import Testing
@testable import CodexCompanion

@Suite(.serialized)
struct CodexAccountSwitchLiveAcceptanceTests {
    private enum LiveAcceptanceError: Error, CustomStringConvertible {
        case missingLiveDefaults
        case missingProfile(String)
        case taskCreation(CodexAppServerTaskCreationOutcome)
        case send(CodexAppServerSendOutcome)
        case timeout(threadID: String, expectedText: String)

        var description: String {
            switch self {
            case .missingLiveDefaults:
                return "The installed Companion preferences domain is unavailable."
            case .missingProfile(let label):
                return "The Companion profile named \(label) is unavailable."
            case .taskCreation(let outcome):
                return "The disposable task could not be created: \(outcome)."
            case .send(let outcome):
                return "The disposable task message was not accepted: \(outcome)."
            case .timeout(let threadID, let expectedText):
                return "Timed out waiting for \(threadID) to become idle with \(expectedText)."
            }
        }
    }

    @Test
    @MainActor
    func disposableTaskRoutesThroughBothPersistentProfileDaemons() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["CODEX_COMPANION_RUN_LIVE_ACCOUNT_SWITCH"] == "1" else {
            return
        }

        guard let defaults = UserDefaults(suiteName: "com.silverfire.codexcompanion") else {
            throw LiveAcceptanceError.missingLiveDefaults
        }
        let sourceLabel = environment[
            "CODEX_COMPANION_LIVE_ACCOUNT_SWITCH_SOURCE_LABEL"
        ] ?? "Main"
        let destinationLabel = environment[
            "CODEX_COMPANION_LIVE_ACCOUNT_SWITCH_DESTINATION_LABEL"
        ] ?? "acc 3"
        let profileStore = CodexAccountProfileStore(defaults: defaults)
        guard let sourceProfile = profileStore.profiles.first(where: {
            $0.label.caseInsensitiveCompare(sourceLabel) == .orderedSame
        }) else {
            throw LiveAcceptanceError.missingProfile(sourceLabel)
        }
        guard let destinationProfile = profileStore.profiles.first(where: {
            $0.label.caseInsensitiveCompare(destinationLabel) == .orderedSame
        }) else {
            throw LiveAcceptanceError.missingProfile(destinationLabel)
        }
        #expect(sourceProfile.id != destinationProfile.id)

        let selectedProfileBeforeTest = profileStore.selectedProfileID
        let profilesByID = Dictionary(
            uniqueKeysWithValues: profileStore.profiles.map { ($0.id, $0) }
        )
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let verifier = CodexAccountProfileRuntimeVerifier(identityStore: identities)
        let processStore = CodexProcessStore(defaults: defaults)
        let workspaceURL = FileManager.default.temporaryDirectory.appendingPathComponent(
            "codex-companion-live-account-switch-\(UUID().uuidString)",
            isDirectory: true
        )
        try FileManager.default.createDirectory(
            at: workspaceURL,
            withIntermediateDirectories: true
        )
        defer { try? FileManager.default.removeItem(at: workspaceURL) }

        let creator = CodexAppServerTaskCreator(
            selectedProfileProvider: { sourceProfile },
            profileProvider: { profilesByID[$0] },
            profileVerifier: { profile in
                await verifier.verify(profile)
            },
            profileBinder: { threadID, profileID in
                bindings.bind(threadID: threadID, to: profileID)
            }
        )
        let created = await creator.create(
            prompt: "Reply exactly LIVE_ROUTE_MAIN_OK and nothing else.",
            cwd: workspaceURL.path,
            reasoningEffort: "low",
            accountProfileID: sourceProfile.id,
            clientMessageID: "live-account-switch-main-\(UUID().uuidString)"
        )
        guard case .created(let threadID) = created else {
            throw LiveAcceptanceError.taskCreation(created)
        }
        #expect(bindings.profileID(for: threadID) == sourceProfile.id)

        var item = try await waitForIdleTask(
            threadID: threadID,
            expectedText: "LIVE_ROUTE_MAIN_OK",
            processStore: processStore
        )
        let rolloutURL = try #require(item.rolloutURL)
        let handoff = CodexThreadAccountHandoffService(
            bindingStore: bindings,
            identityStore: identities
        )

        let destinationHandoff = try await Task.detached(priority: .userInitiated) {
            try handoff.handoff(
                threadID: threadID,
                rolloutURL: rolloutURL,
                hasActiveTurn: false,
                to: destinationProfile
            )
        }.value
        #expect(destinationHandoff.threadID != threadID)
        #expect(bindings.profileID(for: threadID) == sourceProfile.id)
        #expect(bindings.profileID(for: destinationHandoff.threadID) == destinationProfile.id)

        let sender = CodexAppServerSender(
            profileResolver: { candidateThreadID in
                guard let profileID = bindings.profileID(for: candidateThreadID) else {
                    return nil
                }
                return profilesByID[profileID]
            },
            profileBindingResolver: { bindings.profileID(for: $0) },
            profileVerifier: { profile in
                await verifier.verify(profile)
            }
        )
        try await sendReply(
            "Reply exactly LIVE_ROUTE_ACCOUNT3_OK and nothing else.",
            threadID: destinationHandoff.threadID,
            cwd: item.cwd,
            sender: sender
        )
        item = try await waitForIdleTask(
            threadID: destinationHandoff.threadID,
            expectedText: "LIVE_ROUTE_ACCOUNT3_OK",
            processStore: processStore
        )

        let returnHandoff = try await Task.detached(priority: .userInitiated) {
            try handoff.handoff(
                threadID: destinationHandoff.threadID,
                rolloutURL: destinationHandoff.rolloutURL,
                hasActiveTurn: false,
                to: sourceProfile
            )
        }.value
        #expect(returnHandoff.threadID != destinationHandoff.threadID)
        #expect(bindings.profileID(for: destinationHandoff.threadID)
            == destinationProfile.id)
        #expect(bindings.profileID(for: returnHandoff.threadID) == sourceProfile.id)

        try await sendReply(
            "Reply exactly LIVE_ROUTE_MAIN_RETURN_OK and nothing else.",
            threadID: returnHandoff.threadID,
            cwd: item.cwd,
            sender: sender
        )
        _ = try await waitForIdleTask(
            threadID: returnHandoff.threadID,
            expectedText: "LIVE_ROUTE_MAIN_RETURN_OK",
            processStore: processStore
        )

        let selectedProfileAfterTest = CodexAccountProfileStore(
            defaults: defaults
        ).selectedProfileID
        #expect(selectedProfileAfterTest == selectedProfileBeforeTest)
    }

    @Test
    func syntheticUnboundThreadResolvesTheLiveSharedRuntimeAccount() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["CODEX_COMPANION_RUN_LIVE_SOURCE_RESOLUTION"] == "1" else {
            return
        }
        guard let defaults = UserDefaults(suiteName: "com.silverfire.codexcompanion") else {
            throw LiveAcceptanceError.missingLiveDefaults
        }

        let profileStore = CodexAccountProfileStore(defaults: defaults)
        let selectedProfileBeforeTest = profileStore.selectedProfileID
        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let bindings = CodexThreadAccountProfileBindingStore(defaults: defaults)
        let syntheticThreadID = "companion-live-source-resolution-\(UUID().uuidString.lowercased())"
        defer { bindings.removeBinding(for: syntheticThreadID) }

        let sharedIdentity = try await Task.detached(priority: .userInitiated) {
            try CodexSharedAccountRuntimeIdentityReader().identity()
        }.value
        let expectedMatches = profileStore.profiles.filter { profile in
            identities.identity(for: profile.id)?.matchesAccount(sharedIdentity) == true
        }
        #expect(expectedMatches.count == 1)
        let expectedProfile = try #require(expectedMatches.first)

        let resolvedProfileID = try await Task.detached(priority: .userInitiated) {
            try CodexThreadSourceProfileResolver(
                profilesProvider: { profileStore.profiles },
                bindingStore: bindings,
                identityStore: identities
            ).resolveProfileID(for: syntheticThreadID)
        }.value

        #expect(resolvedProfileID == expectedProfile.id)
        #expect(bindings.profileID(for: syntheticThreadID) == expectedProfile.id)
        #expect(CodexAccountProfileStore(defaults: defaults).selectedProfileID
            == selectedProfileBeforeTest)
    }

    @Test
    func temporarySharedRuntimeCanReadTheLiveAccountIdentity() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["CODEX_COMPANION_RUN_LIVE_SOURCE_RESOLUTION"] == "1" else {
            return
        }

        let identity = try await Task.detached(priority: .userInitiated) {
            try CodexSharedAccountRuntimeIdentityReader(
                client: CodexAppServerRPCClient(timeout: 15)
            ).identity()
        }.value

        #expect(identity.accountType == "chatgpt")
        #expect(!identity.email.isEmpty)
    }

    @Test
    func forkSeparatesTheSourceAndDestinationProfileRuntimes() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["CODEX_COMPANION_RUN_LIVE_FORK_PROBE"] == "1" else {
            return
        }
        guard let defaults = UserDefaults(suiteName: "com.silverfire.codexcompanion") else {
            throw LiveAcceptanceError.missingLiveDefaults
        }
        guard
            let sourceThreadID = environment["CODEX_COMPANION_LIVE_FORK_SOURCE_THREAD_ID"]?
                .trimmingCharacters(in: .whitespacesAndNewlines),
            !sourceThreadID.isEmpty
        else {
            throw LiveAcceptanceError.missingLiveDefaults
        }

        let profileStore = CodexAccountProfileStore(defaults: defaults)
        let sourceLabel = environment[
            "CODEX_COMPANION_LIVE_ACCOUNT_SWITCH_SOURCE_LABEL"
        ] ?? "Main"
        let destinationLabel = environment[
            "CODEX_COMPANION_LIVE_ACCOUNT_SWITCH_DESTINATION_LABEL"
        ] ?? "acc 3"
        guard let sourceProfile = profileStore.profiles.first(where: {
            $0.label.caseInsensitiveCompare(sourceLabel) == .orderedSame
        }) else {
            throw LiveAcceptanceError.missingProfile(sourceLabel)
        }
        guard let destinationProfile = profileStore.profiles.first(where: {
            $0.label.caseInsensitiveCompare(destinationLabel) == .orderedSame
        }) else {
            throw LiveAcceptanceError.missingProfile(destinationLabel)
        }

        let identities = CodexAccountProfileIdentityStore(defaults: defaults)
        let verifier = CodexAccountProfileRuntimeVerifier(identityStore: identities)
        #expect(await verifier.verify(sourceProfile))
        #expect(await verifier.verify(destinationProfile))

        let probe = try await Task.detached(priority: .userInitiated) {
            try Self.runForkOwnershipProbe(
                sourceThreadID: sourceThreadID,
                sourceProfile: sourceProfile,
                destinationProfile: destinationProfile
            )
        }.value

        #expect(probe.forkedThreadID != sourceThreadID)
        #expect(probe.forkedFromThreadID == sourceThreadID)
        #expect(probe.forkedRolloutURL.path.hasPrefix(
            CodexAccountProfileRuntime.daemonHomeURL(for: destinationProfile).path + "/sessions/"
        ))
        #expect(!probe.sourceLoadedThreadIDs.contains(probe.forkedThreadID))
        #expect(!probe.destinationLoadedThreadIDs.contains(sourceThreadID))
        #expect(probe.destinationLoadedThreadIDs.contains(probe.forkedThreadID))
    }

    private static func runForkOwnershipProbe(
        sourceThreadID: String,
        sourceProfile: CodexAccountProfile,
        destinationProfile: CodexAccountProfile
    ) throws -> LiveForkOwnershipProbe {
        let provider = CodexAccountProfileDaemonRPCClientProvider()
        let sourceClient = try provider.client(for: sourceProfile)
        let destinationClient = try provider.client(for: destinationProfile)
        let forkRequest = CodexRPCRequest(
            id: 2,
            method: "thread/fork",
            params: [
                "threadId": sourceThreadID,
                "excludeTurns": true,
                "deferGoalContinuation": true,
            ]
        )
        let forkResponses = try destinationClient.perform([forkRequest])
        guard
            let forkResult = forkResponses[forkRequest.id]?.result,
            let thread = forkResult["thread"] as? [String: Any],
            let forkedThreadID = thread["id"] as? String,
            let forkedFromThreadID = thread["forkedFromId"] as? String,
            let forkedPath = thread["path"] as? String
        else {
            throw CodexThreadAccountHandoffError.invalidResponse
        }

        defer {
            let archiveRequest = CodexRPCRequest(
                id: 5,
                method: "thread/archive",
                params: ["threadId": forkedThreadID]
            )
            _ = try? destinationClient.perform([archiveRequest])
        }

        let sourceLoaded = try loadedThreadIDs(client: sourceClient, requestID: 3)
        let destinationLoaded = try loadedThreadIDs(
            client: destinationClient,
            requestID: 4
        )
        return LiveForkOwnershipProbe(
            forkedThreadID: forkedThreadID,
            forkedFromThreadID: forkedFromThreadID,
            forkedRolloutURL: URL(fileURLWithPath: forkedPath).standardizedFileURL,
            sourceLoadedThreadIDs: sourceLoaded,
            destinationLoadedThreadIDs: destinationLoaded
        )
    }

    private static func loadedThreadIDs(
        client: any CodexAppServerRPCPerforming,
        requestID: Int
    ) throws -> Set<String> {
        let request = CodexRPCRequest(
            id: requestID,
            method: "thread/loaded/list",
            params: [:]
        )
        let responses = try client.perform([request])
        guard
            let result = responses[request.id]?.result,
            let threadIDs = result["data"] as? [String]
        else {
            throw CodexThreadAccountHandoffError.invalidResponse
        }
        return Set(threadIDs)
    }

    @MainActor
    private func sendReply(
        _ prompt: String,
        threadID: String,
        cwd: String?,
        sender: CodexAppServerSender
    ) async throws {
        let outcome = await sender.submit(
            prompt: prompt,
            threadID: threadID,
            cwd: cwd,
            action: .reply,
            expectedTurnID: nil,
            clientMessageID: "live-account-switch-reply-\(UUID().uuidString)",
            onQueued: {}
        )
        guard outcome == .sent else {
            throw LiveAcceptanceError.send(outcome)
        }
    }

    @MainActor
    private func waitForIdleTask(
        threadID: String,
        expectedText: String,
        processStore: CodexProcessStore,
        timeout: TimeInterval = 120
    ) async throws -> CodexProcessItem {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            processStore.refresh()
            while processStore.isLoading, Date() < deadline {
                try await Task.sleep(for: .milliseconds(200))
            }
            if let item = processStore.items.first(where: { $0.threadID == threadID }),
               item.status == .completed,
               item.activeTurnID == nil,
               item.fullMessage.contains(expectedText)
            {
                return item
            }
            try await Task.sleep(for: .milliseconds(800))
        }
        throw LiveAcceptanceError.timeout(
            threadID: threadID,
            expectedText: expectedText
        )
    }
}

private struct LiveForkOwnershipProbe: Sendable {
    var forkedThreadID: String
    var forkedFromThreadID: String
    var forkedRolloutURL: URL
    var sourceLoadedThreadIDs: Set<String>
    var destinationLoadedThreadIDs: Set<String>
}
