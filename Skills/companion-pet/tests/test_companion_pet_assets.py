from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw


SKILL_DIR = Path(__file__).resolve().parents[1]
SCRIPT_PATH = SKILL_DIR / "scripts" / "companion_pet_assets.py"


def load_module():
    spec = importlib.util.spec_from_file_location("companion_pet_assets", SCRIPT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {SCRIPT_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_reference(path: Path) -> None:
    image = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse((8, 4, 24, 23), fill=(44, 41, 48, 255))
    draw.point((13, 11), fill=(224, 170, 45, 255))
    draw.point((19, 11), fill=(224, 170, 45, 255))
    draw.point((16, 15), fill=(0, 0, 0, 255))
    image.save(path)


def make_grid_source(
    path: Path,
    key=(0, 255, 255, 255),
    frame_count: int = 8,
    columns: int = 8,
) -> None:
    cell_width = 48
    cell_height = 52
    rows = (frame_count + columns - 1) // columns
    image = Image.new("RGBA", (cell_width * columns, cell_height * rows), key)
    draw = ImageDraw.Draw(image)
    for index in range(frame_count):
        column = index % columns
        row = index // columns
        left = column * cell_width
        top = row * cell_height
        wobble = index % 3
        draw.ellipse(
            (left + 15 - wobble, top + 7, left + 33 + wobble, top + 31),
            fill=(44, 41, 48, 255),
        )
        draw.rectangle(
            (left + 18, top + 29, left + 30, top + 43),
            fill=(44, 41, 48, 255),
        )
        draw.point((left + 20, top + 16), fill=(224, 170, 45, 255))
        draw.point((left + 28, top + 16), fill=(224, 170, 45, 255))
        draw.point((left + 24, top + 21), fill=(0, 0, 0, 255))
    image.save(path)


def approve_review(path: Path) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    for row in data["states"]:
        for frame in row["frames"]:
            frame.update(
                {
                    "legCount": 4,
                    "legsSeparated": True,
                    "limbsComplete": True,
                    "blackNose": True,
                    "goldenEyes": True,
                    "noLabels": True,
                    "noBackground": True,
                    "identityConsistent": True,
                    "anchorScaleConsistent": True,
                    "noExtraObjects": True,
                    "approved": True,
                }
            )
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


class CompanionPetAssetsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def test_prepare_defaults_to_current_codex_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            run_dir = root / "run"
            make_reference(reference)

            self.module.prepare_run(
                run_dir=run_dir,
                pet_id="shadow",
                display_name="Shadow",
                references=[reference],
                states=["thinking", "talking"],
                force=False,
            )
            first_hashes = {
                relative: sha256(run_dir / relative)
                for relative in (
                    "request.json",
                    "jobs.json",
                    "prompts/thinking.md",
                    "prompts/talking.md",
                    "references/layout-guide-1x8.png",
                )
            }
            request = json.loads((run_dir / "request.json").read_text(encoding="utf-8"))
            self.assertEqual(request["hatchCompatibility"]["columns"], 8)
            self.assertEqual(request["hatchCompatibility"]["rows"], 11)
            self.assertEqual(request["companionRuntime"]["columns"], 8)
            self.assertEqual(request["companionRuntime"]["rows"], 11)
            self.assertEqual(request["candidateStates"], ["thinking", "talking"])
            self.assertEqual(request["candidateStateSpecs"]["thinking"]["frameCount"], 8)
            self.assertEqual(request["candidateStateSpecs"]["thinking"]["generationGrid"], [8, 1])
            self.assertFalse(request["candidateRuntimeInstallable"])

            self.module.prepare_run(
                run_dir=run_dir,
                pet_id="shadow",
                display_name="Shadow",
                references=[reference],
                states=["thinking", "talking"],
                force=True,
            )
            second_hashes = {
                relative: sha256(run_dir / relative) for relative in first_hashes
            }
            self.assertEqual(first_hashes, second_hashes)

            prompts = "\n".join(
                (run_dir / "prompts" / f"{state}.md").read_text(encoding="utf-8")
                for state in ("thinking", "talking")
            )
            for phrase in (
                "exactly four total cat legs",
                "black nose",
                "golden eyes",
                "no magenta",
                "no text",
                "1 row of 8",
            ):
                self.assertIn(phrase, prompts.lower())

    def test_prepare_accepts_legacy_16_frame_schema_without_hardcoding_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            schema = root / "legacy-schema.json"
            run_dir = root / "run"
            make_reference(reference)
            schema.write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "source": {"kind": "legacy-companion-manifest"},
                        "cell": {"width": 192, "height": 208},
                        "atlas": {"columns": 16, "rows": 10},
                        "states": {
                            "idle": {"row": 0, "frameCount": 16},
                            "goal-complete": {"row": 9, "frameCount": 16},
                        },
                        "extensionStates": {
                            "thinking": {"frameCount": 16, "runtimeInstallable": False},
                        },
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )

            self.module.prepare_run(
                run_dir=run_dir,
                pet_id="shadow",
                display_name="Shadow",
                references=[reference],
                states=["thinking"],
                force=False,
                schema_path=schema,
            )

            request = json.loads((run_dir / "request.json").read_text(encoding="utf-8"))
            self.assertEqual(request["companionRuntime"]["columns"], 16)
            self.assertEqual(request["candidateStateSpecs"]["thinking"]["frameCount"], 16)
            self.assertEqual(request["candidateStateSpecs"]["thinking"]["generationGrid"], [8, 2])
            self.assertTrue((run_dir / "references" / "layout-guide-2x8.png").is_file())
            prompt = (run_dir / "prompts" / "thinking.md").read_text(encoding="utf-8")
            self.assertIn("exactly 16 separate full-body poses", prompt)
            self.assertIn("2 rows of 8", prompt)

    def test_six_frame_state_pads_runtime_row_to_eight_columns(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            source = root / "thinking-source.png"
            schema = root / "schema.json"
            run_dir = root / "run"
            make_reference(reference)
            make_grid_source(source, frame_count=6, columns=6)
            schema.write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "cell": {"width": 192, "height": 208},
                        "atlas": {"columns": 8, "rows": 11},
                        "states": {"idle": {"row": 0, "frameCount": 6}},
                        "extensionStates": {
                            "thinking": {"frameCount": 6, "runtimeInstallable": False},
                        },
                    }
                ),
                encoding="utf-8",
            )
            self.module.prepare_run(
                run_dir,
                "shadow",
                "Shadow",
                [reference],
                ["thinking"],
                False,
                schema_path=schema,
            )
            result = self.module.ingest_state(
                run_dir, "thinking", source, 6, 1, (0, 255, 255), 8
            )

            self.assertEqual(result["frameCount"], 6)
            with Image.open(run_dir / "rows" / "thinking.png") as row:
                self.assertEqual(row.size, (1536, 208))
                self.assertIsNone(row.crop((6 * 192, 0, 8 * 192, 208)).getbbox())

    def test_inspect_pet_reports_legacy_package_migration_to_current_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            pet_dir = Path(temporary) / "legacy-shadow"
            pet_dir.mkdir()
            manifest = {
                "id": "legacy-shadow",
                "displayName": "Legacy Shadow",
                "spritesheetPath": "spritesheet.webp",
                "spriteColumns": 32,
                "spriteRows": 10,
                "animationFrameCounts": {
                    "idle": 32,
                    "running-right": 32,
                    "running-left": 32,
                    "waving": 32,
                    "jumping": 32,
                    "failed": 32,
                    "waiting": 32,
                    "running": 32,
                    "review": 32,
                    "goal-complete": 32,
                },
            }
            (pet_dir / "pet.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            Image.new("RGBA", (32 * 192, 10 * 208), (0, 0, 0, 0)).save(
                pet_dir / "spritesheet.webp", format="WEBP", lossless=True
            )

            report = self.module.inspect_pet_package(pet_dir)

            self.assertTrue(report["ok"])
            self.assertTrue(report["requiresMigration"])
            self.assertEqual(report["source"]["columns"], 32)
            self.assertEqual(report["source"]["rows"], 10)
            self.assertEqual(report["target"]["columns"], 8)
            self.assertEqual(report["target"]["rows"], 11)
            self.assertIn("goal-complete", report["source"]["animationFrameCounts"])
            self.assertEqual(report["recommendedReference"], "spritesheet.webp")

    def test_ingest_uses_one_global_scale_binary_alpha_and_stable_baseline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            source = root / "thinking-source.png"
            run_dir = root / "run"
            make_reference(reference)
            make_grid_source(source)
            self.module.prepare_run(
                run_dir, "shadow", "Shadow", [reference], ["thinking"], False
            )

            result = self.module.ingest_state(
                run_dir=run_dir,
                state="thinking",
                source_path=source,
                grid_columns=8,
                grid_rows=1,
                chroma_key=(0, 255, 255),
                threshold=8,
            )

            row_path = run_dir / "rows" / "thinking.png"
            self.assertEqual(result["frameCount"], 8)
            with Image.open(row_path) as row:
                self.assertEqual(row.size, (1536, 208))
                self.assertEqual(set(row.getchannel("A").getdata()) - {0, 255}, set())
                baselines = []
                for index in range(8):
                    frame = row.crop((index * 192, 0, (index + 1) * 192, 208))
                    bbox = frame.getchannel("A").getbbox()
                    self.assertIsNotNone(bbox)
                    baselines.append(bbox[3])
                self.assertEqual(len(set(baselines)), 1)
            provenance = json.loads(
                (run_dir / "provenance" / "thinking.json").read_text(encoding="utf-8")
            )
            self.assertEqual(provenance["resampling"], "nearest-neighbor")
            self.assertEqual(provenance["sourceSha256"], sha256(source))

    def test_review_gate_rejects_wrong_leg_count_and_pixel_contamination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            source = root / "thinking-source.png"
            run_dir = root / "run"
            make_reference(reference)
            make_grid_source(source)
            self.module.prepare_run(
                run_dir, "shadow", "Shadow", [reference], ["thinking"], False
            )
            self.module.ingest_state(
                run_dir, "thinking", source, 8, 1, (0, 255, 255), 8
            )
            review_path = self.module.create_review_template(run_dir)
            approve_review(review_path)

            review = json.loads(review_path.read_text(encoding="utf-8"))
            review["states"][0]["frames"][3]["legCount"] = 5
            review_path.write_text(
                json.dumps(review, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            report = self.module.validate_run(run_dir, require_review=True)
            self.assertIn("thinking:03:leg_count_must_equal_4", report["errors"])

            review["states"][0]["frames"][3]["legCount"] = 4
            frame_path = run_dir / "frames" / "thinking" / "03.png"
            with Image.open(frame_path) as opened:
                frame = opened.convert("RGBA")
            frame.putpixel((96, 100), (255, 0, 255, 255))
            frame.save(frame_path)
            self.module.assemble_row(run_dir, "thinking")
            report = self.module.validate_run(run_dir, require_review=False)
            self.assertIn("thinking:03:visible_magenta", report["errors"])

    def test_preview_outputs_are_byte_deterministic_and_nearest_scaled(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            source = root / "talking-source.png"
            run_dir = root / "run"
            make_reference(reference)
            make_grid_source(source)
            self.module.prepare_run(
                run_dir, "shadow", "Shadow", [reference], ["talking"], False
            )
            self.module.ingest_state(
                run_dir, "talking", source, 8, 1, (0, 255, 255), 8
            )

            first = self.module.render_previews(run_dir, scale=2, duration_ms=100)
            first_hashes = {path.name: sha256(path) for path in first}
            second = self.module.render_previews(run_dir, scale=2, duration_ms=100)
            second_hashes = {path.name: sha256(path) for path in second}
            self.assertEqual(first_hashes, second_hashes)

            frame_path = run_dir / "frames" / "talking" / "00.png"
            preview_path = run_dir / "qa" / "talking-preview.gif"
            with Image.open(frame_path) as frame, Image.open(preview_path) as preview:
                source_pixel = frame.convert("RGBA").getpixel((96, 100))
                preview.seek(0)
                enlarged = preview.convert("RGBA")
                block = {
                    enlarged.getpixel((192 + dx, 200 + dy)) for dx in range(2) for dy in range(2)
                }
                self.assertEqual(block, {source_pixel})

    def test_validation_rejects_row_that_does_not_match_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "shadow.png"
            source = root / "thinking-source.png"
            run_dir = root / "run"
            make_reference(reference)
            make_grid_source(source)
            self.module.prepare_run(
                run_dir, "shadow", "Shadow", [reference], ["thinking"], False
            )
            self.module.ingest_state(
                run_dir, "thinking", source, 8, 1, (0, 255, 255), 8
            )

            row_path = run_dir / "rows" / "thinking.png"
            with Image.open(row_path) as opened:
                row = opened.convert("RGBA")
            row.putpixel((96, 100), (255, 0, 255, 255))
            row.save(row_path)

            report = self.module.validate_run(run_dir, require_review=False)

            self.assertIn("thinking:row_frame_mismatch", report["errors"])


if __name__ == "__main__":
    unittest.main()
