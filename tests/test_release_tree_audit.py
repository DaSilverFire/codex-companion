from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
AUDIT_SCRIPT = REPO_ROOT / "scripts" / "audit-release-tree.sh"


class ReleaseTreeAuditTests(unittest.TestCase):
    def run_audit(self, files: dict[str, str]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)

            for relative_path, contents in files.items():
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")

            return subprocess.run(
                [str(AUDIT_SCRIPT), str(root)],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_accepts_clean_release_source(self):
        result = self.run_audit(
            {
                "README.md": "# Clean project\n",
                "Sources/App.swift": "struct App {}\n",
            }
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("release tree audit passed", result.stdout.lower())

    def test_rejects_local_output_and_task_state(self):
        for relative_path in (
            "output/live-capture.png",
            "qa/interaction.mov",
            "session_index.json",
            ".build/debug/CodexCompanion",
        ):
            with self.subTest(relative_path=relative_path):
                result = self.run_audit({relative_path: "local-only\n"})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(relative_path, result.stdout + result.stderr)

    def test_rejects_secrets_and_machine_specific_paths(self):
        samples = (
            "OPENAI_API_" + "KEY=" + "sk-" + "proj-example-secret\n",
            "GITHUB_" + "TOKEN=" + "gho_" + "example_secret\n",
            "path=/" + "Users/example/Library/Application Support/Codex\n",
            "temporary=/var/" + "folders/example/capture.png\n",
        )

        for contents in samples:
            with self.subTest(contents=contents):
                result = self.run_audit({"Sources/Config.txt": contents})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sensitive content", result.stdout.lower() + result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
