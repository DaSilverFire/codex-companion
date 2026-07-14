#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_ROOT="$REPO_ROOT/macos/CodexCompanion"
VERSION="${1:-0.3.0}"
BUILD_NUMBER="${CODEX_COMPANION_BUILD_NUMBER:-$(date -u +%Y%m%d%H%M)}"
BUNDLE_ID="com.silverfire.codexcompanion"
APP_NAME="Codex Companion"
EXECUTABLE_NAME="CodexCompanion"
DIST_DIR="$REPO_ROOT/dist"
WORK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/codex-companion-release.XXXXXX")"
BUILD_SOURCE="$WORK_ROOT/source"
BUILD_ROOT="$WORK_ROOT/build"
APP_BUNDLE="$WORK_ROOT/$APP_NAME.app"
APP_CONTENTS="$APP_BUNDLE/Contents"
APP_MACOS="$APP_CONTENTS/MacOS"
APP_RESOURCES="$APP_CONTENTS/Resources"
INFO_PLIST="$APP_CONTENTS/Info.plist"
VOLUME_ROOT="$WORK_ROOT/volume"
DMG_NAME="CodexCompanion-$VERSION-macOS-universal.dmg"
DMG_PATH="$DIST_DIR/$DMG_NAME"

cleanup() {
  rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

if [[ ! "$VERSION" =~ ^[0-9]+([.][0-9]+){1,3}([+-][A-Za-z0-9.-]+)?$ ]]; then
  echo "Version must be a numeric release version, for example 0.3.0" >&2
  exit 2
fi

for required in Package.swift Sources Tests Assets; do
  if [[ ! -e "$SOURCE_ROOT/$required" ]]; then
    echo "Missing release source: $SOURCE_ROOT/$required" >&2
    exit 1
  fi
done

mkdir -p "$BUILD_SOURCE" "$DIST_DIR"
/usr/bin/ditto "$SOURCE_ROOT/Package.swift" "$BUILD_SOURCE/Package.swift"
/usr/bin/ditto "$SOURCE_ROOT/Sources" "$BUILD_SOURCE/Sources"
/usr/bin/ditto "$SOURCE_ROOT/Tests" "$BUILD_SOURCE/Tests"
/usr/bin/ditto "$SOURCE_ROOT/Assets" "$BUILD_SOURCE/Assets"

swift build \
  --package-path "$BUILD_SOURCE" \
  --scratch-path "$BUILD_ROOT" \
  -c release \
  --arch arm64 \
  --arch x86_64

BUILD_BINARY="$BUILD_ROOT/apple/Products/Release/$EXECUTABLE_NAME"
if [[ ! -x "$BUILD_BINARY" ]]; then
  echo "Universal release binary was not produced: $BUILD_BINARY" >&2
  exit 1
fi

architectures="$(/usr/bin/lipo -archs "$BUILD_BINARY")"
if [[ "$architectures" != *"arm64"* || "$architectures" != *"x86_64"* ]]; then
  echo "Release binary is not universal: $architectures" >&2
  exit 1
fi

mkdir -p "$APP_MACOS" "$APP_RESOURCES"
/usr/bin/ditto "$BUILD_BINARY" "$APP_MACOS/$EXECUTABLE_NAME"
chmod 755 "$APP_MACOS/$EXECUTABLE_NAME"
/usr/bin/ditto "$SOURCE_ROOT/Assets/AppIcon/CodexCompanion.icns" "$APP_RESOURCES/CodexCompanion.icns"
/usr/bin/ditto "$REPO_ROOT/LICENSE" "$APP_RESOURCES/LICENSE.txt"

/usr/bin/plutil -create xml1 "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleDisplayName -string "$APP_NAME" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleExecutable -string "$EXECUTABLE_NAME" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleIdentifier -string "$BUNDLE_ID" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleIconFile -string "CodexCompanion" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleName -string "$APP_NAME" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundlePackageType -string "APPL" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleShortVersionString -string "$VERSION" "$INFO_PLIST"
/usr/bin/plutil -insert CFBundleVersion -string "$BUILD_NUMBER" "$INFO_PLIST"
/usr/bin/plutil -insert LSMinimumSystemVersion -string "14.0" "$INFO_PLIST"
/usr/bin/plutil -insert LSUIElement -bool true "$INFO_PLIST"
/usr/bin/plutil -insert NSHighResolutionCapable -bool true "$INFO_PLIST"
/usr/bin/plutil -insert NSAccessibilityUsageDescription -string "Codex Companion can use Accessibility only when a visible UI fallback is explicitly requested." "$INFO_PLIST"
/usr/bin/plutil -insert NSBonjourServices -json '["_codex-companion._tcp."]' "$INFO_PLIST"
/usr/bin/plutil -insert NSLocalNetworkUsageDescription -string "Codex Companion connects to Companion mobile devices on your local network." "$INFO_PLIST"
/usr/bin/plutil -insert NSPrincipalClass -string "NSApplication" "$INFO_PLIST"

SIGN_IDENTITY="${CODEX_COMPANION_CODESIGN_IDENTITY:--}"
/usr/bin/xattr -cr "$APP_BUNDLE"
/usr/bin/codesign \
  --force \
  --deep \
  --options runtime \
  --timestamp=none \
  --sign "$SIGN_IDENTITY" \
  --identifier "$BUNDLE_ID" \
  "$APP_BUNDLE"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE"

mkdir -p "$VOLUME_ROOT"
/usr/bin/ditto "$APP_BUNDLE" "$VOLUME_ROOT/$APP_NAME.app"
ln -s /Applications "$VOLUME_ROOT/Applications"
/usr/bin/ditto "$REPO_ROOT/installer/README.txt" "$VOLUME_ROOT/Read Me.txt"

rm -f "$DMG_PATH" "$DMG_PATH.sha256"
/usr/bin/hdiutil create \
  -volname "$APP_NAME" \
  -srcfolder "$VOLUME_ROOT" \
  -format UDZO \
  -ov \
  "$DMG_PATH"

(
  cd "$DIST_DIR"
  /usr/bin/shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256"
)

echo "Built $DMG_PATH"
echo "Architectures: $architectures"
echo "Bundle identifier: $BUNDLE_ID"
if [[ "$SIGN_IDENTITY" == "-" ]]; then
  echo "Signing: ad hoc (not notarized)"
else
  echo "Signing identity: $SIGN_IDENTITY"
fi
