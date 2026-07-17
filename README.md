# Codex Companion

Codex Companion expands the Codex pet experience into a native macOS desktop companion. Your animated pet stays nearby while you follow tasks, reply or steer, handle approvals, and check usage and goals.

## Features

- Keep an animated Codex pet on the desktop and let it react to task activity.
- Follow active and recent Codex tasks from the pet's compact menu.
- Reply, steer, and respond to approval requests through native Codex transport.
- Check Codex usage, banked resets, and goal progress.
- Pair approved Companion clients without exposing the Mac bridge to unknown devices.

## Install on macOS

Download the universal macOS DMG from [GitHub Releases](https://github.com/DaSilverFire/codex-companion/releases). The prebuilt app supports Apple silicon and Intel Macs, so Xcode and Swift are not required.

1. Open the downloaded DMG.
2. Double-click `Install Codex Companion.command`.
3. Launch Codex Companion from Applications or Spotlight.

The installer verifies the app, keeps a rollback copy while replacing an existing installation, preserves Companion settings, and installs the bundled pet skill.

Until a Developer ID notarized release is available, macOS may require confirming the download in **System Settings > Privacy & Security**. The installer also asks for confirmation before installing a pre-notarized build.

## Companion Pet Skill

The repository includes [`companion-pet`](Skills/companion-pet/SKILL.md), a Codex skill for creating, validating, and updating animated pets for Codex Companion.

## Development

Source is available for contributors, but most users should install the prebuilt macOS release.

```bash
./script/build_and_run.sh --verify
```

Release maintainers should follow [RELEASING.md](RELEASING.md).
