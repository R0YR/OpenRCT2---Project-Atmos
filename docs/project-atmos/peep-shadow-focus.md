# Guest/staff shadow refinement — 2026-10-21

Build: `bin/Project-Atmos-peep-shadows.exe`. Only the isolated working copy is changed.

## Ride shadows paused

`RideShadows::kRenderingEnabled` disables both vehicle casting and track collection/rasterization. This is a rendering gate, not a save edit. Existing per-ride and distance preferences are retained for future work. The ride checkbox is visibly paused/disabled, and inactive vehicle/track distance sliders are omitted.

## Paving and person size

Pacific contains floor sprites such as image 194804 (32×15) and 132429/132413 (32×8), with three-unit sorting boxes. Small scenery computes its sorting height as object height minus five; an eight-unit floor therefore exceeded the old two-unit flat-sprite limit. Its constant upright depth falsely changed receiving height from row to row.

Small scenery with a sorting height at most three and a low/wide sprite is now classified as flat. Tall images, tall sorting boxes, and entity sprites remain upright. No native paint ordering, element visibility or object/save flags are changed.

Flat scenery formerly had both receiver flags and unconditionally used the separate scenery projection. Its world-circle radius projects 41% wider than the original path ellipse. Same-height paving now uses precisely the original path shadow mask, including subpixel coverage and blur. Caster height extends through the blur skirt solely to reject surfaces on another level. Flat roofs above guests cannot receive that floor shadow.

Guest/staff projected radii are reduced to 4.0/4.4 world units from 5.8/6.2. Upright scenery reception uses the matching isometric radius, softens inward, and is limited to 16 units below the walking level instead of extending down to terrain under an elevated invisible path. Real paths and invisible path simulation remain unchanged.

## Verification

- Native Release build and version smoke pass.
- Exact original-path/custom-paving image equality across 32 guest/staff rotation/zoom cases, including odd anchors; upper roofs excluded.
- Original native renderer suite passes: ground/water stability, airborne trail exclusion, scenery reception, leaf depth of field, light coordinates and UI preservation.
- The smaller guest and staff radii pass 1,152 sun/rotation/zoom/softness sweeps; worst mass variation 10.762%, with no vanished phases (old baseline vanished in 36/81 phases).
- Pacific 1920×1080 daylight repro produces 50,267 peep-only shadow pixels. Toggling 254 ride preferences changes zero pixels. Guest visibility changes zero unrelated receiver flags across 1,294,690 unchanged pixels. Optimized ground/scenery output matches scalar references byte for byte.
- Native support visibility tests pass 36 rotation/height/visibility combinations and actual hidden scenery painting. All 170,814 support-relevant element records match between the original Pacific SV6, repro fixture and current autosave. Depth recording on/off, the original masked-sprite implementation, and an isolated renderer using the original Git Paint.cpp each produce identical raw colour buffers in five 1920×1080 views. This excludes those rendering changes and Tile Inspector element mutations as explanations; it does not establish why the user's earlier view hid the supports.

Evidence is under `artifacts/peep-shadow-focus/`, `artifacts/scenery-support-visibility-tests/`, `artifacts/scenery-support-diagnostics/`, and the native test log directories. Daylight QA overrides are isolated to the harness; the user's profile is not overwritten.

## Support investigation limits

**Resolved follow-up:** see `scenery-support-blockers.md`. The exact ship boxes and matching street cases have support-blocker objects below them; small/large wooden scenery supports ignored their segment blockade. The new release respects it. The following paragraph records the earlier investigation state.

The specific pier's native cross-braces disappear with the native invisible-support view mode (default shortcut 5), while its round object posts remain. Other underwater scenery remains visible: the category probe removes one such patch only with scenery hidden, and traces small-scenery bodies followed by the native water filter. This is distinct from the pier braces. It does not identify every box shown in the user's road/ship screenshots. No support visibility flags, scenery assets or park elements were changed on speculation. The exact earlier-state difference for those road/ship boxes remains unresolved; see `scenery-support-visibility-audit.md` and `pier-category-probe.png` for evidence.
