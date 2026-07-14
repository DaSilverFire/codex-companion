#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = ROOT.parents[1]
BUILD_SCRIPT = REPOSITORY_ROOT / "scripts" / "build-macos-release.sh"
VERIFY_SCRIPT = REPOSITORY_ROOT / "scripts" / "verify-macos-release.sh"
PUBLISH_SCRIPT = REPOSITORY_ROOT / "scripts" / "publish-macos-release.sh"


def test_build_uses_silverfire_namespace_and_never_activates_shared_server():
    source = BUILD_SCRIPT.read_text()

    assert 'BUNDLE_ID="com.silverfire.codexcompanion"' in source
    assert "--arch arm64" in source
    assert "--arch x86_64" in source
    assert "CodexCompanion-$VERSION-macOS-universal.dmg" in source
    assert "CODEX_COMPANION_CONFIGURE_SHARED_APP_SERVER" not in source
    assert "configure_shared_app_server.sh" not in source
    assert "app-server daemon" not in source
    assert "CODEX_APP_SERVER_USE_LOCAL_DAEMON" not in source


def test_release_verifier_checks_architectures_bundle_and_signature():
    source = VERIFY_SCRIPT.read_text()

    assert "arm64" in source
    assert "x86_64" in source
    assert "com.silverfire.codexcompanion" in source
    assert "codesign --verify" in source


def test_publisher_runs_every_release_gate_before_uploading():
    source = PUBLISH_SCRIPT.read_text()

    assert "audit-release-tree.sh" in source
    assert "build-macos-release.sh" in source
    assert "verify-macos-release.sh" in source
    assert "gh release create" in source


if __name__ == "__main__":
    test_build_uses_silverfire_namespace_and_never_activates_shared_server()
    test_release_verifier_checks_architectures_bundle_and_signature()
    test_publisher_runs_every_release_gate_before_uploading()
    print("release packaging regression passed")
