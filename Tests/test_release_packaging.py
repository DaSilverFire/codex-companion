#!/usr/bin/env python3
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
BUILD_SCRIPT = ROOT / "script" / "build_and_run.sh"
SHARED_SERVER_SCRIPT = ROOT / "script" / "configure_shared_app_server.sh"
APP_ENTRY = ROOT / "Sources" / "CodexCompanion" / "App" / "CodexCompanionApp.swift"
LEGACY_SHARED_SERVER_CLEANUP = (
    ROOT
    / "Sources"
    / "CodexCompanion"
    / "Services"
    / "LegacySharedAppServerEnvironmentCleanup.swift"
)
PLUGIN_ROOT = ROOT.parent / "plugins" / "codex-companion" / "0.2.0"
PLUGIN_INSTALL_SCRIPT = PLUGIN_ROOT / "scripts" / "install-codex-companion.sh"
PLUGIN_SHARED_SERVER_SCRIPT = PLUGIN_ROOT / "scripts" / "configure-shared-app-server.sh"
PLUGIN_SKILL = PLUGIN_ROOT / "skills" / "codex-companion" / "SKILL.md"
RELEASE_SCRIPT = ROOT / "script" / "create_release.sh"
RELEASE_INSTALLER = ROOT / "script" / "install_release.sh"
PUBLIC_EXPORT_SCRIPT = ROOT / "script" / "export_public_source.sh"
MANIFEST_SIGNER = ROOT / "script" / "sign_update_manifest.swift"
UPDATE_SERVICE = ROOT / "Sources" / "CodexCompanion" / "Services" / "CompanionUpdateService.swift"
SETTINGS_VIEW = ROOT / "Sources" / "CodexCompanion" / "Views" / "SettingsView.swift"
RELEASING_GUIDE = ROOT / "RELEASING.md"
README = ROOT / "README.md"
BUNDLED_COMPANION_SKILL = ROOT / "Skills" / "companion-pet"
PLUGIN_STAGER = ROOT.parent / "scripts" / "install_personal_codex_companion_plugin.py"


def plugin_fixture_is_available():
    return PLUGIN_INSTALL_SCRIPT.is_file()


def test_release_candidate_uses_one_non_colliding_version_source():
    assert VERSION_FILE.read_text().strip() == "0.3.4"

    build_source = BUILD_SCRIPT.read_text()
    release_source = RELEASE_SCRIPT.read_text()
    assert 'VERSION_FILE="$ROOT_DIR/VERSION"' in build_source
    assert 'VERSION_FILE="$ROOT_DIR/VERSION"' in release_source
    assert "CFBundleShortVersionString" in build_source
    assert "CFBundleShortVersionString" in release_source
    if plugin_fixture_is_available():
        plugin_source = PLUGIN_INSTALL_SCRIPT.read_text()
        assert 'VERSION_FILE="$SOURCE_DIR/VERSION"' in plugin_source
        assert "CFBundleShortVersionString" in plugin_source


def test_build_uses_silverfire_namespace_and_never_activates_shared_server():
    source = BUILD_SCRIPT.read_text()

    assert 'BUNDLE_ID="com.silverfire.codexcompanion"' in source
    assert "CODEX_COMPANION_CONFIGURE_SHARED_APP_SERVER" not in source
    assert "configure_shared_app_server.sh" not in source
    assert "app-server daemon" not in source
    assert "CODEX_APP_SERVER_USE_LOCAL_DAEMON" not in source
    assert "CodexCompanionUpdateManifestURL" in source
    assert "CodexCompanionUpdatePublicKey" in source
    assert "CODEX_COMPANION_UPDATE_MANIFEST_URL" in source
    assert "CODEX_COMPANION_UPDATE_PUBLIC_KEY" in source
    assert "https://github.com/DaSilverFire/codex-companion/releases/latest/download/update.json" in source
    assert 'DEFAULT_UPDATE_PUBLIC_KEY="/b26MOV9HlKeifsp8TCIb3tPDJW5SGBf7o/CE+RooVg="' in source
    assert 'cp "$APP_ICON_SOURCE" "$APP_RESOURCES/$APP_ICON_NAME.icns"' in source
    assert "/Applications/Xcode-beta.app/Contents/Developer" in source
    assert "CODEX_COMPANION_BUILD_SCRATCH_PATH" in source
    assert '--scratch-path "$SWIFT_BUILD_SCRATCH"' in source
    assert 'XDG_CACHE_HOME="$SWIFT_BUILD_SCRATCH/cache"' in source
    assert 'CLANG_MODULE_CACHE_PATH="$SWIFT_BUILD_SCRATCH/clang-module-cache"' in source
    assert 'GIT_OPTIONAL_LOCKS="${GIT_OPTIONAL_LOCKS:-0}"' in source


def test_builds_embed_only_an_explicit_secure_relay_endpoint():
    for script in [BUILD_SCRIPT, RELEASE_SCRIPT]:
        source = script.read_text()
        assert 'RELAY_URL="${CODEX_COMPANION_RELAY_URL:-}"' in source
        assert "CodexCompanionRelayURL" in source
        assert "relay URL must use WSS" in source
        assert "wss://" in source

    release_guide = RELEASING_GUIDE.read_text()
    assert 'CODEX_COMPANION_RELAY_URL="wss://' in release_guide
    assert "Do not use a temporary preview URL for a release" in release_guide


def test_mobile_beta_is_unavailable_in_public_builds_unless_explicitly_authorized():
    for script in [BUILD_SCRIPT, RELEASE_SCRIPT]:
        source = script.read_text()
        assert 'MOBILE_BETA_AUTHORIZED="${CODEX_COMPANION_MOBILE_BETA_AUTHORIZED:-0}"' in source
        assert "CODEX_COMPANION_MOBILE_BETA_AUTHORIZED must be 0 or 1" in source
        assert "CodexCompanionMobileBetaAuthorized" in source
        assert 'if [[ "$MOBILE_BETA_AUTHORIZED" == "1" ]]; then' in source
        assert "NSBonjourServices" in source
        assert "NSLocalNetworkUsageDescription" in source

    runtime_source = (
        ROOT / "Sources" / "CodexCompanion" / "Support" / "CompanionMobileRuntime.swift"
    ).read_text()
    assert "CodexCompanion.mobileBetaAccessGranted.v1" in runtime_source
    assert "bundleAuthorization || defaults.bool" in runtime_source


def test_shared_server_reconfiguration_assets_and_launch_cleanup_are_absent():
    assert not SHARED_SERVER_SCRIPT.exists()
    assert not PLUGIN_SHARED_SERVER_SCRIPT.exists()
    assert not LEGACY_SHARED_SERVER_CLEANUP.exists()

    assert "LegacySharedAppServerEnvironmentCleanup" not in APP_ENTRY.read_text()
    assert "configure_shared_app_server.sh" not in PUBLIC_EXPORT_SCRIPT.read_text()
    if PLUGIN_STAGER.is_file():
        assert "configure_shared_app_server" not in PLUGIN_STAGER.read_text()
        assert "configure-shared-app-server" not in PLUGIN_STAGER.read_text()
    if PLUGIN_SKILL.is_file():
        assert "configure-shared-app-server" not in PLUGIN_SKILL.read_text()
        assert "activate-daemon" not in PLUGIN_SKILL.read_text()


def test_plugin_installer_uses_silverfire_namespace_without_daemon_activation():
    if not plugin_fixture_is_available():
        return

    source = PLUGIN_INSTALL_SCRIPT.read_text()

    assert 'BUNDLE_ID="com.silverfire.codexcompanion"' in source
    assert "configure-shared-app-server.sh" not in source
    assert "app-server daemon" not in source
    assert "CODEX_APP_SERVER_USE_LOCAL_DAEMON" not in source
    assert "CodexCompanionUpdateManifestURL" in source
    assert "CodexCompanionUpdatePublicKey" in source
    assert "CODEX_COMPANION_UPDATE_MANIFEST_URL" in source
    assert "CODEX_COMPANION_UPDATE_PUBLIC_KEY" in source
    assert 'cp "$APP_ICON_SOURCE" "$APP_RESOURCES/CodexCompanion.icns"' in source


def test_release_builder_creates_universal_dmg_without_daemon_activation():
    source = RELEASE_SCRIPT.read_text()

    assert "arm64-apple-macosx" in source
    assert "x86_64-apple-macosx" in source
    assert "lipo" in source and "-create" in source
    assert "diskutil image create from" in source
    assert "hdiutil create" not in source
    assert "codesign" in source
    assert "notarytool" in source
    assert "stapler" in source
    assert "CodexCompanion.icns" in source
    assert "com.silverfire.codexcompanion" in source
    assert "copy_companion_skill()" in source
    assert "--exclude 'tests'" in source
    assert "--exclude '__pycache__'" in source
    assert "app-server daemon" not in source
    assert "configure_shared_app_server" not in source
    assert "CODEX_APP_SERVER_USE_LOCAL_DAEMON" not in source
    assert "com.silverfire.codexcompanion.update-signing-key" in source
    assert "security find-generic-password" in source
    assert "https://github.com/DaSilverFire/codex-companion/releases/latest/download/update.json" in source
    assert "CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64" in source
    assert "/Applications/Xcode-beta.app/Contents/Developer" in source
    assert 'GIT_OPTIONAL_LOCKS="${GIT_OPTIONAL_LOCKS:-0}"' in source


def test_release_installer_is_rollback_safe_and_installs_skill():
    source = RELEASE_INSTALLER.read_text()

    assert "BACKUP_APP" in source
    assert "codesign" in source and "--verify" in source
    assert "com.silverfire.codexcompanion" in source
    assert "Skills/companion-pet" in source
    assert '"$HOME/.codex/skills/companion-pet"' in source
    assert "CODEX_COMPANION_INSTALL_APP_DIR" in source
    assert "CODEX_COMPANION_INSTALL_SKILL_ROOT" in source
    assert "CODEX_COMPANION_SKIP_LAUNCH" in source
    assert "CODEX_COMPANION_INSTALL_TEST_FAIL_AFTER_APP" in source
    assert "CODEX_COMPANION_ALLOW_ADHOC" in source
    assert "TeamIdentifier" in source
    assert "spctl" in source and "--assess" in source
    assert "mv \"$BACKUP_APP\" \"$INSTALLED_APP\"" in source
    assert "defaults delete" not in source
    assert "security delete-generic-password" not in source
    assert 'RELAUNCH_LABEL="com.silverfire.codexcompanion.relauncher"' in source
    assert "LEGACY_RELAUNCH" not in source
    assert "SuccessfulExit" in source
    assert "CODEX_COMPANION_INSTALL_RELAUNCH_AGENT" in source
    assert "launchctl bootstrap" in source
    assert "launchctl bootout" in source
    assert "app-server daemon" not in source
    assert "configure_shared_app_server" not in source
    assert "CODEX_APP_SERVER_USE_LOCAL_DAEMON" not in source


def test_public_export_is_allowlist_driven_and_rejects_local_material():
    source = PUBLIC_EXPORT_SCRIPT.read_text()

    assert "ALLOWLIST" in source
    assert '"Sources"' in source
    assert '"VERSION"' in source
    assert '"Skills/companion-pet"' in source
    assert '"Assets/AppIcon"' in source
    assert "git ls-files" not in source
    assert "command -v rg" in source
    assert 'fail "ripgrep is required for fail-closed content scanning"' in source
    for rejected in [
        ".build",
        "dist",
        "qa",
        "work",
        "/Users/",
        "MacBook-[A-Za-z0-9-]+[.]local",
        "-----BEGIN ([A-Z ]+)?PRIVATE KEY-----",
        "sk-(proj-)?",
        "(?i:[0-9a-f]{8}",
    ]:
        assert rejected in source

    assert "configure_shared_app_server.sh" not in source


def test_public_release_paths_exclude_personal_legacy_namespace():
    personal_namespace = "har" + "lin"
    release_paths = [
        README,
        RELEASING_GUIDE,
        RELEASE_INSTALLER,
        PUBLIC_EXPORT_SCRIPT,
        ROOT / "Sources" / "CodexCompanion",
        ROOT / "Tests",
    ]

    for path in release_paths:
        files = [path] if path.is_file() else [candidate for candidate in path.rglob("*") if candidate.is_file()]
        for candidate in files:
            try:
                source = candidate.read_text()
            except UnicodeDecodeError:
                continue
            assert personal_namespace.lower() not in source.lower(), candidate


def test_public_export_rejects_uppercase_and_lowercase_uuid_literals():
    with tempfile.TemporaryDirectory(prefix="codex-companion-export-test.") as temporary:
        temporary_root = Path(temporary)
        pristine = temporary_root / "pristine"
        initial = subprocess.run(
            [str(PUBLIC_EXPORT_SCRIPT), str(pristine)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert initial.returncode == 0, initial.stderr

        for label, identifier in [
            ("uppercase", "A1B2C3D4" + "-E5F6-47A8-9B0C-D1E2F3A4B5C6"),
            ("lowercase", "a1b2c3d4" + "-e5f6-47a8-9b0c-d1e2f3a4b5c6"),
        ]:
            source = temporary_root / f"source-{label}"
            output = temporary_root / f"output-{label}"
            shutil.copytree(pristine, source)
            readme = source / "README.md"
            readme.write_text(f"{readme.read_text()}\nDevice: {identifier}\n")

            result = subprocess.run(
                [str(source / "script" / "export_public_source.sh"), str(output)],
                capture_output=True,
                text=True,
                check=False,
            )
            assert result.returncode != 0
            assert "device identifier found" in result.stderr


def test_public_export_rejects_personal_namespace():
    personal_namespace = "har" + "lin"
    with tempfile.TemporaryDirectory(prefix="codex-companion-export-test.") as temporary:
        temporary_root = Path(temporary)
        pristine = temporary_root / "pristine"
        initial = subprocess.run(
            [str(PUBLIC_EXPORT_SCRIPT), str(pristine)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert initial.returncode == 0, initial.stderr

        source = temporary_root / "source-personal-namespace"
        output = temporary_root / "output-personal-namespace"
        shutil.copytree(pristine, source)
        readme = source / "README.md"
        readme.write_text(f"{readme.read_text()}\nLegacy namespace: {personal_namespace}\n")

        result = subprocess.run(
            [str(source / "script" / "export_public_source.sh"), str(output)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode != 0
        assert "personal namespace found" in result.stderr


def test_public_readme_is_mac_product_copy_without_mobile_release_notes():
    source = README.read_text()

    assert "expands the Codex pet experience" in source
    assert "Mobile pairing" not in source
    assert "Companion Pocket" not in source
    assert "CodexCompanionRelay" not in source
    assert "/Users/" not in source


def test_release_contract_uses_signed_https_manifest():
    guide = RELEASING_GUIDE.read_text()
    signer = MANIFEST_SIGNER.read_text()

    assert "CodexCompanionUpdateManifestURL" in guide
    assert "CodexCompanionUpdatePublicKey" in guide
    assert "CODEX_COMPANION_UPDATE_PRIVATE_KEY_BASE64" in guide
    assert "GitHub Releases" in guide
    assert "Gatekeeper" in guide
    assert "rollback" in guide.lower()
    assert "Curve25519.Signing.PrivateKey" in signer
    assert "canonicalPayload" in signer


def test_verified_updater_hands_off_to_transactional_installer():
    source = UPDATE_SERVICE.read_text()
    settings = SETTINGS_VIEW.read_text()

    assert "Install and Relaunch" in settings
    assert "/usr/bin/shasum -a 256" in source
    assert "/usr/bin/hdiutil attach -readonly -nobrowse" in source
    assert "Install Codex Companion.command" in source
    assert ".install_release.sh" in source
    assert "CODEX_COMPANION_ALLOW_ADHOC=1" in source
    assert "CODEX_COMPANION_INSTALL_RELAUNCH_AGENT" in source
    assert "case installing(CompanionUpdateManifest)" in source


def test_unsigned_release_wraps_transactional_installer_with_explicit_confirmation():
    source = RELEASE_SCRIPT.read_text()

    assert 'write_installer_command "$DMG_STAGE/Install Codex Companion.command"' in source
    assert '"$DMG_STAGE/.install_release.sh"' in source
    assert "This Codex Companion build is not notarized by Apple." in source
    assert "Install Codex Companion anyway? [y/N]" in source
    assert 'CODEX_COMPANION_ALLOW_ADHOC=1 exec "$SCRIPT_DIR/.install_release.sh"' in source


def test_companion_pet_skill_is_bundled_without_generated_assets():
    assert (BUNDLED_COMPANION_SKILL / "SKILL.md").is_file()
    assert (BUNDLED_COMPANION_SKILL / "scripts" / "companion_pet_assets.py").is_file()
    assert (BUNDLED_COMPANION_SKILL / "references" / "companion-contract.md").is_file()
    forbidden_directories = ["generated", "qa", "work", ".build", "dist"]
    for name in forbidden_directories:
        assert not (BUNDLED_COMPANION_SKILL / name).exists()


if __name__ == "__main__":
    test_release_candidate_uses_one_non_colliding_version_source()
    test_build_uses_silverfire_namespace_and_never_activates_shared_server()
    test_mobile_beta_is_unavailable_in_public_builds_unless_explicitly_authorized()
    test_shared_server_reconfiguration_assets_and_launch_cleanup_are_absent()
    test_plugin_installer_uses_silverfire_namespace_without_daemon_activation()
    test_release_builder_creates_universal_dmg_without_daemon_activation()
    test_release_installer_is_rollback_safe_and_installs_skill()
    test_public_export_is_allowlist_driven_and_rejects_local_material()
    test_public_release_paths_exclude_personal_legacy_namespace()
    test_public_export_rejects_uppercase_and_lowercase_uuid_literals()
    test_public_export_rejects_personal_namespace()
    test_public_readme_is_mac_product_copy_without_mobile_release_notes()
    test_release_contract_uses_signed_https_manifest()
    test_verified_updater_hands_off_to_transactional_installer()
    test_unsigned_release_wraps_transactional_installer_with_explicit_confirmation()
    test_companion_pet_skill_is_bundled_without_generated_assets()
    print("release packaging regression passed")
