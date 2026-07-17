#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-$ROOT_DIR/dist/public-source/CodexCompanion}"

ALLOWLIST=(
  ".gitignore"
  "Package.swift"
  "README.md"
  "RELEASING.md"
  "VERSION"
  "Assets/AppIcon"
  "Sources"
  "Tests/CodexCompanionTests"
  "Tests/test_codex_only_sender.py"
  "Tests/test_release_packaging.py"
  "script/build_and_run.sh"
  "script/create_release.sh"
  "script/export_public_source.sh"
  "script/install_release.sh"
  "script/sign_update_manifest.swift"
  "Skills/companion-pet"
)

fail() {
  echo "Public source export failed: $*" >&2
  exit 1
}

RG_BIN="$(command -v rg || true)"
[[ -n "$RG_BIN" ]] || fail "ripgrep is required for fail-closed content scanning"

case "$OUTPUT_DIR" in
  ""|"/"|"$HOME"|"$ROOT_DIR")
    fail "unsafe output directory: $OUTPUT_DIR"
    ;;
esac

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

for relative_path in "${ALLOWLIST[@]}"; do
  source_path="$ROOT_DIR/$relative_path"
  [[ -e "$source_path" ]] || fail "allowlisted path is missing: $relative_path"
  mkdir -p "$OUTPUT_DIR/$(dirname "$relative_path")"
  /usr/bin/ditto "$source_path" "$OUTPUT_DIR/$relative_path"
done

find "$OUTPUT_DIR" -depth -type d \( \
  -name .git -o \
  -name .build -o \
  -name dist -o \
  -name qa -o \
  -name work -o \
  -name logs -o \
  -name captures -o \
  -name DerivedData -o \
  -name xcuserdata \
\) -exec rm -rf {} +

find "$OUTPUT_DIR" -type f \( \
  -name .DS_Store -o \
  -name '*.log' -o \
  -name '*.crash' -o \
  -name '*.ips' -o \
  -name '*.mov' -o \
  -name '*.mp4' -o \
  -name '*.mobileprovision' -o \
  -name '*.p12' -o \
  -name '*.key' -o \
  -name 'config.toml' \
\) -delete

if find "$OUTPUT_DIR" -type f \( \
  -name '*spritesheet*' -o \
  -name '*contact-sheet*' -o \
  -name '*screen-recording*' -o \
  -name '*screenshot*' \
\) -print -quit | grep -q .; then
  fail "generated sprite or QA capture escaped the allowlist"
fi

# Reject local machine host names, private-key material, API credentials,
# absolute /Users/ paths, and case-insensitive device identifiers.
if "$RG_BIN" -n --hidden \
  --pcre2 \
  --glob '!**/Assets/AppIcon/**' \
  --glob '!**/script/export_public_source.sh' \
  --glob '!**/Tests/test_release_packaging.py' \
  '(/Users/(?!test/)|MacBook-[A-Za-z0-9-]+[.]local|sk-(proj-)?[A-Za-z0-9_-]{20,}|-----BEGIN ([A-Z ]+)?PRIVATE KEY-----|(?i:[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}))' \
  "$OUTPUT_DIR"; then
  fail "local path, host name, API key, signing material, or device identifier found"
fi

PERSONAL_NAMESPACE_PATTERN='har''lin'
if "$RG_BIN" -n --hidden --pcre2 "(?i:${PERSONAL_NAMESPACE_PATTERN})" "$OUTPUT_DIR"; then
  fail "personal namespace found"
fi

if find "$OUTPUT_DIR" -type l -print -quit | grep -q .; then
  fail "symbolic links are not allowed in the public export"
fi

echo "Public source export ready at $OUTPUT_DIR"
echo "This directory was copied from an explicit ALLOWLIST."
