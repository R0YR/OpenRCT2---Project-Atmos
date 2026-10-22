# Explicit scenery foliage selection

The scenery window has a checkbox with a green leaf in its right sidebar, below the cluster tool. It marks the selected small scenery, large scenery or wall object as foliage. All instances of that object share the selection. Global wind and falling-leaf settings still determine the actual effects.

Original trees and watered plants keep their default foliage classification. Explicit original shrub, fern and topiary identifiers cover plants without native tree/watering flags. Custom objects start disabled: names, green pixels and custom tree flags no longer automatically opt them in. Unticking also stops their leaf emission; existing particles are cleared when the selection changes.

Preferences live in `atmosphere-foliage.txt` in the active user profile. Stable object descriptors, rather than transient loaded slots, identify overrides. Choices survive restarts, park changes and object slot changes. No park file, network state or Tile Inspector flags are modified.

The object manager publishes a small atomic lookup table after object load/unload operations. Native paint workers read it without filesystem access. The existing sway mask carries both wind and leaf eligibility. Native support sprites remain rigid even if their owning scenery object is selected. The UI/frame owner reads profile preferences; successful checkbox changes invalidate the scene.

Build: `scripts/build-project-atmos.ps1 -ExecutableName Project-Atmos-foliage-selection.exe`.
Focused verification: `scripts/test-foliage-selection.ps1`. Existing renderer, falling-leaf and scenery-support tests cover adjacent invariants.

Verification passed: production build and version smoke; actual scenery checkbox/native foliage coverage; 2 loaded original defaults and 248 custom objects disabled by default in Pacific; persisted small/large/wall choices, original opt-out, unload and slot remapping. The native window preview is `artifacts/foliage-selection-tests/scenery-foliage-checkbox.png`. The full atmosphere-core suite, 32 leaf placement cases/density test and 24 Pacific support-blocker cases plus nine partial-mask controls pass. Original-version integrity: 11,637 recorded files, zero changes.
