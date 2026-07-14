from __future__ import annotations

import unittest
import json
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
SKILL_PATH = SKILL_ROOT / "SKILL.md"
SCHEMA_PATH = SKILL_ROOT / "references" / "codex-pet-schema-2026-07-13.json"


class SkillContractTests(unittest.TestCase):
    def test_description_uses_trigger_conditions(self):
        text = SKILL_PATH.read_text(encoding="utf-8")
        description = next(
            line for line in text.splitlines() if line.startswith("description:")
        )
        self.assertTrue(description.startswith("description: Use when "))

    def test_skill_is_schema_aware_and_keeps_extensions_staged(self):
        text = SKILL_PATH.read_text(encoding="utf-8").lower()
        required = (
            "$hatch-pet",
            "8 atlas columns and 11 rows",
            "per-state frame counts",
            "runtime schema",
            "runtime-installable",
            "thinking",
            "talking",
            "four total cat legs",
            "black nose",
            "golden eyes",
            "nearest-neighbor",
            "do not install",
        )
        for phrase in required:
            with self.subTest(phrase=phrase):
                self.assertIn(phrase, text)

        self.assertNotIn("one global frame count", text)
        self.assertNotIn("fixed 16-frame", text)

    def test_bundled_schema_matches_verified_runtime_snapshot(self):
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

        self.assertEqual(schema["schemaVersion"], 2)
        self.assertEqual(schema["atlas"]["columns"], 8)
        self.assertEqual(schema["atlas"]["rows"], 11)
        self.assertEqual(schema["cell"]["width"], 192)
        self.assertEqual(schema["cell"]["height"], 208)
        self.assertEqual(schema["states"]["idle"]["frameCount"], 6)
        self.assertEqual(schema["states"]["running-right"]["frameCount"], 8)
        self.assertFalse(schema["extensionStates"]["thinking"]["runtimeInstallable"])
        self.assertFalse(schema["extensionStates"]["talking"]["runtimeInstallable"])

    def test_skill_forbids_visual_shortcuts_and_live_mutation(self):
        text = SKILL_PATH.read_text(encoding="utf-8").lower()
        required = (
            "do not invent missing pet art",
            "never patch, sign, install, relaunch, or overwrite chatgpt/codex",
            "never overwrite an existing pet package",
            "manual semantic review",
            "frame hashes",
        )
        for phrase in required:
            with self.subTest(phrase=phrase):
                self.assertIn(phrase, text)


if __name__ == "__main__":
    unittest.main()
