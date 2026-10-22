/*****************************************************************************
 * Atmosphere: screen-space post-processing (vignette, tilt-shift, grading).
 * Fork-only addition, licensed GPLv3 like OpenRCT2.
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenRCT2::Ui::Atmosphere
{
    // Screen rectangle excluded from all effects (UI windows). right/bottom are exclusive.
    struct ExcludeRect
    {
        int32_t left, top, right, bottom;
    };

    // Bit position of each colour channel inside a 32-bit pixel (derived from the SDL texture format).
    struct PixelLayout
    {
        uint32_t redShift, greenShift, blueShift;
    };

    // Colour adjustments. Neutral values leave the picture untouched. They are applied to the game palette, so the user
    // interface keeps its original colours.
    struct ColourGrade
    {
        float exposure = 0.0f;    // EV, -3 .. +3
        float contrast = 0.0f;    // -1 .. +1
        float saturation = 0.0f;  // -1 .. +1
        float vibrance = 0.0f;    // -1 .. +1 (boosts muted colours more than saturated ones)
        float temperature = 0.0f; // -1 (cool) .. +1 (warm)
        float tint = 0.0f;        // -1 (green) .. +1 (magenta)
        float highlights = 0.0f;  // -1 .. +1
        float shadows = 0.0f;     // -1 .. +1
        float gamma = 1.0f;       // 0.5 .. 2.0

        bool operator==(const ColourGrade&) const = default;

        bool IsNeutral() const
        {
            return exposure == 0.0f && contrast == 0.0f && saturation == 0.0f && vibrance == 0.0f && temperature == 0.0f
                && tint == 0.0f && highlights == 0.0f && shadows == 0.0f && gamma == 1.0f;
        }
    };

    // Where the viewport sits on the screen: pixel (px, py) of the frame is at viewport-space coordinates
    // ((px + originX) * worldPerPixel, (py + originY) * worldPerPixel). Needed to turn depth into world positions.
    struct ViewInfo
    {
        float worldPerPixel = 1.0f;
        int32_t originX = 0;
        int32_t originY = 0;
        int32_t rotation = 0;       // view rotation 0..3
        int32_t viewportWidth = 0;  // size of the viewport in pixels
        int32_t viewportHeight = 0;
    };

    // All user settings. The in-game menu edits this structure directly and calls NotifySettingsChanged().
    // Defaults describe the recommended look and apply when no atmosphere.ini exists yet.
    struct Settings
    {
        bool enabled = false;
        float vignetteStrength = 0.5f; // 0 = off, 1 = corners fully black
        float vignetteInner = 0.35f;   // normalised radius where darkening starts
        float tiltStrength = 0.35f;    // 0 = off, 1 = maximum blur radius
        float tiltFocus = 0.5f;        // vertical centre of the sharp band (0 = top, 1 = bottom)
        float tiltBand = 0.22f;        // half height of the sharp band, fraction of screen height
        float tiltFalloff = 0.5f;      // distance (fraction of height) over which blur ramps to maximum
        int32_t tiltIterations = 3;    // box-blur iterations (more = smoother, slower)
        bool tiltDepth = true;         // tilt-shift uses real scene depth instead of the screen row
        float tiltSmooth = 0.0f;       // optional smoothing of the radius plane: 0 = off (keeps depth edges crisp)
        bool tiltNear = true;          // blurred foreground spreads over sharp pixels behind it
        ColourGrade grade;
        std::string activeLut; // name of the last LUT that was applied (display only)
        // directional shadows (screen space, uses the depth plane)
        float shadowStrength = 0.0f;   // 0 = off .. 1 = fully tinted shadows
        float waterWaves = 0.5f;       // gentle moving light and dark bands on water
        float waterDepth = 0.6f;       // shallow water light, deep water dark (follows the distance to the shore)
        float waterPatches = 0.5f;     // large soft dark patches drifting over the water
        float waterSun = 0.6f;         // sparkles get denser towards the sun
        float waterGlitter = 0.5f;     // random sparkles on water
        float leaves = 0.4f;           // leaves fall from the trees now and then
        bool cinemaBars = true;          // black bars at the top and bottom while cinema mode is on
        float cinemaBarHeight = 0.12f;   // each bar, as a fraction of the screen height
        bool cinemaSmooth = true;        // smooth camera while cinema mode is on
        float cinemaSmoothness = 0.6f;   // 0 = quick, 1 = very slow and gentle
        bool cinemaActive = false;       // cinema mode is switched on (not saved)
        float faceShading = 0.0f;      // sides of buildings that face away from the sun get darker, sides facing it brighter
        float shadowSoftness = 0.65f; // soft silhouette edges, independent of opacity
        // Shadow draw distances in map tiles (32 world units); zero keeps full range and disables zoom LOD.
        float shadowGuestDistance = 80.0f;
        float shadowVehicleDistance = 160.0f;
        float shadowTrackDistance = 160.0f;
        float sunAzimuth = 260.0f;     // degrees, direction towards the sun on the map (0 = +x, 90 = +y, 270 = -y)
        float sunElevation = 45.0f;    // degrees above the horizon
        float sunlight = 0.0f;        // 0 = original lighting .. 1 = brighter sunlit colours
        float sunWarmth = 0.0f;       // 0 = neutral .. 1 = warm daylight
        // Clouds, cloud shadows and fog (map space, they move with the wind)
        float cloudCoverage = 0.55f;   // 0 = no clouds .. 1 = overcast
        float cloudSize = 3000.0f;     // size of the largest cloud structures (map units)
        float cloudSpeed = 1.0f;       // wind speed multiplier
        // Weather: rain and snow replace the overlay of the game, they only fall where there are clouds; trees sway in the wind
        bool weatherEffects = true;
        float precipitation = 1.0f;    // amount of rain / snow
        float windStrength = 0.5f;     // slant of rain and snow, sway of trees
        float snowCover = 0.0f;        // how much snow stays on the ground and on the trees (0 = off)
        bool swayTrees = true;
        float swayAmount = 0.5f;
        float cloudWind = 30.0f;       // wind direction (degrees, map space)
        float cloudOpacity = 0.9f;    // visible cloud layer
        float cloudShadow = 0.5f;      // darkening of the ground below the clouds
        bool cloudFocus = true;        // keep the screen centre free of clouds (shrinks with zoom)
        float cloudClear = 0.5f;       // clear area around the screen centre
        float cloudHeight = 700.0f;    // altitude of the cloud layer, defines the offset of the cloud shadows
        float fog = 0.0f;              // ground fog
        bool cloudWeather = true;      // follow the weather of the game
        // Time of day: warm light at dawn and dusk, blue and dark at night, glowing lamps and windows
        bool timeEnabled = false;
        float timeHour = 16.0f;          // 0..24
        bool timeCycle = false;          // the day runs with the game
        float timeCycleMinutes = 8.0f;   // length of a day in game minutes at normal speed
        float nightDarkness = 0.6f;
        int32_t lightSnap = 0;          // precise placement by default: 0 = free, 1 = quarter tile grid, 2 = tile centre
        bool lightsEnabled = true;      // placed light objects are drawn
        float lightsStrength = 1.0f;    // master brightness of all light objects
        // values of the light that is selected in the menu (not saved; copied to and from the light object)
        float editHeight = 40.0f, editRadius = 96.0f, editIntensity = 1.0f, editR = 1.0f, editG = 0.78f, editB = 0.5f, editSpeed = 1.0f;
        float lightningStrength = 0.7f; // 0 disables storm flashes, including the vanilla palette flash
        float lightningInterval = 10.0f; // average interval in seconds while the game weather is thunder
        float sunRays = 0.6f;         // light shafts falling from the sun between the buildings (0 = off)
        float shadowReach = 70.0f;    // bounded directional silhouette reach in map units
        // developer options (not shown in the menu)
        int32_t devZoom = 100;   // 100 = unset
        bool devPerfLog = false;   // writes <ini>.fps.log (frame interval)
        int32_t devWeather = -1;   // forces the weather type of the game (for tests)
        int32_t devScreenshot = 0; // changing this value takes one screenshot (for automated tests)
        int32_t devDump = 0;       // number of finished frames written as .bmp next to the settings file (automated tests)
        int32_t devDumpAfter = 1500; // frames to wait before the first dump
        int32_t devRotate = 0;   // absolute number of clockwise rotation steps requested
        bool devLock = false;    // keep re-applying dev_zoom every frame
        bool debugMask = false;  // tint UI pixels red
        int32_t debugDepth = 0;  // 1 = show scene depth, 2 = show blur radius
        bool devLightningHold = false; // test: freezes a flash at its peak
        bool devLightMarkers = false; // test: always draw the light markers
        int32_t devLightDemo = 0;   // changing this to a non-zero value spawns demo lights around the screen centre (not saved)
        int32_t devOpenOptions = 0; // changes to a new non-zero value: open the Options window on the Atmosphere page
        int32_t devOptionsSub = 0;  // sub page to show (0 = Effects, 1 = Colour, ...)
    };

    // ---- Light objects: freely placed light sources, saved per park ------------------------------------------------
    enum class LightAnim : uint8_t
    {
        None, Flicker, Pulse, Vibrate, Strobe, Blink, Candle, Flash, Count
    };
    struct LightObject
    {
        int32_t id = 0;
        int32_t x = 0, y = 0;     // map position in world units (32 per tile)
        float height = 40.0f;     // above the ground at x/y
        uint8_t r = 255, g = 200, b = 128;
        float intensity = 1.0f;   // 0 .. 2
        float radius = 96.0f;     // world units
        LightAnim anim = LightAnim::None;
        float speed = 1.0f;
        bool on = true;
        bool bulb = true;         // show the glowing light source itself (off: only the light around it is visible)
    };
    int32_t LightCount();
    const LightObject* LightAt(int32_t index);
    int32_t LightSelected();                      // -1 = none
    void LightSelect(int32_t index);
    float LightPlacementHeight(bool useClipboard); // bulb lift of the same template used by LightAdd
    int32_t LightAdd(int32_t x, int32_t y, bool useClipboard); // new light (ground height + default or copied settings), selected
    void LightDelete(int32_t index);
    void LightMove(int32_t index, int32_t x, int32_t y);
    // Applies the snap setting (tile centre / quarter tile / free) to a ground position.
    void LightSnap(int32_t& x, int32_t& y);
    // The light whose bulb, foot or the line between them is closest to a screen position (-1 = none close enough).
    int32_t LightPickAtScreen(int32_t screenX, int32_t screenY);
    // The 3D point of the surface that is visible under a screen position (from the depth of the game picture).
    bool PickSurfacePoint(int32_t screenX, int32_t screenY, int32_t& mapX, int32_t& mapY, int32_t& z);
    // Puts a light on a spot: it hangs `lift` above the given surface height.
    void LightPlaceAt(int32_t index, int32_t x, int32_t y, int32_t surfaceZ, float lift);
    int32_t LightNearest(int32_t x, int32_t y, int32_t maxDistance); // -1 = none close enough
    void LightCopy();
    bool LightHasClipboard();
    void LightLoadEdit();   // selected light -> Settings::edit*
    void LightApplyEdit();  // Settings::edit* -> selected light (also saves later)
    void LightSetOn(int32_t index, bool on);
    void LightSetBulb(int32_t index, bool bulb);
    void LightSetAnim(int32_t index, LightAnim anim);
    int32_t LightColourPresetCount();
    const char* LightColourPresetName(int32_t index);
    void LightApplyColourPreset(int32_t index);   // to the selected light
    const char* LightAnimName(int32_t index);
    void LightsEditPing();        // the menu calls this while the Lights page is open: markers are drawn
    bool LightsEditing();
    std::string LightsFileName(); // for display

    // ---- LUTs: named, saved colour settings (one small text file each, easy to share) --------------------------------
    struct LutInfo
    {
        bool isCube = false; // a standard .cube 3D lookup table (applied to the colours of the game exactly)
        std::string name; // file name without extension
        std::string path; // full path
    };
    std::string LutDirectory();                 // <user data>/atmosphere/luts (created on demand)
    std::vector<LutInfo> ListLuts();            // all *.atmolut files, sorted by name
    // Saves `grade` under `name` (characters that are not allowed in file names are replaced). Overwrites an existing
    // LUT of the same name. Returns the final name, or an empty string on failure.
    std::string SaveLut(const std::string& name, const ColourGrade& grade);
    bool LoadLut(const std::string& path, ColourGrade& grade);
    // .cube files (3D LUT, e.g. exported from a photo editor): replaces the colour sliders while active.
    bool LoadCubeLut(const std::string& path);
    void ClearCubeLut();
    bool CubeLutActive();

    // Complete looks that set many settings at once (names are shown in the menu).
    int32_t PresetCount();
    const char* PresetName(int32_t index);
    void ApplyPreset(int32_t index);
    bool DeleteLut(const std::string& path);

    // Restore the recommended look of the vignette / tilt-shift settings (keeps `enabled` and the colour grade).
    void ResetEffectSettings();
    // Restore the recommended sun / shadow settings.
    void ResetLightSettings();
    // Time of day as text for the menu, e.g. "18:30" (the hour shown depends on the day cycle).
    float CurrentHourOfDay();
    // Restore the recommended cloud settings.
    void ResetCloudSettings();
    // Restore the recommended weather settings.
    void ResetWeatherSettings();
    // True while rain and snow are drawn by Atmosphere (the vanilla overlay must be skipped).
    bool ReplacesWeather();
    // Back to a neutral colour grade.
    void ResetColourGrade();

    // Live settings (UI thread only). After changing a value call NotifySettingsChanged().
    Settings& GetSettings();
    void NotifySettingsChanged();

    // Location of atmosphere.ini inside the user data directory.
    std::string DefaultSettingsFile();

    // Makes sure settings are loaded from `settingsFile` (idempotent). Used by the engine and by the menu.
    void SetSettingsFile(const char* settingsFile);

    // Re-reads atmosphere.ini when it was changed from outside (at most every 500 ms), saves pending menu changes and
    // returns whether any effect is active.
    bool IsActive(const char* settingsFile);
    // True while the effects are running: the toolbars and bars of the game interface are then drawn fully opaque.
    bool OpaqueInterface();
    // Test setting: writes frame rate logs next to the settings file.
    bool PerfLogEnabled();

    // Cinema mode (black bars + smooth camera). Esc switches it off.
    bool CinemaActive();
    void SetCinemaActive(bool active);
    // Height of each bar as a fraction of the screen (0 = none). Also keeps the camera smoothing in sync.
    float CinemaBarFraction();
    // Increases whenever a setting changed (menu, file reload), so cached frames can be recognised as stale.
    uint32_t SettingsRevision();
    // True when an enabled effect changes over time on its own (clouds, weather, sway, snow): such frames are never reused.
    bool IsAnimated();

    // Developer hooks driven by atmosphere.ini (dev_zoom, dev_rotate) so views can be reproduced for testing.
    // Each returns true once whenever the setting changes; `value` receives the new zoom / the rotation steps to apply.
    bool TakeDevZoom(int32_t& value);
    bool TakeDevRotate(int32_t& steps);
    bool TakeDevScreenshot(int32_t& kind);
    // Writes the finished frame as a 24 bit .bmp when dev_dump asks for it (independent of what covers the window).
    void DumpFrame(const uint32_t* pixels, int32_t width, int32_t height, const PixelLayout& layout);
    bool TakeDevWeather(int32_t& type);

    // Test hook (dev_open_options): returns true once per change, `sub` receives the sub page to show.
    bool TakeDevOpenOptions(int32_t& sub);
    // Remembers which Atmosphere sub page the next Options window should open on (-1 = none).
    void SetPendingOptionsSubPage(int32_t sub);
    bool ConsumePendingOptionsSubPage(int32_t& sub);

    // True if the active effects need the scene depth plane (the engine then records it while painting).
    bool WantsDepth();
    // True when the sway plane (tree and foliage marks) is needed: swaying plants or snow on trees.
    bool WantsSway();

    // Converts the 8bpp frame (`bits`, width*height palette indices) to 32bpp in `out` (width*height pixels) using
    // `palette`, applying the enabled effects. Pixels inside `excluded` (UI windows) are converted but never altered.
    // `depth` (optional, same indexing as `bits`) is the scene depth plane, `worldPerPixel` is 2^zoom of the viewport.
    // `world` (optional) is the world-only copy of `bits`: pixels that differ from it are UI and stay untouched.
    // Processes a finished still image (giant screenshot) in strips. `out` receives width * height RGBA pixels.
    // Returns false when the effects are not active.
    bool ProcessStill(
        const uint8_t* bits, int32_t width, int32_t height, const uint32_t* palette, int32_t viewX, int32_t viewY, int32_t zoom,
        int32_t rotation, uint32_t* out);

    void ProcessFrame(
        const uint8_t* bits, const uint32_t* palette, uint32_t* out, int32_t width, int32_t height, const PixelLayout& layout,
        const std::vector<ExcludeRect>& excluded, const uint16_t* depth, const ViewInfo& view,
        const uint8_t* world, const uint8_t* sway = nullptr);
} // namespace OpenRCT2::Ui::Atmosphere
