#!/usr/bin/env bash
set -euo pipefail

DMG_PATH="${1:?usage: verify-macos-release.sh /path/to/CodexCompanion-VERSION-macOS-universal.dmg}"
EXPECTED_BUNDLE_ID="com.silverfire.codexcompanion"
MOUNT_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/codex-companion-mount.XXXXXX")"
APP_PATH="$MOUNT_ROOT/Codex Companion.app"

cleanup() {
  /usr/bin/hdiutil detach "$MOUNT_ROOT" -quiet >/dev/null 2>&1 || true
  rmdir "$MOUNT_ROOT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [[ ! -f "$DMG_PATH" || ! -f "$DMG_PATH.sha256" ]]; then
  echo "Installer or checksum is missing" >&2
  exit 1
fi

(
  cd "$(dirname "$DMG_PATH")"
  /usr/bin/shasum -a 256 -c "$(basename "$DMG_PATH").sha256"
)

/usr/bin/hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_ROOT" "$DMG_PATH" >/dev/null

if [[ ! -d "$APP_PATH" || ! -L "$MOUNT_ROOT/Applications" ]]; then
  echo "DMG is missing the app or Applications link" >&2
  exit 1
fi

INFO_PLIST="$APP_PATH/Contents/Info.plist"
BINARY="$APP_PATH/Contents/MacOS/CodexCompanion"
bundle_id="$(/usr/bin/plutil -extract CFBundleIdentifier raw "$INFO_PLIST")"
architectures="$(/usr/bin/lipo -archs "$BINARY")"

[[ "$bundle_id" == "$EXPECTED_BUNDLE_ID" ]]
[[ "$architectures" == *"arm64"* ]]
[[ "$architectures" == *"x86_64"* ]]
/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP_PATH"

user_home_root='/'"Users/"
temporary_root='/var/'"folders/"
if /usr/bin/strings "$BINARY" | /usr/bin/grep -Fq -e "$user_home_root" -e "$temporary_root"; then
  echo "Release binary contains a local absolute path" >&2
  exit 1
fi

echo "Verified $(basename "$DMG_PATH")"
echo "Architectures: $architectures"
echo "Bundle identifier: $bundle_id"
