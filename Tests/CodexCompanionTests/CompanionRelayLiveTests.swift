import Foundation
import Testing
@testable import CodexCompanion

@Suite("Companion relay live probe")
struct CompanionRelayLiveTests {
    @Test("paired endpoint completes an encrypted handshake through the public relay")
    func encryptedHandshakeRoundTrip() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard environment["COMPANION_RELAY_LIVE"] == "1" else { return }

        let deviceID = try #require(environment["COMPANION_RELAY_LIVE_DEVICE_ID"])
        let relayURL = try #require(
            environment["COMPANION_RELAY_LIVE_URL"].flatMap(URL.init(string:))
        )
        let record = try #require(
            CompanionPairingRecordStore().record(for: deviceID)
        )
        let probe = CompanionRelayLiveProbe()
        let connection = CompanionRelayConnection(
            url: relayURL,
            channelID: CompanionBridgeSecurity.channelID(secret: record.secret),
            endpointID: record.deviceID,
            stateHandler: { state in
                Task { await probe.record(state: state) }
            },
            envelopeHandler: { envelope in
                Task { await probe.record(envelope: envelope) }
            },
            failureHandler: { failure in
                Task { await probe.record(failure: failure) }
            }
        )

        await connection.start()
        do {
            try await probe.waitForPeer()
            let request = CompanionBridgeRequest(operation: .handshake)
            let sequence = UInt64(
                max(1, CompanionBridgeSecurity.milliseconds(since1970: Date()))
            )
            let envelope = try CompanionBridgeSecurity.seal(
                request,
                secret: record.secret,
                senderID: record.deviceID,
                sequence: sequence
            )

            try await connection.send(envelope)
            let responseEnvelope = try await probe.waitForEnvelope()
            let response = try CompanionBridgeSecurity.open(
                responseEnvelope,
                secret: record.secret,
                as: CompanionBridgeResponse.self
            )

            #expect(response.id == request.id)
            #expect(response.operation == .handshake)
            #expect(response.succeeded)
            #expect(response.macDeviceID != nil)

            if let threadID = environment["COMPANION_RELAY_LIVE_THREAD_ID"] {
                let requests = [
                    CompanionBridgeRequest(
                        operation: .loadMessages,
                        limit: 30,
                        threadID: threadID
                    ),
                    CompanionBridgeRequest(
                        operation: .loadMessages,
                        limit: 30,
                        threadID: threadID
                    ),
                ]
                for (offset, request) in requests.enumerated() {
                    let envelope = try CompanionBridgeSecurity.seal(
                        request,
                        secret: record.secret,
                        senderID: record.deviceID,
                        sequence: sequence + UInt64(offset + 1)
                    )
                    try await connection.send(envelope)
                }

                let responses = try await [
                    probe.waitForEnvelope(),
                    probe.waitForEnvelope(),
                ].map { envelope in
                    try CompanionBridgeSecurity.open(
                        envelope,
                        secret: record.secret,
                        as: CompanionBridgeResponse.self
                    )
                }
                #expect(Set(responses.map(\.id)) == Set(requests.map(\.id)))
                #expect(responses.allSatisfy { $0.operation == .loadMessages })
                #expect(responses.allSatisfy { $0.succeeded })
                #expect(responses.allSatisfy { $0.threadID == threadID })
                #expect(responses.allSatisfy { $0.messages?.isEmpty == false })
            }
        } catch {
            await connection.stop()
            throw error
        }
        await connection.stop()
    }
}

private actor CompanionRelayLiveProbe {
    private var peerAvailable = false
    private var envelopes: [CompanionBridgeEncryptedEnvelope] = []
    private var failure: String?

    func record(state: CompanionRelayConnection.State) {
        if state == .registered {
            peerAvailable = true
        }
    }

    func record(envelope: CompanionBridgeEncryptedEnvelope) {
        envelopes.append(envelope)
    }

    func record(failure: String) {
        self.failure = failure
    }

    func waitForPeer() async throws {
        try await waitUntil {
            self.peerAvailable
        }
    }

    func waitForEnvelope() async throws -> CompanionBridgeEncryptedEnvelope {
        try await waitUntil {
            !self.envelopes.isEmpty
        }
        return envelopes.removeFirst()
    }

    private func waitUntil(_ condition: () -> Bool) async throws {
        for _ in 0..<150 {
            if let failure {
                throw CompanionRelayLiveProbeError.transportFailed(failure)
            }
            if condition() { return }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw CompanionRelayLiveProbeError.timedOut
    }
}

private enum CompanionRelayLiveProbeError: LocalizedError {
    case timedOut
    case transportFailed(String)

    var errorDescription: String? {
        switch self {
        case .timedOut:
            "The live encrypted relay probe timed out."
        case .transportFailed(let message):
            "The live encrypted relay probe failed: \(message)"
        }
    }
}
