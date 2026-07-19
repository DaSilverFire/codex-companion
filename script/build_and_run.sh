#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${DEVELOPER_DIR:-}" ]]; then
  if [[ -d /Applications/Xcode-beta.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
  elif [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  fi
fi

MODE="${1:-run}"
APP_NAME="CodexCompanion"
BUNDLE_NAME="Codex Companion"
BUNDLE_ID="com.silverfire.codexcompanion"
MIN_SYSTEM_VERSION="14.0"
DEFAULT_SIGN_IDENTITY="Codex Companion Trusted Local Code Signing"
DEFAULT_UPDATE_MANIFEST_URL="https://github.com/DaSilverFire/codex-companion/releases/latest/download/update.json"
DEFAULT_UPDATE_PUBLIC_KEY="/b26MOV9HlKeifsp8TCIb3tPDJW5SGBf7o/CE+RooVg="
UPDATE_MANIFEST_URL="${CODEX_COMPANION_UPDATE_MANIFEST_URL:-$DEFAULT_UPDATE_MANIFEST_URL}"
UPDATE_PUBLIC_KEY="${CODEX_COMPANION_UPDATE_PUBLIC_KEY:-$DEFAULT_UPDATE_PUBLIC_KEY}"
RELAY_URL="${CODEX_COMPANION_RELAY_URL:-}"
MOBILE_BETA_AUTHORIZED="${CODEX_COMPANION_MOBILE_BETA_AUTHORIZED:-0}"
MOBILE_BETA_PLIST_BOOL=false
RELAUNCH_LABEL="com.silverfire.codexcompanion.relauncher"
USER_DOMAIN="gui/$(id -u)"
RELAUNCH_PLIST="$HOME/Library/LaunchAgents/$RELAUNCH_LABEL.plist"
RELAUNCH_WAS_LOADED=0
RELAUNCH_RESTORED=0

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$ROOT_DIR/VERSION"
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
DIST_DIR="$ROOT_DIR/dist"
STAGING_DIR="${TMPDIR:-/tmp}/codex-companion-build"
SWIFT_BUILD_SCRATCH="${CODEX_COMPANION_BUILD_SCRATCH_PATH:-${TMPDIR:-/tmp}/codex-companion-swift-build}"
APP_BUNDLE="$STAGING_DIR/$BUNDLE_NAME.app"
DIST_APP_BUNDLE="$DIST_DIR/$BUNDLE_NAME.app"
INSTALLED_APP_BUNDLE="/Applications/$BUNDLE_NAME.app"
APP_CONTENTS="$APP_BUNDLE/Contents"
APP_MACOS="$APP_CONTENTS/MacOS"
APP_BINARY="$APP_MACOS/$APP_NAME"
INFO_PLIST="$APP_CONTENTS/Info.plist"
APP_RESOURCES="$APP_CONTENTS/Resources"
APP_ICON_SOURCE="$ROOT_DIR/Assets/AppIcon/CodexCompanion.icns"
APP_ICON_NAME="CodexCompanion"

export XDG_CACHE_HOME="$SWIFT_BUILD_SCRATCH/cache"
export CLANG_MODULE_CACHE_PATH="$SWIFT_BUILD_SCRATCH/clang-module-cache"
export GIT_OPTIONAL_LOCKS="${GIT_OPTIONAL_LOCKS:-0}"
mkdir -p "$SWIFT_BUILD_SCRATCH" "$XDG_CACHE_HOME" "$CLANG_MODULE_CACHE_PATH"

running_app_pids() {
  pgrep -f "$INSTALLED_APP_BUNDLE/Contents/MacOS/$APP_NAME" || true
}

kill_existing_app() {
  local deadline=$((SECONDS + 5))
  local pids

  pkill -x "$APP_NAME" >/dev/null 2>&1 || true
  while true; do
    pids="$(running_app_pids)"
    [[ -z "$pids" ]] && break
    [[ $SECONDS -ge $deadline ]] && break
    kill $pids >/dev/null 2>&1 || true
    sleep 0.2
  done

  pids="$(running_app_pids)"
  if [[ -n "$pids" ]]; then
    kill -9 $pids >/dev/null 2>&1 || true
    sleep 0.2
  fi
}

wait_for_app() {
  local attempt
  local pids
  for attempt in {1..30}; do
    pids="$(running_app_pids)"
    if [[ -n "$pids" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

suspend_relaunch_agent() {
  if /bin/launchctl print "$USER_DOMAIN/$RELAUNCH_LABEL" >/dev/null 2>&1; then
    RELAUNCH_WAS_LOADED=1
    /bin/launchctl bootout "$USER_DOMAIN/$RELAUNCH_LABEL" >/dev/null 2>&1 || true
  fi
}

restore_relaunch_agent() {
  [[ $RELAUNCH_WAS_LOADED -eq 1 ]] || return 1
  [[ $RELAUNCH_RESTORED -eq 0 ]] || return 0
  [[ -f "$RELAUNCH_PLIST" ]] || return 1
  /bin/launchctl bootstrap "$USER_DOMAIN" "$RELAUNCH_PLIST"
  RELAUNCH_RESTORED=1
}

cleanup() {
  local exit_code=$?
  trap - EXIT INT TERM
  if [[ $RELAUNCH_WAS_LOADED -eq 1 && $RELAUNCH_RESTORED -eq 0 && -f "$RELAUNCH_PLIST" ]]; then
    /bin/launchctl bootstrap "$USER_DOMAIN" "$RELAUNCH_PLIST" >/dev/null 2>&1 || true
  fi
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

if [[ "$MOBILE_BETA_AUTHORIZED" != "0" && "$MOBILE_BETA_AUTHORIZED" != "1" ]]; then
  echo "CODEX_COMPANION_MOBILE_BETA_AUTHORIZED must be 0 or 1" >&2
  exit 2
fi
if [[ -n "$RELAY_URL" && "$RELAY_URL" != wss://* ]]; then
  echo "Codex Companion relay URL must use WSS" >&2
  exit 2
fi
if [[ "$MOBILE_BETA_AUTHORIZED" == "1" ]]; then
  MOBILE_BETA_PLIST_BOOL=true
fi

suspend_relaunch_agent
kill_existing_app

swift build --package-path "$ROOT_DIR" --scratch-path "$SWIFT_BUILD_SCRATCH"
BUILD_BINARY="$(swift build --package-path "$ROOT_DIR" --scratch-path "$SWIFT_BUILD_SCRATCH" --show-bin-path)/$APP_NAME"

rm -rf "$APP_BUNDLE"
mkdir -p "$STAGING_DIR" "$DIST_DIR"
mkdir -p "$APP_MACOS"
mkdir -p "$APP_RESOURCES"
cp "$BUILD_BINARY" "$APP_BINARY"
chmod +x "$APP_BINARY"
if [[ -f "$APP_ICON_SOURCE" ]]; then
  cp "$APP_ICON_SOURCE" "$APP_RESOURCES/$APP_ICON_NAME.icns"
fi

cat >"$INFO_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDisplayName</key>
  <string>$BUNDLE_NAME</string>
  <key>CFBundleExecutable</key>
  <string>$APP_NAME</string>
  <key>CFBundleIdentifier</key>
  <string>$BUNDLE_ID</string>
  <key>CFBundleIconFile</key>
  <string>$APP_ICON_NAME</string>
  <key>CFBundleName</key>
  <string>$BUNDLE_NAME</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>$VERSION</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSMinimumSystemVersion</key>
  <string>$MIN_SYSTEM_VERSION</string>
  <key>LSUIElement</key>
  <true/>
  <key>NSLocationUsageDescription</key>
  <string>Codex Companion uses your location only when you ask the on-device assistant for a location-aware answer.</string>
  <key>NSLocationWhenInUseUsageDescription</key>
  <string>Codex Companion uses your location only when you ask the on-device assistant for a location-aware answer.</string>
  <key>NSCalendarsFullAccessUsageDescription</key>
  <string>Codex Companion reads upcoming calendar events only when you ask the on-device assistant about your schedule.</string>
  <key>NSCalendarsUsageDescription</key>
  <string>Codex Companion reads upcoming calendar events only when you ask the on-device assistant about your schedule.</string>
  <key>NSRemindersFullAccessUsageDescription</key>
  <string>Codex Companion reads incomplete reminders only when you ask the on-device assistant about them.</string>
  <key>NSRemindersUsageDescription</key>
  <string>Codex Companion reads incomplete reminders only when you ask the on-device assistant about them.</string>
  <key>NSPrincipalClass</key>
  <string>NSApplication</string>
</dict>
</plist>
PLIST

/usr/bin/plutil -insert CodexCompanionMobileBetaAuthorized -bool "$MOBILE_BETA_PLIST_BOOL" "$INFO_PLIST"
if [[ "$MOBILE_BETA_AUTHORIZED" == "1" ]]; then
  /usr/bin/plutil -insert NSBonjourServices -json '["_codex-companion._tcp."]' "$INFO_PLIST"
  /usr/bin/plutil -insert NSLocalNetworkUsageDescription -string \
    "Codex Companion connects to authorized Companion mobile clients on your local network." "$INFO_PLIST"
fi

if [[ -n "$UPDATE_MANIFEST_URL" ]]; then
  /usr/bin/plutil -insert CodexCompanionUpdateManifestURL -string "$UPDATE_MANIFEST_URL" "$INFO_PLIST"
fi
if [[ -n "$UPDATE_PUBLIC_KEY" ]]; then
  /usr/bin/plutil -insert CodexCompanionUpdatePublicKey -string "$UPDATE_PUBLIC_KEY" "$INFO_PLIST"
fi
if [[ -n "$RELAY_URL" ]]; then
  /usr/bin/plutil -insert CodexCompanionRelayURL -string "$RELAY_URL" "$INFO_PLIST"
fi

sign_bundle() {
  local bundle="$1"
  local identity="${CODEX_COMPANION_CODESIGN_IDENTITY:-}"
  if [[ -z "$identity" ]]; then
    if [[ -n "$DEFAULT_SIGN_IDENTITY" ]] && /usr/bin/security find-identity -v -p codesigning 2>/dev/null | /usr/bin/grep -Fq "$DEFAULT_SIGN_IDENTITY"; then
      identity="$DEFAULT_SIGN_IDENTITY"
    else
      identity="-"
    fi
  fi

  /usr/bin/codesign --force --deep --sign "$identity" --identifier "$BUNDLE_ID" "$bundle" >/dev/null
}

xattr -cr "$APP_BUNDLE"
sign_bundle "$APP_BUNDLE"

rm -rf "$DIST_APP_BUNDLE"
/usr/bin/ditto "$APP_BUNDLE" "$DIST_APP_BUNDLE"

rm -rf "$INSTALLED_APP_BUNDLE"
/usr/bin/ditto "$APP_BUNDLE" "$INSTALLED_APP_BUNDLE"
/usr/bin/xattr -cr "$INSTALLED_APP_BUNDLE"
sign_bundle "$INSTALLED_APP_BUNDLE"

open_app() {
  /usr/bin/open "$INSTALLED_APP_BUNDLE"
}

launch_app_once() {
  if ! restore_relaunch_agent; then
    open_app
  fi
}

case "$MODE" in
  run)
    launch_app_once
    ;;
  --debug|debug)
    lldb -- "$APP_BINARY"
    ;;
  --logs|logs)
    launch_app_once
    /usr/bin/log stream --info --style compact --predicate "process == \"$APP_NAME\""
    ;;
  --telemetry|telemetry)
    launch_app_once
    /usr/bin/log stream --info --style compact --predicate "subsystem == \"$BUNDLE_ID\""
    ;;
  --verify|verify)
    launch_app_once
    wait_for_app
    ;;
  *)
    echo "usage: $0 [run|--debug|--logs|--telemetry|--verify]" >&2
    exit 2
    ;;
esac
