#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_ID="com.silverfire.codexcompanion"
APP_NAME="CodexCompanion"
BUNDLE_NAME="Codex Companion"
RELAUNCH_LABEL="com.silverfire.codexcompanion.relauncher"
SOURCE_APP="$SCRIPT_DIR/$BUNDLE_NAME.app"
SOURCE_SKILL="$SCRIPT_DIR/Skills/companion-pet"
APP_INSTALL_DIR="${CODEX_COMPANION_INSTALL_APP_DIR:-/Applications}"
DEFAULT_INSTALLED_SKILL="$HOME/.codex/skills/companion-pet"
SKILL_PARENT="${CODEX_COMPANION_INSTALL_SKILL_ROOT:-$(dirname "$DEFAULT_INSTALLED_SKILL")}"
INSTALLED_APP="$APP_INSTALL_DIR/$BUNDLE_NAME.app"
INSTALLED_SKILL="$SKILL_PARENT/companion-pet"
SKIP_LAUNCH="${CODEX_COMPANION_SKIP_LAUNCH:-0}"
ALLOW_ADHOC="${CODEX_COMPANION_ALLOW_ADHOC:-0}"
INSTALL_RELAUNCH_AGENT="${CODEX_COMPANION_INSTALL_RELAUNCH_AGENT:-auto}"
LAUNCH_AGENT_DIR="${CODEX_COMPANION_LAUNCH_AGENT_DIR:-$HOME/Library/LaunchAgents}"
USER_DOMAIN="gui/$(id -u)"
INSTALLED_RELAUNCH_PLIST="$LAUNCH_AGENT_DIR/$RELAUNCH_LABEL.plist"
TRANSACTION_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/codex-companion-install.XXXXXX")"
STAGED_APP="$TRANSACTION_ROOT/$BUNDLE_NAME.app"
BACKUP_APP="$TRANSACTION_ROOT/$BUNDLE_NAME.previous.app"
STAGED_RELAUNCH_PLIST="$TRANSACTION_ROOT/$RELAUNCH_LABEL.plist"
BACKUP_RELAUNCH_PLIST="$TRANSACTION_ROOT/$RELAUNCH_LABEL.previous.plist"
STAGED_SKILL="$SKILL_PARENT/.companion-pet.install.$$"
BACKUP_SKILL="$SKILL_PARENT/.companion-pet.backup.$$"
APP_REPLACED=0
SKILL_REPLACED=0
RELAUNCH_REPLACED=0
RELAUNCH_BOOTSTRAPPED=0
RELAUNCH_WAS_LOADED=0
COMMITTED=0

if [[ "$INSTALL_RELAUNCH_AGENT" == "auto" ]]; then
  if [[ "$APP_INSTALL_DIR" == "/Applications" && "$SKIP_LAUNCH" != "1" ]]; then
    INSTALL_RELAUNCH_AGENT=1
  else
    INSTALL_RELAUNCH_AGENT=0
  fi
fi

cleanup() {
  local exit_code=$?

  if [[ $COMMITTED -ne 1 ]]; then
    if [[ $RELAUNCH_BOOTSTRAPPED -eq 1 ]]; then
      /bin/launchctl bootout "$USER_DOMAIN/$RELAUNCH_LABEL" >/dev/null 2>&1 || true
    fi
    if [[ $RELAUNCH_REPLACED -eq 1 ]]; then
      rm -f "$INSTALLED_RELAUNCH_PLIST"
      if [[ -f "$BACKUP_RELAUNCH_PLIST" ]]; then
        mv "$BACKUP_RELAUNCH_PLIST" "$INSTALLED_RELAUNCH_PLIST"
      fi
    fi
    if [[ $APP_REPLACED -eq 1 ]]; then
      rm -rf "$INSTALLED_APP"
      if [[ -e "$BACKUP_APP" || -L "$BACKUP_APP" ]]; then
        mv "$BACKUP_APP" "$INSTALLED_APP"
      fi
    fi
    if [[ $SKILL_REPLACED -eq 1 ]]; then
      rm -rf "$INSTALLED_SKILL"
      if [[ -e "$BACKUP_SKILL" || -L "$BACKUP_SKILL" ]]; then
        mv "$BACKUP_SKILL" "$INSTALLED_SKILL"
      fi
    fi
    if [[ $RELAUNCH_WAS_LOADED -eq 1 && -f "$INSTALLED_RELAUNCH_PLIST" ]]; then
      /bin/launchctl bootstrap "$USER_DOMAIN" "$INSTALLED_RELAUNCH_PLIST" >/dev/null 2>&1 || true
    fi
  fi

  rm -rf "$STAGED_SKILL" "$BACKUP_SKILL" "$TRANSACTION_ROOT"
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

fail() {
  echo "Codex Companion installation failed: $*" >&2
  exit 1
}

bundle_identifier() {
  /usr/bin/plutil -extract CFBundleIdentifier raw -o - "$1/Contents/Info.plist" 2>/dev/null || true
}

validate_app() {
  local app="$1"
  local architectures
  local team_identifier

  [[ -d "$app" ]] || fail "missing app bundle at $app"
  [[ "$(bundle_identifier "$app")" == "$BUNDLE_ID" ]] || fail "unexpected bundle identifier"
  [[ -x "$app/Contents/MacOS/$APP_NAME" ]] || fail "missing app executable"
  [[ -f "$app/Contents/Resources/CodexCompanion.icns" ]] || fail "missing app icon"
  /usr/bin/codesign --verify --deep --strict "$app" || fail "code signature verification failed"
  team_identifier="$(/usr/bin/codesign -dvvv "$app" 2>&1 | /usr/bin/awk -F= '/^TeamIdentifier=/{print $2; exit}')"
  if [[ -z "$team_identifier" || "$team_identifier" == "not set" ]]; then
    [[ "$ALLOW_ADHOC" == "1" ]] || fail "ad-hoc app rejected; use a signed and notarized GitHub Release"
  else
    /usr/sbin/spctl --assess --type execute --verbose=2 "$app" || fail "Gatekeeper assessment failed"
  fi
  architectures="$(/usr/bin/lipo -archs "$app/Contents/MacOS/$APP_NAME")"
  [[ " $architectures " == *" arm64 "* ]] || fail "app is missing arm64"
  [[ " $architectures " == *" x86_64 "* ]] || fail "app is missing x86_64"
}

validate_skill() {
  local skill="$1"

  [[ -f "$skill/SKILL.md" ]] || fail "missing Companion pet skill entrypoint"
  [[ -f "$skill/scripts/companion_pet_assets.py" ]] || fail "missing Companion pet tooling"
  [[ -f "$skill/references/companion-contract.md" ]] || fail "missing Companion pet contract"
}

job_is_loaded() {
  /bin/launchctl print "$USER_DOMAIN/$1" >/dev/null 2>&1
}

stop_relaunch_agents() {
  if job_is_loaded "$RELAUNCH_LABEL"; then
    RELAUNCH_WAS_LOADED=1
  fi
  /bin/launchctl bootout "$USER_DOMAIN/$RELAUNCH_LABEL" >/dev/null 2>&1 || true
}

generate_relaunch_plist() {
  local plist="$1"
  local executable="$INSTALLED_APP/Contents/MacOS/$APP_NAME"

  /usr/bin/plutil -create xml1 "$plist"
  /usr/bin/plutil -insert Label -string "$RELAUNCH_LABEL" "$plist"
  /usr/bin/plutil -insert ProgramArguments -json "[\"$executable\"]" "$plist"
  /usr/bin/plutil -insert RunAtLoad -bool true "$plist"
  /usr/bin/plutil -insert KeepAlive -json '{"SuccessfulExit":false}' "$plist"
  /usr/bin/plutil -insert ProcessType -string Interactive "$plist"
  /usr/bin/plutil -insert ThrottleInterval -integer 5 "$plist"
}

validate_relaunch_plist() {
  local plist="$1"
  local executable="$INSTALLED_APP/Contents/MacOS/$APP_NAME"

  /usr/bin/plutil -lint "$plist" >/dev/null || fail "invalid relaunch agent plist"
  [[ "$(/usr/bin/plutil -extract Label raw -o - "$plist")" == "$RELAUNCH_LABEL" ]] \
    || fail "unexpected relaunch agent label"
  [[ "$(/usr/bin/plutil -extract ProgramArguments.0 raw -o - "$plist")" == "$executable" ]] \
    || fail "unexpected relaunch executable"
  [[ "$(/usr/bin/plutil -extract KeepAlive.SuccessfulExit raw -o - "$plist")" == "false" ]] \
    || fail "relaunch agent must stop after a normal quit"
  if /usr/bin/grep -Eq 'app-server|ChatGPT' "$plist"; then
    fail "relaunch agent must only manage Codex Companion"
  fi
}

stop_running_companion() {
  local deadline=$((SECONDS + 8))
  local pids

  pids="$(pgrep -f "$INSTALLED_APP/Contents/MacOS/$APP_NAME" || true)"
  [[ -z "$pids" ]] && return 0
  kill $pids >/dev/null 2>&1 || true
  while [[ $SECONDS -lt $deadline ]]; do
    pids="$(pgrep -f "$INSTALLED_APP/Contents/MacOS/$APP_NAME" || true)"
    [[ -z "$pids" ]] && return 0
    sleep 0.2
  done
  kill -9 $pids >/dev/null 2>&1 || true
}

[[ -d "$SOURCE_APP" ]] || fail "open the release disk image and run this installer from it"
[[ -d "$SOURCE_SKILL" ]] || fail "the release is missing Skills/companion-pet"
validate_app "$SOURCE_APP"
validate_skill "$SOURCE_SKILL"

if [[ "$INSTALL_RELAUNCH_AGENT" == "1" ]]; then
  mkdir -p "$LAUNCH_AGENT_DIR"
  stop_relaunch_agents
  generate_relaunch_plist "$STAGED_RELAUNCH_PLIST"
  validate_relaunch_plist "$STAGED_RELAUNCH_PLIST"
  if [[ -f "$INSTALLED_RELAUNCH_PLIST" ]]; then
    mv "$INSTALLED_RELAUNCH_PLIST" "$BACKUP_RELAUNCH_PLIST"
  fi
fi

/usr/bin/ditto "$SOURCE_APP" "$STAGED_APP"
validate_app "$STAGED_APP"

mkdir -p "$APP_INSTALL_DIR" "$SKILL_PARENT"
rm -rf "$STAGED_SKILL" "$BACKUP_SKILL"
/usr/bin/ditto "$SOURCE_SKILL" "$STAGED_SKILL"
validate_skill "$STAGED_SKILL"

stop_running_companion

if [[ -e "$INSTALLED_APP" || -L "$INSTALLED_APP" ]]; then
  mv "$INSTALLED_APP" "$BACKUP_APP"
fi
APP_REPLACED=1
/usr/bin/ditto "$STAGED_APP" "$INSTALLED_APP"
validate_app "$INSTALLED_APP"
if [[ "${CODEX_COMPANION_INSTALL_TEST_FAIL_AFTER_APP:-0}" == "1" ]]; then
  fail "injected rollback verification failure"
fi

if [[ -e "$INSTALLED_SKILL" || -L "$INSTALLED_SKILL" ]]; then
  mv "$INSTALLED_SKILL" "$BACKUP_SKILL"
fi
SKILL_REPLACED=1
mv "$STAGED_SKILL" "$INSTALLED_SKILL"
validate_skill "$INSTALLED_SKILL"

if [[ "$INSTALL_RELAUNCH_AGENT" == "1" ]]; then
  mv "$STAGED_RELAUNCH_PLIST" "$INSTALLED_RELAUNCH_PLIST"
  RELAUNCH_REPLACED=1
  validate_relaunch_plist "$INSTALLED_RELAUNCH_PLIST"
  /bin/launchctl bootstrap "$USER_DOMAIN" "$INSTALLED_RELAUNCH_PLIST" \
    || fail "could not activate the Companion relaunch agent"
  RELAUNCH_BOOTSTRAPPED=1
fi

COMMITTED=1
rm -rf "$BACKUP_APP" "$BACKUP_SKILL" "$BACKUP_RELAUNCH_PLIST"

echo "Installed $INSTALLED_APP"
echo "Installed $INSTALLED_SKILL"
echo "Existing SilverFire settings and Keychain items were preserved."
echo "ChatGPT and the shared Codex app-server were not changed or relaunched."
if [[ "$INSTALL_RELAUNCH_AGENT" == "1" ]]; then
  echo "Installed crash recovery at $INSTALLED_RELAUNCH_PLIST (normal Quit remains closed)."
elif [[ "$SKIP_LAUNCH" != "1" ]]; then
  /usr/bin/open "$INSTALLED_APP"
fi
