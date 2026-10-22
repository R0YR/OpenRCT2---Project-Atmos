# Atmosphere: cloud and directional-shadow revision

Implemented on 2026-10-11. The original working tree already contained the Atmosphere fork; those changes are preserved. No commit was created.

## Behaviour and controls

Choose **Effects > Choose a look... > Soft clouds and sunlight** with Atmosphere enabled. This enables bright cloud banks, directional object shadows and subtle warm daylight. The preset uses independent cloud coverage, rather than following the game's weather; that checkbox remains available.

- Clouds now use finite-support density and optical transmittance. Partial coverage produces actual gaps, thin edges and dense cores. Size-relative samples toward the sun add self-occlusion and a luminous edge. Half-resolution planes interpolate smoothly at compositing. The focus centre clears fully at close zoom, then shrinks through the four zoom levels.
- Object shadows use separate world-space volumes for buildings, scenery, entrances, tree crowns/trunks, walls, tracks/supports, guests, staff and vehicles. Elevated ribbons leave light beneath their empty air. Flat raised surfaces can receive shadows; upright sprites are excluded because their recorded depth cannot identify a reliable receiving surface.
- Sun direction, elevation and maximum reach have consistent map-space meaning across rotation. The maximum reach is enforced even when a taller distant blocker obscures a nearer blocker in the height map.
- Light adds **Sunlight** and **Sun warmth**. Clouds adds the existing **Cloud height** setting. These values round-trip through atmosphere.ini and reset correctly; Everything off clears both new light adjustments.
- Original exterior runs and UI pixels are preserved. Black pixels inside sprites remain eligible for effects.

## Ownership and ordering

Settings remain owned by the UI thread. Geometry, static shadow raster and entity projections are prepared before row workers start. Static rasterization has one writer; overlapping casters no longer race. The finite-ray cache uses an atomic packed exact-height/result value when row workers share texels. Old and new entity rays invalidate the cache before worker reads. Geometric changes invalidate static data; visual-only tile invalidations now compare actual caster/terrain geometry and retain the raster when it is unchanged.

## Verification

- Release x64 UI library compiled successfully, with no C++ warnings/errors. The final executable linked successfully and starts as OpenRCT2 v0.5.6.
- This sandbox's MSVC PDB manager rejects /Zi. Verification used /Z7 embedded compiler debug data and /DEBUG:NONE linking, retaining the existing release optimization. The normal build-atmo.sh source workflow is unchanged.
- scripts/test-atmosphere-clouds.py: PASS for bounded density/opacity, actual gaps, monotone coverage, overcast coverage and continuity at world-per-pixel 1/2/4/8.
- scripts/test-atmosphere-shadows.ps1: PASS for direction, elevated receivers, suspended rails, rounded foliage, finite reach, distant/nearer blockers, entity-cache invalidation, exact receiver heights, concurrent cache readers and visual-only geometry revisions.
- scripts/test-atmosphere-compositor.ps1: PASS for settings persistence/clamping, Everything off, odd frame dimensions, exterior/UI preservation, dense cores, flat receivers, upright exclusion and last-row behaviour.
- Existing tests.exe: 41/42 selected tests passed. RideRatings.BigMap could not load its fixture because required objects are missing. That executable predates this revision and does not link the UI effects.
- Runtime screenshots checked all four zoom levels, rotated shadows, both menu pages, black exterior and night tint. Test park: Gal Ard - Season One - 0374.park, opened with a separate workspace-local user-data directory. Some RCT1 objects use fallback sprites because this installation has no linked RCT1 data.
- Measured postprocessing after the geometry-cache correction: about 4.1-4.3 ms/frame at 1280x720 with clouds and shadows; about 10-10.5 ms/frame at 1920x1080 in the tested daylight view. Tilt-shift/vignette were disabled to isolate these changes. These are CPU effect timings, not whole-game frame timings. Night/low-sun views are more expensive (~12.4 ms in this scene). 1440p/4K and other machines are not measured.

Screenshots and logs are in artifacts/atmosphere-reference. final-clouds-z0 through z3, final-shadow-z1/z2, final-shadow-rotation1, final-light-menu, final-cloud-menu and final-night are the verified examples.

## Representation limits

This remains a CPU postprocess over 2D sprites. Cloud volume and self-lighting are approximations, not a 3D cloud raymarch. Custom building silhouettes, tree foliage and curved/looping tracks use geometry proxies; tracks are oriented tile ribbons with simple supports. Sunlight/warmth adjusts world colours, without reconstructing façade normals. Giant screenshots still lack depth, so object shadows are not included there. Exact sprite silhouettes and façade relighting require additional paint metadata and are not claimed by this revision.
