#!/usr/bin/env python3
"""Prepare, ingest, review, validate, and preview Companion pet candidate rows."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
from pathlib import Path
from statistics import median
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from PIL import Image, ImageDraw


CELL_WIDTH = 192
CELL_HEIGHT = 208
MAX_GENERATION_COLUMNS = 8
CHROMA_KEY = (0, 255, 255)
BASELINE_Y = 196
MAX_SPRITE_WIDTH = 172
MAX_SPRITE_HEIGHT = 184
DEFAULT_SCHEMA_PATH = (
    Path(__file__).resolve().parents[1]
    / "references"
    / "codex-pet-schema-2026-07-13.json"
)

SEMANTIC_FIELDS = (
    "legsSeparated",
    "limbsComplete",
    "blackNose",
    "goldenEyes",
    "noLabels",
    "noBackground",
    "identityConsistent",
    "anchorScaleConsistent",
    "noExtraObjects",
    "approved",
)

STATE_ACTIONS = {
    "thinking": (
        "Create a quiet thinking loop using only small cat motions: attentive head tilts, "
        "ear turns, natural blinks, a tiny tail-tip shift, and a return to the first pose. "
        "Do not add thought bubbles, dots, punctuation, icons, papers, screens, or props."
    ),
    "talking": (
        "Create a conversational talking loop using small readable mouth shapes, subtle "
        "cheek and ear motion, natural blinks, and a return to the first pose. Keep the body "
        "stable. Do not add speech bubbles, sound marks, text, punctuation, or props."
    ),
}


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="PNG", optimize=False, compress_level=9)


def validate_state_name(state: str) -> str:
    normalized = state.strip().lower()
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", normalized):
        raise ValueError(f"invalid state name: {state!r}")
    return normalized


def parse_states(raw: str) -> List[str]:
    states = [validate_state_name(item) for item in raw.split(",") if item.strip()]
    if not states:
        raise ValueError("at least one state is required")
    if len(states) != len(set(states)):
        raise ValueError("state names must be unique")
    return states


def _positive_int(value: object, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{label} must be a positive integer")
    return value


def generation_grid(frame_count: int) -> Tuple[int, int]:
    frame_count = _positive_int(frame_count, "frame count")
    for columns in range(min(MAX_GENERATION_COLUMNS, frame_count), 0, -1):
        if frame_count % columns == 0:
            return columns, frame_count // columns
    raise AssertionError("every positive frame count is divisible by one")


def load_pet_schema(schema_path: Optional[Path] = None) -> Dict[str, object]:
    path = Path(schema_path or DEFAULT_SCHEMA_PATH).expanduser().resolve()
    try:
        schema = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise FileNotFoundError(f"pet schema not found: {path}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid pet schema JSON: {path}") from error
    if not isinstance(schema, dict):
        raise ValueError(f"pet schema must be a JSON object: {path}")

    cell = schema.get("cell")
    atlas = schema.get("atlas")
    states = schema.get("states")
    extensions = schema.get("extensionStates", {})
    if not isinstance(cell, dict) or not isinstance(atlas, dict):
        raise ValueError("pet schema requires cell and atlas objects")
    if not isinstance(states, dict) or not isinstance(extensions, dict):
        raise ValueError("pet schema states and extensionStates must be objects")
    width = _positive_int(cell.get("width"), "cell.width")
    height = _positive_int(cell.get("height"), "cell.height")
    if (width, height) != (CELL_WIDTH, CELL_HEIGHT):
        raise ValueError(
            f"this pipeline currently supports {CELL_WIDTH}x{CELL_HEIGHT} cells; "
            f"schema declares {width}x{height}"
        )
    columns = _positive_int(atlas.get("columns"), "atlas.columns")
    rows = _positive_int(atlas.get("rows"), "atlas.rows")

    occupied_rows = set()
    for state, raw_spec in states.items():
        validate_state_name(state)
        if not isinstance(raw_spec, dict):
            raise ValueError(f"state {state!r} must be an object")
        row = raw_spec.get("row")
        if not isinstance(row, int) or isinstance(row, bool) or not 0 <= row < rows:
            raise ValueError(f"state {state!r} has invalid row {row!r}")
        if row in occupied_rows:
            raise ValueError(f"pet schema assigns more than one state to row {row}")
        occupied_rows.add(row)
        count = _positive_int(raw_spec.get("frameCount"), f"{state}.frameCount")
        if count > columns:
            raise ValueError(
                f"state {state!r} uses {count} frames but atlas has {columns} columns"
            )
    for state, raw_spec in extensions.items():
        validate_state_name(state)
        if not isinstance(raw_spec, dict):
            raise ValueError(f"extension state {state!r} must be an object")
        count = _positive_int(raw_spec.get("frameCount"), f"{state}.frameCount")
        if count > columns:
            raise ValueError(
                f"extension state {state!r} uses {count} frames but atlas has {columns} columns"
            )
    schema["_path"] = str(path)
    return schema


def _runtime_contract(schema: Mapping[str, object]) -> Dict[str, object]:
    atlas = schema["atlas"]
    cell = schema["cell"]
    states = schema["states"]
    assert isinstance(atlas, dict) and isinstance(cell, dict) and isinstance(states, dict)
    ordered_states = sorted(states, key=lambda name: states[name]["row"])
    return {
        "columns": atlas["columns"],
        "rows": atlas["rows"],
        "cellWidth": cell["width"],
        "cellHeight": cell["height"],
        "states": ordered_states,
        "stateSpecs": {state: states[state] for state in ordered_states},
    }


def _candidate_specs(
    schema: Mapping[str, object], states: Sequence[str]
) -> Dict[str, Dict[str, object]]:
    runtime_states = schema["states"]
    extension_states = schema.get("extensionStates", {})
    assert isinstance(runtime_states, dict) and isinstance(extension_states, dict)
    atlas = schema["atlas"]
    assert isinstance(atlas, dict)
    result: Dict[str, Dict[str, object]] = {}
    for state in states:
        source = extension_states.get(state, runtime_states.get(state))
        if source is None:
            source = {"frameCount": atlas["columns"], "runtimeInstallable": False}
        if not isinstance(source, dict):
            raise ValueError(f"schema entry for {state!r} must be an object")
        frame_count = _positive_int(source.get("frameCount"), f"{state}.frameCount")
        if frame_count > atlas["columns"]:
            raise ValueError(
                f"state {state!r} uses {frame_count} frames but atlas has "
                f"{atlas['columns']} columns"
            )
        columns, rows = generation_grid(frame_count)
        result[state] = {
            "frameCount": frame_count,
            "generationGrid": [columns, rows],
            "runtimeInstallable": bool(source.get("runtimeInstallable", state in runtime_states)),
        }
    return result


def _state_spec(request: Mapping[str, object], state: str) -> Dict[str, object]:
    specs = request.get("candidateStateSpecs")
    if isinstance(specs, dict) and isinstance(specs.get(state), dict):
        return dict(specs[state])
    # Backward compatibility for runs prepared by companion-pet schema version 1.
    frame = request.get("frame", {})
    runtime = request.get("companionRuntime", {})
    count = frame.get("count", runtime.get("columns", 16)) if isinstance(frame, dict) else 16
    count = _positive_int(count, f"{state}.frameCount")
    columns, rows = generation_grid(count)
    return {
        "frameCount": count,
        "generationGrid": [columns, rows],
        "runtimeInstallable": False,
    }


def _runtime_columns(request: Mapping[str, object]) -> int:
    runtime = request.get("companionRuntime")
    if not isinstance(runtime, dict):
        raise ValueError("request is missing companionRuntime")
    return _positive_int(runtime.get("columns"), "companionRuntime.columns")


def parse_hex_color(value: str) -> Tuple[int, int, int]:
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
        raise ValueError(f"invalid color {value!r}; expected #RRGGBB")
    return tuple(int(value[index : index + 2], 16) for index in (1, 3, 5))


def _safe_reset_run(run_dir: Path, force: bool) -> None:
    if not run_dir.exists() or not any(run_dir.iterdir()):
        return
    if not force:
        raise FileExistsError(f"run directory is not empty: {run_dir}")

    marker = run_dir / "request.json"
    try:
        request = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(
            f"refusing to reset unrecognized directory without companion-pet marker: {run_dir}"
        ) from error
    if request.get("createdBy") != "companion-pet":
        raise ValueError(f"refusing to reset directory not owned by companion-pet: {run_dir}")

    for child in run_dir.iterdir():
        if child.is_dir() and not child.is_symlink():
            shutil.rmtree(child)
        else:
            child.unlink()


def create_layout_guide(path: Path, columns: int, rows: int) -> None:
    columns = _positive_int(columns, "guide columns")
    rows = _positive_int(rows, "guide rows")
    width = columns * CELL_WIDTH
    height = rows * CELL_HEIGHT
    image = Image.new("RGBA", (width, height), (*CHROMA_KEY, 255))
    draw = ImageDraw.Draw(image)
    for row in range(rows):
        for column in range(columns):
            left = column * CELL_WIDTH
            top = row * CELL_HEIGHT
            right = left + CELL_WIDTH - 1
            bottom = top + CELL_HEIGHT - 1
            draw.rectangle((left, top, right, bottom), outline=(48, 48, 48, 255), width=1)
            draw.rectangle(
                (left + 18, top + 16, right - 18, bottom - 16),
                outline=(128, 128, 128, 255),
                width=1,
            )
            draw.line(
                (left + CELL_WIDTH // 2, top + 16, left + CELL_WIDTH // 2, bottom - 16),
                fill=(96, 96, 96, 255),
                width=1,
            )
    save_png(image, path)


def build_prompt(
    pet_id: str,
    display_name: str,
    state: str,
    frame_count: int,
    grid_columns: int,
    grid_rows: int,
) -> str:
    action = STATE_ACTIONS.get(
        state,
        f"Create a clear looping `{state}` animation using pose changes only and return to the first pose.",
    )
    row_word = "row" if grid_rows == 1 else "rows"
    return f"""Create a {frame_count}-frame `{state}` animation candidate for the Codex Companion pet `{pet_id}` ({display_name}).

Use every attached Shadow reference as an identity lock and the layout guide only for invisible slot placement. This is a production candidate, not a finished install.

Identity and anatomy:
- Preserve the same compact black cat, head and ear shape, proportions, charcoal palette, outline weight, tail, and face in every frame.
- Show exactly four total cat legs in every frame. Keep all four legs anatomically distinct: no extra, duplicated, merged, hidden-as-one, malformed, or cut-off limbs.
- Keep a black nose in every frame.
- Keep golden eyes with the same shape and placement whenever the eyes are open; blink frames may close the eyelids but may not change eye color.

Animation:
- {action}
- Keep one stable bottom-center anchor and one consistent character scale across all {frame_count} frames.
- Keep the full cat inside each slot with generous transparent-safe padding. No body part may touch or cross a slot boundary.

Layout:
- Output exactly {frame_count} separate full-body poses in a {grid_rows} {row_word} of {grid_columns} arrangement, read left-to-right and then top-to-bottom.
- Use one complete cat pose per invisible equal-size slot. Fill every slot.
- Use a perfectly flat solid #00FFFF cyan chroma-key background only.
- The guide is construction-only. Do not reproduce its boxes, lines, marks, or colors.

Forbidden contamination:
- Use no magenta anywhere, including fringe pixels.
- Include no text, labels, frame numbers, logos, watermarks, punctuation, speech bubbles, thought bubbles, UI, scenery, floor, shadows, detached effects, motion marks, or background objects.
- Include no checkerboard, gradient, glow, blur, antialiasing haze, or cyan inside the cat.
- Do not redesign the cat or introduce accessories.

Style:
- Codex digital-pet sprite style with deliberate hard pixel clusters, a limited palette, flat shading, crisp stepped contours, and no soft gradients or realistic fur.
"""


def prepare_run(
    run_dir: Path,
    pet_id: str,
    display_name: str,
    references: Sequence[Path],
    states: Sequence[str],
    force: bool,
    schema_path: Optional[Path] = None,
) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    pet_id = validate_state_name(pet_id)
    display_name = display_name.strip()
    if not display_name:
        raise ValueError("display_name cannot be empty")
    normalized_states = [validate_state_name(state) for state in states]
    if not normalized_states or len(normalized_states) != len(set(normalized_states)):
        raise ValueError("states must contain unique state names")
    schema = load_pet_schema(schema_path)
    candidate_specs = _candidate_specs(schema, normalized_states)
    runtime_contract = _runtime_contract(schema)

    reference_paths = [Path(path).expanduser().resolve() for path in references]
    if not reference_paths:
        raise ValueError("at least one identity reference is required")
    for reference in reference_paths:
        if not reference.is_file():
            raise FileNotFoundError(f"reference not found: {reference}")

    run_dir.mkdir(parents=True, exist_ok=True)
    _safe_reset_run(run_dir, force)
    for relative in (
        "prompts",
        "references",
        "sources",
        "frames",
        "rows",
        "provenance",
        "qa",
    ):
        (run_dir / relative).mkdir(parents=True, exist_ok=True)

    copied_references = []
    for index, source in enumerate(reference_paths, start=1):
        suffix = source.suffix.lower() if source.suffix else ".png"
        destination = run_dir / "references" / f"identity-{index:02d}{suffix}"
        shutil.copyfile(source, destination)
        copied_references.append(
            {
                "path": destination.relative_to(run_dir).as_posix(),
                "sha256": sha256_path(destination),
            }
        )

    source_schema_path = Path(str(schema.pop("_path")))
    copied_schema_path = run_dir / "references" / "runtime-schema.json"
    shutil.copyfile(source_schema_path, copied_schema_path)
    guide_paths: Dict[Tuple[int, int], Path] = {}
    for spec in candidate_specs.values():
        columns, rows = spec["generationGrid"]
        key = (int(columns), int(rows))
        if key not in guide_paths:
            guide_path = run_dir / "references" / f"layout-guide-{rows}x{columns}.png"
            create_layout_guide(guide_path, int(columns), int(rows))
            guide_paths[key] = guide_path

    installable = all(
        bool(candidate_specs[state]["runtimeInstallable"]) for state in normalized_states
    )

    request: Dict[str, object] = {
        "schemaVersion": 2,
        "createdBy": "companion-pet",
        "pet": {"id": pet_id, "displayName": display_name},
        "frame": {"width": CELL_WIDTH, "height": CELL_HEIGHT},
        "schemaSource": {
            "path": copied_schema_path.relative_to(run_dir).as_posix(),
            "sha256": sha256_path(copied_schema_path),
        },
        "hatchCompatibility": runtime_contract,
        "companionRuntime": runtime_contract,
        "candidateStates": normalized_states,
        "candidateStateSpecs": candidate_specs,
        "candidateRuntimeInstallable": installable,
        "candidateReason": (
            "Each candidate is installable only when the supplied runtime schema explicitly "
            "declares that state. Extension rows remain standalone until runtime support exists."
        ),
        "generationOrder": "row-major",
        "chromaKey": "#00FFFF",
        "references": copied_references,
        "layoutGuides": {
            f"{rows}x{columns}": path.relative_to(run_dir).as_posix()
            for (columns, rows), path in sorted(guide_paths.items())
        },
    }
    write_json(run_dir / "request.json", request)

    jobs = []
    for state in normalized_states:
        spec = candidate_specs[state]
        frame_count = int(spec["frameCount"])
        columns, rows = (int(value) for value in spec["generationGrid"])
        guide_path = guide_paths[(columns, rows)]
        input_images = [
            {"path": item["path"], "role": "pet identity reference"}
            for item in copied_references
        ]
        input_images.append(
            {
                "path": guide_path.relative_to(run_dir).as_posix(),
                "role": (
                    f"layout-only {rows}x{columns} guide; never copy guide pixels"
                ),
            }
        )
        prompt_path = run_dir / "prompts" / f"{state}.md"
        prompt_path.write_text(
            build_prompt(
                pet_id,
                display_name,
                state,
                frame_count,
                columns,
                rows,
            ),
            encoding="utf-8",
        )
        jobs.append(
            {
                "id": state,
                "kind": "companion-extension-row",
                "status": "pending",
                "promptFile": prompt_path.relative_to(run_dir).as_posix(),
                "inputImages": input_images,
                "expectedFrames": frame_count,
                "generationGrid": [columns, rows],
                "outputRow": f"rows/{state}.png",
                "runtimeInstallable": bool(spec["runtimeInstallable"]),
            }
        )
    write_json(
        run_dir / "jobs.json",
        {"schemaVersion": 1, "createdBy": "companion-pet", "jobs": jobs},
    )
    return {"ok": True, "runDir": str(run_dir), "states": normalized_states}


def load_request(run_dir: Path) -> Dict[str, object]:
    path = run_dir / "request.json"
    try:
        request = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise FileNotFoundError(f"missing companion-pet request: {path}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid companion-pet request: {path}") from error
    if request.get("createdBy") != "companion-pet":
        raise ValueError(f"run is not owned by companion-pet: {run_dir}")
    return request


def inspect_pet_package(
    pet_dir: Path, target_schema_path: Optional[Path] = None
) -> Dict[str, object]:
    pet_dir = Path(pet_dir).expanduser().resolve()
    manifest_path = pet_dir / "pet.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise FileNotFoundError(f"missing pet manifest: {manifest_path}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid pet manifest JSON: {manifest_path}") from error
    if not isinstance(manifest, dict):
        raise ValueError(f"pet manifest must be a JSON object: {manifest_path}")

    columns = _positive_int(manifest.get("spriteColumns"), "spriteColumns")
    rows = _positive_int(manifest.get("spriteRows"), "spriteRows")
    spritesheet_value = manifest.get("spritesheetPath", "spritesheet.webp")
    if not isinstance(spritesheet_value, str) or not spritesheet_value.strip():
        raise ValueError("spritesheetPath must be a non-empty relative path")
    relative_sheet = Path(spritesheet_value)
    if relative_sheet.is_absolute() or ".." in relative_sheet.parts:
        raise ValueError("spritesheetPath must stay inside the pet directory")
    spritesheet_path = (pet_dir / relative_sheet).resolve()
    if pet_dir not in spritesheet_path.parents:
        raise ValueError("spritesheetPath escapes the pet directory")
    if not spritesheet_path.is_file():
        raise FileNotFoundError(f"missing pet spritesheet: {spritesheet_path}")
    with Image.open(spritesheet_path) as opened:
        sheet_width, sheet_height = opened.size
    if sheet_width % columns or sheet_height % rows:
        raise ValueError(
            f"spritesheet size {sheet_width}x{sheet_height} is not divisible by "
            f"{columns}x{rows}"
        )
    cell_width = sheet_width // columns
    cell_height = sheet_height // rows

    raw_counts = manifest.get("animationFrameCounts", {})
    if raw_counts is None:
        raw_counts = {}
    if not isinstance(raw_counts, dict):
        raise ValueError("animationFrameCounts must be an object when present")
    animation_counts = {
        validate_state_name(state): _positive_int(count, f"{state}.frameCount")
        for state, count in raw_counts.items()
    }
    if any(count > columns for count in animation_counts.values()):
        raise ValueError("animationFrameCounts cannot exceed spriteColumns")

    target_schema = load_pet_schema(target_schema_path)
    target_runtime = _runtime_contract(target_schema)
    target_states = target_runtime["stateSpecs"]
    assert isinstance(target_states, dict)
    counts_differ = any(
        state in target_states and count != target_states[state]["frameCount"]
        for state, count in animation_counts.items()
    )
    requires_migration = (
        columns != target_runtime["columns"]
        or rows != target_runtime["rows"]
        or cell_width != target_runtime["cellWidth"]
        or cell_height != target_runtime["cellHeight"]
        or counts_differ
    )
    return {
        "ok": True,
        "pet": {
            "id": manifest.get("id"),
            "displayName": manifest.get("displayName"),
        },
        "source": {
            "columns": columns,
            "rows": rows,
            "cellWidth": cell_width,
            "cellHeight": cell_height,
            "animationFrameCounts": animation_counts,
            "manifestSha256": sha256_path(manifest_path),
            "spritesheetSha256": sha256_path(spritesheet_path),
        },
        "target": {
            "columns": target_runtime["columns"],
            "rows": target_runtime["rows"],
            "cellWidth": target_runtime["cellWidth"],
            "cellHeight": target_runtime["cellHeight"],
            "stateSpecs": target_runtime["stateSpecs"],
        },
        "requiresMigration": requires_migration,
        "recommendedReference": relative_sheet.as_posix(),
        "migrationWorkflow": (
            "Use the old spritesheet as an identity and motion reference, prepare against the "
            "target schema, regenerate or normalize each target row, validate, then package to "
            "a new directory. Never overwrite the source pet in place."
        ),
    }


def color_distance(pixel: Tuple[int, int, int], key: Tuple[int, int, int]) -> float:
    return math.sqrt(sum((pixel[index] - key[index]) ** 2 for index in range(3)))


def remove_chroma_binary(
    image: Image.Image, chroma_key: Tuple[int, int, int], threshold: float
) -> Image.Image:
    rgba = image.convert("RGBA")
    output = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
    output_pixels = output.load()
    for y in range(rgba.height):
        for x in range(rgba.width):
            red, green, blue, alpha = rgba.getpixel((x, y))
            if alpha <= 16 or color_distance((red, green, blue), chroma_key) <= threshold:
                continue
            output_pixels[x, y] = (red, green, blue, 255)
    return output


def split_grid(
    image: Image.Image, columns: int, rows: int
) -> List[Image.Image]:
    if columns <= 0 or rows <= 0:
        raise ValueError("grid columns and rows must be positive")
    if image.width % columns or image.height % rows:
        raise ValueError(
            f"source size {image.width}x{image.height} is not divisible by {columns}x{rows}"
        )
    cell_width = image.width // columns
    cell_height = image.height // rows
    return [
        image.crop(
            (
                column * cell_width,
                row * cell_height,
                (column + 1) * cell_width,
                (row + 1) * cell_height,
            )
        )
        for row in range(rows)
        for column in range(columns)
    ]


def _global_scale(crops: Sequence[Image.Image]) -> float:
    widths = [crop.width for crop in crops]
    heights = [crop.height for crop in crops]
    return min(
        MAX_SPRITE_WIDTH / max(widths),
        MAX_SPRITE_HEIGHT / max(heights),
        1.0,
    )


def _place_on_frame(sprite: Image.Image, scale: float) -> Image.Image:
    if scale < 1.0:
        sprite = sprite.resize(
            (
                max(1, round(sprite.width * scale)),
                max(1, round(sprite.height * scale)),
            ),
            Image.Resampling.NEAREST,
        )
    frame = Image.new("RGBA", (CELL_WIDTH, CELL_HEIGHT), (0, 0, 0, 0))
    left = (CELL_WIDTH - sprite.width) // 2
    top = BASELINE_Y - sprite.height
    frame.alpha_composite(sprite, (left, top))
    return frame


def assemble_row(run_dir: Path, state: str) -> Path:
    run_dir = Path(run_dir).expanduser().resolve()
    state = validate_state_name(state)
    request = load_request(run_dir)
    spec = _state_spec(request, state)
    frame_count = int(spec["frameCount"])
    runtime_columns = _runtime_columns(request)
    if frame_count > runtime_columns:
        raise ValueError(
            f"state {state!r} has {frame_count} frames but runtime row has "
            f"{runtime_columns} columns"
        )
    frame_dir = run_dir / "frames" / state
    row = Image.new(
        "RGBA", (CELL_WIDTH * runtime_columns, CELL_HEIGHT), (0, 0, 0, 0)
    )
    for index in range(frame_count):
        frame_path = frame_dir / f"{index:02d}.png"
        if not frame_path.is_file():
            raise FileNotFoundError(f"missing frame: {frame_path}")
        with Image.open(frame_path) as opened:
            frame = opened.convert("RGBA")
        if frame.size != (CELL_WIDTH, CELL_HEIGHT):
            raise ValueError(f"wrong frame size for {frame_path}: {frame.size}")
        row.alpha_composite(frame, (index * CELL_WIDTH, 0))
    output = run_dir / "rows" / f"{state}.png"
    save_png(row, output)
    return output


def ingest_state(
    run_dir: Path,
    state: str,
    source_path: Path,
    grid_columns: Optional[int],
    grid_rows: Optional[int],
    chroma_key: Tuple[int, int, int],
    threshold: float,
) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    source_path = Path(source_path).expanduser().resolve()
    state = validate_state_name(state)
    request = load_request(run_dir)
    if state not in request.get("candidateStates", []):
        raise ValueError(f"state {state!r} is not declared in request.json")
    spec = _state_spec(request, state)
    frame_count = int(spec["frameCount"])
    expected_columns, expected_rows = (int(value) for value in spec["generationGrid"])
    grid_columns = expected_columns if grid_columns is None else grid_columns
    grid_rows = expected_rows if grid_rows is None else grid_rows
    if grid_columns * grid_rows != frame_count:
        raise ValueError(f"generation grid must contain exactly {frame_count} slots")
    if threshold < 0:
        raise ValueError("threshold cannot be negative")
    if not source_path.is_file():
        raise FileNotFoundError(f"source not found: {source_path}")

    copied_source = run_dir / "sources" / f"{state}{source_path.suffix.lower() or '.png'}"
    copied_source.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source_path, copied_source)
    with Image.open(copied_source) as opened:
        source = opened.convert("RGBA")

    slots = split_grid(source, grid_columns, grid_rows)
    keyed = [remove_chroma_binary(slot, chroma_key, threshold) for slot in slots]
    bboxes = [slot.getchannel("A").getbbox() for slot in keyed]
    empty = [index for index, bbox in enumerate(bboxes) if bbox is None]
    if empty:
        raise ValueError(f"empty generated slot(s): {empty}")
    crops = [slot.crop(bbox) for slot, bbox in zip(keyed, bboxes) if bbox is not None]
    scale = _global_scale(crops)

    frame_dir = run_dir / "frames" / state
    if frame_dir.exists():
        shutil.rmtree(frame_dir)
    frame_dir.mkdir(parents=True)
    slot_edge_alpha = []
    for index, (slot, crop) in enumerate(zip(keyed, crops)):
        slot_bbox = slot.getchannel("A").getbbox()
        if slot_bbox is None:
            raise ValueError(f"empty generated slot: {index}")
        touches_edge = (
            slot_bbox[0] == 0
            or slot_bbox[1] == 0
            or slot_bbox[2] == slot.width
            or slot_bbox[3] == slot.height
        )
        if touches_edge:
            slot_edge_alpha.append(index)
        save_png(_place_on_frame(crop, scale), frame_dir / f"{index:02d}.png")

    row_path = assemble_row(run_dir, state)
    provenance: Dict[str, object] = {
        "schemaVersion": 1,
        "state": state,
        "source": copied_source.relative_to(run_dir).as_posix(),
        "sourceSha256": sha256_path(copied_source),
        "sourceGrid": [grid_columns, grid_rows],
        "frameCount": frame_count,
        "chromaKey": "#{:02X}{:02X}{:02X}".format(*chroma_key),
        "chromaThreshold": threshold,
        "globalScale": scale,
        "anchor": {"horizontal": "center", "baselineY": BASELINE_Y},
        "resampling": "nearest-neighbor",
        "sourceSlotsTouchingEdges": slot_edge_alpha,
        "row": row_path.relative_to(run_dir).as_posix(),
    }
    write_json(run_dir / "provenance" / f"{state}.json", provenance)

    jobs_path = run_dir / "jobs.json"
    jobs = json.loads(jobs_path.read_text(encoding="utf-8"))
    for job in jobs.get("jobs", []):
        if job.get("id") == state:
            job["status"] = "ingested"
            job["sourceSha256"] = provenance["sourceSha256"]
            job["provenance"] = f"provenance/{state}.json"
    write_json(jobs_path, jobs)
    return {
        "ok": True,
        "state": state,
        "frameCount": frame_count,
        "row": str(row_path),
        "sourceSlotsTouchingEdges": slot_edge_alpha,
    }


def create_review_template(run_dir: Path) -> Path:
    run_dir = Path(run_dir).expanduser().resolve()
    request = load_request(run_dir)
    states = []
    for state in request.get("candidateStates", []):
        frame_count = int(_state_spec(request, state)["frameCount"])
        frame_records = []
        for index in range(frame_count):
            frame_path = run_dir / "frames" / state / f"{index:02d}.png"
            if not frame_path.is_file():
                raise FileNotFoundError(f"missing frame for review: {frame_path}")
            record: Dict[str, object] = {
                "index": index,
                "path": frame_path.relative_to(run_dir).as_posix(),
                "sha256": sha256_path(frame_path),
                "legCount": None,
                "notes": "",
            }
            record.update({field: None for field in SEMANTIC_FIELDS})
            frame_records.append(record)
        states.append({"state": state, "frames": frame_records})
    output = run_dir / "qa" / "semantic-review.json"
    write_json(
        output,
        {
            "schemaVersion": 1,
            "reviewType": "manual-semantic-frame-review",
            "requestSha256": sha256_path(run_dir / "request.json"),
            "states": states,
        },
    )
    return output


def _visible_magenta(pixel: Tuple[int, int, int, int]) -> bool:
    red, green, blue, alpha = pixel
    return (
        alpha > 16
        and red >= 140
        and blue >= 120
        and green <= 100
        and red + blue - 2 * green >= 150
    )


def _visible_cyan(pixel: Tuple[int, int, int, int]) -> bool:
    red, green, blue, alpha = pixel
    return alpha > 16 and red <= 100 and green >= 150 and blue >= 150


def _golden_pixel(pixel: Tuple[int, int, int, int]) -> bool:
    red, green, blue, alpha = pixel
    return alpha > 16 and red >= 150 and green >= 90 and blue <= 100 and red > green


def _review_index(review: Mapping[str, object]) -> Dict[Tuple[str, int], Mapping[str, object]]:
    result = {}
    for state_record in review.get("states", []):
        if not isinstance(state_record, dict):
            continue
        state = state_record.get("state")
        for frame in state_record.get("frames", []):
            if isinstance(state, str) and isinstance(frame, dict) and isinstance(frame.get("index"), int):
                result[(state, frame["index"])] = frame
    return result


def validate_run(run_dir: Path, require_review: bool = True) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    request = load_request(run_dir)
    errors: List[str] = []
    warnings: List[str] = []
    state_reports = []

    review_path = run_dir / "qa" / "semantic-review.json"
    review_index: Dict[Tuple[str, int], Mapping[str, object]] = {}
    if require_review:
        if not review_path.is_file():
            errors.append("missing_semantic_review")
        else:
            try:
                review = json.loads(review_path.read_text(encoding="utf-8"))
                review_index = _review_index(review)
            except json.JSONDecodeError:
                errors.append("invalid_semantic_review_json")

    for state in request.get("candidateStates", []):
        spec = _state_spec(request, state)
        frame_count = int(spec["frameCount"])
        runtime_columns = _runtime_columns(request)
        row_path = run_dir / "rows" / f"{state}.png"
        if not row_path.is_file():
            errors.append(f"{state}:missing_row")
            continue
        with Image.open(row_path) as opened:
            row = opened.convert("RGBA")
        if row.size != (CELL_WIDTH * runtime_columns, CELL_HEIGHT):
            errors.append(f"{state}:row_size_mismatch")
            continue

        bboxes = []
        frame_hashes = []
        frame_reports = []
        row_frame_mismatch = False
        for index in range(frame_count):
            prefix = f"{state}:{index:02d}"
            frame_path = run_dir / "frames" / state / f"{index:02d}.png"
            if not frame_path.is_file():
                errors.append(f"{prefix}:missing_frame")
                continue
            with Image.open(frame_path) as opened:
                frame = opened.convert("RGBA")
            row_cell = row.crop(
                (index * CELL_WIDTH, 0, (index + 1) * CELL_WIDTH, CELL_HEIGHT)
            )
            if row_cell.tobytes() != frame.tobytes():
                row_frame_mismatch = True
            frame_errors = []
            if frame.size != (CELL_WIDTH, CELL_HEIGHT):
                frame_errors.append("frame_size_mismatch")
                bbox = None
            else:
                bbox = frame.getchannel("A").getbbox()
                if bbox is None:
                    frame_errors.append("empty_frame")
                else:
                    bboxes.append((index, bbox))
                    if bbox[0] == 0 or bbox[1] == 0 or bbox[2] == CELL_WIDTH or bbox[3] == CELL_HEIGHT:
                        frame_errors.append("cell_edge_alpha")

                alpha_values = set(frame.getchannel("A").getdata())
                if not alpha_values.issubset({0, 255}):
                    frame_errors.append("nonbinary_alpha")
                pixels = list(frame.getdata())
                if any(_visible_magenta(pixel) for pixel in pixels):
                    frame_errors.append("visible_magenta")
                if any(_visible_cyan(pixel) for pixel in pixels):
                    frame_errors.append("visible_chroma_key")
                if any(alpha == 0 and (red or green or blue) for red, green, blue, alpha in pixels):
                    frame_errors.append("nonzero_transparent_rgb")
                visible_count = sum(1 for _red, _green, _blue, alpha in pixels if alpha > 16)
                if visible_count > int(CELL_WIDTH * CELL_HEIGHT * 0.65):
                    frame_errors.append("opaque_background_suspected")
                golden_count = sum(1 for pixel in pixels if _golden_pixel(pixel))
                if golden_count == 0:
                    warnings.append(f"{prefix}:no_automated_golden_eye_signal")

            frame_hash = sha256_path(frame_path)
            frame_hashes.append(frame_hash)
            if require_review:
                record = review_index.get((state, index))
                if record is None:
                    frame_errors.append("missing_review_record")
                else:
                    if record.get("sha256") != frame_hash:
                        frame_errors.append("stale_review_hash")
                    if record.get("legCount") != 4:
                        frame_errors.append("leg_count_must_equal_4")
                    for field in SEMANTIC_FIELDS:
                        if record.get(field) is not True:
                            frame_errors.append(f"review_{field}_must_be_true")

            for code in frame_errors:
                errors.append(f"{prefix}:{code}")
            frame_reports.append(
                {
                    "index": index,
                    "sha256": frame_hash,
                    "bbox": list(bbox) if bbox is not None else None,
                    "errors": frame_errors,
                }
            )

        if row_frame_mismatch:
            errors.append(f"{state}:row_frame_mismatch")

        if bboxes:
            baselines = [bbox[3] for _index, bbox in bboxes]
            centers = [(bbox[0] + bbox[2]) / 2 for _index, bbox in bboxes]
            widths = [bbox[2] - bbox[0] for _index, bbox in bboxes]
            heights = [bbox[3] - bbox[1] for _index, bbox in bboxes]
            areas = [width * height for width, height in zip(widths, heights)]
            if max(baselines) - min(baselines) > 2:
                errors.append(f"{state}:baseline_drift")
            if any(abs(center - CELL_WIDTH / 2) > 20 for center in centers):
                errors.append(f"{state}:anchor_drift")
            median_width = median(widths)
            median_height = median(heights)
            median_area = median(areas)
            for index, (width, height, area) in enumerate(zip(widths, heights, areas)):
                if not 0.70 <= width / median_width <= 1.30:
                    errors.append(f"{state}:{index:02d}:width_scale_drift")
                if not 0.75 <= height / median_height <= 1.25:
                    errors.append(f"{state}:{index:02d}:height_scale_drift")
                if not 0.60 <= area / median_area <= 1.55:
                    errors.append(f"{state}:{index:02d}:area_scale_drift")

        unique_frames = len(set(frame_hashes))
        if unique_frames < 4:
            warnings.append(f"{state}:low_frame_variation:{unique_frames}")
        state_reports.append(
            {
                "state": state,
                "rowSha256": sha256_path(row_path),
                "uniqueFrameCount": unique_frames,
                "frames": frame_reports,
            }
        )

    report: Dict[str, object] = {
        "ok": not errors,
        "errors": sorted(set(errors)),
        "warnings": sorted(set(warnings)),
        "states": state_reports,
        "semanticReviewRequired": require_review,
    }
    write_json(run_dir / "qa" / "validation.json", report)
    return report


def checkerboard(size: Tuple[int, int], tile: int = 8) -> Image.Image:
    image = Image.new("RGBA", size, (238, 238, 238, 255))
    draw = ImageDraw.Draw(image)
    for y in range(0, size[1], tile):
        for x in range(0, size[0], tile):
            if (x // tile + y // tile) % 2:
                draw.rectangle(
                    (x, y, min(size[0] - 1, x + tile - 1), min(size[1] - 1, y + tile - 1)),
                    fill=(204, 204, 204, 255),
                )
    return image


def _gif_frame(frame: Image.Image, scale: int) -> Image.Image:
    enlarged = frame.resize(
        (frame.width * scale, frame.height * scale), Image.Resampling.NEAREST
    )
    alpha = enlarged.getchannel("A")
    paletted = enlarged.convert("RGB").convert(
        "P", palette=Image.Palette.ADAPTIVE, colors=255, dither=Image.Dither.NONE
    )
    transparent = alpha.point(lambda value: 255 if value == 0 else 0)
    paletted.paste(255, mask=transparent)
    paletted.info["transparency"] = 255
    return paletted


def render_previews(
    run_dir: Path, scale: int = 2, duration_ms: int = 120
) -> List[Path]:
    run_dir = Path(run_dir).expanduser().resolve()
    request = load_request(run_dir)
    if scale <= 0 or duration_ms <= 0:
        raise ValueError("scale and duration_ms must be positive")
    states = list(request.get("candidateStates", []))
    if not states:
        raise ValueError("request has no candidate states")

    state_layouts = []
    for state in states:
        spec = _state_spec(request, state)
        columns, rows = (int(value) for value in spec["generationGrid"])
        state_layouts.append(
            (state, int(spec["frameCount"]), columns, rows)
        )
    contact_columns = max(columns for _state, _count, columns, _rows in state_layouts)
    contact_rows = sum(rows for _state, _count, _columns, rows in state_layouts)

    contact = checkerboard(
        (
            contact_columns * CELL_WIDTH * scale,
            contact_rows * CELL_HEIGHT * scale,
        ),
        tile=8 * scale,
    )
    outputs: List[Path] = []
    row_offset = 0
    for state, frame_count, generation_columns, generation_rows in state_layouts:
        frames = []
        for index in range(frame_count):
            frame_path = run_dir / "frames" / state / f"{index:02d}.png"
            if not frame_path.is_file():
                raise FileNotFoundError(f"missing frame for preview: {frame_path}")
            with Image.open(frame_path) as opened:
                frame = opened.convert("RGBA")
            frames.append(frame)
            enlarged = frame.resize(
                (CELL_WIDTH * scale, CELL_HEIGHT * scale), Image.Resampling.NEAREST
            )
            column = index % generation_columns
            row = row_offset + index // generation_columns
            contact.alpha_composite(
                enlarged, (column * CELL_WIDTH * scale, row * CELL_HEIGHT * scale)
            )

        gif_frames = [_gif_frame(frame, scale) for frame in frames]
        gif_path = run_dir / "qa" / f"{state}-preview.gif"
        gif_path.parent.mkdir(parents=True, exist_ok=True)
        gif_frames[0].save(
            gif_path,
            format="GIF",
            save_all=True,
            append_images=gif_frames[1:],
            duration=duration_ms,
            loop=0,
            disposal=2,
            transparency=255,
            optimize=False,
        )
        outputs.append(gif_path)
        row_offset += generation_rows

    contact_path = run_dir / "qa" / "contact-sheet.png"
    save_png(contact, contact_path)
    outputs.insert(0, contact_path)
    return outputs


def _print(value: Mapping[str, object]) -> None:
    print(json.dumps(value, allow_nan=False, indent=2, sort_keys=True))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="Create a deterministic candidate run.")
    prepare.add_argument("--run-dir", required=True)
    prepare.add_argument("--pet-id", default="shadow")
    prepare.add_argument("--display-name", default="Shadow")
    prepare.add_argument("--reference", action="append", required=True)
    prepare.add_argument("--states", default="thinking,talking")
    prepare.add_argument(
        "--schema",
        help=(
            "Runtime pet schema JSON. Defaults to the bundled inspected ChatGPT schema; "
            "pass a pet or legacy schema explicitly when migrating an older package."
        ),
    )
    prepare.add_argument("--force", action="store_true")

    ingest = subparsers.add_parser(
        "ingest", help="Split a generated source using the prepared state's frame schema."
    )
    ingest.add_argument("--run-dir", required=True)
    ingest.add_argument("--state", required=True)
    ingest.add_argument("--source", required=True)
    ingest.add_argument("--grid-columns", type=int)
    ingest.add_argument("--grid-rows", type=int)
    ingest.add_argument("--chroma-key", default="#00FFFF")
    ingest.add_argument("--threshold", type=float, default=48.0)

    review = subparsers.add_parser("review-template", help="Create a hash-bound semantic review file.")
    review.add_argument("--run-dir", required=True)

    validate = subparsers.add_parser("validate", help="Validate rows and semantic review records.")
    validate.add_argument("--run-dir", required=True)
    validate.add_argument("--allow-unreviewed", action="store_true")

    preview = subparsers.add_parser("preview", help="Create nearest-neighbor contact and GIF previews.")
    preview.add_argument("--run-dir", required=True)
    preview.add_argument("--scale", type=int, default=2)
    preview.add_argument("--duration-ms", type=int, default=120)

    inspect_pet = subparsers.add_parser(
        "inspect-pet",
        help="Audit an existing pet package and report migration to the target schema.",
    )
    inspect_pet.add_argument("--pet-dir", required=True)
    inspect_pet.add_argument(
        "--target-schema",
        help="Target schema JSON; defaults to the bundled current ChatGPT snapshot.",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "prepare":
        result = prepare_run(
            Path(args.run_dir),
            args.pet_id,
            args.display_name,
            [Path(path) for path in args.reference],
            parse_states(args.states),
            args.force,
            Path(args.schema) if args.schema else None,
        )
    elif args.command == "ingest":
        result = ingest_state(
            Path(args.run_dir),
            args.state,
            Path(args.source),
            args.grid_columns,
            args.grid_rows,
            parse_hex_color(args.chroma_key),
            args.threshold,
        )
    elif args.command == "review-template":
        result = {"ok": True, "review": str(create_review_template(Path(args.run_dir)))}
    elif args.command == "validate":
        result = validate_run(Path(args.run_dir), require_review=not args.allow_unreviewed)
    elif args.command == "preview":
        paths = render_previews(Path(args.run_dir), args.scale, args.duration_ms)
        result = {"ok": True, "outputs": [str(path) for path in paths]}
    elif args.command == "inspect-pet":
        result = inspect_pet_package(
            Path(args.pet_dir),
            Path(args.target_schema) if args.target_schema else None,
        )
    else:
        raise RuntimeError(f"unsupported command: {args.command}")
    _print(result)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
