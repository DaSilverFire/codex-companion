---
name: companion-pet
description: Use when creating, auditing, migrating, validating, packaging, or previewing Codex Companion pet animations; when turning a normal Codex pet into a Companion-ready package; when an older pet package differs from the current ChatGPT/Codex schema; or when preparing Companion goal-complete, thinking, and talking states.
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
- Compact mobile presence art is authored separately. It is not a downscaled desktop spritesheet or a fallback replacement for desktop animation rows.

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
2. Select whole-body identity references with complete, correct anatomy, stable identity details, no labels, no magenta, and no detached marks.
3. Prepare Companion candidates against the current schema:

```bash
SKILL_DIR="${CODEX_HOME:-$HOME/.codex}/skills/companion-pet"
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" prepare \
  --run-dir /absolute/path/to/companion-run \
  --pet-id nova \
  --display-name Nova \
  --reference /absolute/path/to/nova-reference.png \
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

## Convert A Normal Codex Pet

`prepare-conversion` derives a 12-row Companion contract from the source pet instead of assuming a fixed column count. It preserves rows 0-8 as the standard Codex animations, stages distinct goal-complete, thinking, and talking rows, and records the immutable source hashes:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" prepare-conversion \
  --run-dir /absolute/path/to/companion-run \
  --source-pet-dir /absolute/path/to/native-pet \
  --output-pet-id native-pet-companion
```

Generate, ingest, review, validate, and preview the three jobs exactly as in the new-pet workflow. After every semantic review field is approved and bound to current frame hashes, package into a new directory:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" package-companion \
  --run-dir /absolute/path/to/companion-run \
  --source-pet-dir /absolute/path/to/native-pet \
  --output-dir /absolute/path/to/native-pet-companion
```

The package command never overwrites the source or an existing output. It copies native rows 0-8 into a new Companion sheet, adds goal-complete, thinking, and talking at rows 9-11, and retains the original 11-row atlas byte-for-byte as `look-spritesheet.webp` so cursor-looking still uses the native near/far poses. A 9-row legacy source remains usable but cannot provide directional-look metadata.

The result is a staged Companion-ready package, not an automatic installation. Install it to a new Companion pet directory only after package inspection and live playback verification.

## Compact Mobile Presence

Mobile presence packages are separate compact animations for the 48-point pet shown beside an active response. They contain idle, thinking, and talking only. Frame counts are variable per state, from 1-32 frames, and never change the desktop pet.

Author mobile presence as a close-up portrait medallion, not a shrunken desktop sprite. Each frame shows one recognizable subject using a reference-appropriate head, upper-body, face, or equivalent focal crop. Do not invent ears, paws, limbs, or facial features that the source character does not have. Favor expressive gaze, blink, expression, mouth, or equivalent communication beats that remain readable inside the circular presentation.

Keep presentation effects outside the atlas when the runtime clips the portrait to a medallion. A requested thinking bubble is a separate frame-based bitmap animation anchored just beyond the medallion, not a foreign object baked into the pet frame and not runtime-drawn SwiftUI geometry or material. It must appear only while the state is `thinking`, animate through a readable form/hold/idea/dissolve sequence, expose a stable poster frame under Reduced Motion, avoid covering response text, and disappear immediately when the state becomes talking, idle, or failed. The underlying thinking row still needs a coordinated focused character performance and may not merely replay idle or a cursor-look loop.

Prepare direct mobile-scale candidates:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" prepare-mobile-presence \
  --run-dir /absolute/path/to/mobile-presence-run \
  --pet-id nova \
  --display-name Nova \
  --reference /absolute/path/to/nova-reference.png \
  --cell-width 144 --cell-height 144 \
  --state idle=12 --state thinking=12 --state talking=12
```

Generate each job directly at the declared compact cell size. Then use the existing `ingest`, `review-template`, `validate`, and `preview` commands. The mobile review profile checks a recognizable single-subject crop, correct reference-specific anatomy and identity details, stable portrait framing, and absence of unexpected body parts or components. Review every frame and loop seam at native scale; do not reuse a desktop strip or accept a mechanically resized version.

Package only after the hash-bound semantic review passes:

```bash
python3 "$SKILL_DIR/scripts/companion_pet_assets.py" package-mobile-presence \
  --run-dir /absolute/path/to/mobile-presence-run \
  --output-dir /absolute/path/to/new-mobile-presence-package
```

The command writes `manifest.json`, `atlas.png`, and `thumbnail.png` through a temporary sibling followed by atomic promotion. It refuses existing output, cells over 256 pixels, atlases over 8192 pixels, states over 32 frames, duration/count mismatches, stale review hashes, invalid PNG transparency, incorrect SHA-256 values, and packages over 8 MiB. Generated candidates remain outside installed pet folders until review and validation pass.

## Identity And Anatomy Gate

Every accepted full-body desktop frame must preserve the source character's correct native anatomy and component count, with no extra, duplicated, merged, malformed, hidden-as-one, or cut-off parts. Every accepted mobile portrait frame uses a reference-appropriate compact crop and must not invent or remove identity-defining anatomy.

Every accepted frame must also have:

- the source pet's species or object type, face or focal details, applicable full-body silhouette or portrait crop, proportions, palette, markings, outline grammar, and reference-approved accessories;
- identity-defining feature colors and placement whenever visible; a blink or pose change may occlude a feature but may not recolor or relocate it;
- no magenta, labels, text, numbers, watermarks, guide marks, punctuation, UI, scenery, floor, opaque background, detached atlas effects, shadows, glow, blur, or slot crossing; a requested mobile thinking bubble is allowed only as a separate state-bound Companion presentation effect outside the clipped atlas;
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
| Hardcoding 16 frames because an older package used 16 | Use the state count in the copied runtime schema. |
| Treating all eight atlas cells as animated frames | Use the state's declared count; leave unused runtime cells transparent. |
| Overwriting an old pet during migration | Build a new package and retain the source for rollback. |
| Reusing another state as thinking or talking | Add a distinct runtime state or keep the candidate uninstalled. |
| Baking a thought bubble into a clipped mobile frame or drawing it from SwiftUI circles/material | Keep the portrait atlas clean and render a dedicated bitmap effect animation outside the medallion only while `thinking`. |
| Counting alpha components as valid anatomy | Inspect the reference-specific anatomy manually in every frame. |
| Resizing poses independently | Use one global scale and a fixed bottom-center anchor. |
| Accepting a green validator without playback | Inspect native frames, adjacent transitions, and the loop seam. |
