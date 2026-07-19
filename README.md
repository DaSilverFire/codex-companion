# Codex Companion

Codex Companion expands the Codex pet experience into a native macOS desktop companion. Your animated pet stays close by while you follow tasks, reply or steer, handle approvals, and check usage and goals.

## Features

- Keep an animated Codex pet on the desktop and let it react to task activity.
- Follow active and recent Codex tasks from the pet's compact menu.
- Reply, steer, and respond to approval requests through native Codex transport.
- Check Codex usage, banked resets, and goal progress.
- Download authenticated updates from inside the app.

## Install on macOS

Normal users do not need Xcode or Swift.

1. Download the universal macOS DMG from GitHub Releases.
2. Open the DMG.
3. Double-click `Install Codex Companion.command`.

The installer validates the app, replaces `/Applications/Codex Companion.app` with rollback protection, installs the bundled `companion-pet` skill, preserves existing settings, and opens Companion. It also installs a Companion-only relaunch agent for abnormal exits; a normal Quit remains closed.

Pre-notarized releases ask for confirmation during installation. macOS may also require approval in **System Settings > Privacy & Security**.

## Build from source

Requirements:

- macOS 14 or newer
- Xcode with the macOS SDK
- Swift 5.10 or newer

Run and verify a local development build:

```bash
./script/build_and_run.sh --verify
```

Create a universal release artifact using the version in `VERSION`:

```bash
CODEX_COMPANION_BUILD_NUMBER=1 ./script/create_release.sh
```

See [RELEASING.md](RELEASING.md) for signing, notarization, update-feed, and public-export requirements.

## Companion Pet Skill

The repository includes `Skills/companion-pet`, a Codex skill for creating, validating, and updating animated pets for Codex Companion.
