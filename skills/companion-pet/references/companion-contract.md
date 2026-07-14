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

## Thinking Choreography

Scale the beats to the declared frame count rather than assuming 16 frames:

- attentive neutral settle;
- small cat-like head tilt and ear turn;
- natural blink and restrained tail-tip movement;
- return to the starting pose without a loop pop.

Use pose and expression only. No thought bubbles, dots, punctuation, icons, papers, screens, or props.

## Talking Choreography

Scale the beats to the declared frame count:

- closed mouth into a small readable open-mouth shape;
- two or more subtle mouth-shape changes with ear/cheek motion;
- optional natural blink while speech motion continues;
- close the mouth and return to the starting pose.

Keep the body planted. No speech bubbles, sound marks, punctuation, text, tongue, or props. Shadow's nose remains black.

## Shadow Identity And Anatomy

Every frame requires:

- one compact black cat;
- exactly four total cat legs, all anatomically distinct;
- no extra, duplicated, merged, hidden-as-one, malformed, or cut-off limbs;
- stable head, ears, muzzle, tail, proportions, outline weight, and charcoal palette;
- black nose in the same facial position;
- golden eyes whenever open;
- complete silhouette inside the cell;
- no detached component or foreign object.

Blinking may hide the irises temporarily. It may not recolor or relocate them. Talking mouth shapes may change only the mouth region.

## Contamination Gate

Reject any visible:

- magenta, cyan key color, or colored fringe;
- opaque/checkerboard background;
- label, frame number, text, logo, watermark, guide line, or border;
- speech/thought bubble, punctuation, UI, scenery, or floor;
- shadow, glow, blur, aura, dust, motion line, or detached effect;
- neighboring-slot fragment or edge contact.

## Semantic Review Schema

The review template binds each decision to a frame SHA-256. Required fields:

| Field | Required value |
| --- | --- |
| `legCount` | `4` |
| `legsSeparated` | `true` |
| `limbsComplete` | `true` |
| `blackNose` | `true` |
| `goldenEyes` | `true` |
| `noLabels` | `true` |
| `noBackground` | `true` |
| `identityConsistent` | `true` |
| `anchorScaleConsistent` | `true` |
| `noExtraObjects` | `true` |
| `approved` | `true` |

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
