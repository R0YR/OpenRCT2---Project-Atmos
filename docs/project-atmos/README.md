# Project:Atmos – code overview

Project:Atmos is a fork of OpenRCT2 `develop`. Most of the new code lives in a few new files; the rest are small hooks into existing OpenRCT2 code.

## New files

| Area | Files |
|:--|:--|
| Effects compositor (clouds, weather, lights, shadows, tilt-shift, vignette, colour grading) | `src/openrct2-ui/drawing/Atmosphere.cpp`, `Atmosphere.h` |
| Helpers for the compositor | `AtmosphereCoordinates.h` (map ↔ screen), `AtmosphereLeaf.h` (falling leaves), `AtmosphereLightPlacement.h` (light objects), `AtmosphereRideShadows.h`, `AtmosphereSceneryShadows.h`, `AtmosphereShadowDistance.h` |
| Scene depth (used by tilt-shift and shadows) | `src/openrct2/drawing/DepthPlane.cpp`, `DepthPlane.h` |
| Settings window ("Atmosphere" tab group) | `src/openrct2-ui/windows/OptionsAtmosphere.cpp`, `.h`, `AtmosphereWindow.cpp` |
| Foliage selection (swaying trees, falling leaves per object) | `src/openrct2/drawing/AtmosphereFoliage.h` |
| Scenery support handling for shadows | `src/openrct2/paint/tile_element/ScenerySupports.h` |
| Guest variety | `src/openrct2/peep/GuestAppearance.h`, `resources/guest-diversity/objects/` |
| Title screen banner | `resources/project-atmos/project-atmos-logo.png` |

## Hooks into OpenRCT2

The compositor runs after the normal frame has been drawn (`HardwareDisplayDrawingEngine`, `X8DrawingEngine`). The painter records depth and shadow receivers (`Paint.cpp`, `Drawing.Sprite*.cpp`). The remaining changes add the options window, a toolbar/shortcut entry (`Ctrl+Shift+A`) and the foliage toggle in the scenery window.

## Settings

Settings are stored in `atmosphere.ini` in the user directory. Some keys starting with `dev_` are test hooks used during development; they are not meant for normal use.

## Known limits

- Shadows are a geometric approximation, not full 3D silhouettes.
- Performance is not fully optimised yet.
