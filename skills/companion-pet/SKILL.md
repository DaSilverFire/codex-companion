---
name: companion-pet
description: Use when creating, auditing, migrating, validating, or previewing Codex Companion pet animations; when an older pet package differs from the current ChatGPT/Codex schema; or when preparing Companion-only thinking and talking candidates.
---

# Companion Pet

## Overview

Extend `$hatch-pet` with schema-aware Companion states. Discover the active pet contract first, preserve old packages as immutable inputs, and keep generated candidates outside installed apps until source, package, and live-runtime verification all pass.

**Required supporting skills:** use `$hatch-pet` for the base pet package, `$imagegen` for visual generation, `pixel-art-studio` for native-grid review, and `analyze-and-review` before accepting motion or anatomy.

Read [references/companion-contract.md](references/companion-contract.md) before preparing or reviewing assets.

## Boundaries

- Never patch, sign, install, relaunch, or overwrite ChatGPT/Codex as part of an asset run.
- Never overwrite an existing pet package. Migrate into a new directory and retain the original as rollback input.
- Do not assume 8, 16, 32, or another global frame count. Read the current runtime schema and the per-state `frameCount`.
- Do not install `thinking`, `talking`, or another extension state unless the current Companion runtime explicitly exposes it and the user requested installation.
- Do not invent missing pet art with Pillow, SVG, canvas, mirroring, or procedural body-part transforms. Deterministic code may split, chroma-key, align, assemble, hash, validate, and preview generated art.
- Never mark a generation complete by hand. Use `ingest` so source hashes and nearest-neighbor provenance are recorded.

## Discover The Contract

Inspect the installed ChatGPT/Codex renderer, target `pet.json`, and Companion source before choosing a layout. The bundled schema is a dated fallback, not permanent truth.

The 2026-07-13 ChatGPT `26.707.71524 (5263)` snapshot uses:

- `192x208` cells;
- 8 atlas columns and 11 rows for sprite version 2;
- per-state frame counts rather than one global count;
- rows 0-8 for idle/run/wave/jump/failure/waiting/review;
- rows 9-10 for directional look poses;
- no native `thinking` or `talking` state in the inspected renderer.

If current source disagrees, create or supply a newer schema JSON and record its provenance. Never force an extension into `review`, `waiting`, a look row, or another unrelated state.

## New Pet Workflow

1. Run `$hatch-pet` for the base identity and standard runtime rows.
2. Select whole-body identity references with complete anatomy, black nose, correct eyes, no labels, no magenta, and no detached marks.
3. Prepare Companion candidates against the current schema:

```bash
SKILL_DIR="${CODEX_HOME:-$HOME/.codex}/skills/companion-pet"
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" prepare \
  --run-dir /absolute/path/to/companion-run \
  --pet-id shadow \
  --display-name Shadow \
  --reference /absolute/path/to/shadow-reference.png \
  --states thinking,talking
```

Use `--schema /absolute/path/to/runtime-schema.json` when a freshly inspected runtime differs from the bundled snapshot.

`request.json` records the copied schema hash, atlas dimensions, per-state frame counts, generation grids, and whether each state is runtime-installable. `jobs.json` is the generation queue.

4. Generate one grounded visual job per state through `$imagegen`. Attach every reference listed by that job. A state with 8 frames uses a 1x8 guide; a legacy 16-frame schema uses 2x8. Follow the job, not a memorized grid.
5. Reject raw output with a wrong pose count, identity drift, extra or merged limbs, text, guide marks, scenery, shadows, detached effects, chroma inside the pet, or slot crossings.
6. Ingest each state. Grid flags are optional because they default to the prepared state schema:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" ingest \
  --run-dir /absolute/path/to/companion-run \
  --state thinking \
  --source /absolute/path/to/generated-thinking.png
```

7. Create a hash-bound review template, inspect every native frame plus every adjacent transition and loop seam, then complete the semantic fields:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" review-template \
  --run-dir /absolute/path/to/companion-run
```

8. Validate and preview:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" validate \
  --run-dir /absolute/path/to/companion-run

python3 "$SKILL_DIR/scripts/companion_pet_assets.py" preview \
  --run-dir /absolute/path/to/companion-run \
  --scale 2 \
  --duration-ms 120
```

Any frame edit invalidates its review hash. Regenerate the review template and inspect again.

## Update An Old Pet

Audit before touching the package:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" inspect-pet \
  --pet-dir /absolute/path/to/old-pet
```

The report verifies manifest/atlas geometry, hashes the source, compares it with the target schema, and identifies whether migration is required. For migration:

1. Copy nothing into the old package.
2. Use the old spritesheet and approved frame crops as identity/motion references.
3. Prepare a new run against the current target schema.
4. Regenerate or normalize every target row using that row's frame count.
5. Validate, package under a new pet id, install to a new directory, and live-test.
6. Keep the source pet and previous installed package as rollback options.

Pass `--target-schema` to `inspect-pet` when Companion targets a different explicit contract.

## Shadow Gate

Every accepted frame must have:

- exactly four total cat legs, with no extra, duplicated, merged, malformed, hidden-as-one, or cut-off limbs;
- Shadow's compact black-cat silhouette, face, proportions, tail, palette, outline grammar, and black nose;
- golden eyes in open-eye frames; eyelids may cover them during a blink but may not recolor them;
- no magenta, labels, text, numbers, watermarks, guide marks, bubbles, punctuation, UI, scenery, floor, opaque background, detached effects, shadows, glow, blur, or slot crossing;
- one stable bottom-center anchor and consistent scale across the state's declared frame count;
- lossless RGBA frame/row output, binary alpha, zero RGB under transparency, and nearest-neighbor scaling.

Automation cannot prove anatomy, identity, or coherent motion. Manual semantic review remains mandatory and must be bound to the exact frame hashes.

## Completion Report

Keep these gates separate:

- current schema discovery;
- base hatch-pet compatibility;
- deterministic candidate validation;
- exhaustive visual and transition review;
- Companion runtime support;
- packaged installation freshness;
- live animation verification.

A validated row is not proof that the current runtime supports or displays that state.

## Common Mistakes

| Mistake | Correction |
| --- | --- |
| Hardcoding 16 frames because an older Shadow used 16 | Use the state count in the copied runtime schema. |
| Treating all eight atlas cells as animated frames | Use the state's declared count; leave unused runtime cells transparent. |
| Overwriting an old pet during migration | Build a new package and retain the source for rollback. |
| Reusing another state as thinking or talking | Add a distinct runtime state or keep the candidate uninstalled. |
| Counting alpha components as legs | Inspect anatomy manually in every frame. |
| Resizing poses independently | Use one global scale and a fixed bottom-center anchor. |
| Accepting a green validator without playback | Inspect native frames, adjacent transitions, and the loop seam. |
