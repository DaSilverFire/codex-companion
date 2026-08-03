from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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
    image = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse((10, 4, 38, 34), fill=(45, 42, 50, 255))
    draw.rectangle((15, 30, 33, 43), fill=(45, 42, 50, 255))
    draw.point((20, 17), fill=(224, 170, 45, 255))
    draw.point((28, 17), fill=(224, 170, 45, 255))
    draw.point((24, 23), fill=(0, 0, 0, 255))
    image.save(path)


def make_grid_source(path: Path, frame_count: int, columns: int) -> None:
    cell_width = 40
    cell_height = 44
    rows = (frame_count + columns - 1) // columns
    image = Image.new(
        "RGBA",
        (cell_width * columns, cell_height * rows),
        (0, 255, 255, 255),
    )
    draw = ImageDraw.Draw(image)
    for index in range(frame_count):
        column = index % columns
        row = index // columns
        left = column * cell_width
        top = row * cell_height
        wobble = index % 4
        draw.ellipse(
            (left + 10 - wobble, top + 5, left + 30 + wobble, top + 28),
            fill=(45, 42, 50, 255),
        )
        draw.rectangle(
            (left + 13, top + 26, left + 27, top + 39),
            fill=(45, 42, 50, 255),
        )
        draw.point((left + 16, top + 15), fill=(224, 170, 45, 255))
        draw.point((left + 24, top + 15), fill=(224, 170, 45, 255))
        draw.point((left + 20, top + 20), fill=(0, 0, 0, 255))
        draw.point((left + 14 + wobble, top + 34), fill=(55 + index, 50, 60, 255))
    image.save(path)


def approve_review(path: Path) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    self_profile = data.get("semanticProfile")
    if self_profile != "mobile-portrait-medallion":
        raise AssertionError(f"unexpected mobile semantic profile: {self_profile}")
    for state in data["states"]:
        for frame in state["frames"]:
            frame.update(
                {
                    "cropCompositionCorrect": True,
                    "anatomyCorrect": True,
                    "identityDetailsPreserved": True,
                    "noUnexpectedBodyParts": True,
                    "noLabels": True,
                    "noBackground": True,
                    "identityConsistent": True,
                    "anchorScaleConsistent": True,
                    "noExtraObjects": True,
                    "approved": True,
                }
            )
    path.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


class MobilePresencePetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module()

    def test_binary_chroma_removal_neutralizes_cyan_edge_spill(self):
        source = Image.new("RGBA", (3, 1), (0, 0, 0, 0))
        source.putpixel((0, 0), (0, 255, 255, 255))
        source.putpixel((1, 0), (4, 84, 92, 255))
        source.putpixel((2, 0), (45, 42, 50, 255))

        result = self.module.remove_chroma_binary(
            source,
            (0, 255, 255),
            threshold=110,
        )

        self.assertEqual(result.getpixel((0, 0)), (0, 0, 0, 0))
        edge = result.getpixel((1, 0))
        self.assertEqual(edge[3], 255)
        self.assertLessEqual(abs(edge[1] - edge[0]), 8)
        self.assertLessEqual(abs(edge[2] - edge[0]), 8)
        self.assertEqual(result.getpixel((2, 0)), (45, 42, 50, 255))

    def prepare_reviewed_run(
        self,
        root: Path,
        states: list[tuple[str, int]] | None = None,
        cell_size: tuple[int, int] = (144, 144),
    ) -> Path:
        states = states or [("idle", 4), ("thinking", 3), ("talking", 2)]
        reference = root / "nova.png"
        run_dir = root / "run"
        make_reference(reference)
        self.module.prepare_mobile_presence_run(
            run_dir=run_dir,
            pet_id="nova",
            display_name="Nova",
            references=[reference],
            states=states,
            cell_width=cell_size[0],
            cell_height=cell_size[1],
            force=False,
        )
        request = json.loads((run_dir / "request.json").read_text(encoding="utf-8"))
        for state, frame_count in states:
            columns, rows = request["candidateStateSpecs"][state]["generationGrid"]
            source = root / f"{state}.png"
            make_grid_source(source, frame_count=frame_count, columns=columns)
            self.module.ingest_state(
                run_dir,
                state,
                source,
                columns,
                rows,
                (0, 255, 255),
                8,
            )
        review = self.module.create_review_template(run_dir)
        approve_review(review)
        return run_dir

    def rebind_review_to_request(self, run_dir: Path) -> None:
        review_path = run_dir / "qa" / "semantic-review.json"
        review = json.loads(review_path.read_text(encoding="utf-8"))
        review["requestSha256"] = sha256(run_dir / "request.json")
        review_path.write_text(
            json.dumps(review, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def test_prepare_mobile_presence_records_variable_geometry_and_cli_state_specs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "nova.png"
            run_dir = root / "run"
            make_reference(reference)

            result = self.module.prepare_mobile_presence_run(
                run_dir=run_dir,
                pet_id="nova",
                display_name="Nova",
                references=[reference],
                states=[("idle", 12), ("thinking", 10), ("talking", 8)],
                cell_width=144,
                cell_height=144,
                force=False,
            )

            self.assertTrue(result["ok"])
            request = json.loads((run_dir / "request.json").read_text(encoding="utf-8"))
            self.assertEqual(request["runKind"], "mobile-presence")
            self.assertEqual(
                request["semanticProfile"],
                "mobile-portrait-medallion",
            )
            self.assertEqual(request["frame"], {"width": 144, "height": 144})
            self.assertEqual(request["mobilePresence"]["columns"], 12)
            self.assertEqual(request["mobilePresence"]["rows"], 3)
            self.assertEqual(
                [request["candidateStateSpecs"][state]["row"] for state in request["candidateStates"]],
                [0, 1, 2],
            )
            for state, frame_count in (("idle", 12), ("thinking", 10), ("talking", 8)):
                spec = request["candidateStateSpecs"][state]
                self.assertEqual(spec["frameCount"], frame_count)
                self.assertEqual(len(spec["frameDurationsMilliseconds"]), frame_count)
                self.assertLess(spec["posterFrame"], frame_count)
            self.assertEqual(
                request["candidateStateSpecs"]["idle"]["generationGrid"],
                [4, 3],
            )
            idle_durations = request["candidateStateSpecs"]["idle"][
                "frameDurationsMilliseconds"
            ]
            self.assertGreater(idle_durations[0], 500)
            self.assertLess(min(idle_durations), 120)
            self.assertGreater(
                len(
                    set(
                        request["candidateStateSpecs"]["talking"][
                            "frameDurationsMilliseconds"
                        ]
                    )
                ),
                1,
            )

            parsed = self.module.build_parser().parse_args(
                [
                    "prepare-mobile-presence",
                    "--run-dir",
                    str(root / "cli-run"),
                    "--reference",
                    str(reference),
                    "--state",
                    "idle=12",
                    "--state",
                    "thinking=10",
                    "--state",
                    "talking=8",
                    "--cell-width",
                    "144",
                    "--cell-height",
                    "144",
                ]
            )
            self.assertEqual(parsed.state, ["idle=12", "thinking=10", "talking=8"])

            idle_prompt = (run_dir / "prompts" / "idle.md").read_text(encoding="utf-8")
            self.assertIn("portrait medallion", idle_prompt.lower())
            self.assertIn("reference-appropriate", idle_prompt.lower())
            self.assertIn("equivalent focal crop", idle_prompt.lower())
            self.assertNotIn("four total cat legs", idle_prompt.lower())
            self.assertNotIn("full-body poses", idle_prompt.lower())
            self.assertNotIn("tail-tip shift", idle_prompt.lower())
            self.assertNotIn("all four paws", idle_prompt.lower())
            self.assertNotIn("black nose", idle_prompt.lower())
            self.assertNotIn("golden eyes", idle_prompt.lower())
            self.assertIn("avoid whole-portrait shifts", idle_prompt.lower())

            thinking_prompt = (
                run_dir / "prompts" / "thinking.md"
            ).read_text(encoding="utf-8")
            self.assertNotIn("tail-tip shift", thinking_prompt.lower())
            self.assertIn("stable portrait anchor", thinking_prompt.lower())

    def test_mobile_review_template_uses_portrait_anatomy_instead_of_desktop_legs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            review = json.loads(
                (run_dir / "qa" / "semantic-review.json").read_text(encoding="utf-8")
            )

            self.assertEqual(review["semanticProfile"], "mobile-portrait-medallion")
            frame = review["states"][0]["frames"][0]
            self.assertTrue(frame["cropCompositionCorrect"])
            self.assertTrue(frame["anatomyCorrect"])
            self.assertTrue(frame["identityDetailsPreserved"])
            self.assertTrue(frame["noUnexpectedBodyParts"])
            self.assertNotIn("legCount", frame)
            self.assertNotIn("legsSeparated", frame)
            self.assertNotIn("subjectCount", frame)
            self.assertNotIn("earCount", frame)
            self.assertNotIn("visibleFrontPawCount", frame)

    def test_prepare_mobile_presence_rejects_unsafe_counts_cells_and_states(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "nova.png"
            make_reference(reference)
            invalid_cases = [
                ([('idle', 0), ('thinking', 2), ('talking', 2)], 144, 144),
                ([('idle', 33), ('thinking', 2), ('talking', 2)], 144, 144),
                ([('idle', 2), ('thinking', 2), ('talking', 2)], 257, 144),
                ([('idle', 2), ('thinking', 2), ('talking', 2)], 144, 257),
                ([('idle', 2), ('thinking', 2), ('review', 2)], 144, 144),
                ([('idle', 2), ('idle', 2), ('talking', 2)], 144, 144),
            ]
            for index, (states, width, height) in enumerate(invalid_cases):
                with self.subTest(index=index):
                    with self.assertRaises(ValueError):
                        self.module.prepare_mobile_presence_run(
                            run_dir=root / f"run-{index}",
                            pet_id="nova",
                            display_name="Nova",
                            references=[reference],
                            states=states,
                            cell_width=width,
                            cell_height=height,
                            force=False,
                        )

    def test_package_mobile_presence_writes_exact_hashes_transparent_pngs_and_content_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            output_dir = root / "package"

            result = self.module.package_mobile_presence(run_dir, output_dir)

            self.assertTrue(result["ok"])
            manifest_path = output_dir / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["schemaVersion"], 1)
            self.assertEqual(manifest["atlas"]["columns"], 4)
            self.assertEqual(manifest["atlas"]["rows"], 3)
            self.assertEqual(manifest["atlas"]["cellWidth"], 144)
            self.assertEqual(manifest["atlas"]["cellHeight"], 144)
            self.assertEqual(manifest["atlas"]["file"]["sha256"], sha256(output_dir / "atlas.png"))
            self.assertEqual(manifest["thumbnail"]["sha256"], sha256(output_dir / "thumbnail.png"))
            self.assertEqual(
                manifest["contentHash"],
                self.module.mobile_presence_content_hash(manifest),
            )
            for name in ("atlas.png", "thumbnail.png"):
                with Image.open(output_dir / name) as image:
                    self.assertEqual(image.format, "PNG")
                    self.assertIn("A", image.getbands())
                    self.assertEqual(image.getchannel("A").getextrema()[0], 0)
            self.assertTrue(self.module.validate_mobile_presence_package(output_dir)["ok"])

    def test_package_mobile_presence_rejects_stale_review_duration_mismatch_and_bad_row(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            frame_path = run_dir / "frames" / "thinking" / "00.png"
            with Image.open(frame_path) as opened:
                changed = opened.convert("RGBA")
            changed.putpixel((1, 1), (1, 1, 1, 255))
            changed.save(frame_path)
            with self.assertRaisesRegex(ValueError, "stale_review_hash"):
                self.module.package_mobile_presence(run_dir, root / "stale")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            request_path = run_dir / "request.json"
            request = json.loads(request_path.read_text(encoding="utf-8"))
            request["candidateStateSpecs"]["thinking"]["frameDurationsMilliseconds"] = [100]
            request_path.write_text(json.dumps(request, indent=2, sort_keys=True) + "\n")
            self.rebind_review_to_request(run_dir)
            with self.assertRaisesRegex(ValueError, "duration"):
                self.module.package_mobile_presence(run_dir, root / "bad-duration")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            request_path = run_dir / "request.json"
            request = json.loads(request_path.read_text(encoding="utf-8"))
            request["candidateStateSpecs"]["talking"]["row"] = 3
            request_path.write_text(json.dumps(request, indent=2, sort_keys=True) + "\n")
            self.rebind_review_to_request(run_dir)
            with self.assertRaisesRegex(ValueError, "row"):
                self.module.package_mobile_presence(run_dir, root / "bad-row")

    def test_package_mobile_presence_refuses_overwrite_and_enforces_size_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            output_dir = root / "package"
            self.module.package_mobile_presence(run_dir, output_dir)
            with self.assertRaises(FileExistsError):
                self.module.package_mobile_presence(run_dir, output_dir)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_dir = self.prepare_reviewed_run(root)
            with mock.patch.object(self.module, "MAX_MOBILE_PACKAGE_BYTES", 128):
                with self.assertRaisesRegex(ValueError, "8 MiB|size limit"):
                    self.module.package_mobile_presence(run_dir, root / "too-large")


if __name__ == "__main__":
    unittest.main()
