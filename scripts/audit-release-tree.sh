#!/usr/bin/env bash
set -euo pipefail

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

if ! git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  printf 'error: not a git worktree: %s\n' "$root" >&2
  exit 2
fi

path_pattern='(^|/)(\.build|DerivedData|dist|artifacts|work|qa|output|generated|tmp)(/|$)|(^|/)(session_index\.json|task-history[^/]*\.json|device-identities\.json|local-settings\.json)$|\.(log|trace|memgraph|sqlite|sqlite-shm|sqlite-wal|p12|cer|key|pem|mobileprovision|provisionprofile|token|api-key)$'
content_pattern='OPENAI_API_''KEY[[:space:]]*=|GITHUB_''TOKEN[[:space:]]*=|sk-''proj-[A-Za-z0-9_-]{8,}|gh[opsur]_[A-Za-z0-9_]{8,}|/''Users/[A-Za-z0-9._-]+/|/var/''folders/'

failed=0
while IFS= read -r relative_path; do
  [ -n "$relative_path" ] || continue

  if printf '%s\n' "$relative_path" | grep -E -q "$path_pattern"; then
    printf 'forbidden release path: %s\n' "$relative_path" >&2
    failed=1
    continue
  fi

  absolute_path="$root/$relative_path"
  if [ -f "$absolute_path" ] && grep -Iq . "$absolute_path"; then
    if LC_ALL=C grep -E -n -m 1 "$content_pattern" "$absolute_path" >/dev/null 2>&1; then
      printf 'sensitive content in: %s\n' "$relative_path" >&2
      failed=1
    fi
  fi
done < <(git -C "$root" ls-files --cached --others --exclude-standard)

if [ "$failed" -ne 0 ]; then
  exit 1
fi

printf 'Release tree audit passed: %s\n' "$root"
