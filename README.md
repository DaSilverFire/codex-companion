# Codex Companion

Codex Companion is a native macOS app for keeping Codex tasks, approvals, usage, goals, and animated pets close at hand.

## Features

- Follow active and recent Codex tasks from a compact desktop companion.
- Reply, steer, and respond to approval requests.
- Check Codex usage and goal progress.
- Use animated pets that react to task activity.
- Download updates from inside the app.

## Install on macOS

Download `CodexCompanion-<version>-macOS-universal.dmg` from GitHub Releases. It contains a prebuilt app for both Apple silicon and Intel Macs, so Xcode, Swift, Homebrew, and Terminal are not required.

1. Open the downloaded `.dmg`.
2. Drag Codex Companion to Applications.
3. Launch Codex Companion from Applications or Spotlight.

Replacing the app preserves existing Companion settings, pets, task history, and saved credentials. After the first install, use **Codex Companion > Settings > Updates** to check for future releases.

Until a Developer ID notarized release is available, macOS may require confirming the first launch in **System Settings > Privacy & Security**.

## Mobile Support Coming Soon

An iPhone companion is in active development. A public mobile release is coming soon.

## Companion Pet Skill

The repository includes [`companion-pet`](skills/companion-pet/SKILL.md), a Codex skill for creating, validating, and updating animated pets for Codex Companion.

## Privacy

Release packages do not include local task history, device information, logs, API keys, or other personal data.

## Development

Source is available for contributors, but most users should install the prebuilt macOS release.

Maintainers can build and publish a verified release with `scripts/publish-macos-release.sh VERSION`.
