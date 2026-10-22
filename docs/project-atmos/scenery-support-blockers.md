# Scenery support blockers — 2026-10-21

The user's clarification rules out the support view toggle as the intended solution. The park uses scenery/Tile Inspector layout to block unwanted automatic supports.

## Reproduced cause

The exact underwater boxes beside the ship are native wooden support sprites. At tile (162,87), terrain is at Z224 and water at Z272. The tile contains object 241, `ja227--Support Blocker`, at Z224, then object 201, `Steinblock`, at Z240. The unwanted support sprite is 3391.

The blocker has zero object height, occupies the full tile, has no supports of its own, and disallows supports above. It correctly marks all nine support segments as blocked. Its manually set tile clearance is Z232, but zero object height leaves the general support height at the terrain level. Wooden supports previously consulted that general height without consulting the segment block mask, so they generated a short frame underneath the next scenery object.

The same arrangement is reproduced on street/garden tiles (65,113) and (59,129): blocker 241 at Z416, then large scenery at Z432. Automatic large-scenery supports generate sprites 3391/3393 despite the full blocker. These are concrete park cases matching the reported kind of street artifacts; the supplied street screenshots were not mapped pixel for pixel to those coordinates.

This is a real mismatch between stored support blocking and wooden scenery support generation. The earlier comparison found no lost Tile Inspector flags and no changed raw output from the original paint/sprite implementations. A particular historical commit causing the visual difference has not been established.

## Correction

Small and large scenery support generation now respects an existing full nine-segment blockade through `ScenerySupportsAreBlocked`. Object body painting and support-height bookkeeping continue normally. The correction belongs to scenery's automatic supports; it does not change track support rules, force a global visibility flag, edit park elements, or identify blocker objects by name.

Release `bin/Project-Atmos-blocker-fix.exe` builds and passes its version smoke test. The Project-Atmos launcher selects this executable.

The full native renderer regression suite passes, including the guest/staff paving improvements, scenery shadow reception, light coordinates, water, exterior, leaf depth of field and UI preservation. Pacific daylight rendering passes with zero unrelated ground flag changes over 1,296,530 unchanged pixels. Ground/scenery optimized output still matches its scalar reference. The native support visibility/Inspector tests also pass.

Focused tests exercise the actual Pacific blocker/object combinations at all four rotations, with blocker enabled and removed: 24 cases retain one object body, emit zero automatic supports with the blocker, and restore supports without it. Nine partial-mask controls preserve ordinary support generation when the full blockade is absent.

Run `scripts/test-scenery-support-blocking.ps1` to exercise these cases against the built production libraries. A native before/after render of the ship region confirms that the underwater frames disappear without any hide flags: `artifacts/scenery-support-diagnostics/ship-blocker-before-after.png`. The object bodies and underwater scenery remain visible. Original integrity check: all 11,637 source-version files retain their recorded hashes.

This report supersedes the unresolved blocker diagnosis in `scenery-support-visibility-audit.md` and `peep-shadow-focus.md`.
