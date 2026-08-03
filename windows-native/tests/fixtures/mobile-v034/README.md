# Mobile v0.3.4 fixtures

These payloads lock the two bridge wire profiles used by the published
Codex Companion v0.3.4 source at commit
`35b9ded3a96e9b4fd2787cbd7ef1e8859264ff1b`.

- `bridge-request-nearby.json` uses the nearby millisecond date profile.
- `bridge-response-nearby.json` uses the nearby millisecond date profile.

SHA-256:

```text
a61a06b98c84a1c5348d65e237865e5684f5b3b8249dcc7923013dce41326d83  bridge-request-nearby.json
0a278a56e5d214a3084b861c0db1fa674bf829f05580a1bf80e0cb25096e1b6b  bridge-response-nearby.json
693aaa876c5801dc24c9979be81cdfae1bc92c13b5e642c72a11709511cb2df8  swift-relay-vector.json
```

The payloads intentionally include every additive v0.3.4 bridge field.
Relay payloads use the canonical sorted profile covered by
`codex-v034/bridge-relay-canonical.json`.

`swift-relay-vector.json` uses the fixed inputs documented in
`tools/reference/emit-v034-mobile-fixtures.swift`. The checked-in bytes were
independently derived with Node.js/OpenSSL on Windows because CryptoKit is not
available in the Windows Swift toolchain. Regenerate the file with the Swift
emitter on macOS before release and require byte-for-byte equality.
The emitter encodes `instant` as a real
`Date(timeIntervalSinceReferenceDate: 0)`, whose canonical relay wire value is
the JSON number `0`.
