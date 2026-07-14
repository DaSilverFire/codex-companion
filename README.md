# Codex Companion

Codex Companion expands the Codex pet experience into a native macOS desktop companion. Your animated pet stays close by while you follow tasks, reply or steer, handle approvals, and check usage and goals.

## Features

- Keep an animated Codex pet on the desktop and let it react to task activity.
- Follow active and recent Codex tasks from the pet's compact menu.
- Reply, steer, and respond to approval requests.
- Check Codex usage and goal progress.
- Download updates from inside the app.

## Install on macOS

Download `CodexCompanion-<version>-macOS-universal.dmg` from GitHub Releases. It contains a prebuilt app for both Apple silicon and Intel Macs, so Xcode, Swift, Homebrew, and Terminal are not required.

1. Open the downloaded `.dmg`.
2. Drag Codex Companion to Applications.
3. Launch Codex Companion from Applications or Spotlight.

Replacing the app preserves existing Companion settings, pets, task history, and saved credentials. After the first install, use **Codex Companion > Settings > Updates** to check for future releases.

Until a Developer ID notarized release is available, macOS may require confirming the first launch in **System Settings > Privacy & Security**.

## Companion Pet Skill

The repository includes [`companion-pet`](skills/companion-pet/SKILL.md), a Codex skill for creating, validating, and updating animated pets for Codex Companion.

## Development

Source is available for contributors, but most users should install the prebuilt macOS release.

Maintainers can build and publish a verified release with `scripts/publish-macos-release.sh VERSION`.
