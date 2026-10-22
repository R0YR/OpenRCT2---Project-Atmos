# Shadow draw distances

## User controls

The Sun page has Guest range, Vehicle range and Track range sliders (0..256 map tiles). Recommended defaults are 80 tiles for visitors/staff and 160 for vehicles and tracks. Zero disables distance and zoom LOD for that category. Settings are loaded/saved in atmosphere.ini and reset through the existing light defaults button. Per-ride shadow opt-outs still override these global preferences.

Distance is measured from the map point below the viewport centre. Stable underlying receiver depths determine its height; visible guest/vehicle depths are never used as the centre anchor. A fallback uses the local terrain height. The outer 20% uses smoothstep fading. Bounds include the displaced shadow footprint, so offscreen casters can still cast into the selected receiving area.

Finite guest distances also fade shadow opacity between 2 and 8 world units per pixel; vehicles/tracks fade later, between 4 and 16. Zero bypasses this fade, including far zooms. Ground and scenery receive identical caster opacity. Fade applies before coverage union/softening. No distance state or fade value is cached across camera moves; the current view is evaluated each frame.

Ground capsules are now rejected using their complete projected ellipse/capsule envelope instead of the former fixed 400-pixel endpoint margin. Segments crossing the view are retained even if both endpoints are offscreen. Scenery continues to use clipped pixel bounds and actual receiver depth.

## Validation

- Production helper tests: monotonic range/zoom curves, zero/full mode, 2400 capsule envelopes (1209 safely rejected), half opacity, rotated world-coordinate recovery and offscreen-to-onscreen shadows.
- Native core: near pixels unchanged, far shadow fades, zero restores full detail, guest zoom LOD, far overview skip, settings load/save/default reset. Existing scenery/vehicle/ride, cloud/water, UI, odd-size blur and leaf tests pass with serial/scalar byte-equivalence oracles.
- Actual Pacific park close view: recommended and unlimited shadow images have zero differing pixels.
- Actual Pacific park zoom 3 overview: 52,542 pixels differ, all only lighter under LOD; no darker artifacts. Ride toggles and guest/water depth invariance pass (207,365 unchanged receiver pixels).
- The eleven Sun sliders were rendered through the actual native widget/scroll code and visually checked; no overlap or clipping. Preview: artifacts/shadow-distance/pacific-close/sun-distance-controls.png.

## Performance

Controlled full-HD Pacific overview (zoom 3, rotation 3), 20 warm-up + 300 measured frames. Both cases use the same native park and effect settings; leaves and foliage sway are disabled only in the reproducible fixture, not in the user profile. Unlimited: Atmosphere mean 29.67 ms, median 29.12, p95 36.53; native mean 21.73 ms. Recommended distances: Atmosphere mean 25.21 ms, median 24.68, p95 30.80; native mean 21.37 ms. Atmosphere time falls about 15%; the now-separated shadow stage falls from 12.93 to 8.78 ms (about 32%). These are isolated render costs, not interactive game FPS.

Build: Project-Atmos-shadow-distance-final.exe. Original source checkout and original park saves are preserved.
