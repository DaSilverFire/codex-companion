# Releasing Codex Companion

This repository can produce a ready-to-install universal DMG. Developer ID signing, notarization, and stapling are required before describing a release as notarized or Gatekeeper-trusted.

## Release contract

Each release uses:

- bundle identifier `com.silverfire.codexcompanion`;
- a semantic `CFBundleShortVersionString`;
- a positive, monotonically increasing integer `CFBundleVersion`;
- arm64 and x86_64 slices;
- `CodexCompanion.icns` in the app resources;
- the schema-aware `companion-pet` skill in both the app resources and DMG;
- a signed HTTPS update manifest when the update channel is enabled.

The release process must never change shared Codex app-server configuration or start, stop, bootstrap, or replace the shared server.

## Required release settings

For a public release, configure these outside the repository:

```bash
export CODEX_COMPANION_BUILD_NUMBER="1"
export CODEX_COMPANION_CODESIGN_IDENTITY="Developer ID Application: SilverFire (YOUR_TEAM_ID)"
export CODEX_COMPANION_NOTARY_PROFILE="codex-companion-notary"
export CODEX_COMPANION_RELAY_URL="wss://YOUR-STABLE-RELAY.example/relay"
```

Create the notary profile with `xcrun notarytool store-credentials`. Keep certificates, App Store Connect keys, profiles, and passwords out of the repository.

`VERSION` is the default app version for local builds, release artifacts, and the plugin source snapshot. A release version must be unused by both local tags and GitHub Releases. `CODEX_COMPANION_VERSION` is reserved for an intentional release-job override and must match the approved release plan.

`CODEX_COMPANION_RELAY_URL` must be the verified, durable `wss://` endpoint owned by the release operator. The app embeds only the relay origin and path; opaque per-device channel capabilities and encrypted task payloads are derived after nearby pairing and are never part of the release artifact. Do not use a temporary preview URL for a release.

If signing credentials are absent, `create_release.sh` creates an ad-hoc signed DMG and prints a Gatekeeper warning. Its installer clearly identifies the build as pre-notarized, requires explicit confirmation, and then enables the transactional installer's ad-hoc verification path. A GitHub release using this path must be labeled pre-notarized and must not be described as notarized or Gatekeeper-trusted.

The underlying installer still rejects an ad-hoc app by default. `CODEX_COMPANION_ALLOW_ADHOC=1` is enabled only by the generated confirmation wrapper, by the authenticated in-app updater, or by a maintainer exercising a local artifact directly.

## Signed update channel

The Settings updater consumes a small JSON manifest hosted with GitHub Releases. Official SilverFire builds default to the repository's `latest/download/update.json` feed and embedded public key. Override those values only when testing a separate release channel:

```bash
export CODEX_COMPANION_UPDATE_MANIFEST_URL="https://github.com/OWNER/REPOSITORY/releases/latest/download/update.json"
export CODEX_COMPANION_UPDATE_PUBLIC_KEY="BASE64_RAW_32_BYTE_PUBLIC_KEY"
export CODEX_COMPANION_UPDATE_DOWNLOAD_URL="https://github.com/OWNER/REPOSITORY/releases/download/vVERSION/Codex-Companion-VERSION-BUILD-universal.dmg"
export CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64="BASE64_RAW_32_BYTE_PRIVATE_KEY"
```

For local SilverFire releases, `create_release.sh` also looks for the private key in the login Keychain item `com.silverfire.codexcompanion.update-signing-key` with account `release`. The private key is never embedded in the app or source tree.

The build maps the first two values into the `CodexCompanionUpdateManifestURL` and `CodexCompanionUpdatePublicKey` Info.plist keys.

The key pair is a CryptoKit `Curve25519.Signing.PrivateKey`. Store the raw 32-byte private key only in the release secret store. Embed only the raw public key. `sign_update_manifest.swift` derives the public key from the private key and rejects a configured public-key mismatch.

The manifest schema is version 1 and signs this exact UTF-8 payload, without a trailing newline:

```text
schemaVersion
version
build
minimumSystemVersion
publishedAt
downloadURL
sha256
size
```

The `signature` field is the base64 Ed25519 signature over that payload. The app rejects non-HTTPS downloads, malformed digests, invalid sizes, unknown schemas, and invalid signatures.

The updater checks and authenticates the release channel, downloads the DMG, and verifies its signed size and SHA-256 digest. The user must then choose **Install and Relaunch** in Settings. That explicit action launches a detached helper which rechecks the artifact, mounts it read-only, and runs the release's transactional installer so the old app can stop without terminating the update. The installer preserves the app-and-skill rollback boundary and reopens Companion after a successful replacement. Builds pointed at a custom channel without a manifest URL and public key display an explicit unavailable state.

For an older authenticated release that wraps an ad-hoc local payload in an interactive first-install warning, the updater invokes the bundled transactional installer directly and permits the ad-hoc signature only after the signed manifest, size, and SHA-256 checks pass. Manual installer launches retain their normal warning and trust policy.

An authorized mobile-beta build seeds a local access grant on first launch. Signed updates preserve that grant, so an authorized installation does not lose Mobile Companion when it later installs a normal public update. Fresh public installs do not receive the grant, and disabling Mobile Companion still tears down discovery, bridge, relay, task, and power-availability services.

## Build the artifact

```bash
./script/create_release.sh
```

The script:

1. builds separate arm64 and x86_64 release binaries;
2. combines them with `lipo`;
3. packages the app icon and Companion pet skill;
4. signs and verifies the app;
5. creates the installer DMG;
6. signs, notarizes, and staples it when credentials are configured;
7. emits `SHA256SUMS` and a signed `update.json` when the update private key is configured.

Never commit `dist/`, certificates, private keys, notarization credentials, or generated manifests containing a private value. The manifest itself contains no secret and can be attached to a GitHub Release.

## Installer and rollback behavior

`Install Codex Companion.command` verifies the SilverFire bundle identifier, signature, universal slices, icon, and skill payload before changing the machine. It stops only the running Companion app, backs up the previous app and skill, installs and re-verifies the replacements, restores both on failure, and then launches Companion.

For a normal `/Applications` install, the installer also replaces `~/Library/LaunchAgents/com.silverfire.codexcompanion.relauncher.plist` transactionally. The job runs only the Companion executable, uses `KeepAlive.SuccessfulExit = false`, and therefore restarts an abnormal exit while leaving a normal Quit closed. A failed install restores the previous plist and previously loaded relaunch job where possible. Test installs outside `/Applications` skip launch-agent management unless explicitly enabled.

The installer never deletes defaults or Keychain items. Existing preferences, SwiftUI window restoration, and API credentials stored under the current SilverFire identity remain in place during upgrades.

The pet window uses the stable autosave name `CodexCompanionPetWindow`. Settings persists the autonomous-roaming policy separately; disabling roaming does not disable dragging or task-reaction animations.

## Clean public source export

Create a source-visible tree from an explicit allowlist:

```bash
./script/export_public_source.sh /absolute/output/CodexCompanion
```

The export excludes build products, distribution output, logs, QA captures, generated sprites, work directories, local configuration, task data, device identifiers, signing files, Keychain material, API keys, and temporary test artifacts. The exporter fails if it detects a local user path, host name, API-key shape, private-key block, or uppercase device UUID.

Review the exported directory before creating a repository. This script does not create a remote, commit, push, tag, publish, or upload a release.
