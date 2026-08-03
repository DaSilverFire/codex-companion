# Companion Pet Contract

## Inspected ChatGPT Snapshot

This snapshot was verified from the installed `/Applications/ChatGPT.app` version `26.707.71524 (5263)` on 2026-07-13.

- Built-in Codex spritesheet: `1536x2288` WebP.
- Cell size: `192x208`.
- Sprite version 2: 8 columns x 11 rows.
- Renderer background size: `800%` by `rowCount * 100%`.
- Sprite-version row mapping: version 1 = 9 rows, version 2 = 11 rows.
- Frame counts and timings are state-specific.

| Row | State | Frames | Timing in milliseconds |
| ---: | --- | ---: | --- |
| 0 | `idle` | 6 | 280, 110, 110, 140, 140, 320 |
| 1 | `running-right` | 8 | 120 each, final 220 |
| 2 | `running-left` | 8 | 120 each, final 220 |
| 3 | `waving` | 4 | 140 each, final 280 |
| 4 | `jumping` | 5 | 140 each, final 280 |
| 5 | `failed` | 8 | 140 each, final 240 |
| 6 | `waiting` | 6 | 150 each, final 260 |
| 7 | `running` | 6 | 120 each, final 220 |
| 8 | `review` | 6 | 150 each, final 280 |
| 9 | directional look, near set | 8 | direct pose selection |
| 10 | directional look, far set | 8 | direct pose selection |

The inspected native renderer does not define `goal-complete`, `thinking`, or `talking` as normal animation states. Companion may implement additional rows, but its current source and manifest must explicitly declare them before installation.

The machine-readable snapshot is [codex-pet-schema-2026-07-13.json](codex-pet-schema-2026-07-13.json). It is a fallback for reproducibility, not permanent authority.

## Dynamic Schema Rules

Every run copies its target schema into `references/runtime-schema.json` and records the SHA-256 in `request.json`.

The schema controls:

- cell width and height;
- runtime atlas columns and rows;
- state-to-row mapping;
- used frames per state;
- optional timing data;
- extension-state frame counts and runtime installability.

The pipeline rejects a state whose frame count exceeds the atlas columns. A runtime row is always atlas-width; unused cells remain fully transparent. Generation grids contain only the used frame count and are derived deterministically from that count. Examples:

- 4 frames -> 1x4;
- 6 frames -> 1x6;
- 8 frames -> 1x8;
- legacy 16 frames with a 16-column target -> 2x8.

Never infer runtime support from the existence of generated art.

## Existing Pet Audit And Migration

`inspect-pet` verifies:

- `pet.json` structure;
- safe relative `spritesheetPath`;
- atlas dimensions and cell geometry;
- `animationFrameCounts` bounds;
- manifest and spritesheet hashes;
- source versus target schema differences.

Migration is copy-first:

1. Keep the source package unchanged.
2. Use its sheet/frames as references.
3. Create a new run against the current target schema.
4. Rebuild every target row using its target frame count.
5. Package under a new id and directory.
6. Install and live-test only after validation and manual review.

The old package remains the rollback source.

## Normal Pet To Companion Package

`prepare-conversion` derives its columns and standard frame counts from a normal Codex package with `192x208` cells and at least rows 0-8. The generated Companion target uses:

- source rows 0-8 unchanged for standard Codex motion;
- row 9 for `goal-complete`;
- row 10 for `thinking`;
- row 11 for `talking`;
- the source atlas as `look-spritesheet.webp` when native directional-look rows 9-10 exist.

`package-companion` requires a current hash-bound semantic review, verifies that source manifest and spritesheet hashes still match preparation, builds through a temporary sibling directory, and refuses to replace an existing path. Packaging does not install or mutate either application.

## Compact Mobile Presence Contract

A mobile presence package is authored separately from the desktop atlas. It is not a resized desktop row and never changes desktop render identity, frame counts, timing, or selection. The package contains only `idle`, `thinking`, and `talking` in distinct rows.

The default mobile semantic profile is `mobile-portrait-medallion`. It is a direct close-up for the 48-point circular presentation: one recognizable subject using a reference-appropriate head, upper-body, face, or equivalent focal region. The frame review requires a coherent compact crop, correct source-specific anatomy and identity details, no unexpected body parts or components, stable portrait scale, and a consistent bottom-center anchor. Desktop rows keep the source pet's complete full-body anatomy.

The prepared request declares variable frame counts per state, one fixed cell size, state-specific positive frame durations, one poster frame per state, and atlas columns equal to the largest state count. Limits are:

- 1-32 frames per state;
- cell width and height no larger than 256 pixels;
- atlas width and height no larger than 8192 pixels;
- exactly one row each for idle, thinking, and talking;
- total packaged `manifest.json`, `atlas.png`, and `thumbnail.png` no larger than 8 MiB.

`package-mobile-presence` requires the same current frame-hash and request-hash semantic review as desktop candidates. It writes to a temporary sibling, validates exact file SHA-256 values, byte counts, lossless RGBA PNG decoding, alpha transparency, geometry, row bounds, duration counts, and a canonical manifest content hash, then atomically renames the sibling. It never replaces an existing output.

```text
mobile-presence-package/
  manifest.json
  atlas.png
  thumbnail.png
```

The content hash is SHA-256 over canonical JSON with sorted keys and compact separators after removing `contentHash` itself. File records cover `atlas.png` and `thumbnail.png`; the manifest is not self-hashed as a file record.

## Thinking Choreography

Scale the beats to the declared frame count rather than assuming 16 frames:

- attentive neutral settle;
- small identity-appropriate pose or focus change without repeatedly sweeping the subject from side to side;
- natural blink when applicable and restrained secondary movement for full-body rows, or subtle expression and focused movement for the mobile portrait;
- return to the starting pose without a loop pop.

Desktop and atlas-authored motion use pose and expression only. Do not bake thought bubbles, dots, punctuation, icons, papers, screens, or props into a pet frame.

When the user requests a Lumo-like thinking cue for the compact mobile presentation, Companion may render a separate thought bubble outside the clipped medallion. That effect is presentation-owned rather than atlas-owned: it is visible only for `thinking`, forms and settles with a restrained spring, holds as a readable bubble, dissolves before the next loop, becomes static under Reduced Motion, and disappears as soon as the state changes. It must not reserve a permanent text column or overlap the response.

## Talking Choreography

Scale the beats to the declared frame count:

- closed mouth into a small readable open-mouth shape;
- two or more subtle mouth-shape changes with ear/cheek motion;
- optional natural blink while speech motion continues;
- close the mouth and return to the starting pose.

Keep the body planted. No speech bubbles, sound marks, punctuation, text, or new props. Preserve every identity-defining facial or focal detail from the references.

## Identity And Anatomy

Every frame requires one recognizable source character. Full-body desktop frames preserve the correct native number and arrangement of limbs and components with no extra, duplicated, merged, hidden-as-one, malformed, or cut-off anatomy. Mobile portrait-medallion frames use a reference-appropriate focal crop with no invented or detached anatomy and no second character.

All framings also require:

- stable silhouette, proportions, palette, markings, outline weight, face or focal details, and reference-approved accessories;
- identity-defining feature colors and placement whenever visible;
- complete silhouette inside the cell;
- no detached component or foreign object.

Blinking or pose changes may hide a feature temporarily. They may not recolor, relocate, remove, or duplicate identity-defining details. Talking motion may change only the intended communication region or reference-appropriate cue.

## Contamination Gate

Reject any visible:

- magenta, cyan key color, or colored fringe;
- opaque/checkerboard background;
- label, frame number, text, logo, watermark, guide line, or border;
- speech/thought bubble inside an atlas frame, punctuation, UI, scenery, or floor; a separately rendered mobile thinking effect outside the medallion is reviewed with the live presentation rather than this atlas contamination gate;
- shadow, glow, blur, aura, dust, motion line, or detached effect;
- neighboring-slot fragment or edge contact.

## Semantic Review Schema

The review template binds each decision to a frame SHA-256. Full-body desktop reviews require:

| Field | Required value |
| --- | --- |
| `anatomyCorrect` | `true` |
| `limbsComplete` | `true` |
| `identityDetailsPreserved` | `true` |
| `noLabels` | `true` |
| `noBackground` | `true` |
| `identityConsistent` | `true` |
| `anchorScaleConsistent` | `true` |
| `noExtraObjects` | `true` |
| `approved` | `true` |

Mobile `mobile-portrait-medallion` reviews use:

| Field | Required value |
| --- | --- |
| `cropCompositionCorrect` | `true` |
| `anatomyCorrect` | `true` |
| `identityDetailsPreserved` | `true` |
| `noUnexpectedBodyParts` | `true` |

They retain the shared contamination, identity, anchor, object, and approval fields above.

Recreate and repeat review after any frame changes. A stale hash is a validation failure.

## Run Outputs

```text
run/
  request.json
  jobs.json
  prompts/<state>.md
  references/identity-*.<ext>
  references/runtime-schema.json
  references/layout-guide-<rows>x<columns>.png
  sources/<state>.<ext>
  frames/<state>/<index>.png
  rows/<state>.png
  provenance/<state>.json
  qa/semantic-review.json
  qa/validation.json
  qa/contact-sheet.png
  qa/<state>-preview.gif
```

These are staged candidates, not an installed Companion pet package.

A successful `package-companion` run additionally creates:

```text
new-pet-package/
  pet.json
  spritesheet.webp
  look-spritesheet.webp  # only when the source has native look rows
  provenance.json
```
