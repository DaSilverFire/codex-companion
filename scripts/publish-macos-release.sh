#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="${1:?usage: publish-macos-release.sh VERSION}"
TAG="v$VERSION"
DMG="$REPO_ROOT/dist/CodexCompanion-$VERSION-macOS-universal.dmg"

cd "$REPO_ROOT"

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub CLI is required to publish a release" >&2
  exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Commit source changes before publishing a release" >&2
  exit 1
fi

"$SCRIPT_DIR/audit-release-tree.sh" "$REPO_ROOT"
"$SCRIPT_DIR/build-macos-release.sh" "$VERSION"
"$SCRIPT_DIR/verify-macos-release.sh" "$DMG"

if gh release view "$TAG" >/dev/null 2>&1; then
  echo "Release already exists: $TAG" >&2
  exit 1
fi

gh release create "$TAG" \
  --target "$(git rev-parse HEAD)" \
  --title "Codex Companion $VERSION" \
  --generate-notes \
  "$DMG" \
  "$DMG.sha256"

echo "Published Codex Companion $VERSION"
