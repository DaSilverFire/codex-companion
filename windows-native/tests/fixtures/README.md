# Codex protocol fixtures

`codex-v034` fixtures are source-authoritative v0.3.4 Companion bridge
payloads. `bridge-request-full.json` and `bridge-response-full.json` exercise
every v0.3.4 additive field. The `v033-compatible` fixtures intentionally
omit those additive keys.

`bridge-relay-canonical.json` is the compact, recursively sorted relay-profile
golden. Its field values and canonical profile follow the extracted macOS
v0.3.4 model source at commit `35b9ded3a96e9b4fd2787cbd7ef1e8859264ff1b`,
tree `ab9a0f3aabfbbe02f28625cfa826d6896552a90b`, using Swift
`JSONEncoder.outputFormatting = [.sortedKeys]`.

`mobile-v034` mirrors the complete request and response through the nearby
millisecond profile so Windows nearby and relay transports share one typed
bridge contract.

`companion-mobile-probe --scenario loopback` consumes `mobile-v034` before
starting the assembled Windows mobile host. The probe writes only a redacted
`summary.json`; pairing codes, secrets, private addresses, and transfer data
remain in an auto-removed temporary directory.
