import Foundation
import Testing
@testable import CodexCompanion

@Suite("Companion relay connection")
struct CompanionRelayConnectionTests {
    @Test("keepalive repeats until the transport reports a failure")
    func keepAliveRepeatsUntilFailure() async {
        let counter = RelayKeepAliveCounter()

        do {
            try await CompanionRelayKeepAliveLoop.run(
                intervalNanoseconds: 1_000_000
            ) {
                let count = await counter.increment()
                if count == 3 {
                    throw RelayKeepAliveTestError.expectedFailure
                }
            }
            Issue.record("The keepalive loop should surface the transport failure.")
        } catch RelayKeepAliveTestError.expectedFailure {
            // Expected: the owner can now reconnect the relay.
        } catch {
            Issue.record("Unexpected keepalive error: \(error)")
        }

        #expect(await counter.value == 3)
    }

    @Test("keepalive stops before pinging when its owner cancels it")
    func keepAliveStopsWhenCancelled() async {
        let counter = RelayKeepAliveCounter()
        let task = Task {
            try await CompanionRelayKeepAliveLoop.run(
                intervalNanoseconds: 1_000_000_000
            ) {
                _ = await counter.increment()
            }
        }

        task.cancel()
        do {
            try await task.value
            Issue.record("Cancellation should stop the keepalive loop.")
        } catch is CancellationError {
            // Expected.
        } catch {
            Issue.record("Unexpected keepalive cancellation error: \(error)")
        }

        #expect(await counter.value == 0)
    }

    @Test("uses the bundled secure relay when no user override exists")
    func configuredURLUsesBundledDefault() throws {
        let defaults = try makeDefaults()

        let url = CompanionRelaySettings.configuredURL(
            defaults: defaults,
            bundledURLString: "wss://relay.codexcompanion.example/relay"
        )

        #expect(url?.absoluteString == "wss://relay.codexcompanion.example/relay")
    }

    @Test("a user relay override wins over the bundled secure relay")
    func configuredURLPrefersUserOverride() throws {
        let defaults = try makeDefaults()
        defaults.set(
            "wss://override.example/relay",
            forKey: CompanionRelaySettings.relayURLKey
        )

        let url = CompanionRelaySettings.configuredURL(
            defaults: defaults,
            bundledURLString: "wss://relay.codexcompanion.example/relay"
        )

        #expect(url?.absoluteString == "wss://override.example/relay")
    }

    @Test("explicitly disabling remote access suppresses the bundled relay")
    func configuredURLHonorsExplicitDisable() throws {
        let defaults = try makeDefaults()

        CompanionRelaySettings.setRemoteAccessEnabled(false, defaults: defaults)

        #expect(
            CompanionRelaySettings.configuredURL(
                defaults: defaults,
                bundledURLString: "wss://relay.codexcompanion.example/relay"
            ) == nil
        )
    }

    @Test("re-enabling remote access restores the bundled relay")
    func configuredURLRestoresBundledRelayAfterEnable() throws {
        let defaults = try makeDefaults()
        CompanionRelaySettings.setRemoteAccessEnabled(false, defaults: defaults)

        CompanionRelaySettings.setRemoteAccessEnabled(true, defaults: defaults)

        #expect(
            CompanionRelaySettings.configuredURL(
                defaults: defaults,
                bundledURLString: "wss://relay.codexcompanion.example/relay"
            )?.absoluteString == "wss://relay.codexcompanion.example/relay"
        )
    }

    @Test("restoring automatic relay removes a stale custom override")
    func useBundledRelayClearsOverrideAndEnablesRemoteAccess() throws {
        let defaults = try makeDefaults()
        defaults.set(
            "wss://stale-override.example/relay",
            forKey: CompanionRelaySettings.relayURLKey
        )
        CompanionRelaySettings.setRemoteAccessEnabled(false, defaults: defaults)

        CompanionRelaySettings.useBundledRelay(defaults: defaults)

        #expect(defaults.string(forKey: CompanionRelaySettings.relayURLKey) == nil)
        #expect(
            CompanionRelaySettings.configuredURL(
                defaults: defaults,
                bundledURLString: "wss://relay.codexcompanion.example/relay"
            )?.absoluteString == "wss://relay.codexcompanion.example/relay"
        )
    }

    @Test("clearing the relay field explicitly disables automatic remote access")
    func clearingRelayFieldDisablesBundledRelay() throws {
        let defaults = try makeDefaults()

        #expect(CompanionRelaySettings.setRelayURL("   ", defaults: defaults))
        #expect(
            CompanionRelaySettings.configuredURL(
                defaults: defaults,
                bundledURLString: "wss://relay.codexcompanion.example/relay"
            ) == nil
        )
    }

    @Test("an insecure public relay does not replace the current secure override")
    func insecurePublicRelayIsRejectedWithoutMutation() throws {
        let defaults = try makeDefaults()
        defaults.set(
            "wss://known-good.example/relay",
            forKey: CompanionRelaySettings.relayURLKey
        )

        #expect(
            !CompanionRelaySettings.setRelayURL(
                "ws://relay.example/relay",
                defaults: defaults
            )
        )
        #expect(
            CompanionRelaySettings.configuredURL(
                defaults: defaults,
                bundledURLString: "wss://bundled.example/relay"
            )?.absoluteString == "wss://known-good.example/relay"
        )
    }

    @Test("adds the opaque channel route without dropping relay query options")
    func routedURLPreservesExistingQuery() throws {
        let base = try #require(
            URL(string: "wss://relay.example/relay?region=iad")
        )

        let routed = CompanionRelayConnection.routedURL(
            base,
            channelID: "Y2hhbm5lbC1h"
        )
        let components = try #require(
            URLComponents(url: routed, resolvingAgainstBaseURL: false)
        )
        let values = Dictionary(
            uniqueKeysWithValues: (components.queryItems ?? []).map {
                ($0.name, $0.value)
            }
        )

        #expect(values["region"] == "iad")
        #expect(values["channel"] == "Y2hhbm5lbC1h")
    }

    @Test("replaces a stale channel route instead of duplicating it")
    func routedURLReplacesChannel() throws {
        let base = try #require(
            URL(string: "ws://127.0.0.1:8080/relay?channel=stale")
        )

        let routed = CompanionRelayConnection.routedURL(
            base,
            channelID: "ZnJlc2gtY2hhbm5lbA"
        )
        let components = try #require(
            URLComponents(url: routed, resolvingAgainstBaseURL: false)
        )
        let channelItems = (components.queryItems ?? []).filter {
            $0.name == "channel"
        }

        #expect(channelItems.count == 1)
        #expect(channelItems.first?.value == "ZnJlc2gtY2hhbm5lbA")
    }

    private func makeDefaults() throws -> UserDefaults {
        let suiteName = "CompanionRelayConnectionTests.\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suiteName))
        defaults.removePersistentDomain(forName: suiteName)
        return defaults
    }
}

private actor RelayKeepAliveCounter {
    private(set) var value = 0

    func increment() -> Int {
        value += 1
        return value
    }
}

private enum RelayKeepAliveTestError: Error {
    case expectedFailure
}
