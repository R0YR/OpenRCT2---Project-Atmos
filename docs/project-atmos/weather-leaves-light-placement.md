# Weather, leaves and precise light placement — 2026-10-20

Working build: `bin/Project-Atmos-weather-lights-final.exe`, selected by the Project-Atmos launcher. Original checkout and user settings are preserved.

## Changes

- Leaf spawn rate increased from `5 + 12 * wind` to `12 + 28 * wind`, about 2.3 times the previous rate. Bounded accumulation and a 600-particle ceiling prevent bursts. Existing translucent three-tone sprites, slow fall, world height, foreground occlusion and tilt-shift remain in place.
- Storms no longer increase cloud coverage beyond the selected slider value. Moderate coverage has clearer gaps between banks, and automatic snow/blizzard fog is reduced. Explicit 100% still produces full coverage.
- Light anchors remain free integer world coordinates. The placement tool now intersects the cursor ray with the bulb height instead of placing its foot at the terrain click. Native terrain picking supplies only the initial height; it no longer clamps the final coordinates into the clicked tile.
- A local exact inverse matches native integer isometric projection, including odd coordinates and negative screen coordinates. Light rendering, markers and picking use the same integer projection parity. No global viewport inverse was changed.
- Moving lights preserves absolute bulb height across terrain changes. Rotation or zoom during dragging rebases the grab offset without changing the stored anchor. Invalid terrain/height intersections leave the existing light in place; clipboard placement retains the copied height.

## Ownership and verification

The existing light store owns world XY and height above terrain; tool state owns only drag height, offset and camera revision. Rendering reads the stored anchor. Rotation never writes anchor coordinates by itself. Existing save/preferences paths are unchanged.

Release build and version smoke passed. Compiled production placement/drag/storage tests passed 512 free placements and 16 picked-stem drags across terrain, camera rotation and zoom. Native projection tests passed 11,664 roundtrips; the previous inverse failed 5,184 of these odd-coordinate cases. Leaf density simulation produced 143 live leaves after 16 seconds at 40% intensity. Weather tests passed 2,424 coverage cases, including zero and explicit 100%, plus 1,280 cloud projection cases.

The actual Pacific Ocean Park native renderer passed a forced blizzard export at 1920×1080. Surface classification remained identical across 1,186,860 unchanged pixels outside native guest/staff sprite bounds. The full native renderer regression suite passed, including scenery shadow receivers, water, airborne sprites, leaf depth of field and UI preservation.

Evidence: `artifacts/weather-light-fixes/blizzard-before/`, `artifacts/weather-light-fixes/blizzard-final/`, and test logs under `artifacts/`. Screenshot exports were visually inspected. Automated tools exercise production input methods; a live interactive placement session was not performed.
