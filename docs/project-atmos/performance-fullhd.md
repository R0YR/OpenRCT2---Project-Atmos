# Full-HD performance pass

## Measurement

Pacific Ocean Park isolated autosave, 1920x1080, saved camera (zoom -1, rotation 3), working-profile effect strengths unchanged. Falling leaves and tree sway are disabled in the isolated benchmark fixture for reproducibility; the actual user profile is not changed. Native paint and Atmosphere are timed separately: 20 warm-up frames followed by 300 measured frames, simulation paused/static. This benchmark repaints the full viewport and measures rendering, not total interactive game FPS or GPU presentation. Background processes and scheduling remain variable.

Baseline: native 5.76 ms mean (5.40 median, 8.78 p95); Atmosphere 94.84 ms mean (94.46 median, 109.64 p95). Shadow stage 67.93 ms, including 60.22 ms entity work.

Final measured frames: native 6.57 ms mean (6.27 median, 9.81 p95); Atmosphere 52.50 ms mean (51.48 median, 65.76 p95). Instrumented stage averages: Atmosphere 51.75 ms; shadow 21.01 ms, entity work 14.25 ms, track work 2.66 ms, softening 2.88 ms. This removes about 45% of Atmosphere CPU time. No effect sliders, sample counts, resolution, weather coverage or quality settings changed.

## Changes

Scenery receivers are unprojected once per frame into sorted sparse rows. Only receivers inside each caster footprint are examined. Disjoint row ranges run on the existing worker pool; each pixel retains the same max-union and coverage formula. Two unused full-plane shadow-height dilation passes were removed. Face-shading scans are skipped when no face geometry was produced.

Opt-in ATMOS_QA_VERIFY_SCENERY computes the former dense scalar raster and compares every coverage byte in the same frame. It passed both native integration tests (20 view/zoom combinations) and actual Pacific scenery/ride-toggle renders. The previous stored PNGs used different timing/profile states and were therefore not used to claim pixel identity. Native water/cloud/guest depth, UI and ride-toggle regressions all pass.

## GPU status and next architecture

SOFTWARE_HWD currently paints the park and every Atmosphere pass on CPU; SDL accelerated presentation uploads/scales/displays the resulting texture. Registry hardware includes NVIDIA GeForce RTX 3080 and AMD integrated graphics, but does not prove which GPU SDL selected.

OpenRCT2 has an OpenGL 3.3 renderer, but it does not provide Atmosphere's X8 auxiliary planes or invoke its pipeline. Switching rendering settings alone would lose effects. A GPU implementation should preserve native X8 painting and depth/material ownership, upload immutable planes, and execute the continuous Atmosphere pixel chain in shaders. Existing GL shader/framebuffer wrappers are reusable. Porting only depth-of-field would force readback because water, lights, rays, precipitation and lightning still follow it on CPU. GPU output should remain resident until presentation; CPU fallback and image comparisons are required. No shader backend was added in this CPU optimization pass.

Build: Project-Atmos-performance.exe. Original source checkout remains preserved.


## Second CPU pass

- Ground and rail capsules are queued, binned into 32-row bands and rasterized by workers owning disjoint output bytes. Caster ordering and equal-coverage height ties remain unchanged. ATMOS_QA_VERIFY_GROUND compares every coverage byte and height against the former serial raster before softening.
- Sparse scenery receiving rows are reused only when both native input planes are byte-identical and the complete view key (dimensions, origin, zoom and rotation) matches. Changing depth, coverage or view forces reconstruction; there is no timed shadow reuse. The scalar scenery oracle still checks output.
- Conversion and half-resolution downsampling execute in one task range, retaining identical four-pixel averaging. Blur compositing and finish are fused, eliminating an intermediate full-image write/read. ATMOS_QA_VERIFY_BLUR compares conversion, radii and final RGBA output against the original separate passes in the same frame. Odd/even dimensions, resizing and UI masks are covered.
- Water-only frames no longer copy/zero unused foliage buffers. Necessary water/foliage clearing runs in the existing row jobs. Integer blur weights are immutable constants; an exhaustive -20..300 radius test proves equivalence to the smoothstep formula.
- Profiling boundaries now distinguish water/foliage displacement, ground composition, water shading, rays and precipitation. Historical “shadow”, “focus” and “lightning” fields also included adjacent stages and must not be interpreted as pure costs.

Intermediate 300-frame measurements: parallel ground raster 38.85 ms Atmosphere mean; fused finish 34.59 ms. A repeat including immutable initialization measured 36.70 ms, illustrating run-to-run scheduling variation. Final-build measurements are stored in artifacts/performance-fullhd/v2-final/benchmark.tsv. No resolution, shadow samples, blur iterations, effect strengths or production quality settings were reduced.

Validation passed: native core with all three byte-equivalence oracles; actual Pacific park rendering and 254 per-ride toggles; ground/water invariance; 576 shadow sun/rotation/zoom/softness sweeps; water/cloud/UI/void compositor; lightning/cloud weather helpers. Existing asset warnings concern duplicate scenery and RCT1 fallback images.

Active build: Project-Atmos-performance-v2.exe. This remains a CPU pipeline; no GPU shader backend has been introduced.

Final build measured over 300 frames: Atmosphere mean 35.32 ms, median 34.43 ms, p95 46.79 ms; native mean 5.29 ms. This is about 33% less Atmosphere time than the preceding 52.50 ms build, and 63% less than the initial 94.84 ms baseline. These are isolated rendering measurements, not total interactive FPS.
