#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${DEVELOPER_DIR:-}" ]]; then
  if [[ -d /Applications/Xcode-beta.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
  elif [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  fi
fi

export GIT_OPTIONAL_LOCKS="${GIT_OPTIONAL_LOCKS:-0}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$ROOT_DIR/VERSION"
APP_NAME="CodexCompanion"
BUNDLE_NAME="Codex Companion"
BUNDLE_ID="com.silverfire.codexcompanion"
MIN_SYSTEM_VERSION="${CODEX_COMPANION_MIN_SYSTEM_VERSION:-14.0}"
ARM64_TRIPLE="arm64-apple-macosx${MIN_SYSTEM_VERSION}"
X86_64_TRIPLE="x86_64-apple-macosx${MIN_SYSTEM_VERSION}"
VERSION="${CODEX_COMPANION_VERSION:-$(tr -d '[:space:]' < "$VERSION_FILE")}"
BUILD_NUMBER="${CODEX_COMPANION_BUILD_NUMBER:-1}"
OUTPUT_DIR="${CODEX_COMPANION_RELEASE_OUTPUT_DIR:-$ROOT_DIR/dist/release}"
DEFAULT_UPDATE_MANIFEST_URL="https://github.com/DaSilverFire/codex-companion/releases/latest/download/update.json"
DEFAULT_UPDATE_PUBLIC_KEY="/b26MOV9HlKeifsp8TCIb3tPDJW5SGBf7o/CE+RooVg="
UPDATE_MANIFEST_URL="${CODEX_COMPANION_UPDATE_MANIFEST_URL:-$DEFAULT_UPDATE_MANIFEST_URL}"
UPDATE_PUBLIC_KEY="${CODEX_COMPANION_UPDATE_PUBLIC_KEY:-$DEFAULT_UPDATE_PUBLIC_KEY}"
RELAY_URL="${CODEX_COMPANION_RELAY_URL:-}"
UPDATE_DOWNLOAD_URL="${CODEX_COMPANION_UPDATE_DOWNLOAD_URL:-https://github.com/DaSilverFire/codex-companion/releases/download/v$VERSION/Codex-Companion-$VERSION-$BUILD_NUMBER-universal.dmg}"
UPDATE_PRIVATE_KEY="${CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64:-}"
UPDATE_KEYCHAIN_SERVICE="com.silverfire.codexcompanion.update-signing-key"
MOBILE_BETA_AUTHORIZED="${CODEX_COMPANION_MOBILE_BETA_AUTHORIZED:-0}"
MOBILE_BETA_PLIST_BOOL=false
SIGN_IDENTITY="${CODEX_COMPANION_CODESIGN_IDENTITY:-}"
NOTARY_PROFILE="${CODEX_COMPANION_NOTARY_PROFILE:-}"
WORK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/codex-companion-release.XXXXXX")"
APP_BUNDLE="$WORK_ROOT/$BUNDLE_NAME.app"
APP_CONTENTS="$APP_BUNDLE/Contents"
APP_BINARY="$APP_CONTENTS/MacOS/$APP_NAME"
APP_RESOURCES="$APP_CONTENTS/Resources"
INFO_PLIST="$APP_CONTENTS/Info.plist"
DMG_STAGE="$WORK_ROOT/dmg"
DMG_NAME="Codex-Companion-$VERSION-$BUILD_NUMBER-universal.dmg"
DMG_PATH="$OUTPUT_DIR/$DMG_NAME"
MANIFEST_PATH="$OUTPUT_DIR/update.json"
APP_ICON_SOURCE="$ROOT_DIR/Assets/AppIcon/CodexCompanion.icns"
COMPANION_SKILL_SOURCE="$ROOT_DIR/Skills/companion-pet"

if [[ -z "$UPDATE_PRIVATE_KEY" ]]; then
  UPDATE_PRIVATE_KEY="$(security find-generic-password -a release -s "$UPDATE_KEYCHAIN_SERVICE" -w 2>/dev/null || true)"
fi
if [[ -n "$UPDATE_PRIVATE_KEY" ]]; then
  export CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64="$UPDATE_PRIVATE_KEY"
fi

cleanup() {
  rm -rf "$WORK_ROOT"
}
trap cleanup EXIT INT TERM

fail() {
  echo "Codex Companion release failed: $*" >&2
  exit 1
}

validate_inputs() {
  [[ "$VERSION" =~ ^[0-9]+([.][0-9]+){1,3}([+-][0-9A-Za-z.-]+)?$ ]] || fail "invalid version: $VERSION"
  [[ "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ ]] || fail "build number must be a positive integer"
  [[ "$MOBILE_BETA_AUTHORIZED" == "0" || "$MOBILE_BETA_AUTHORIZED" == "1" ]] \
    || fail "CODEX_COMPANION_MOBILE_BETA_AUTHORIZED must be 0 or 1"
  [[ -f "$APP_ICON_SOURCE" ]] || fail "missing app icon"
  [[ -f "$COMPANION_SKILL_SOURCE/SKILL.md" ]] || fail "missing Companion pet skill"
  if [[ -n "$UPDATE_MANIFEST_URL" && "$UPDATE_MANIFEST_URL" != https://* ]]; then
    fail "update manifest URL must use HTTPS"
  fi
  if [[ -n "$UPDATE_DOWNLOAD_URL" && "$UPDATE_DOWNLOAD_URL" != https://* ]]; then
    fail "update download URL must use HTTPS"
  fi
  if [[ -n "$RELAY_URL" && "$RELAY_URL" != wss://* ]]; then
    fail "relay URL must use WSS"
  fi
  if [[ -n "$UPDATE_PRIVATE_KEY" && -z "$UPDATE_DOWNLOAD_URL" ]]; then
    fail "CODEX_COMPANION_UPDATE_DOWNLOAD_URL is required when signing a manifest"
  fi
  case "$OUTPUT_DIR" in
    ""|"/"|"$HOME"|"$ROOT_DIR")
      fail "unsafe release output directory: $OUTPUT_DIR"
      ;;
  esac
}

if [[ "$MOBILE_BETA_AUTHORIZED" == "1" ]]; then
  MOBILE_BETA_PLIST_BOOL=true
fi

build_architecture() {
  local architecture="$1"
  local triple="$2"
  local scratch="$WORK_ROOT/build-$architecture"
  local binary_path

  swift build \
    --package-path "$ROOT_DIR" \
    --configuration release \
    --scratch-path "$scratch" \
    --triple "$triple" >&2
  binary_path="$(swift build \
    --package-path "$ROOT_DIR" \
    --configuration release \
    --scratch-path "$scratch" \
    --triple "$triple" \
    --show-bin-path)/$APP_NAME"
  [[ -x "$binary_path" ]] || fail "missing $architecture release binary"
  printf '%s\n' "$binary_path"
}

write_info_plist() {
  /usr/bin/plutil -create xml1 "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleDisplayName -string "$BUNDLE_NAME" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleExecutable -string "$APP_NAME" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleIdentifier -string "$BUNDLE_ID" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleIconFile -string "CodexCompanion" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleName -string "$BUNDLE_NAME" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundlePackageType -string "APPL" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleShortVersionString -string "$VERSION" "$INFO_PLIST"
  /usr/bin/plutil -insert CFBundleVersion -string "$BUILD_NUMBER" "$INFO_PLIST"
  /usr/bin/plutil -insert LSMinimumSystemVersion -string "$MIN_SYSTEM_VERSION" "$INFO_PLIST"
  /usr/bin/plutil -insert LSUIElement -bool true "$INFO_PLIST"
  /usr/bin/plutil -insert CodexCompanionMobileBetaAuthorized -bool "$MOBILE_BETA_PLIST_BOOL" "$INFO_PLIST"
  if [[ "$MOBILE_BETA_AUTHORIZED" == "1" ]]; then
    /usr/bin/plutil -insert NSBonjourServices -json '["_codex-companion._tcp."]' "$INFO_PLIST"
    /usr/bin/plutil -insert NSLocalNetworkUsageDescription -string \
      "Codex Companion connects to authorized Companion mobile clients on your local network." "$INFO_PLIST"
  fi
  /usr/bin/plutil -insert NSLocationUsageDescription -string \
    "Codex Companion uses your location only when you ask the on-device assistant for a location-aware answer." "$INFO_PLIST"
  /usr/bin/plutil -insert NSLocationWhenInUseUsageDescription -string \
    "Codex Companion uses your location only when you ask the on-device assistant for a location-aware answer." "$INFO_PLIST"
  /usr/bin/plutil -insert NSCalendarsFullAccessUsageDescription -string \
    "Codex Companion reads upcoming calendar events only when you ask the on-device assistant about your schedule." "$INFO_PLIST"
  /usr/bin/plutil -insert NSCalendarsUsageDescription -string \
    "Codex Companion reads upcoming calendar events only when you ask the on-device assistant about your schedule." "$INFO_PLIST"
  /usr/bin/plutil -insert NSRemindersFullAccessUsageDescription -string \
    "Codex Companion reads incomplete reminders only when you ask the on-device assistant about them." "$INFO_PLIST"
  /usr/bin/plutil -insert NSRemindersUsageDescription -string \
    "Codex Companion reads incomplete reminders only when you ask the on-device assistant about them." "$INFO_PLIST"
  /usr/bin/plutil -insert NSPrincipalClass -string "NSApplication" "$INFO_PLIST"
  if [[ -n "$UPDATE_MANIFEST_URL" ]]; then
    /usr/bin/plutil -insert CodexCompanionUpdateManifestURL -string "$UPDATE_MANIFEST_URL" "$INFO_PLIST"
  fi
  if [[ -n "$UPDATE_PUBLIC_KEY" ]]; then
    /usr/bin/plutil -insert CodexCompanionUpdatePublicKey -string "$UPDATE_PUBLIC_KEY" "$INFO_PLIST"
  fi
  if [[ -n "$RELAY_URL" ]]; then
    /usr/bin/plutil -insert CodexCompanionRelayURL -string "$RELAY_URL" "$INFO_PLIST"
  fi
}

sign_app() {
  /usr/bin/xattr -cr "$APP_BUNDLE"
  if [[ -n "$SIGN_IDENTITY" ]]; then
    /usr/bin/codesign \
      --force \
      --deep \
      --options runtime \
      --timestamp \
      --sign "$SIGN_IDENTITY" \
      --identifier "$BUNDLE_ID" \
      "$APP_BUNDLE"
  else
    /usr/bin/codesign \
      --force \
      --deep \
      --sign - \
      --identifier "$BUNDLE_ID" \
      "$APP_BUNDLE"
    echo "Warning: produced an ad-hoc signed build. Gatekeeper will not trust it on another Mac." >&2
  fi
  /usr/bin/codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"
}

published_at() {
  if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
    /bin/date -u -r "$SOURCE_DATE_EPOCH" '+%Y-%m-%dT%H:%M:%SZ'
  else
    /bin/date -u '+%Y-%m-%dT%H:%M:%SZ'
  fi
}

create_manifest_if_configured() {
  local digest="$1"
  local size="$2"

  rm -f "$MANIFEST_PATH"
  if [[ -z "$UPDATE_PRIVATE_KEY" ]]; then
    echo "No update private key supplied; update.json was not generated." >&2
    return 0
  fi
  xcrun swift "$ROOT_DIR/script/sign_update_manifest.swift" \
    --version "$VERSION" \
    --build "$BUILD_NUMBER" \
    --minimum-system-version "$MIN_SYSTEM_VERSION" \
    --published-at "$(published_at)" \
    --download-url "$UPDATE_DOWNLOAD_URL" \
    --sha256 "$digest" \
    --size "$size" \
    --output "$MANIFEST_PATH"
}

copy_companion_skill() {
  local destination="$1"
  mkdir -p "$destination"
  /usr/bin/rsync -a \
    --exclude '.DS_Store' \
    --exclude '__pycache__' \
    --exclude 'tests' \
    "$COMPANION_SKILL_SOURCE/" \
    "$destination/"
}

write_installer_command() {
  local destination="$1"

  if [[ -n "$SIGN_IDENTITY" ]]; then
    /usr/bin/ditto "$ROOT_DIR/script/install_release.sh" "$destination"
    chmod +x "$destination"
    return 0
  fi

  /usr/bin/ditto "$ROOT_DIR/script/install_release.sh" "$DMG_STAGE/.install_release.sh"
  /bin/cat >"$destination" <<'INSTALLER'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "This Codex Companion build is not notarized by Apple."
echo "Only continue if you downloaded it from the official SilverFire GitHub release."
printf "Install Codex Companion anyway? [y/N] "
read -r confirmation
case "$confirmation" in
  y|Y|yes|YES|Yes) ;;
  *) echo "Installation canceled."; exit 1 ;;
esac

CODEX_COMPANION_ALLOW_ADHOC=1 exec "$SCRIPT_DIR/.install_release.sh"
INSTALLER
  chmod +x "$destination" "$DMG_STAGE/.install_release.sh"
}

validate_inputs
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR" "$APP_CONTENTS/MacOS" "$APP_RESOURCES"

ARM64_BINARY="$(build_architecture arm64 "$ARM64_TRIPLE")"
X86_64_BINARY="$(build_architecture x86_64 "$X86_64_TRIPLE")"
/usr/bin/lipo -create "$ARM64_BINARY" "$X86_64_BINARY" -output "$APP_BINARY"
chmod +x "$APP_BINARY"
ARCHITECTURES="$(/usr/bin/lipo -archs "$APP_BINARY")"
[[ " $ARCHITECTURES " == *" arm64 "* ]] || fail "universal app is missing arm64"
[[ " $ARCHITECTURES " == *" x86_64 "* ]] || fail "universal app is missing x86_64"

/usr/bin/ditto "$APP_ICON_SOURCE" "$APP_RESOURCES/CodexCompanion.icns"
copy_companion_skill "$APP_RESOURCES/Skills/companion-pet"
write_info_plist
sign_app

mkdir -p "$DMG_STAGE/Skills"
/usr/bin/ditto "$APP_BUNDLE" "$DMG_STAGE/$BUNDLE_NAME.app"
write_installer_command "$DMG_STAGE/Install Codex Companion.command"
copy_companion_skill "$DMG_STAGE/Skills/companion-pet"
ln -s /Applications "$DMG_STAGE/Applications"

rm -f "$DMG_PATH"
/usr/sbin/diskutil image create from \
  --format UDZO \
  --volumeName "$BUNDLE_NAME" \
  "$DMG_STAGE" \
  "$DMG_PATH"

if [[ -n "$SIGN_IDENTITY" ]]; then
  /usr/bin/codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG_PATH"
fi

if [[ -n "$NOTARY_PROFILE" ]]; then
  [[ -n "$SIGN_IDENTITY" ]] || fail "notarization requires CODEX_COMPANION_CODESIGN_IDENTITY"
  xcrun notarytool submit "$DMG_PATH" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG_PATH"
  xcrun stapler validate "$DMG_PATH"
else
  echo "Notarization skipped; set CODEX_COMPANION_NOTARY_PROFILE for a public release." >&2
fi

DMG_SHA256="$(/usr/bin/shasum -a 256 "$DMG_PATH" | /usr/bin/awk '{print $1}')"
DMG_SIZE="$(/usr/bin/stat -f '%z' "$DMG_PATH")"
create_manifest_if_configured "$DMG_SHA256" "$DMG_SIZE"

printf '%s  %s\n' "$DMG_SHA256" "$DMG_NAME" >"$OUTPUT_DIR/SHA256SUMS"
printf 'Release artifact: %s\n' "$DMG_PATH"
printf 'SHA-256: %s\n' "$DMG_SHA256"
printf 'Architectures: %s\n' "$ARCHITECTURES"
