#!/usr/bin/env python3
"""Prepare, ingest, review, validate, and preview Companion pet candidate rows."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import tempfile
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
MOBILE_PRESENCE_STATES = ("idle", "thinking", "talking")
MAX_MOBILE_FRAME_COUNT = 32
MAX_MOBILE_CELL_DIMENSION = 256
MAX_MOBILE_ATLAS_DIMENSION = 8192
MAX_MOBILE_PACKAGE_BYTES = 8 * 1024 * 1024
MOBILE_STATE_DURATION_MS = {
    "idle": 180,
    "thinking": 180,
    "talking": 120,
}
DEFAULT_SCHEMA_PATH = (
    Path(__file__).resolve().parents[1]
    / "references"
    / "codex-pet-schema-2026-07-13.json"
)

SEMANTIC_FIELDS = (
    "anatomyCorrect",
    "limbsComplete",
    "identityDetailsPreserved",
    "noLabels",
    "noBackground",
    "identityConsistent",
    "anchorScaleConsistent",
    "noExtraObjects",
    "approved",
)

MOBILE_PORTRAIT_SEMANTIC_PROFILE = "mobile-portrait-medallion"
MOBILE_PORTRAIT_SEMANTIC_FIELDS = (
    "cropCompositionCorrect",
    "anatomyCorrect",
    "identityDetailsPreserved",
    "noUnexpectedBodyParts",
    "noLabels",
    "noBackground",
    "identityConsistent",
    "anchorScaleConsistent",
    "noExtraObjects",
    "approved",
)

STATE_ACTIONS = {
    "idle": (
        "Create a centered idle loop with subtle breathing, restrained reference-appropriate "
        "secondary motion, and a readable multi-frame blink when the character has eyelids. "
        "Keep the character grounded in its native resting pose and return cleanly to the start."
    ),
    "goal-complete": (
        "Create a joyful but grounded celebration loop using identity-appropriate posture and "
        "expression changes, a small rise or proud turn when anatomically natural, and a clean "
        "return to the first pose. Do not add confetti, stars, trophies, text, or new props."
    ),
    "thinking": (
        "Create a quiet thinking loop using small identity-appropriate focus, pose, gaze, "
        "expression, or secondary-motion changes and a clean return to the first pose. "
        "Do not add thought bubbles, dots, punctuation, icons, papers, screens, or props."
    ),
    "talking": (
        "Create a conversational talking loop using small readable mouth shapes or another "
        "reference-appropriate communication cue, subtle expression motion, and a return to "
        "the first pose. Keep the body stable. Do not add speech bubbles, sound marks, text, "
        "punctuation, or new props."
    ),
}

MOBILE_STATE_ACTIONS = {
    "idle": (
        "Create a centered portrait idle loop with restrained breathing or equivalent "
        "secondary motion plus a readable multi-frame blink when the character has eyelids. "
        "Keep the focal crop planted, avoid whole-portrait shifts, and return cleanly to the "
        "first pose."
    ),
    "thinking": (
        "Create a quiet thinking loop using only small portrait motions: attentive focus "
        "changes, one restrained pose shift, a natural blink when applicable, and a clean "
        "return to the first pose. Preserve a stable portrait anchor throughout. "
        "Do not add thought bubbles, dots, punctuation, icons, papers, screens, or props."
    ),
    "talking": (
        "Create a conversational portrait loop using small readable mouth shapes or another "
        "reference-appropriate communication cue, subtle expression motion, and a clean "
        "return to the first pose. Preserve a stable portrait anchor. Do not add speech "
        "bubbles, sound marks, text, punctuation, or new props."
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


def canonical_json_bytes(value: Mapping[str, object]) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def mobile_presence_content_hash(manifest: Mapping[str, object]) -> str:
    payload = dict(manifest)
    payload.pop("contentHash", None)
    return hashlib.sha256(canonical_json_bytes(payload)).hexdigest()


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


def parse_mobile_state_specs(raw_specs: Sequence[str]) -> List[Tuple[str, int]]:
    parsed: Dict[str, int] = {}
    for raw in raw_specs:
        if "=" not in raw:
            raise ValueError(f"invalid mobile state {raw!r}; expected state=frameCount")
        raw_state, raw_count = raw.split("=", 1)
        state = validate_state_name(raw_state)
        if state not in MOBILE_PRESENCE_STATES:
            raise ValueError(f"unsupported mobile presence state: {state}")
        if state in parsed:
            raise ValueError(f"duplicate mobile presence state: {state}")
        try:
            count = int(raw_count)
        except ValueError as error:
            raise ValueError(f"invalid frame count for {state}: {raw_count!r}") from error
        parsed[state] = count
    if set(parsed) != set(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence requires idle, thinking, and talking states")
    return [(state, parsed[state]) for state in MOBILE_PRESENCE_STATES]


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


def compact_generation_grid(frame_count: int) -> Tuple[int, int]:
    frame_count = _positive_int(frame_count, "frame count")
    candidates = [
        (columns, frame_count // columns)
        for columns in range(1, min(MAX_GENERATION_COLUMNS, frame_count) + 1)
        if frame_count % columns == 0 and columns >= frame_count // columns
    ]
    if candidates:
        return min(candidates, key=lambda pair: (pair[0] - pair[1], pair[0]))
    return generation_grid(frame_count)


def mobile_frame_durations(state: str, frame_count: int) -> List[int]:
    frame_count = _positive_int(frame_count, "frame count")
    base = MOBILE_STATE_DURATION_MS[state]
    durations = [base] * frame_count
    if frame_count == 1:
        return durations
    if state == "idle":
        durations[0] = 720
        durations[-1] = 840
        blink_start = max(1, frame_count // 4)
        blink_values = (90, 80, 100, 90)
        for offset, value in enumerate(blink_values):
            index = blink_start + offset
            if index < frame_count - 1:
                durations[index] = value
    elif state == "thinking":
        durations[0] = 220
        durations[-1] = 300
        blink_start = min(frame_count - 2, max(1, round(frame_count * 2 / 3)))
        durations[blink_start] = 110
        durations[blink_start + 1] = 130
    elif state == "talking":
        speech_cadence = (130, 90, 105, 90, 110, 100, 90, 105)
        durations = [speech_cadence[index % len(speech_cadence)] for index in range(frame_count)]
        durations[-1] = 150
    return durations


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
    runtime = (
        request.get("mobilePresence")
        if request.get("runKind") == "mobile-presence"
        else request.get("companionRuntime")
    )
    if not isinstance(runtime, dict):
        raise ValueError("request is missing its atlas contract")
    return _positive_int(runtime.get("columns"), "atlas columns")


def _frame_dimensions(request: Mapping[str, object]) -> Tuple[int, int]:
    frame = request.get("frame")
    if not isinstance(frame, dict):
        raise ValueError("request is missing frame geometry")
    return (
        _positive_int(frame.get("width"), "frame.width"),
        _positive_int(frame.get("height"), "frame.height"),
    )


def _placement_contract(request: Mapping[str, object]) -> Tuple[int, int, int, int, int]:
    width, height = _frame_dimensions(request)
    if request.get("runKind") != "mobile-presence":
        return width, height, MAX_SPRITE_WIDTH, MAX_SPRITE_HEIGHT, BASELINE_Y
    horizontal_padding = max(8, round(width * 0.10))
    top_padding = max(6, round(height * 0.08))
    bottom_padding = max(6, round(height * 0.06))
    return (
        width,
        height,
        width - horizontal_padding * 2,
        height - top_padding - bottom_padding,
        height - bottom_padding,
    )


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


def create_layout_guide(
    path: Path,
    columns: int,
    rows: int,
    cell_width: int = CELL_WIDTH,
    cell_height: int = CELL_HEIGHT,
) -> None:
    columns = _positive_int(columns, "guide columns")
    rows = _positive_int(rows, "guide rows")
    cell_width = _positive_int(cell_width, "guide cell width")
    cell_height = _positive_int(cell_height, "guide cell height")
    width = columns * cell_width
    height = rows * cell_height
    image = Image.new("RGBA", (width, height), (*CHROMA_KEY, 255))
    draw = ImageDraw.Draw(image)
    for row in range(rows):
        for column in range(columns):
            left = column * cell_width
            top = row * cell_height
            right = left + cell_width - 1
            bottom = top + cell_height - 1
            if (cell_width, cell_height) == (CELL_WIDTH, CELL_HEIGHT):
                inset_x, inset_y = 18, 16
            else:
                inset_x = max(4, round(cell_width * 0.09))
                inset_y = max(4, round(cell_height * 0.08))
            draw.rectangle((left, top, right, bottom), outline=(48, 48, 48, 255), width=1)
            draw.rectangle(
                (left + inset_x, top + inset_y, right - inset_x, bottom - inset_y),
                outline=(128, 128, 128, 255),
                width=1,
            )
            draw.line(
                (
                    left + cell_width // 2,
                    top + inset_y,
                    left + cell_width // 2,
                    bottom - inset_y,
                ),
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

Use every attached identity reference as a lock and the layout guide only for invisible slot placement. This is a production candidate, not a finished install.

Identity and anatomy:
- Preserve the subject's species or object type, silhouette, proportions, palette, markings, face or focal details, outline grammar, and reference-approved accessories in every frame.
- Preserve the character's correct native number and arrangement of limbs and body parts. Keep every visible component anatomically coherent, attached, complete, and distinct: no extra, duplicated, merged, malformed, or cut-off anatomy.
- Preserve identity-defining colors and feature placement whenever those features are visible; blinks or pose changes may occlude a feature but may not recolor or relocate it.

Animation:
- {action}
- Keep one stable bottom-center anchor and one consistent character scale across all {frame_count} frames.
- Keep the full subject inside each slot with generous transparent-safe padding. No body part or component may touch or cross a slot boundary.

Layout:
- Output exactly {frame_count} separate full-body poses in a {grid_rows} {row_word} of {grid_columns} arrangement, read left-to-right and then top-to-bottom.
- Use one complete subject pose per invisible equal-size slot. Fill every slot.
- Use a perfectly flat solid #00FFFF cyan chroma-key background only.
- The guide is construction-only. Do not reproduce its boxes, lines, marks, or colors.

Forbidden contamination:
- Use no magenta anywhere, including fringe pixels.
- Include no text, labels, frame numbers, logos, watermarks, punctuation, speech bubbles, thought bubbles, UI, scenery, floor, shadows, detached effects, motion marks, or background objects.
- Include no checkerboard, gradient, glow, blur, antialiasing haze, or cyan inside the subject.
- Do not redesign the character or introduce accessories absent from the identity references.

Style:
- Codex digital-pet sprite style with deliberate hard pixel clusters, a limited palette, flat shading, crisp stepped contours, and no soft gradients or realism inconsistent with the references.
"""


def build_mobile_presence_prompt(
    pet_id: str,
    display_name: str,
    state: str,
    frame_count: int,
    grid_columns: int,
    grid_rows: int,
    cell_width: int,
    cell_height: int,
) -> str:
    action = MOBILE_STATE_ACTIONS.get(
        state,
        f"Create a clear looping `{state}` animation and return cleanly to the first pose.",
    )
    row_word = "row" if grid_rows == 1 else "rows"
    return f"""Create a {frame_count}-frame `{state}` animation candidate for the Codex Companion mobile pet `{pet_id}` ({display_name}).

Use every attached identity reference as a lock and the layout guide only for invisible slot placement. Author this directly as a close-up portrait medallion for a 48-point circular presentation. It is not a resized or traced desktop pet.

Portrait identity and anatomy:
- Show exactly one recognizable subject using a reference-appropriate head, upper-body, face, or equivalent focal crop that remains readable at compact size.
- Preserve the subject's silhouette, proportions, palette, markings, face or focal details, outline grammar, and reference-approved accessories.
- Keep every visible body part or component complete, attached, and anatomically coherent. Do not invent ears, paws, limbs, facial features, accessories, or a second character that the references do not contain.
- Preserve identity-defining colors and feature placement whenever visible; blinks or pose changes may occlude a feature but may not recolor or relocate it.

Animation:
- {action}
- Favor readable face, gaze, expression, mouth or equivalent communication cues, and restrained secondary motion at compact size.
- Keep one stable bottom-center anchor and one consistent portrait scale across all {frame_count} frames.
- Keep the complete portrait inside each slot with generous transparent-safe padding. Nothing may touch or cross a slot boundary.

Layout:
- Output exactly {frame_count} separate portrait poses in {grid_rows} {row_word} of {grid_columns}, read left-to-right and then top-to-bottom.
- Use one complete portrait per invisible equal-size {cell_width}x{cell_height} slot and fill every slot.
- Use a perfectly flat solid #00FFFF cyan chroma-key background only.
- The guide is construction-only. Do not reproduce its boxes, lines, marks, or colors.

Forbidden contamination:
- Use no magenta and no cyan inside the subject, including fringe pixels.
- Include no text, labels, frame numbers, logos, watermarks, punctuation, bubbles, UI, scenery, floor, shadows, detached effects, motion marks, or background objects.
- Include no checkerboard, gradient, glow, blur, antialiasing haze, or accessories absent from the identity references.

Style:
- Polished Codex digital-pet pixel art with deliberate hard clusters, a limited palette, flat shading, crisp stepped contours, and an expressive mobile-mascot presence.
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


def prepare_mobile_presence_run(
    run_dir: Path,
    pet_id: str,
    display_name: str,
    references: Sequence[Path],
    states: Sequence[Tuple[str, int]],
    cell_width: int,
    cell_height: int,
    force: bool,
    package_id: Optional[str] = None,
    asset_version: str = "1",
) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    pet_id = validate_state_name(pet_id)
    display_name = display_name.strip()
    if not display_name:
        raise ValueError("display_name cannot be empty")
    cell_width = _positive_int(cell_width, "cell width")
    cell_height = _positive_int(cell_height, "cell height")
    if cell_width > MAX_MOBILE_CELL_DIMENSION or cell_height > MAX_MOBILE_CELL_DIMENSION:
        raise ValueError(
            f"mobile presence cells cannot exceed {MAX_MOBILE_CELL_DIMENSION} pixels"
        )

    counts: Dict[str, int] = {}
    for raw_state, raw_count in states:
        state = validate_state_name(raw_state)
        if state not in MOBILE_PRESENCE_STATES:
            raise ValueError(f"unsupported mobile presence state: {state}")
        if state in counts:
            raise ValueError(f"duplicate mobile presence state: {state}")
        count = _positive_int(raw_count, f"{state}.frameCount")
        if count > MAX_MOBILE_FRAME_COUNT:
            raise ValueError(
                f"{state}.frameCount cannot exceed {MAX_MOBILE_FRAME_COUNT}"
            )
        counts[state] = count
    if set(counts) != set(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence requires idle, thinking, and talking states")

    columns = max(counts.values())
    rows = len(MOBILE_PRESENCE_STATES)
    if columns * cell_width > MAX_MOBILE_ATLAS_DIMENSION:
        raise ValueError("mobile presence atlas width exceeds 8192 pixels")
    if rows * cell_height > MAX_MOBILE_ATLAS_DIMENSION:
        raise ValueError("mobile presence atlas height exceeds 8192 pixels")

    reference_paths = [Path(path).expanduser().resolve() for path in references]
    if not reference_paths:
        raise ValueError("at least one identity reference is required")
    for reference in reference_paths:
        if not reference.is_file():
            raise FileNotFoundError(f"reference not found: {reference}")

    normalized_package_id = validate_state_name(
        package_id or f"{pet_id}-mobile-presence-v1"
    )
    asset_version = asset_version.strip()
    if not asset_version or len(asset_version) > 64:
        raise ValueError("asset version must be a non-empty value of at most 64 characters")

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

    candidate_specs: Dict[str, Dict[str, object]] = {}
    guide_paths: Dict[Tuple[int, int], Path] = {}
    for row, state in enumerate(MOBILE_PRESENCE_STATES):
        frame_count = counts[state]
        generation_columns, generation_rows = compact_generation_grid(frame_count)
        guide_key = (generation_columns, generation_rows)
        if guide_key not in guide_paths:
            guide_path = (
                run_dir
                / "references"
                / f"mobile-layout-guide-{generation_rows}x{generation_columns}.png"
            )
            create_layout_guide(
                guide_path,
                generation_columns,
                generation_rows,
                cell_width,
                cell_height,
            )
            guide_paths[guide_key] = guide_path
        candidate_specs[state] = {
            "row": row,
            "frameCount": frame_count,
            "frameDurationsMilliseconds": mobile_frame_durations(state, frame_count),
            "posterFrame": 0,
            "generationGrid": [generation_columns, generation_rows],
            "runtimeInstallable": True,
        }

    request: Dict[str, object] = {
        "schemaVersion": 3,
        "runKind": "mobile-presence",
        "semanticProfile": MOBILE_PORTRAIT_SEMANTIC_PROFILE,
        "createdBy": "companion-pet",
        "pet": {"id": pet_id, "displayName": display_name},
        "frame": {"width": cell_width, "height": cell_height},
        "candidateStates": list(MOBILE_PRESENCE_STATES),
        "candidateStateSpecs": candidate_specs,
        "mobilePresence": {
            "schemaVersion": 1,
            "packageID": normalized_package_id,
            "assetVersion": asset_version,
            "columns": columns,
            "rows": rows,
            "cellWidth": cell_width,
            "cellHeight": cell_height,
        },
        "generationOrder": "row-major",
        "chromaKey": "#00FFFF",
        "references": copied_references,
        "layoutGuides": {
            f"{generation_rows}x{generation_columns}": path.relative_to(run_dir).as_posix()
            for (generation_columns, generation_rows), path in sorted(guide_paths.items())
        },
    }
    write_json(run_dir / "request.json", request)

    jobs = []
    for state in MOBILE_PRESENCE_STATES:
        spec = candidate_specs[state]
        frame_count = int(spec["frameCount"])
        generation_columns, generation_rows = (
            int(value) for value in spec["generationGrid"]
        )
        prompt_path = run_dir / "prompts" / f"{state}.md"
        prompt = build_mobile_presence_prompt(
            pet_id,
            display_name,
            state,
            frame_count,
            generation_columns,
            generation_rows,
            cell_width,
            cell_height,
        )
        prompt_path.write_text(prompt, encoding="utf-8")
        guide_path = guide_paths[(generation_columns, generation_rows)]
        input_images = [
            {"path": item["path"], "role": "pet identity reference"}
            for item in copied_references
        ]
        input_images.append(
            {
                "path": guide_path.relative_to(run_dir).as_posix(),
                "role": (
                    f"layout-only {generation_rows}x{generation_columns} mobile guide; "
                    "never copy guide pixels"
                ),
            }
        )
        jobs.append(
            {
                "id": state,
                "kind": "mobile-presence-state",
                "status": "pending",
                "promptFile": prompt_path.relative_to(run_dir).as_posix(),
                "inputImages": input_images,
                "expectedFrames": frame_count,
                "generationGrid": [generation_columns, generation_rows],
                "outputRow": f"rows/{state}.png",
            }
        )
    write_json(
        run_dir / "jobs.json",
        {"schemaVersion": 1, "createdBy": "companion-pet", "jobs": jobs},
    )
    return {
        "ok": True,
        "runDir": str(run_dir),
        "states": list(MOBILE_PRESENCE_STATES),
        "packageID": normalized_package_id,
    }


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


def _source_pet_contract(pet_dir: Path) -> Dict[str, object]:
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
    if rows < 9:
        raise ValueError("a Companion conversion source requires at least the 9 standard rows")

    raw_sheet_path = manifest.get("spritesheetPath", "spritesheet.webp")
    if not isinstance(raw_sheet_path, str) or not raw_sheet_path.strip():
        raise ValueError("spritesheetPath must be a non-empty relative path")
    relative_sheet_path = Path(raw_sheet_path)
    if relative_sheet_path.is_absolute() or ".." in relative_sheet_path.parts:
        raise ValueError("spritesheetPath must stay inside the pet directory")
    sheet_path = (pet_dir / relative_sheet_path).resolve()
    if pet_dir not in sheet_path.parents or not sheet_path.is_file():
        raise FileNotFoundError(f"missing pet spritesheet: {sheet_path}")

    with Image.open(sheet_path) as opened:
        sheet_size = opened.size
    if sheet_size[0] % columns or sheet_size[1] % rows:
        raise ValueError(
            f"spritesheet size {sheet_size[0]}x{sheet_size[1]} is not divisible by "
            f"{columns}x{rows}"
        )
    cell_width = sheet_size[0] // columns
    cell_height = sheet_size[1] // rows
    if (cell_width, cell_height) != (CELL_WIDTH, CELL_HEIGHT):
        raise ValueError(
            f"Companion conversion requires {CELL_WIDTH}x{CELL_HEIGHT} cells; "
            f"source uses {cell_width}x{cell_height}"
        )

    raw_counts = manifest.get("animationFrameCounts", {}) or {}
    if not isinstance(raw_counts, dict):
        raise ValueError("animationFrameCounts must be an object when present")
    animation_counts = {
        validate_state_name(state): _positive_int(count, f"{state}.frameCount")
        for state, count in raw_counts.items()
    }
    if any(count > columns for count in animation_counts.values()):
        raise ValueError("animationFrameCounts cannot exceed spriteColumns")

    pet_id = manifest.get("id") or pet_dir.name
    if not isinstance(pet_id, str) or not pet_id.strip():
        raise ValueError("source pet id must be a non-empty string")
    display_name = manifest.get("displayName") or pet_id
    if not isinstance(display_name, str) or not display_name.strip():
        raise ValueError("source pet displayName must be a non-empty string")
    return {
        "directory": pet_dir,
        "manifest": manifest,
        "manifestPath": manifest_path,
        "sheetPath": sheet_path,
        "relativeSheetPath": relative_sheet_path.as_posix(),
        "columns": columns,
        "rows": rows,
        "animationFrameCounts": animation_counts,
        "petID": pet_id.strip(),
        "displayName": display_name.strip(),
    }


def _companion_conversion_schema(
    source: Mapping[str, object], extension_frame_count: Optional[int]
) -> Dict[str, object]:
    columns = int(source["columns"])
    extension_count = columns if extension_frame_count is None else _positive_int(
        extension_frame_count, "extension frame count"
    )
    if extension_count > columns:
        raise ValueError("extension frame count cannot exceed source spriteColumns")

    fallback = load_pet_schema()
    fallback_states = fallback["states"]
    assert isinstance(fallback_states, dict)
    source_counts = source["animationFrameCounts"]
    assert isinstance(source_counts, dict)
    standard_states: Dict[str, Dict[str, object]] = {}
    for state, raw_spec in fallback_states.items():
        if state in {"look-near", "look-far"}:
            continue
        assert isinstance(raw_spec, dict)
        count = int(source_counts.get(state, min(int(raw_spec["frameCount"]), columns)))
        spec: Dict[str, object] = {
            "row": int(raw_spec["row"]),
            "frameCount": count,
        }
        timings = raw_spec.get("timingsMs")
        if isinstance(timings, list) and len(timings) == count:
            spec["timingsMs"] = timings
        standard_states[state] = spec

    standard_states.update(
        {
            "goal-complete": {
                "row": 9,
                "frameCount": extension_count,
                "runtimeInstallable": True,
            },
            "thinking": {
                "row": 10,
                "frameCount": extension_count,
                "runtimeInstallable": True,
            },
            "talking": {
                "row": 11,
                "frameCount": extension_count,
                "runtimeInstallable": True,
            },
        }
    )
    return {
        "schemaVersion": 3,
        "source": {
            "kind": "companion-conversion",
            "sourcePetID": source["petID"],
            "sourceManifestSha256": sha256_path(Path(source["manifestPath"])),
            "sourceSpritesheetSha256": sha256_path(Path(source["sheetPath"])),
        },
        "cell": {"width": CELL_WIDTH, "height": CELL_HEIGHT},
        "atlas": {"columns": columns, "rows": 12},
        "states": standard_states,
        "extensionStates": {},
    }


def prepare_conversion_run(
    run_dir: Path,
    source_pet_dir: Path,
    output_pet_id: Optional[str],
    force: bool,
    extension_frame_count: Optional[int] = None,
) -> Dict[str, object]:
    source = _source_pet_contract(source_pet_dir)
    output_id = validate_state_name(
        output_pet_id or f"{source['petID']}-companion"
    )
    schema = _companion_conversion_schema(source, extension_frame_count)

    with tempfile.TemporaryDirectory(prefix="companion-pet-schema-") as temporary:
        schema_path = Path(temporary) / "runtime-schema.json"
        write_json(schema_path, schema)
        result = prepare_run(
            run_dir=run_dir,
            pet_id=output_id,
            display_name=str(source["displayName"]),
            references=[Path(source["sheetPath"])],
            states=["goal-complete", "thinking", "talking"],
            force=force,
            schema_path=schema_path,
        )

    run_dir = Path(run_dir).expanduser().resolve()
    copied_manifest = run_dir / "references" / "source-pet.json"
    shutil.copyfile(Path(source["manifestPath"]), copied_manifest)
    request = load_request(run_dir)
    reference_path = request["references"][0]["path"]
    request["conversionSource"] = {
        "petID": source["petID"],
        "displayName": source["displayName"],
        "columns": source["columns"],
        "rows": source["rows"],
        "spritesheetPath": source["relativeSheetPath"],
        "manifestSha256": sha256_path(Path(source["manifestPath"])),
        "spritesheetSha256": sha256_path(Path(source["sheetPath"])),
        "manifestReferencePath": copied_manifest.relative_to(run_dir).as_posix(),
        "referencePath": reference_path,
        "preservesDirectionalLookRows": int(source["rows"]) >= 11,
    }
    write_json(run_dir / "request.json", request)
    result["sourcePetDir"] = str(Path(source["directory"]))
    result["outputPetID"] = output_id
    return result


def _save_lossless_webp(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="WEBP", lossless=True, method=6, exact=True)


def package_companion_pet(
    run_dir: Path,
    source_pet_dir: Path,
    output_dir: Path,
) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    source = _source_pet_contract(source_pet_dir)
    output_dir = Path(output_dir).expanduser().resolve()
    source_dir = Path(source["directory"])
    if output_dir.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output_dir}")
    if output_dir == source_dir or source_dir in output_dir.parents:
        raise ValueError("output directory must not be inside the immutable source pet")

    request = load_request(run_dir)
    conversion = request.get("conversionSource")
    if not isinstance(conversion, dict):
        raise ValueError("run was not prepared with prepare-conversion")
    if conversion.get("manifestSha256") != sha256_path(Path(source["manifestPath"])):
        raise ValueError("source pet manifest changed after conversion preparation")
    if conversion.get("spritesheetSha256") != sha256_path(Path(source["sheetPath"])):
        raise ValueError("source pet spritesheet changed after conversion preparation")
    if int(conversion.get("columns", 0)) != int(source["columns"]):
        raise ValueError("source pet columns changed after conversion preparation")
    if int(conversion.get("rows", 0)) != int(source["rows"]):
        raise ValueError("source pet rows changed after conversion preparation")

    validation = validate_run(run_dir, require_review=True)
    if not validation.get("ok"):
        errors = ", ".join(str(error) for error in validation.get("errors", []))
        raise ValueError(f"conversion validation failed: {errors}")

    runtime = request.get("companionRuntime")
    if not isinstance(runtime, dict):
        raise ValueError("request is missing companionRuntime")
    columns = _positive_int(runtime.get("columns"), "companionRuntime.columns")
    rows = _positive_int(runtime.get("rows"), "companionRuntime.rows")
    if columns != int(source["columns"]) or rows != 12:
        raise ValueError("conversion package requires source-width columns and 12 rows")
    state_specs = runtime.get("stateSpecs")
    if not isinstance(state_specs, dict):
        raise ValueError("request is missing companionRuntime.stateSpecs")

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary_dir = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.building-", dir=output_dir.parent)
    )
    try:
        with Image.open(Path(source["sheetPath"])) as opened:
            native_sheet = opened.convert("RGBA")
        companion_sheet = Image.new(
            "RGBA", (columns * CELL_WIDTH, rows * CELL_HEIGHT), (0, 0, 0, 0)
        )
        companion_sheet.alpha_composite(
            native_sheet.crop((0, 0, columns * CELL_WIDTH, 9 * CELL_HEIGHT)),
            (0, 0),
        )
        for state in ("goal-complete", "thinking", "talking"):
            spec = state_specs.get(state)
            if not isinstance(spec, dict) or not isinstance(spec.get("row"), int):
                raise ValueError(f"runtime schema is missing row assignment for {state}")
            row_path = run_dir / "rows" / f"{state}.png"
            with Image.open(row_path) as opened:
                row_image = opened.convert("RGBA")
            expected_size = (columns * CELL_WIDTH, CELL_HEIGHT)
            if row_image.size != expected_size:
                raise ValueError(f"wrong row size for {state}: {row_image.size}")
            companion_sheet.alpha_composite(
                row_image,
                (0, int(spec["row"]) * CELL_HEIGHT),
            )

        output_sheet = temporary_dir / "spritesheet.webp"
        _save_lossless_webp(companion_sheet, output_sheet)
        if int(source["rows"]) >= 11:
            shutil.copyfile(
                Path(source["sheetPath"]), temporary_dir / "look-spritesheet.webp"
            )

        source_manifest = source["manifest"]
        assert isinstance(source_manifest, dict)
        pet = request.get("pet")
        if not isinstance(pet, dict):
            raise ValueError("request is missing pet identity")
        animation_counts = {
            state: int(spec["frameCount"])
            for state, spec in state_specs.items()
            if isinstance(state, str)
            and isinstance(spec, dict)
            and isinstance(spec.get("frameCount"), int)
        }
        manifest: Dict[str, object] = {
            "id": pet["id"],
            "displayName": pet["displayName"],
            "description": source_manifest.get("description")
            or "Companion-ready Codex pet.",
            "spritesheetPath": output_sheet.name,
            "spriteColumns": columns,
            "spriteRows": rows,
            "animationFrameCounts": animation_counts,
        }
        if int(source["rows"]) >= 11:
            manifest.update(
                {
                    "lookSpritesheetPath": "look-spritesheet.webp",
                    "lookSpriteColumns": source["columns"],
                    "lookSpriteRows": source["rows"],
                    "lookFrameStartRow": 9,
                }
            )
        write_json(temporary_dir / "pet.json", manifest)
        write_json(
            temporary_dir / "provenance.json",
            {
                "schemaVersion": 1,
                "createdBy": "companion-pet package-companion",
                "requestSha256": sha256_path(run_dir / "request.json"),
                "validationSha256": sha256_path(run_dir / "qa" / "validation.json"),
                "source": {
                    "petID": source["petID"],
                    "manifestSha256": conversion["manifestSha256"],
                    "spritesheetSha256": conversion["spritesheetSha256"],
                },
                "output": {
                    "manifestSha256": sha256_path(temporary_dir / "pet.json"),
                    "spritesheetSha256": sha256_path(output_sheet),
                },
            },
        )
        temporary_dir.replace(output_dir)
    except Exception:
        shutil.rmtree(temporary_dir, ignore_errors=True)
        raise

    return {
        "ok": True,
        "outputDir": str(output_dir),
        "petID": request["pet"]["id"],
        "columns": columns,
        "rows": rows,
        "directionalLookPreserved": int(source["rows"]) >= 11,
    }


def color_distance(pixel: Tuple[int, int, int], key: Tuple[int, int, int]) -> float:
    return math.sqrt(sum((pixel[index] - key[index]) ** 2 for index in range(3)))


def neutralize_chroma_spill(
    pixel: Tuple[int, int, int], chroma_key: Tuple[int, int, int]
) -> Tuple[int, int, int]:
    high_channels = [index for index, value in enumerate(chroma_key) if value >= 200]
    low_channels = [index for index, value in enumerate(chroma_key) if value <= 55]
    if not high_channels or not low_channels:
        return pixel
    baseline = max(pixel[index] for index in low_channels)
    if not all(pixel[index] >= baseline + 20 for index in high_channels):
        return pixel
    channels = list(pixel)
    ceiling = min(255, baseline + 8)
    for index in high_channels:
        channels[index] = min(channels[index], ceiling)
    return tuple(channels)


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
            red, green, blue = neutralize_chroma_spill(
                (red, green, blue), chroma_key
            )
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


def _global_scale(
    crops: Sequence[Image.Image],
    maximum_width: int = MAX_SPRITE_WIDTH,
    maximum_height: int = MAX_SPRITE_HEIGHT,
) -> float:
    widths = [crop.width for crop in crops]
    heights = [crop.height for crop in crops]
    return min(
        maximum_width / max(widths),
        maximum_height / max(heights),
        1.0,
    )


def _place_on_frame(
    sprite: Image.Image,
    scale: float,
    frame_width: int = CELL_WIDTH,
    frame_height: int = CELL_HEIGHT,
    baseline_y: int = BASELINE_Y,
) -> Image.Image:
    if scale < 1.0:
        sprite = sprite.resize(
            (
                max(1, round(sprite.width * scale)),
                max(1, round(sprite.height * scale)),
            ),
            Image.Resampling.NEAREST,
        )
    frame = Image.new("RGBA", (frame_width, frame_height), (0, 0, 0, 0))
    left = (frame_width - sprite.width) // 2
    top = baseline_y - sprite.height
    frame.alpha_composite(sprite, (left, top))
    return frame


def assemble_row(run_dir: Path, state: str) -> Path:
    run_dir = Path(run_dir).expanduser().resolve()
    state = validate_state_name(state)
    request = load_request(run_dir)
    spec = _state_spec(request, state)
    frame_count = int(spec["frameCount"])
    runtime_columns = _runtime_columns(request)
    frame_width, frame_height = _frame_dimensions(request)
    if frame_count > runtime_columns:
        raise ValueError(
            f"state {state!r} has {frame_count} frames but runtime row has "
            f"{runtime_columns} columns"
        )
    frame_dir = run_dir / "frames" / state
    row = Image.new(
        "RGBA", (frame_width * runtime_columns, frame_height), (0, 0, 0, 0)
    )
    for index in range(frame_count):
        frame_path = frame_dir / f"{index:02d}.png"
        if not frame_path.is_file():
            raise FileNotFoundError(f"missing frame: {frame_path}")
        with Image.open(frame_path) as opened:
            frame = opened.convert("RGBA")
        if frame.size != (frame_width, frame_height):
            raise ValueError(f"wrong frame size for {frame_path}: {frame.size}")
        row.alpha_composite(frame, (index * frame_width, 0))
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
    (
        frame_width,
        frame_height,
        maximum_sprite_width,
        maximum_sprite_height,
        baseline_y,
    ) = _placement_contract(request)
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
    scale = _global_scale(crops, maximum_sprite_width, maximum_sprite_height)

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
        save_png(
            _place_on_frame(
                crop,
                scale,
                frame_width,
                frame_height,
                baseline_y,
            ),
            frame_dir / f"{index:02d}.png",
        )

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
        "anchor": {"horizontal": "center", "baselineY": baseline_y},
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
    semantic_profile = str(request.get("semanticProfile") or "full-body")
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
                "notes": "",
            }
            if semantic_profile == MOBILE_PORTRAIT_SEMANTIC_PROFILE:
                record.update(
                    {field: None for field in MOBILE_PORTRAIT_SEMANTIC_FIELDS}
                )
            else:
                record.update({field: None for field in SEMANTIC_FIELDS})
            frame_records.append(record)
        states.append({"state": state, "frames": frame_records})
    output = run_dir / "qa" / "semantic-review.json"
    write_json(
        output,
        {
            "schemaVersion": 1,
            "reviewType": "manual-semantic-frame-review",
            "semanticProfile": semantic_profile,
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
    frame_width, frame_height = _frame_dimensions(request)
    errors: List[str] = []
    warnings: List[str] = []
    state_reports = []
    semantic_profile = str(request.get("semanticProfile") or "full-body")

    review_path = run_dir / "qa" / "semantic-review.json"
    review_index: Dict[Tuple[str, int], Mapping[str, object]] = {}
    if require_review:
        if not review_path.is_file():
            errors.append("missing_semantic_review")
        else:
            try:
                review = json.loads(review_path.read_text(encoding="utf-8"))
                if review.get("requestSha256") != sha256_path(run_dir / "request.json"):
                    errors.append("stale_review_request_hash")
                if str(review.get("semanticProfile") or "full-body") != semantic_profile:
                    errors.append("semantic_review_profile_mismatch")
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
        if row.size != (frame_width * runtime_columns, frame_height):
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
                (index * frame_width, 0, (index + 1) * frame_width, frame_height)
            )
            if row_cell.tobytes() != frame.tobytes():
                row_frame_mismatch = True
            frame_errors = []
            if frame.size != (frame_width, frame_height):
                frame_errors.append("frame_size_mismatch")
                bbox = None
            else:
                bbox = frame.getchannel("A").getbbox()
                if bbox is None:
                    frame_errors.append("empty_frame")
                else:
                    bboxes.append((index, bbox))
                    if (
                        bbox[0] == 0
                        or bbox[1] == 0
                        or bbox[2] == frame_width
                        or bbox[3] == frame_height
                    ):
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
                if visible_count > int(frame_width * frame_height * 0.65):
                    frame_errors.append("opaque_background_suspected")
            frame_hash = sha256_path(frame_path)
            frame_hashes.append(frame_hash)
            if require_review:
                record = review_index.get((state, index))
                if record is None:
                    frame_errors.append("missing_review_record")
                else:
                    if record.get("sha256") != frame_hash:
                        frame_errors.append("stale_review_hash")
                    if semantic_profile == MOBILE_PORTRAIT_SEMANTIC_PROFILE:
                        for field in MOBILE_PORTRAIT_SEMANTIC_FIELDS:
                            if record.get(field) is not True:
                                frame_errors.append(f"review_{field}_must_be_true")
                    else:
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
            center_tolerance = max(8, round(frame_width * 0.105))
            if any(abs(center - frame_width / 2) > center_tolerance for center in centers):
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


def _mobile_presence_request_contract(
    request: Mapping[str, object],
) -> Dict[str, object]:
    if request.get("runKind") != "mobile-presence":
        raise ValueError("run was not prepared with prepare-mobile-presence")
    frame_width, frame_height = _frame_dimensions(request)
    if (
        frame_width > MAX_MOBILE_CELL_DIMENSION
        or frame_height > MAX_MOBILE_CELL_DIMENSION
    ):
        raise ValueError("mobile presence cell dimensions exceed 256 pixels")

    mobile = request.get("mobilePresence")
    specs = request.get("candidateStateSpecs")
    states = request.get("candidateStates")
    pet = request.get("pet")
    if not isinstance(mobile, dict) or not isinstance(specs, dict):
        raise ValueError("mobile presence request is missing atlas or state specs")
    if not isinstance(pet, dict):
        raise ValueError("mobile presence request is missing pet identity")
    if states != list(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence states must be idle, thinking, and talking")

    columns = _positive_int(mobile.get("columns"), "mobilePresence.columns")
    rows = _positive_int(mobile.get("rows"), "mobilePresence.rows")
    if rows != len(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence atlas row count must equal its state count")
    if columns * frame_width > MAX_MOBILE_ATLAS_DIMENSION:
        raise ValueError("mobile presence atlas width exceeds 8192 pixels")
    if rows * frame_height > MAX_MOBILE_ATLAS_DIMENSION:
        raise ValueError("mobile presence atlas height exceeds 8192 pixels")
    if mobile.get("cellWidth") != frame_width or mobile.get("cellHeight") != frame_height:
        raise ValueError("mobile presence frame and atlas cell dimensions disagree")

    package_id = mobile.get("packageID")
    asset_version = mobile.get("assetVersion")
    pet_id = pet.get("id")
    display_name = pet.get("displayName")
    if not isinstance(package_id, str):
        raise ValueError("mobile presence packageID must be a string")
    validate_state_name(package_id)
    if not isinstance(asset_version, str) or not asset_version:
        raise ValueError("mobile presence assetVersion must be a non-empty string")
    if not isinstance(pet_id, str) or not isinstance(display_name, str):
        raise ValueError("mobile presence pet identity must contain strings")

    used_rows = set()
    animations = []
    maximum_count = 0
    for state in MOBILE_PRESENCE_STATES:
        spec = specs.get(state)
        if not isinstance(spec, dict):
            raise ValueError(f"mobile presence is missing {state} state spec")
        row = spec.get("row")
        if not isinstance(row, int) or isinstance(row, bool) or not 0 <= row < rows:
            raise ValueError(f"{state} row must be within the mobile atlas")
        if row in used_rows:
            raise ValueError(f"more than one mobile state uses row {row}")
        used_rows.add(row)
        frame_count = _positive_int(spec.get("frameCount"), f"{state}.frameCount")
        if frame_count > MAX_MOBILE_FRAME_COUNT or frame_count > columns:
            raise ValueError(f"{state}.frameCount exceeds the mobile atlas")
        maximum_count = max(maximum_count, frame_count)
        durations = spec.get("frameDurationsMilliseconds")
        if not isinstance(durations, list) or len(durations) != frame_count:
            raise ValueError(f"{state} frame duration count must equal frameCount")
        normalized_durations = [
            _positive_int(value, f"{state}.frameDurationsMilliseconds")
            for value in durations
        ]
        poster_frame = spec.get("posterFrame")
        if (
            not isinstance(poster_frame, int)
            or isinstance(poster_frame, bool)
            or not 0 <= poster_frame < frame_count
        ):
            raise ValueError(f"{state} poster frame is outside its frame range")
        generation_grid_value = spec.get("generationGrid")
        if (
            not isinstance(generation_grid_value, list)
            or len(generation_grid_value) != 2
            or not all(isinstance(value, int) for value in generation_grid_value)
            or generation_grid_value[0] * generation_grid_value[1] != frame_count
        ):
            raise ValueError(f"{state} generation grid must contain every frame exactly once")
        animations.append(
            {
                "state": state,
                "row": row,
                "frameCount": frame_count,
                "frameDurationsMilliseconds": normalized_durations,
                "posterFrame": poster_frame,
            }
        )
    if columns != maximum_count:
        raise ValueError("mobile presence atlas columns must equal the largest frame count")
    return {
        "packageID": package_id,
        "petID": pet_id,
        "displayName": display_name,
        "assetVersion": asset_version,
        "columns": columns,
        "rows": rows,
        "cellWidth": frame_width,
        "cellHeight": frame_height,
        "animations": animations,
    }


def _presence_file_record(path: Path) -> Dict[str, object]:
    return {
        "name": path.name,
        "sha256": sha256_path(path),
        "byteCount": path.stat().st_size,
    }


def _validated_presence_file(
    package_dir: Path,
    raw_record: object,
    expected_name: str,
) -> Path:
    if not isinstance(raw_record, dict):
        raise ValueError(f"mobile presence {expected_name} file record is missing")
    name = raw_record.get("name")
    if name != expected_name:
        raise ValueError(f"mobile presence file must be named {expected_name}")
    relative = Path(expected_name)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("mobile presence file path must stay inside its package")
    path = package_dir / relative
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"mobile presence file is missing: {expected_name}")
    if raw_record.get("byteCount") != path.stat().st_size:
        raise ValueError(f"mobile presence byte count mismatch: {expected_name}")
    if raw_record.get("sha256") != sha256_path(path):
        raise ValueError(f"mobile presence SHA-256 mismatch: {expected_name}")
    return path


def _validate_presence_png(
    path: Path,
    expected_size: Tuple[int, int],
) -> None:
    try:
        with Image.open(path) as opened:
            if opened.format != "PNG":
                raise ValueError(f"mobile presence image is not PNG: {path.name}")
            if opened.size != expected_size:
                raise ValueError(f"mobile presence image size mismatch: {path.name}")
            if "A" not in opened.getbands():
                raise ValueError(f"mobile presence image lacks alpha: {path.name}")
            alpha = opened.getchannel("A")
            if alpha.getextrema()[0] != 0:
                raise ValueError(f"mobile presence image lacks transparency: {path.name}")
    except OSError as error:
        raise ValueError(f"mobile presence PNG could not be decoded: {path.name}") from error


def validate_mobile_presence_package(package_dir: Path) -> Dict[str, object]:
    package_dir = Path(package_dir).expanduser().resolve()
    manifest_path = package_dir / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid mobile presence manifest: {manifest_path}") from error
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1:
        raise ValueError("unsupported mobile presence manifest schema")
    if manifest.get("contentHash") != mobile_presence_content_hash(manifest):
        raise ValueError("mobile presence content hash mismatch")

    atlas = manifest.get("atlas")
    if not isinstance(atlas, dict):
        raise ValueError("mobile presence manifest is missing atlas geometry")
    columns = _positive_int(atlas.get("columns"), "atlas.columns")
    rows = _positive_int(atlas.get("rows"), "atlas.rows")
    cell_width = _positive_int(atlas.get("cellWidth"), "atlas.cellWidth")
    cell_height = _positive_int(atlas.get("cellHeight"), "atlas.cellHeight")
    if cell_width > MAX_MOBILE_CELL_DIMENSION or cell_height > MAX_MOBILE_CELL_DIMENSION:
        raise ValueError("mobile presence cell dimensions exceed 256 pixels")
    if (
        columns * cell_width > MAX_MOBILE_ATLAS_DIMENSION
        or rows * cell_height > MAX_MOBILE_ATLAS_DIMENSION
    ):
        raise ValueError("mobile presence atlas dimensions exceed 8192 pixels")

    animations = manifest.get("animations")
    if not isinstance(animations, list) or len(animations) != len(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence manifest requires three animations")
    by_state = {
        item.get("state"): item
        for item in animations
        if isinstance(item, dict) and isinstance(item.get("state"), str)
    }
    if set(by_state) != set(MOBILE_PRESENCE_STATES):
        raise ValueError("mobile presence manifest has invalid animation states")
    used_rows = set()
    maximum_count = 0
    for state in MOBILE_PRESENCE_STATES:
        animation = by_state[state]
        row = animation.get("row")
        count = animation.get("frameCount")
        durations = animation.get("frameDurationsMilliseconds")
        poster = animation.get("posterFrame")
        if not isinstance(row, int) or isinstance(row, bool) or not 0 <= row < rows:
            raise ValueError(f"{state} row is outside the mobile atlas")
        if row in used_rows:
            raise ValueError("mobile presence animation rows must be unique")
        used_rows.add(row)
        count = _positive_int(count, f"{state}.frameCount")
        if count > MAX_MOBILE_FRAME_COUNT or count > columns:
            raise ValueError(f"{state}.frameCount exceeds the mobile atlas")
        maximum_count = max(maximum_count, count)
        if not isinstance(durations, list) or len(durations) != count:
            raise ValueError(f"{state} frame duration count must equal frameCount")
        for duration in durations:
            _positive_int(duration, f"{state}.frameDurationsMilliseconds")
        if not isinstance(poster, int) or isinstance(poster, bool) or not 0 <= poster < count:
            raise ValueError(f"{state} poster frame is outside its frame range")
    if columns != maximum_count:
        raise ValueError("mobile presence atlas columns do not match its animations")

    atlas_path = _validated_presence_file(package_dir, atlas.get("file"), "atlas.png")
    thumbnail_path = _validated_presence_file(
        package_dir,
        manifest.get("thumbnail"),
        "thumbnail.png",
    )
    _validate_presence_png(
        atlas_path,
        (columns * cell_width, rows * cell_height),
    )
    _validate_presence_png(thumbnail_path, (cell_width, cell_height))
    package_bytes = sum(
        path.stat().st_size for path in (manifest_path, atlas_path, thumbnail_path)
    )
    if package_bytes > MAX_MOBILE_PACKAGE_BYTES:
        raise ValueError("mobile presence package exceeds the 8 MiB size limit")
    return {
        "ok": True,
        "packageID": manifest.get("packageID"),
        "contentHash": manifest.get("contentHash"),
        "byteCount": package_bytes,
    }


def package_mobile_presence(run_dir: Path, output_dir: Path) -> Dict[str, object]:
    run_dir = Path(run_dir).expanduser().resolve()
    output_dir = Path(output_dir).expanduser().resolve()
    if output_dir.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output_dir}")
    request = load_request(run_dir)
    contract = _mobile_presence_request_contract(request)
    validation = validate_run(run_dir, require_review=True)
    if not validation.get("ok"):
        errors = ", ".join(str(error) for error in validation.get("errors", []))
        raise ValueError(f"mobile presence validation failed: {errors}")

    columns = int(contract["columns"])
    rows = int(contract["rows"])
    cell_width = int(contract["cellWidth"])
    cell_height = int(contract["cellHeight"])
    animations = contract["animations"]
    assert isinstance(animations, list)

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary_dir = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.building-", dir=output_dir.parent)
    )
    try:
        atlas_image = Image.new(
            "RGBA",
            (columns * cell_width, rows * cell_height),
            (0, 0, 0, 0),
        )
        for animation in animations:
            state = str(animation["state"])
            row = int(animation["row"])
            row_path = run_dir / "rows" / f"{state}.png"
            with Image.open(row_path) as opened:
                row_image = opened.convert("RGBA")
            expected_size = (columns * cell_width, cell_height)
            if row_image.size != expected_size:
                raise ValueError(f"wrong mobile row size for {state}: {row_image.size}")
            atlas_image.alpha_composite(row_image, (0, row * cell_height))

        atlas_path = temporary_dir / "atlas.png"
        save_png(atlas_image, atlas_path)
        idle = next(item for item in animations if item["state"] == "idle")
        poster = int(idle["posterFrame"])
        idle_row = int(idle["row"])
        thumbnail = atlas_image.crop(
            (
                poster * cell_width,
                idle_row * cell_height,
                (poster + 1) * cell_width,
                (idle_row + 1) * cell_height,
            )
        )
        thumbnail_path = temporary_dir / "thumbnail.png"
        save_png(thumbnail, thumbnail_path)

        manifest: Dict[str, object] = {
            "schemaVersion": 1,
            "packageID": contract["packageID"],
            "petID": contract["petID"],
            "displayName": contract["displayName"],
            "assetVersion": contract["assetVersion"],
            "atlas": {
                "file": _presence_file_record(atlas_path),
                "cellWidth": cell_width,
                "cellHeight": cell_height,
                "columns": columns,
                "rows": rows,
            },
            "thumbnail": _presence_file_record(thumbnail_path),
            "animations": animations,
        }
        manifest["contentHash"] = mobile_presence_content_hash(manifest)
        write_json(temporary_dir / "manifest.json", manifest)
        package_bytes = sum(
            path.stat().st_size
            for path in (
                temporary_dir / "manifest.json",
                atlas_path,
                thumbnail_path,
            )
        )
        if package_bytes > MAX_MOBILE_PACKAGE_BYTES:
            raise ValueError("mobile presence package exceeds the 8 MiB size limit")
        package_report = validate_mobile_presence_package(temporary_dir)
        temporary_dir.replace(output_dir)
    except Exception:
        shutil.rmtree(temporary_dir, ignore_errors=True)
        raise
    return {
        "ok": True,
        "outputDir": str(output_dir),
        "packageID": contract["packageID"],
        "contentHash": package_report["contentHash"],
        "byteCount": package_report["byteCount"],
    }


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
    frame_width, frame_height = _frame_dimensions(request)
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
            contact_columns * frame_width * scale,
            contact_rows * frame_height * scale,
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
                (frame_width * scale, frame_height * scale), Image.Resampling.NEAREST
            )
            column = index % generation_columns
            row = row_offset + index // generation_columns
            contact.alpha_composite(
                enlarged,
                (column * frame_width * scale, row * frame_height * scale),
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
    prepare.add_argument("--pet-id", default="companion-pet")
    prepare.add_argument("--display-name", default="Companion Pet")
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

    prepare_conversion = subparsers.add_parser(
        "prepare-conversion",
        help="Prepare new Companion rows from an immutable normal Codex pet package.",
    )
    prepare_conversion.add_argument("--run-dir", required=True)
    prepare_conversion.add_argument("--source-pet-dir", required=True)
    prepare_conversion.add_argument("--output-pet-id")
    prepare_conversion.add_argument("--extension-frame-count", type=int)
    prepare_conversion.add_argument("--force", action="store_true")

    prepare_mobile = subparsers.add_parser(
        "prepare-mobile-presence",
        help="Prepare direct compact idle, thinking, and talking animation candidates.",
    )
    prepare_mobile.add_argument("--run-dir", required=True)
    prepare_mobile.add_argument("--pet-id", default="companion-pet")
    prepare_mobile.add_argument("--display-name", default="Companion Pet")
    prepare_mobile.add_argument("--reference", action="append", required=True)
    prepare_mobile.add_argument("--state", action="append", required=True)
    prepare_mobile.add_argument("--cell-width", type=int, required=True)
    prepare_mobile.add_argument("--cell-height", type=int, required=True)
    prepare_mobile.add_argument("--package-id")
    prepare_mobile.add_argument("--asset-version", default="1")
    prepare_mobile.add_argument("--force", action="store_true")

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

    package = subparsers.add_parser(
        "package-companion",
        help="Build a new Companion-ready package after review and validation.",
    )
    package.add_argument("--run-dir", required=True)
    package.add_argument("--source-pet-dir", required=True)
    package.add_argument("--output-dir", required=True)

    package_mobile = subparsers.add_parser(
        "package-mobile-presence",
        help="Build a reviewed compact presence atlas and manifest without overwriting output.",
    )
    package_mobile.add_argument("--run-dir", required=True)
    package_mobile.add_argument("--output-dir", required=True)
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
    elif args.command == "prepare-conversion":
        result = prepare_conversion_run(
            run_dir=Path(args.run_dir),
            source_pet_dir=Path(args.source_pet_dir),
            output_pet_id=args.output_pet_id,
            force=args.force,
            extension_frame_count=args.extension_frame_count,
        )
    elif args.command == "prepare-mobile-presence":
        result = prepare_mobile_presence_run(
            run_dir=Path(args.run_dir),
            pet_id=args.pet_id,
            display_name=args.display_name,
            references=[Path(path) for path in args.reference],
            states=parse_mobile_state_specs(args.state),
            cell_width=args.cell_width,
            cell_height=args.cell_height,
            force=args.force,
            package_id=args.package_id,
            asset_version=args.asset_version,
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
    elif args.command == "package-companion":
        result = package_companion_pet(
            run_dir=Path(args.run_dir),
            source_pet_dir=Path(args.source_pet_dir),
            output_dir=Path(args.output_dir),
        )
    elif args.command == "package-mobile-presence":
        result = package_mobile_presence(
            run_dir=Path(args.run_dir),
            output_dir=Path(args.output_dir),
        )
    else:
        raise RuntimeError(f"unsupported command: {args.command}")
    _print(result)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
