# Codex Companion

Codex Companion is a separate macOS companion for Codex tasks and animated pets. It does not patch or re-sign the ChatGPT app.

## Install on macOS

Download `CodexCompanion-<version>-macOS-universal.dmg` from GitHub Releases. It contains a prebuilt app for both Apple silicon and Intel Macs, so Xcode, Swift, Homebrew, and Terminal are not required.

1. Open the downloaded `.dmg`.
2. Drag Codex Companion to Applications.
3. Launch Codex Companion from Applications or Spotlight.

Replacing the app preserves existing Companion settings, pet packages, task history, and Keychain credentials because those live outside the app bundle. After the first install, **Codex Companion > Settings > Updates** can check GitHub Releases, download the matching DMG and checksum, verify SHA-256 integrity, and open the installer.

Until a Developer ID notarized release is available, macOS may require confirming the first launch in **System Settings > Privacy & Security**. Release notes identify the exact signing and notarization status of each artifact.

## Companion Pet Skill

The repository includes [`companion-pet`](skills/companion-pet/SKILL.md), a Codex skill for creating and migrating animated pet packages against the currently installed pet renderer schema.

It does not assume a fixed 16-frame layout. It discovers atlas geometry and per-state frame counts, preserves the source pet during migration, and keeps unsupported states such as thinking and talking staged until the Companion runtime explicitly supports them.

## Privacy

The repository and release packages do not contain local task history, device identifiers, logs, generated QA evidence, signing credentials, ChatGPT cookies, API keys, or Keychain data. User secrets stay in the macOS Keychain.

## Development

Source builds remain available for contributors, but end users should use the prebuilt installer. Release artifacts are produced from a clean checkout and validated for both architectures, bundle identity, signature integrity, checksum integrity, and machine-specific path leakage before publication.

Maintainers can build and publish a verified release with `scripts/publish-macos-release.sh VERSION`. The command refuses a dirty source tree, runs the release audit, rebuilds the universal DMG, verifies the mounted artifact, and uploads only the DMG and its checksum.

The macOS bundle identifier is `com.silverfire.codexcompanion`.
