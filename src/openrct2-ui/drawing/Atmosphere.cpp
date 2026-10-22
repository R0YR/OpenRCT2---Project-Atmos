#include <openrct2/drawing/Drawing.Screen.h>
#include <openrct2/drawing/AtmosphereFoliage.h>
#include <array>
/*****************************************************************************
 * Atmosphere: screen-space post-processing. See Atmosphere.h.
 *
 * Frame pipeline (all stages run on a small worker pool, split by rows or column strips):
 *   1. Convert   8bpp palette frame -> 32bpp "premultiplied" buffer A. UI pixels become 0, world pixels get the
 *                unused byte lane set to 255. That lane is a coverage weight, so blurs ignore UI pixels
 *                (no toolbar colours smeared into the world).
 *   2. Tilt-shift  box blur with a per-row radius, `iterations` x (horizontal pass A->B, vertical pass B->A).
 *   3. Finish    normalise by coverage, apply vignette, write the final frame. UI pixels are restored from the palette.
 * Without tilt-shift, stages 1-2 are skipped and Finish converts directly.
 *****************************************************************************/

#include "Atmosphere.h"
#include "AtmosphereCoordinates.h"
#include "AtmosphereLeaf.h"
#include "AtmosphereLightPlacement.h"
#include "AtmosphereSceneryShadows.h"
#include "AtmosphereShadowDistance.h"
#include "AtmosphereRideShadows.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <openrct2/Context.h>
#include <openrct2/config/Config.h>
#include <openrct2/GameState.h>
#include <openrct2/drawing/DepthPlane.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/Peep.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/object/LargeSceneryEntry.h>
#include <openrct2/object/Object.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/PathAdditionEntry.h>
#include <openrct2/object/SmallSceneryEntry.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/Weather.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/LargeSceneryElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/core/FileSystem.hpp>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace OpenRCT2::Ui::Atmosphere
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        Settings _settings;
        Clock::time_point _lastSettingsCheck{};
        std::string _settingsPath;
        std::filesystem::file_time_type _knownMtime{};
        bool _settingsDirty = false;
        Clock::time_point _dirtySince{};

        // Frame-time feedback (the only observability this module has).
        double _accumMs = 0;
        int32_t _accumFrames = 0;
        // Geometry of the full image while a still image is processed in strips (vignette and tilt-shift need it).
        struct StillGeometry
        {
            bool active = false;
            int32_t fullHeight = 0, rowOffset = 0;
        };
        StillGeometry _still;

        bool _lastActive = false; // result of the last IsActive call
        uint32_t _settingsRevision = 1;
        float _precipLevelNow = 0.0f; // strength of rain / snow, set by the cloud stage (storms shake the trees harder)

        enum Stage { kStageCloud, kStageShadow, kStageMask, kStageFocus, kStageRadBuild, kStageRadClose, kStageRadNear, kStageRadSmooth, kStageRadMax, kStageConvert, kStageDown, kStageBlurH, kStageBlurV, kStageComposite, kStageFinish, kStageLights, kStageLightning, kStageSway, kStageGround, kStageWater, kStageFaces, kStageRays, kStagePrecip, kStageCount };
        const char* const kStageNames[kStageCount] = { "cloud", "shadow", "mask", "focus", "radBuild", "radClose", "nearBuild", "nearProp+smooth", "nearApply+max", "convert", "down", "blurH", "blurV", "composite", "finish", "lights", "lightning", "sway+waterWarp", "ground", "water", "faces", "rays", "precip" };
        double _stageMs[kStageCount] = {};

        // ---------------------------------------------------------------------------------------------------------
        // Worker pool: ParallelFor(count, fn) runs fn(0..count-1) on all workers plus the calling thread.
        // ---------------------------------------------------------------------------------------------------------
        class WorkerPool
        {
        public:
            static WorkerPool& Get()
            {
                static WorkerPool pool;
                return pool;
            }

            void ParallelFor(int32_t count, const std::function<void(int32_t)>& fn)
            {
                if (count <= 0)
                    return;
                if (_threads.empty() || count == 1)
                {
                    for (int32_t i = 0; i < count; i++)
                        fn(i);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _fn = &fn;
                    _count = count;
                    _next.store(0);
                    _busy.store(static_cast<int32_t>(_threads.size()));
                    _generation.fetch_add(1, std::memory_order_release);
                }
                _cvWork.notify_all();
                Drain();
                // Workers usually finish within microseconds: spin briefly before blocking.
                const auto spinEnd = Clock::now() + std::chrono::microseconds(300);
                while (_busy.load(std::memory_order_acquire) != 0 && Clock::now() < spinEnd)
                    _mm_pause();
                if (_busy.load(std::memory_order_acquire) != 0)
                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    _cvDone.wait(lock, [this] { return _busy.load() == 0; });
                }
            }

            WorkerPool(const WorkerPool&) = delete;
            WorkerPool& operator=(const WorkerPool&) = delete;

        private:
            WorkerPool()
            {
                const auto hw = std::thread::hardware_concurrency();
                const auto total = std::clamp<uint32_t>(hw == 0 ? 4 : hw, 2, 12);
                for (uint32_t i = 0; i + 1 < total; i++)
                    _threads.emplace_back([this] { Loop(); });
            }

            ~WorkerPool()
            {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _stop.store(true);
                }
                _cvWork.notify_all();
                for (auto& t : _threads)
                    t.join();
            }

            void Drain()
            {
                for (;;)
                {
                    const int32_t i = _next.fetch_add(1);
                    if (i >= _count)
                        break;
                    (*_fn)(i);
                }
            }

            void Loop()
            {
                uint64_t seen = 0;
                for (;;)
                {
                    // Stay hot for a short while: passes of one frame follow each other within microseconds.
                    const auto spinEnd = Clock::now() + std::chrono::microseconds(150);
                    while (_generation.load(std::memory_order_acquire) == seen && !_stop.load() && Clock::now() < spinEnd)
                        _mm_pause();
                    if (_generation.load(std::memory_order_acquire) == seen && !_stop.load())
                    {
                        std::unique_lock<std::mutex> lock(_mutex);
                        _cvWork.wait(lock, [&] { return _stop.load() || _generation.load() != seen; });
                    }
                    if (_stop.load())
                        return;
                    seen = _generation.load(std::memory_order_acquire);
                    Drain();
                    if (_busy.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        _cvDone.notify_one();
                    }
                }
            }

            std::vector<std::thread> _threads;
            std::mutex _mutex;
            std::condition_variable _cvWork, _cvDone;
            const std::function<void(int32_t)>* _fn = nullptr;
            std::atomic<int32_t> _next{ 0 };
            int32_t _count = 0;
            std::atomic<int32_t> _busy{ 0 };
            std::atomic<uint64_t> _generation{ 0 };
            std::atomic<bool> _stop{ false };
        };

        // ---------------------------------------------------------------------------------------------------------
        // Settings
        // ---------------------------------------------------------------------------------------------------------
        std::string Trim(const std::string& s)
        {
            auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos)
                return {};
            auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        float ParseFloat(const std::string& v, float fallback)
        {
            char* end = nullptr;
            float f = std::strtof(v.c_str(), &end);
            return (end == v.c_str() || !std::isfinite(f)) ? fallback : f;
        }

        // A file that exists but lacks a key means "off" for that key (older/test files); a missing file uses the defaults.
        Settings LoadSettings(const std::string& path, bool* existed = nullptr)
        {
            Settings s;
            std::ifstream in(path);
            if (existed != nullptr)
                *existed = static_cast<bool>(in);
            if (!in)
                return s; // no file yet: recommended defaults, but disabled
            s.vignetteStrength = 0.0f;
            s.vignetteInner = 0.45f;
            s.tiltStrength = 0.0f;
            s.tiltBand = 0.15f;
            s.tiltFalloff = 0.30f;
            s.tiltIterations = 2;
            s.shadowStrength = 0.0f;
            s.sunlight = 0.0f;
            s.sunWarmth = 0.0f;
            s.faceShading = 0.0f;
            s.sunAzimuth = 260.0f;
            s.cloudCoverage = 0.0f;
            s.fog = 0.0f;
            s.weatherEffects = true;
            s.swayTrees = true;
            s.snowCover = 0.0f;
            std::string line;
            while (std::getline(in, line))
            {
                auto hash = line.find('#');
                if (hash != std::string::npos)
                    line.resize(hash);
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                auto key = Trim(line.substr(0, eq));
                auto val = Trim(line.substr(eq + 1));
                if (key == "enabled")
                    s.enabled = (val == "1" || val == "true");
                else if (key == "debug_mask")
                    s.debugMask = (val == "1" || val == "true");
                else if (key == "dev_zoom")
                    s.devZoom = static_cast<int32_t>(ParseFloat(val, 100.0f));
                else if (key == "dev_lock")
                    s.devLock = (val == "1" || val == "true");
                else if (key == "dev_rotate")
                    s.devRotate = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "dev_perf_log")
                    s.devPerfLog = (val == "1" || val == "true");
                else if (key == "dev_weather")
                    s.devWeather = static_cast<int32_t>(ParseFloat(val, -1.0f));
                else if (key == "dev_screenshot")
                    s.devScreenshot = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "dev_dump")
                    s.devDump = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "dev_dump_after")
                    s.devDumpAfter = static_cast<int32_t>(ParseFloat(val, 1500.0f));
                else if (key == "dev_open_options")
                    s.devOpenOptions = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "dev_light_demo")
                    s.devLightDemo = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "dev_lightning_hold")
                    s.devLightningHold = (val == "1");
                else if (key == "dev_light_markers")
                    s.devLightMarkers = (val == "1");
                else if (key == "dev_options_sub")
                    s.devOptionsSub = static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "debug_depth")
                    s.debugDepth = (val == "true") ? 1 : static_cast<int32_t>(ParseFloat(val, 0.0f));
                else if (key == "tilt_depth")
                    s.tiltDepth = (val == "1" || val == "true");
                else if (key == "vignette")
                    s.vignetteStrength = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "vignette_inner")
                    s.vignetteInner = std::clamp(ParseFloat(val, 0.45f), 0.0f, 0.95f);
                else if (key == "tilt_shift")
                    s.tiltStrength = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "tilt_focus")
                    s.tiltFocus = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "tilt_band")
                    s.tiltBand = std::clamp(ParseFloat(val, 0.15f), 0.0f, 0.5f);
                else if (key == "tilt_falloff")
                    s.tiltFalloff = std::clamp(ParseFloat(val, 0.30f), 0.02f, 1.0f);
                else if (key == "tilt_smooth")
                    s.tiltSmooth = std::clamp(ParseFloat(val, 0.0f), 0.0f, 4.0f);
                else if (key == "shadow_strength")
                    s.shadowStrength = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "water_depth")
                    s.waterDepth = std::clamp(ParseFloat(val, 0.6f), 0.0f, 1.0f);
                else if (key == "water_patches")
                    s.waterPatches = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "water_sun")
                    s.waterSun = std::clamp(ParseFloat(val, 0.6f), 0.0f, 1.0f);
                else if (key == "water_waves")
                    s.waterWaves = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "water_glitter")
                    s.waterGlitter = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "falling_leaves")
                    s.leaves = std::clamp(ParseFloat(val, 0.4f), 0.0f, 1.0f);
                else if (key == "dev_cinema")
                    s.cinemaActive = (val == "1");
                else if (key == "cinema_bars")
                    s.cinemaBars = (val == "1" || val == "true");
                else if (key == "cinema_bar_height")
                    s.cinemaBarHeight = std::clamp(ParseFloat(val, 0.12f), 0.03f, 0.3f);
                else if (key == "cinema_smooth")
                    s.cinemaSmooth = (val == "1" || val == "true");
                else if (key == "cinema_smoothness")
                    s.cinemaSmoothness = std::clamp(ParseFloat(val, 0.6f), 0.0f, 1.0f);
                else if (key == "face_shading")
                    s.faceShading = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "shadow_guest_distance")
                    s.shadowGuestDistance = std::clamp(ParseFloat(val, 80.0f), 0.0f, 256.0f);
                else if (key == "shadow_vehicle_distance")
                    s.shadowVehicleDistance = std::clamp(ParseFloat(val, 160.0f), 0.0f, 256.0f);
                else if (key == "shadow_track_distance")
                    s.shadowTrackDistance = std::clamp(ParseFloat(val, 160.0f), 0.0f, 256.0f);
                else if (key == "shadow_softness")
                    s.shadowSoftness = std::clamp(ParseFloat(val, 0.65f), 0.0f, 1.0f);
                else if (key == "sun_azimuth")
                    s.sunAzimuth = ParseFloat(val, 250.0f);
                else if (key == "sun_elevation")
                    s.sunElevation = std::clamp(ParseFloat(val, 40.0f), 5.0f, 85.0f);
                else if (key == "sunlight")
                    s.sunlight = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "sun_warmth")
                    s.sunWarmth = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "sun_rays")
                    s.sunRays = std::clamp(ParseFloat(val, 0.4f), 0.0f, 1.0f);
                else if (key == "shadow_reach")
                    s.shadowReach = std::clamp(ParseFloat(val, 220.0f), 20.0f, 800.0f);
                else if (key == "weather_effects")
                    s.weatherEffects = (val == "1" || val == "true");
                else if (key == "precipitation")
                    s.precipitation = std::clamp(ParseFloat(val, 1.0f), 0.0f, 1.5f);
                else if (key == "wind_strength")
                    s.windStrength = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "time_enabled")
                    s.timeEnabled = (val == "1" || val == "true");
                else if (key == "time_hour")
                    s.timeHour = std::clamp(ParseFloat(val, 16.0f), 0.0f, 24.0f);
                else if (key == "time_cycle")
                    s.timeCycle = (val == "1" || val == "true");
                else if (key == "time_cycle_minutes")
                    s.timeCycleMinutes = std::clamp(ParseFloat(val, 8.0f), 1.0f, 120.0f);
                else if (key == "night_darkness")
                    s.nightDarkness = std::clamp(ParseFloat(val, 0.6f), 0.0f, 1.0f);
                else if (key == "lights_enabled")
                    s.lightsEnabled = (val == "1" || val == "true");
                else if (key == "light_snap")
                    s.lightSnap = std::clamp(static_cast<int32_t>(ParseFloat(val, 0.0f)), 0, 2);
                else if (key == "lights_strength")
                    s.lightsStrength = std::clamp(ParseFloat(val, 1.0f), 0.0f, 1.5f);
                else if (key == "lightning_strength")
                    s.lightningStrength = std::clamp(ParseFloat(val, 0.4f), 0.0f, 1.0f);
                else if (key == "lightning_interval")
                    s.lightningInterval = std::clamp(ParseFloat(val, 20.0f), 5.0f, 90.0f);
                else if (key == "sway_trees")
                    s.swayTrees = (val == "1" || val == "true");
                else if (key == "sway_amount")
                    s.swayAmount = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "clouds")
                    s.cloudCoverage = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "cloud_size")
                    s.cloudSize = std::clamp(ParseFloat(val, 3000.0f), 300.0f, 9000.0f);
                else if (key == "cloud_speed")
                    s.cloudSpeed = std::clamp(ParseFloat(val, 1.0f), 0.0f, 6.0f);
                else if (key == "cloud_wind")
                    s.cloudWind = ParseFloat(val, 30.0f);
                else if (key == "cloud_opacity")
                    s.cloudOpacity = std::clamp(ParseFloat(val, 0.75f), 0.0f, 1.0f);
                else if (key == "cloud_shadow")
                    s.cloudShadow = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "cloud_focus")
                    s.cloudFocus = (val == "1" || val == "true");
                else if (key == "cloud_clear")
                    s.cloudClear = std::clamp(ParseFloat(val, 0.5f), 0.0f, 1.0f);
                else if (key == "cloud_height")
                    s.cloudHeight = std::clamp(ParseFloat(val, 700.0f), 100.0f, 2000.0f);
                else if (key == "fog")
                    s.fog = std::clamp(ParseFloat(val, 0.0f), 0.0f, 1.0f);
                else if (key == "cloud_weather")
                    s.cloudWeather = (val == "1" || val == "true");
                else if (key == "lut")
                    s.activeLut = val;
                else if (key == "grade_exposure")
                    s.grade.exposure = std::clamp(ParseFloat(val, 0.0f), -3.0f, 3.0f);
                else if (key == "grade_contrast")
                    s.grade.contrast = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_saturation")
                    s.grade.saturation = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_vibrance")
                    s.grade.vibrance = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_temperature")
                    s.grade.temperature = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_tint")
                    s.grade.tint = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_highlights")
                    s.grade.highlights = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_shadows")
                    s.grade.shadows = std::clamp(ParseFloat(val, 0.0f), -1.0f, 1.0f);
                else if (key == "grade_gamma")
                    s.grade.gamma = std::clamp(ParseFloat(val, 1.0f), 0.5f, 2.0f);
                else if (key == "tilt_near")
                    s.tiltNear = (val == "1" || val == "true");
                else if (key == "tilt_iterations")
                    s.tiltIterations = std::clamp(static_cast<int32_t>(ParseFloat(val, 2.0f)), 1, 3);
            }
            return s;
        }

        void SaveSettings(const std::string& path, const Settings& v)
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out)
                return;
            out << "# Atmosphere settings (written by the in-game menu, hot-reloaded when edited by hand)\n";
            out << "enabled = " << (v.enabled ? 1 : 0) << "\n";
            out << "vignette = " << v.vignetteStrength << "\n";
            out << "vignette_inner = " << v.vignetteInner << "\n";
            out << "tilt_shift = " << v.tiltStrength << "\n";
            out << "tilt_focus = " << v.tiltFocus << "\n";
            out << "tilt_band = " << v.tiltBand << "\n";
            out << "tilt_falloff = " << v.tiltFalloff << "\n";
            out << "tilt_iterations = " << v.tiltIterations << "\n";
            out << "tilt_depth = " << (v.tiltDepth ? 1 : 0) << "\n";
            out << "tilt_near = " << (v.tiltNear ? 1 : 0) << "\n";
            out << "tilt_smooth = " << v.tiltSmooth << "\n";
            out << "shadow_strength = " << v.shadowStrength << "\n";
            out << "water_depth = " << v.waterDepth << "\n";
            out << "water_patches = " << v.waterPatches << "\n";
            out << "water_sun = " << v.waterSun << "\n";
            out << "water_waves = " << v.waterWaves << "\n";
            out << "water_glitter = " << v.waterGlitter << "\n";
            out << "falling_leaves = " << v.leaves << "\n";
            out << "cinema_bars = " << (v.cinemaBars ? 1 : 0) << "\n";
            out << "cinema_bar_height = " << v.cinemaBarHeight << "\n";
            out << "cinema_smooth = " << (v.cinemaSmooth ? 1 : 0) << "\n";
            out << "cinema_smoothness = " << v.cinemaSmoothness << "\n";
            out << "face_shading = " << v.faceShading << "\n";
            out << "shadow_softness = " << v.shadowSoftness << "\n";
            out << "shadow_guest_distance = " << v.shadowGuestDistance << "\n";
            out << "shadow_vehicle_distance = " << v.shadowVehicleDistance << "\n";
            out << "shadow_track_distance = " << v.shadowTrackDistance << "\n";
            out << "sun_azimuth = " << v.sunAzimuth << "\n";
            out << "sun_elevation = " << v.sunElevation << "\n";
            out << "sunlight = " << v.sunlight << "\n";
            out << "sun_warmth = " << v.sunWarmth << "\n";
            out << "sun_rays = " << v.sunRays << "\n";
            out << "shadow_reach = " << v.shadowReach << "\n";
            out << "weather_effects = " << (v.weatherEffects ? 1 : 0) << "\n";
            out << "precipitation = " << v.precipitation << "\n";
            out << "wind_strength = " << v.windStrength << "\n";
            out << "time_enabled = " << (v.timeEnabled ? 1 : 0) << "\n";
            out << "time_hour = " << v.timeHour << "\n";
            out << "time_cycle = " << (v.timeCycle ? 1 : 0) << "\n";
            out << "time_cycle_minutes = " << v.timeCycleMinutes << "\n";
            out << "night_darkness = " << v.nightDarkness << "\n";
            out << "lights_enabled = " << (v.lightsEnabled ? 1 : 0) << "\n";
            out << "light_snap = " << v.lightSnap << "\n";
            out << "lights_strength = " << v.lightsStrength << "\n";
            out << "snow_cover = " << v.snowCover << "\n";
            out << "sway_trees = " << (v.swayTrees ? 1 : 0) << "\n";
            out << "sway_amount = " << v.swayAmount << "\n";
            out << "clouds = " << v.cloudCoverage << "\n";
            out << "cloud_size = " << v.cloudSize << "\n";
            out << "cloud_speed = " << v.cloudSpeed << "\n";
            out << "cloud_wind = " << v.cloudWind << "\n";
            out << "cloud_opacity = " << v.cloudOpacity << "\n";
            out << "cloud_shadow = " << v.cloudShadow << "\n";
            out << "cloud_focus = " << (v.cloudFocus ? 1 : 0) << "\n";
            out << "cloud_clear = " << v.cloudClear << "\n";
            out << "cloud_height = " << v.cloudHeight << "\n";
            out << "fog = " << v.fog << "\n";
            out << "cloud_weather = " << (v.cloudWeather ? 1 : 0) << "\n";
            if (!v.activeLut.empty())
                out << "lut = " << v.activeLut << "\n";
            out << "grade_exposure = " << v.grade.exposure << "\n";
            out << "grade_contrast = " << v.grade.contrast << "\n";
            out << "grade_saturation = " << v.grade.saturation << "\n";
            out << "grade_vibrance = " << v.grade.vibrance << "\n";
            out << "grade_temperature = " << v.grade.temperature << "\n";
            out << "grade_tint = " << v.grade.tint << "\n";
            out << "grade_highlights = " << v.grade.highlights << "\n";
            out << "grade_shadows = " << v.grade.shadows << "\n";
            out << "grade_gamma = " << v.grade.gamma << "\n";
            if (v.devLock)
                out << "dev_lock = 1\n";
            if (v.devZoom != 100)
                out << "dev_zoom = " << v.devZoom << "\n";
            if (v.devRotate != 0)
                out << "dev_rotate = " << v.devRotate << "\n";
            if (v.debugMask)
                out << "debug_mask = 1\n";
            if (v.debugDepth != 0)
                out << "debug_depth = " << v.debugDepth << "\n";
        }

        std::filesystem::file_time_type FileTime(const std::string& path)
        {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(path, ec);
            return ec ? std::filesystem::file_time_type{} : t;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Frame state (resized lazily, main thread only)
        // ---------------------------------------------------------------------------------------------------------
        struct Span
        {
            int32_t from, to;
        };

        struct FrameState
        {
            int32_t width = 0, height = 0;
            std::vector<uint32_t> bufA, bufB, groundColour;
            std::vector<uint16_t> leafDepth; // transient scene depth with visible leaf pixels for depth-of-field
            std::vector<uint32_t> lifeA, lifeB; // rain, snow and leaves as their own premultiplied layer (own blur)
            std::vector<uint8_t> lifeRadius, lifeRowMax;
            std::vector<uint16_t> vignetteMap; // 0..256 per pixel
            float mapStrength = -1.0f, mapInner = -1.0f;
            std::vector<int32_t> radius; // blur radius per row (fallback where no depth is known)
            std::vector<uint8_t> radiusRaw, radiusSmooth, radiusFinal; // per-pixel blur radius planes
            std::vector<uint8_t> uiMask;                               // 1 = user interface pixel (never touched)
            // Half resolution working set for the blur.
            int32_t halfWidth = 0, halfHeight = 0;
            std::vector<uint32_t> halfA, halfB;
            std::vector<uint8_t> radiusHalf; // blur radius in quarter half-resolution pixels (4 = one half-res pixel)
            std::vector<uint8_t> rowMaxHalf;
            // Directional shadows (half resolution): raw rays, blurred result
            std::vector<uint8_t> shadowRaw, shadowSoft;
            std::vector<uint8_t> sceneryShadow; // full-resolution, receiver-height-tested entity shadows
            struct SceneryPoint { int px; float x, y, z; };
            std::vector<std::vector<SceneryPoint>> sceneryRows;
            std::vector<uint8_t> sceneryInputFlags;
            std::vector<uint16_t> sceneryInputDepth;
            bool sceneryCacheValid = false;
            int sceneryCacheWidth = 0, sceneryCacheHeight = 0, sceneryCacheX = 0, sceneryCacheY = 0, sceneryCacheRotation = 0;
            float sceneryCacheWpp = 0;


            bool hasFaceShading = false;
            std::vector<uint8_t> faceVal;   // 1 = wall facing +x (screen left), 2 = wall facing +y (screen right)
            std::vector<int16_t> faceDepth; // depth (map units / 4) of that wall
            std::vector<int16_t> shadowZ, shadowZTmp; // ground height under each shadow pixel
            std::vector<uint16_t> shadowCoverage; // 4x4 sample union per half-resolution cell
            std::vector<uint8_t> rayPlane;               // sun shafts, quarter resolution
            int32_t rayW = 0, rayH = 0;
            bool rayValid = false;
            std::vector<uint8_t> lightR, lightG, lightB; // unused (lights are lit per pixel with depth now) // light objects, one plane per colour channel (half resolution)
            std::vector<int32_t> lightMin, lightMax;   // per half-res row: horizontal range that holds any light
            std::vector<uint8_t> cloudSeg, cloudSegTmp;
            std::vector<uint8_t> snowPlane; // half resolution snow cover looked up from the snow map
            std::vector<uint8_t> swayAltBits, swayFracBits;
            std::vector<uint8_t> swayBits; // copy of the frame with swaying plants displaced
            std::vector<uint32_t> waterColour; // fractional water displacement, before world lighting/depth-of-field
            // Clouds (half resolution planes, 0..255) and their coarse node grid
            std::vector<uint8_t> cloudShadowPlane, cloudAlphaPlane, cloudLitPlane, fogPlane, precipPlane, cloudRawPlane, skyPlane;
            std::vector<float> nodeCloud, nodeLit, nodeShadow, nodeFog;
            int32_t nodesX = 0, nodesY = 0;
            // depth -> blur radius table, centred on the focus depth (stored depth units)
            std::vector<uint8_t> radiusLut;
            int32_t lutHalf = 0, lutFocusStored = 0;
            uint8_t lutFar = 0;
            // Coarse grid used to spread the blur of near objects over the sharp pixels behind them.
            int32_t cell = 4, cellsX = 0, cellsY = 0;
            std::vector<uint8_t> cellRadius;
            std::vector<uint16_t> cellDepth;
            // Per column / per row cell indices and interpolation weights (0..256) of the grid.
            std::vector<int32_t> tableX0, tableX1, tableY0, tableY1;
            std::vector<uint16_t> tableWX, tableWY;
            std::vector<uint8_t> rowMax;                               // max radius per row of radiusFinal
            float focusSmoothed = 0.0f, lastWorldPerPixel = 0.0f;
            bool focusValid = false;
        };
        FrameState _frame;

        void RebuildVignetteMap(FrameState& f)
        {
            f.vignetteMap.assign(static_cast<size_t>(f.width) * f.height, 256);
            const float cx = (f.width - 1) * 0.5f;
            const int32_t fullHeight = _still.active ? _still.fullHeight : f.height;
            const int32_t rowOffset = _still.active ? _still.rowOffset : 0;
            const float cy = (fullHeight - 1) * 0.5f;
            const float inner = _settings.vignetteInner;
            const float strength = _settings.vignetteStrength;
            for (int32_t y = 0; y < f.height; y++)
            {
                const float ny = (static_cast<float>(y + rowOffset) - cy) / cy;
                for (int32_t x = 0; x < f.width; x++)
                {
                    const float nx = (x - cx) / cx;
                    const float d = std::sqrt(nx * nx + ny * ny) * 0.70710678f; // 0 centre .. 1 corners
                    float t = std::clamp((d - inner) / (1.0f - inner), 0.0f, 1.0f);
                    t = t * t * (3.0f - 2.0f * t); // smoothstep
                    f.vignetteMap[static_cast<size_t>(y) * f.width + x] = static_cast<uint16_t>((1.0f - strength * t) * 256.0f + 0.5f);
                }
            }
            f.mapStrength = strength;
            f.mapInner = inner;
        }

        void BuildRadii(FrameState& f)
        {
            f.radius.assign(f.height, 0);
            if (_settings.tiltStrength <= 0.0f)
                return;
            const int32_t fullHeight = _still.active ? _still.fullHeight : f.height;
            const int32_t rowOffset = _still.active ? _still.rowOffset : 0;
            const float maxRadius = std::min(120.0f, 32.0f * _settings.tiltStrength * static_cast<float>(fullHeight) / 1080.0f);
            for (int32_t y = 0; y < f.height; y++)
            {
                const float d = std::abs((static_cast<float>(y + rowOffset) + 0.5f) / static_cast<float>(fullHeight) - _settings.tiltFocus);
                float t = std::clamp((d - _settings.tiltBand) / _settings.tiltFalloff, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                f.radius[y] = static_cast<int32_t>(maxRadius * t + 0.5f);
            }
        }

        // Merged, sorted list of the excluded x-intervals on row y (clipped to [0, width)).
        int32_t RowSpans(int32_t y, int32_t width, const std::vector<ExcludeRect>& excluded, Span* out, int32_t cap)
        {
            int32_t n = 0;
            for (const auto& r : excluded)
            {
                if (y < r.top || y >= r.bottom)
                    continue;
                const int32_t l = std::max(r.left, 0);
                const int32_t rr = std::min(r.right, width);
                if (l >= rr || n >= cap)
                    continue;
                out[n++] = { l, rr };
            }
            std::sort(out, out + n, [](const Span& a, const Span& b) { return a.from < b.from; });
            int32_t m = 0;
            for (int32_t i = 0; i < n; i++)
            {
                if (m > 0 && out[i].from <= out[m - 1].to)
                    out[m - 1].to = std::max(out[m - 1].to, out[i].to);
                else
                    out[m++] = out[i];
            }
            return m;
        }

        constexpr int32_t kMaxSpans = 192;

        // Multiplies all four byte lanes of p by m/256 (m in 0..256). No cross-lane overflow: 255*256 < 65536.
        inline uint32_t ScaleLanes(uint32_t p, uint32_t m)
        {
            const uint32_t even = (((p & 0x00FF00FFu) * m) >> 8) & 0x00FF00FFu;
            const uint32_t odd = ((((p >> 8) & 0x00FF00FFu) * m) >> 8) & 0x00FF00FFu;
            return even | (odd << 8);
        }

        // Same as ScaleLanes but rounds to nearest.
        inline uint32_t ScaleLanesRound(uint32_t p, uint32_t m)
        {
            const uint32_t even = (((p & 0x00FF00FFu) * m + 0x00800080u) >> 8) & 0x00FF00FFu;
            const uint32_t odd = (((((p >> 8) & 0x00FF00FFu) * m) + 0x00800080u) >> 8) & 0x00FF00FFu;
            return even | (odd << 8);
        }

        // Weight a source pixel gets when other pixels are blurred around it: sharp pixels (radius 0) contribute little so
        // they do not bleed into blurred surroundings, blurred pixels (radius >= 3) contribute fully.
        uint8_t SourceWeight(int32_t radius)
        {
            // Rounded smoothstep weights for integer radii 0,1,2; every radius >=3 is fully weighted.
            // Immutable constants need neither per-pixel lazy-initialization guards nor worker synchronization.
            static constexpr uint8_t lut[4] = { 24, 84, 195, 255 };
            return lut[std::clamp(radius, 0, 3)];
        }

        inline __m128i UnpackLanes(uint32_t p)
        {
            const __m128i zero = _mm_setzero_si128();
            __m128i v = _mm_cvtsi32_si128(static_cast<int>(p));
            v = _mm_unpacklo_epi8(v, zero);
            return _mm_unpacklo_epi16(v, zero);
        }

        // Rounds sum * invCount back to four byte lanes.
        inline uint32_t AverageLanes(__m128i sum, float invCount)
        {
            const __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(sum), _mm_set1_ps(invCount));
            const __m128i i32 = _mm_cvtps_epi32(f);
            const __m128i i16 = _mm_packs_epi32(i32, i32);
            const __m128i i8 = _mm_packus_epi16(i16, i16);
            return static_cast<uint32_t>(_mm_cvtsi128_si32(i8));
        }

        constexpr int32_t kRowChunk = 8;
        constexpr int32_t kStripWidth = 16;

        struct Frame
        {
            const uint8_t* bits;
            const uint32_t* palette;
            uint32_t* out;
            int32_t width, height;
            uint32_t freeShift;
            const std::vector<ExcludeRect>* excluded;
            uint32_t rShift, gShift, bShift;
            const uint8_t* ui; // per-pixel UI mask (1 = UI)
            const uint32_t* worldPalette; // graded palette for world pixels (palette = original, used for the UI)
            const uint16_t* depth = nullptr; // unknown also includes blended water; not a world coverage mask
            float worldPerPixel = 1.0f;
            const uint8_t* shadowFlags = nullptr;
            const uint8_t* swayAlt = nullptr;  // second palette index of a swaying pixel (the one beyond the sub pixel offset)
            const uint8_t* swayFrac = nullptr; // 0..255: how much of the second index is mixed in
            const uint32_t* groundColour = nullptr; // world lighting/shadows, before depth-of-field filtering
            const uint32_t* waterColour = nullptr; // bilinear water samples, zero outside displaced water
        };

        // Exterior is owned by the native painter, pixel by pixel. A balloon must never change
        // the classification of the empty pixels between itself and the edge of the viewport.
        void ProtectExteriorRow(const uint8_t* world, uint8_t* mask, int32_t width,
            const uint8_t* surfaceFlags, const uint16_t* depth)
        {
            for (int32_t x = 0; x < width; x++)
            {
                if (mask[x] != 0)
                    continue;
                const bool nativeExterior = surfaceFlags != nullptr
                    && (surfaceFlags[x] & Drawing::kExteriorSurface) != 0;
                // Native weather filters can recolour an empty viewport. Coverage, not its resulting
                // palette index, decides whether it may receive ground lighting.
                const bool emptyBackdrop = (surfaceFlags == nullptr || surfaceFlags[x] == 0)
                    && (depth != nullptr ? depth[x] == Drawing::kDepthUnknown
                        : (world[x] == static_cast<uint8_t>(Drawing::PaletteIndex::transparent)
                            || world[x] == static_cast<uint8_t>(Drawing::PaletteIndex::pi10)));
                if (nativeExterior || emptyBackdrop)
                    mask[x] = 2;
            }
        }

        // Stage 0: protected mask = UI windows/different world pixels, plus the original dark exterior.
        void BuildUiMaskRows(const Frame& fr, const uint8_t* world, uint8_t* ui, int32_t y0, int32_t y1, [[maybe_unused]] const ViewInfo& view)
        {
            Span spans[kMaxSpans];
            for (int32_t y = y0; y < y1; y++)
            {
                uint8_t* m = ui + static_cast<size_t>(y) * fr.width;
                const uint8_t* b = fr.bits + static_cast<size_t>(y) * fr.width;
                if (world != nullptr)
                {
                    const uint8_t* wo = world + static_cast<size_t>(y) * fr.width;
                    for (int32_t x = 0; x < fr.width; x++)
                        m[x] = b[x] != wo[x] ? 1 : 0;
                }
                else
                {
                    std::memset(m, 0, fr.width);
                }
                const int32_t n = RowSpans(y, fr.width, *fr.excluded, spans, kMaxSpans);
                for (int32_t i = 0; i < n; i++)
                    std::memset(m + spans[i].from, 1, spans[i].to - spans[i].from);
                ProtectExteriorRow(world != nullptr ? world + static_cast<size_t>(y) * fr.width : b, m, fr.width,
                    fr.shadowFlags != nullptr ? fr.shadowFlags + static_cast<size_t>(y) * fr.width : nullptr,
                    fr.depth != nullptr ? fr.depth + static_cast<size_t>(y) * fr.width : nullptr);
            }
        }

        // Stage 1: palette -> premultiplied 32bpp. UI pixels are zero (colour and weight lane). World pixels carry the
        // weight lane = SourceWeight(radius) and colours premultiplied by it (weighted blur, normalised again in Finish).
        void ConvertRows(const Frame& fr, const uint8_t* radius, uint32_t* dst, int32_t y0, int32_t y1)
        {
            const uint32_t freeMask = 0xFFu << fr.freeShift;
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* b = fr.bits + static_cast<size_t>(y) * fr.width;
                const uint8_t* m = fr.ui + static_cast<size_t>(y) * fr.width;
                const uint8_t* r = radius != nullptr ? radius + static_cast<size_t>(y) * fr.width : nullptr;
                uint32_t* d = dst + static_cast<size_t>(y) * fr.width;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    if (m[x])
                    {
                        d[x] = 0u;
                        continue;
                    }
                    const uint32_t p = fr.groundColour != nullptr
                        ? fr.groundColour[static_cast<size_t>(y) * fr.width + x] : fr.worldPalette[b[x]];
                    if (r == nullptr || r[x] >= 3)
                    {
                        d[x] = p | freeMask;
                        continue;
                    }
                    const uint32_t w = SourceWeight(r[x]);
                    const uint32_t scale = (w * 257u + 128u) >> 8; // 255 -> 256
                    d[x] = (ScaleLanesRound(p, scale) & ~freeMask) | (w << fr.freeShift);
                }
            }
        }

        // Averages four premultiplied pixels (per byte lane, rounded).
        inline uint32_t Average4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            const uint32_t rb = (((a & 0x00FF00FFu) + (b & 0x00FF00FFu) + (c & 0x00FF00FFu) + (d & 0x00FF00FFu) + 0x00020002u) >> 2)
                & 0x00FF00FFu;
            const uint32_t ga = ((((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + ((c >> 8) & 0x00FF00FFu)
                                  + ((d >> 8) & 0x00FF00FFu) + 0x00020002u)
                                 >> 2)
                & 0x00FF00FFu;
            return rb | (ga << 8);
        }

        // Bilinear 2x upsampling weights (9,3,3,1)/16 per byte lane.
        inline uint32_t Lerp4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
        {
            const uint32_t rb = ((a & 0x00FF00FFu) * 9 + (b & 0x00FF00FFu) * 3 + (c & 0x00FF00FFu) * 3 + (d & 0x00FF00FFu)
                                 + 0x00080008u)
                    >> 4
                & 0x00FF00FFu;
            const uint32_t ga = (((a >> 8) & 0x00FF00FFu) * 9 + ((b >> 8) & 0x00FF00FFu) * 3 + ((c >> 8) & 0x00FF00FFu) * 3
                                 + ((d >> 8) & 0x00FF00FFu) + 0x00080008u)
                    >> 4
                & 0x00FF00FFu;
            return rb | (ga << 8);
        }

        // Stage 2.0: 2x2 downsample of the premultiplied frame and of the radius plane (half-res radius in quarter px).
        void DownsampleRows(
            const Frame& fr, const FrameState& f, const uint32_t* src, const uint8_t* radius, uint32_t* dst, uint8_t* radiusHalf,
            int32_t y0, int32_t y1)
        {
            const int32_t w = fr.width, h = fr.height, hw = f.halfWidth;
            for (int32_t y = y0; y < y1; y++)
            {
                const int32_t ya = std::min(2 * y, h - 1), yb = std::min(2 * y + 1, h - 1);
                const uint32_t* s0 = src + static_cast<size_t>(ya) * w;
                const uint32_t* s1 = src + static_cast<size_t>(yb) * w;
                const uint8_t* r0 = radius + static_cast<size_t>(ya) * w;
                const uint8_t* r1 = radius + static_cast<size_t>(yb) * w;
                uint32_t* d = dst + static_cast<size_t>(y) * hw;
                uint8_t* rd = radiusHalf + static_cast<size_t>(y) * hw;
                for (int32_t x = 0; x < hw; x++)
                {
                    const int32_t xa = std::min(2 * x, w - 1), xb = std::min(2 * x + 1, w - 1);
                    d[x] = Average4(s0[xa], s0[xb], s1[xa], s1[xb]);
                    // mean full-res radius / 2 (half-res pixels) * 4 (quarter units) = sum of four / 2
                    rd[x] = static_cast<uint8_t>(std::min(255, (r0[xa] + r0[xb] + r1[xa] + r1[xb] + 1) >> 1));
                }
            }
        }

        // Average of the window [x0, x1] from prefix sums, as float lanes.
        inline __m128 WindowAverage(const int32_t* prefixBase, int32_t lo, int32_t hi, size_t stride)
        {
            const __m128i hiSum = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prefixBase + static_cast<size_t>(hi + 1) * stride));
            const __m128i loSum = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prefixBase + static_cast<size_t>(lo) * stride));
            return _mm_mul_ps(_mm_cvtepi32_ps(_mm_sub_epi32(hiSum, loSum)), _mm_set1_ps(1.0f / static_cast<float>(hi - lo + 1)));
        }

        inline uint32_t PackFloatLanes(__m128 v)
        {
            const __m128i i32 = _mm_cvtps_epi32(v);
            const __m128i i16 = _mm_packs_epi32(i32, i32);
            const __m128i i8 = _mm_packus_epi16(i16, i16);
            return static_cast<uint32_t>(_mm_cvtsi128_si32(i8));
        }

        // Stage 2a: horizontal box blur with a fractional radius per pixel (quarter pixel units) on the half-res frame.
        // Uses row prefix sums, so the cost does not depend on the radius.
        void BlurRows(
            int32_t w, const uint8_t* radius, const uint8_t* rowMax, const uint32_t* src, uint32_t* dst, int32_t y0, int32_t y1)
        {
            thread_local std::vector<int32_t> prefix;
            prefix.resize(static_cast<size_t>(w + 1) * 4);
            for (int32_t y = y0; y < y1; y++)
            {
                const uint32_t* s = src + static_cast<size_t>(y) * w;
                uint32_t* d = dst + static_cast<size_t>(y) * w;
                if (rowMax[y] == 0)
                {
                    std::memcpy(d, s, sizeof(uint32_t) * w);
                    continue;
                }
                const uint8_t* rr = radius + static_cast<size_t>(y) * w;
                __m128i acc = _mm_setzero_si128();
                _mm_storeu_si128(reinterpret_cast<__m128i*>(prefix.data()), acc);
                for (int32_t x = 0; x < w; x++)
                {
                    acc = _mm_add_epi32(acc, UnpackLanes(s[x]));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(prefix.data() + static_cast<size_t>(x + 1) * 4), acc);
                }
                for (int32_t x = 0; x < w; x++)
                {
                    const int32_t rq = rr[x];
                    if (rq == 0)
                    {
                        d[x] = s[x];
                        continue;
                    }
                    const int32_t r0 = rq >> 2, frac = rq & 3;
                    __m128 a = WindowAverage(prefix.data(), std::max(0, x - r0), std::min(w - 1, x + r0), 4);
                    if (frac != 0)
                    {
                        const __m128 b = WindowAverage(prefix.data(), std::max(0, x - r0 - 1), std::min(w - 1, x + r0 + 1), 4);
                        a = _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), _mm_set1_ps(static_cast<float>(frac) * 0.25f)));
                    }
                    d[x] = PackFloatLanes(a);
                }
            }
        }

        // Stage 2b: vertical box blur (same fractional radii) in column strips with column prefix sums. Prefix sums are
        // only built for the row ranges that really contain blurred pixels; the sharp band is copied.
        void BlurStrip(int32_t w, int32_t h, const uint8_t* radius, const uint32_t* src, uint32_t* dst, int32_t strip)
        {
            const int32_t x0 = strip * kStripWidth;
            const int32_t sw = std::min(kStripWidth, w - x0);

            thread_local std::vector<uint8_t> stripMax;
            stripMax.assign(h, 0);
            for (int32_t y = 0; y < h; y++)
            {
                const uint8_t* rr = radius + static_cast<size_t>(y) * w + x0;
                uint8_t m = 0;
                for (int32_t i = 0; i < sw; i++)
                    m = std::max(m, rr[i]);
                stripMax[y] = m;
            }

            constexpr size_t kRowInts = static_cast<size_t>(kStripWidth) * 4;
            thread_local std::vector<int32_t> prefix;

            int32_t y = 0;
            while (y < h)
            {
                if (stripMax[y] == 0)
                {
                    std::memcpy(dst + static_cast<size_t>(y) * w + x0, src + static_cast<size_t>(y) * w + x0, sizeof(uint32_t) * sw);
                    y++;
                    continue;
                }
                int32_t yEnd = y, maxR = 0;
                while (yEnd < h && stripMax[yEnd] != 0)
                {
                    maxR = std::max<int32_t>(maxR, (stripMax[yEnd] >> 2) + 1);
                    yEnd++;
                }
                const int32_t a = std::max(0, y - maxR);
                const int32_t b = std::min(h, yEnd + maxR);
                prefix.resize(static_cast<size_t>(b - a + 1) * kRowInts);
                std::fill_n(prefix.data(), kRowInts, 0);
                for (int32_t row = a; row < b; row++)
                {
                    const uint32_t* s = src + static_cast<size_t>(row) * w + x0;
                    const int32_t* prev = prefix.data() + static_cast<size_t>(row - a) * kRowInts;
                    int32_t* cur = prefix.data() + static_cast<size_t>(row - a + 1) * kRowInts;
                    for (int32_t i = 0; i < sw; i++)
                    {
                        const __m128i v = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i * 4)), UnpackLanes(s[i]));
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(cur + i * 4), v);
                    }
                }
                for (int32_t row = y; row < yEnd; row++)
                {
                    const uint32_t* s = src + static_cast<size_t>(row) * w + x0;
                    uint32_t* d = dst + static_cast<size_t>(row) * w + x0;
                    const uint8_t* rr = radius + static_cast<size_t>(row) * w + x0;
                    for (int32_t i = 0; i < sw; i++)
                    {
                        const int32_t rq = rr[i];
                        if (rq == 0)
                        {
                            d[i] = s[i];
                            continue;
                        }
                        const int32_t r0 = rq >> 2, frac = rq & 3;
                        const int32_t* base = prefix.data() + i * 4 - static_cast<size_t>(a) * kRowInts;
                        __m128 av = WindowAverage(base, std::max(a, row - r0), std::min(b - 1, row + r0), kRowInts);
                        if (frac != 0)
                        {
                            const __m128 bv = WindowAverage(base, std::max(a, row - r0 - 1), std::min(b - 1, row + r0 + 1), kRowInts);
                            av = _mm_add_ps(av, _mm_mul_ps(_mm_sub_ps(bv, av), _mm_set1_ps(static_cast<float>(frac) * 0.25f)));
                        }
                        d[i] = PackFloatLanes(av);
                    }
                }
                y = yEnd;
            }
        }

        // Stage 2c: blend the upsampled half-res blur over the full-res frame. Radius 0 stays sharp, >= 2 is fully blurred.
        void CompositeRows(
            const Frame& fr, const FrameState& f, const uint8_t* radius, const uint32_t* half, uint32_t* frame, int32_t y0,
            int32_t y1)
        {
            const int32_t w = fr.width, h = fr.height, hw = f.halfWidth, hh = f.halfHeight;
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* rr = radius + static_cast<size_t>(y) * w;
                uint32_t* d = frame + static_cast<size_t>(y) * w;
                const int32_t hy = y >> 1;
                const int32_t hyo = std::clamp(hy + ((y & 1) ? 1 : -1), 0, hh - 1);
                const uint32_t* h0 = half + static_cast<size_t>(hy) * hw;
                const uint32_t* h1 = half + static_cast<size_t>(hyo) * hw;
                for (int32_t x = 0; x < w; x++)
                {
                    const int32_t r = rr[x];
                    if (r == 0)
                        continue;
                    const int32_t hx = x >> 1;
                    const int32_t hxo = std::clamp(hx + ((x & 1) ? 1 : -1), 0, hw - 1);
                    const uint32_t blurred = Lerp4(h0[hx], h0[hxo], h1[hx], h1[hxo]);
                    // The half resolution image is already softer than the sharp one: fade over a wide radius range, so there
                    // is no visible line where the blur starts.
                    if (r >= 5)
                    {
                        d[x] = blurred;
                        continue;
                    }
                    // smoothstep over radius 0..5: short enough that the sharp and the blurred image never ghost
                    static const uint32_t kRamp[5] = { 0u, 40u, 120u, 200u, 240u };
                    const uint32_t wgt = kRamp[r];
                    const uint32_t s = d[x];
                    uint32_t mixed = 0;
                    for (uint32_t lane = 0; lane < 32; lane += 8)
                    {
                        const int32_t sa = static_cast<int32_t>((s >> lane) & 0xFFu), sb = static_cast<int32_t>((blurred >> lane) & 0xFFu);
                        mixed |= static_cast<uint32_t>(sa + (((sb - sa) * static_cast<int32_t>(wgt)) >> 8)) << lane;
                    }
                    d[x] = mixed;
                }
            }
            (void)h;
        }

        // Identical integer composite, evaluated in the final pixel pass instead of writing an intermediate frame.
        inline uint32_t CompositeBlurPixel(uint32_t sharp, int radius, const uint32_t* half,
            int x, int y, int halfWidth, int halfHeight)
        {
            if (radius == 0) return sharp;
            const int hx=x>>1,hy=y>>1;
            const int hxo=std::clamp(hx+((x&1)?1:-1),0,halfWidth-1);
            const int hyo=std::clamp(hy+((y&1)?1:-1),0,halfHeight-1);
            const auto blurred=Lerp4(half[static_cast<size_t>(hy)*halfWidth+hx],half[static_cast<size_t>(hy)*halfWidth+hxo],
                half[static_cast<size_t>(hyo)*halfWidth+hx],half[static_cast<size_t>(hyo)*halfWidth+hxo]);
            if(radius>=5)return blurred;
            static constexpr uint32_t ramp[5]={0u,40u,120u,200u,240u};
            uint32_t mixed=0;
            for(uint32_t lane=0;lane<32;lane+=8) {
                const int a=(sharp>>lane)&255u,b=(blurred>>lane)&255u;
                mixed|=static_cast<uint32_t>(a+(((b-a)*static_cast<int>(ramp[radius]))>>8))<<lane;
            }
            return mixed;
        }

        // A loaded .cube 3D lookup table. The world palette only has 256 colours, so applying it costs next to nothing.
        struct CubeLut
        {
            int32_t size = 0;
            std::vector<float> data; // size^3 * 3, red index fastest
        };
        CubeLut _cube;
        std::string _cubeName;

        void ApplyCube(float& r, float& g, float& b)
        {
            const int32_t n = _cube.size;
            const float scale = static_cast<float>(n - 1);
            const float fr = std::clamp(r, 0.0f, 1.0f) * scale, fg = std::clamp(g, 0.0f, 1.0f) * scale, fb = std::clamp(b, 0.0f, 1.0f) * scale;
            const int32_t r0 = std::min(static_cast<int32_t>(fr), n - 2), g0 = std::min(static_cast<int32_t>(fg), n - 2),
                          b0 = std::min(static_cast<int32_t>(fb), n - 2);
            const float tr = fr - static_cast<float>(r0), tg = fg - static_cast<float>(g0), tb = fb - static_cast<float>(b0);
            float result[3] = { 0.0f, 0.0f, 0.0f };
            for (int32_t corner = 0; corner < 8; corner++)
            {
                const int32_t dr = corner & 1, dg = (corner >> 1) & 1, db = (corner >> 2) & 1;
                const float weight = (dr ? tr : 1.0f - tr) * (dg ? tg : 1.0f - tg) * (db ? tb : 1.0f - tb);
                const size_t index = (static_cast<size_t>(r0 + dr) + static_cast<size_t>(n) * (static_cast<size_t>(g0 + dg) + static_cast<size_t>(n) * static_cast<size_t>(b0 + db))) * 3;
                for (int32_t k = 0; k < 3; k++)
                    result[k] += _cube.data[index + k] * weight;
            }
            r = result[0];
            g = result[1];
            b = result[2];
        }

        // Time of day: colour of the light and the sun, recomputed every frame.
        struct TimeState
        {
            bool active = false;
            float tint[3] = { 1.0f, 1.0f, 1.0f };
            float night = 0.0f; // 0 day .. 1 night
            float sunAzimuth = 260.0f, sunElevation = 40.0f;
            uint8_t emissive[256] = {}; // palette entries that stay bright at night (lamps, lit windows)
        };
        TimeState _time;

        float CurrentHour()
        {
            float hour = _settings.timeHour;
            if (_settings.timeCycle)
            {
                const double cycleTicks = static_cast<double>(_settings.timeCycleMinutes) * 60.0 * 40.0;
                const double position = std::fmod(static_cast<double>(getGameState().currentTicks), cycleTicks) / cycleTicks;
                hour = static_cast<float>(std::fmod(static_cast<double>(hour) + 24.0 * position, 24.0));
            }
            return hour;
        }

        inline float SmoothStep01(float a, float b, float x);

        void UpdateTimeState()
        {
            const bool wasActive = _time.active;
            _time.active = _settings.timeEnabled;
            if (!_time.active)
            {
                if (wasActive)
                    _time = TimeState{};
                return;
            }
            const float hour = CurrentHour();
            const float rise = SmoothStep01(4.8f, 7.6f, hour);
            const float set = 1.0f - SmoothStep01(17.8f, 20.6f, hour);
            const float day = rise * set;
            const float dawn = (hour - 7.0f) / 1.1f, dusk = (hour - 18.6f) / 1.3f;
            const float golden = std::min(1.0f, std::exp(-dawn * dawn) + std::exp(-dusk * dusk));
            _time.night = 1.0f - day;
            const float nm = 0.45f + 0.55f * (1.0f - _settings.nightDarkness);
            const float dayTint[3] = { 1.0f + 0.10f * golden, 1.0f - 0.07f * golden, 1.0f - 0.26f * golden };
            const float nightTint[3] = { 0.40f * nm, 0.52f * nm, 0.90f * nm };
            for (int32_t i = 0; i < 3; i++)
                _time.tint[i] = dayTint[i] + (nightTint[i] - dayTint[i]) * _time.night;
            const float t = std::clamp((hour - 6.0f) / 12.0f, 0.0f, 1.0f);
            _time.sunAzimuth = 95.0f + 170.0f * t;
            _time.sunElevation = 8.0f + 62.0f * std::sin(3.14159265f * t);
        }

        float EffectiveSunAzimuth()
        {
            return _time.active ? _time.sunAzimuth : _settings.sunAzimuth;
        }

        float EffectiveSunElevation()
        {
            return _time.active ? _time.sunElevation : _settings.sunElevation;
        }

        bool LightningPossible()
        {
            const auto type = getGameState().weatherCurrent.weatherType;
            return _settings.lightningStrength > 0.0f && (type == Weather::Type::thunder || type == Weather::Type::heavyRain);
        }

        float WeatherGloomForLights()
        {
            return std::clamp(static_cast<float>(getGameState().weatherCurrent.weatherGloom) * 0.12f, 0.0f, 0.3f);
        }

        // Light objects: freely placed sources (see the Lights page). Storage and persistence are further down.
        std::vector<LightObject> _lights;
        int32_t _lightSelected = -1;
        std::string _lightsPark;
        bool _lightsDirty = false;
        Clock::time_point _lightsDirtySince{};
        Clock::time_point _lightsPing{};
        LightObject _lightClipboard;
        bool _lightHasClipboard = false;
        int32_t _lightNextId = 1;

        int32_t _lastFrameWidth = 0, _lastFrameHeight = 0;
        float _sunBlockByWeather = 0.0f; // 0 = clear sky .. 1 = overcast / rain: no sun shafts

        bool LightsEditingNow()
        {
            return _settings.devLightMarkers || Clock::now() - _lightsPing < std::chrono::milliseconds(500);
        }

        bool ObjectLightsWanted()
        {
            if (!_settings.enabled)
                return false;
            if (LightsEditingNow())
                return true;
            return _settings.lightsEnabled && _settings.lightsStrength > 0.0f && !_lights.empty();
        }

        // Local visual timing never advances the simulation RNG or modifies weather/game state.
        struct LightningState
        {
            bool storm = false;
            double next = 0.0, fired = -1000.0;
            uint32_t random = 0x39AD712Fu, ticks = 0;
            float interval = 0.0f;
            uint32_t boltSeed = 1;
        };
        LightningState _lightning;

        float LightningEnvelope(float age)
        {
            if (age < 0.0f || age >= 0.5f)
                return 0.0f;
            return std::clamp(1.7f * (1.0f - std::exp(-age * 60.0f)) * std::exp(-age * 14.0f), 0.0f, 1.0f);
        }

        // A strike flickers like real lightning: a hard first flash and, most of the time, a second one a moment later
        // (and sometimes a third) along the same channel.
        float LightningFlicker(float age, uint32_t seed)
        {
            float value = LightningEnvelope(age);
            if ((seed & 3u) != 0u)
                value = std::max(value, 0.72f * LightningEnvelope(age - 0.26f));
            if ((seed & 12u) == 0u)
                value = std::max(value, 0.5f * LightningEnvelope(age - 0.47f));
            return value;
        }

        float UpdateLightning(LightningState& state, double seconds, uint32_t ticks, bool thunder, float interval)
        {
            if (!thunder || ticks < state.ticks)
            {
                state.storm = false;
                state.fired = -1000.0;
            }
            state.ticks = ticks;
            if (!thunder)
                return 0.0f;
            if (!state.storm || state.interval != interval)
            {
                state.storm = true;
                state.interval = interval;
                state.next = seconds + std::min(2.0, static_cast<double>(interval) * 0.5);
                state.fired = -1000.0;
            }
            if (seconds >= state.next)
            {
                state.fired = seconds;
                state.random = state.random * 1664525u + 1013904223u;
                state.boltSeed = state.random ^ 0x9E3779B9u;
                const double jitter = 0.75 + 0.5 * static_cast<double>(state.random & 65535u) / 65535.0;
                state.next = seconds + std::max(5.0, static_cast<double>(interval) * jitter);
            }
            return LightningFlicker(static_cast<float>(seconds - state.fired), state.boltSeed);
        }

        // Horizontal position (0..1) where a bolt with this seed leaves the clouds.
        float BoltOrigin(uint32_t seed)
        {
            const uint32_t r = seed * 1664525u + 1013904223u;
            return 0.12f + 0.76f * static_cast<float>((r >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
        }

        // Cloud planes of the current frame (half resolution, built by BuildCloudPlanes, consumed by FinishRows).
        const int16_t* _shadowZ = nullptr;
        const uint8_t* _sceneryShadow = nullptr;
        int32_t _faceRotation = 0;
        float _shadowOriginY = 0.0f, _shadowWpp = 1.0f;

        struct CloudPlanes
        {
            const uint8_t* shadow = nullptr;
            const uint8_t* cloud = nullptr;
            const uint8_t* fog = nullptr;
            const uint8_t* lit = nullptr;
            const uint8_t* sky = nullptr; // 255 where there is no map (the void around it is painted as a pale sky)
            float gloom = 0.0f; // 0 = bright weather .. 1 = dark storm clouds
            // per-frame tables (built by BuildCloudPlanes)
            uint32_t shadowFactor[3][256];
            const uint8_t* seg = nullptr; // per 8x1 half-resolution cell: any cloud, shadow or fog around?
            int32_t segCols = 0;
            int32_t fogColour[3];
            int32_t cloudColour[3][256]; // by light 0..255
        };
        CloudPlanes _cloudPlanes;

        inline uint32_t SampleHalfPlane(const uint8_t* plane, int32_t x, int32_t y, int32_t halfWidth, int32_t halfHeight)
        {
            const int32_t hx = x >> 1, hy = y >> 1;
            const int32_t hxo = std::clamp(hx + ((x & 1) ? 1 : -1), 0, halfWidth - 1);
            const int32_t hyo = std::clamp(hy + ((y & 1) ? 1 : -1), 0, halfHeight - 1);
            const uint32_t s0 = plane[static_cast<size_t>(hy) * halfWidth + hx];
            const uint32_t s1 = plane[static_cast<size_t>(hy) * halfWidth + hxo];
            const uint32_t s2 = plane[static_cast<size_t>(hyo) * halfWidth + hx];
            const uint32_t s3 = plane[static_cast<size_t>(hyo) * halfWidth + hxo];
            return (9 * s0 + 3 * s1 + 3 * s2 + s3 + 8) >> 4;
        }

        // Cloud cover (0..1) at a pixel from the half resolution cloud plane (0 without clouds).
        inline float CloudCoverAt(const Frame& fr, int32_t x, int32_t y)
        {
            const auto coverage = [&](const uint8_t* plane) {
                return plane == nullptr ? 0.0f : static_cast<float>(SampleHalfPlane(
                    plane, x, y, (fr.width + 1) / 2, (fr.height + 1) / 2)) * (1.0f / 255.0f);
            };
            // Match the two sequential alpha blends in FinishRows, including fog without a cloud layer.
            return 1.0f - (1.0f - coverage(_cloudPlanes.cloud)) * (1.0f - coverage(_cloudPlanes.fog));
        }

        // Lightning: white light that is brightest around the place where the bolt leaves the clouds. It lights the
        // clouds from inside (they flare up more than the ground) and still shows through them.
        void LightningRows(const Frame& fr, float flash, uint32_t seed, int32_t y0, int32_t y1)
        {
            if (flash <= 0.001f)
                return;
            const float bx = BoltOrigin(seed) * static_cast<float>(fr.width), by = 0.18f * static_cast<float>(fr.height);
            const float rx = 0.75f * static_cast<float>(fr.width), ry = 0.7f * static_cast<float>(fr.height);
            const uint32_t mask = (255u << fr.rShift) | (255u << fr.gShift) | (255u << fr.bShift);
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = 0; x < fr.width; x++)
                {
                    const size_t i = static_cast<size_t>(y) * fr.width + x;
                    if (fr.ui[i])
                        continue;
                    const float nx = (static_cast<float>(x) - bx) / rx, ny = (static_cast<float>(y) - by) / ry;
                    const float glow = std::clamp(1.0f - std::sqrt(nx * nx + ny * ny), 0.0f, 1.0f);
                    const float cover = CloudCoverAt(fr, x, y);
                    // Local cloud scattering reaches neutral white even over blue storm lighting. Keep the
                    // distant ground flash softer so a strike does not flatten the entire park to white.
                    const float k = std::clamp(flash * (0.35f + 1.15f * glow * glow
                        + 1.8f * cover * glow), 0.0f, 1.0f);
                    const uint32_t p = fr.out[i];
                    const auto channel = [&](uint32_t shift) {
                        const float value = static_cast<float>((p >> shift) & 255u);
                        return static_cast<uint32_t>(std::clamp(value + (255.0f - value) * k, 0.0f, 255.0f) + 0.5f) << shift;
                    };
                    fr.out[i] = (p & ~mask) | channel(fr.rShift) | channel(fr.gShift) | channel(fr.bShift);
                }
        }

        // A jagged white bolt with a glow, from the clouds towards the ground (drawn while the flash is bright).
        void DrawLightningBolt(const Frame& fr, uint32_t seed, float intensity)
        {
            if (intensity <= 0.05f || fr.width < 64 || fr.height < 64)
                return;
            uint32_t r = seed;
            const auto rnd = [&]() {
                r = r * 1664525u + 1013904223u;
                return static_cast<float>((r >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
            };
            const uint32_t mask = (255u << fr.rShift) | (255u << fr.gShift) | (255u << fr.bShift);
            const auto plot = [&](int32_t x, int32_t y, float k) {
                if (x < 0 || y < 0 || x >= fr.width || y >= fr.height || fr.ui[static_cast<size_t>(y) * fr.width + x])
                    return;
                uint32_t& p = fr.out[static_cast<size_t>(y) * fr.width + x];
                const auto lane = [&](uint32_t shift) {
                    const float v = static_cast<float>((p >> shift) & 255u);
                    return static_cast<uint32_t>(v + (255.0f - v) * std::min(1.0f, k)) << shift;
                };
                p = (p & ~mask) | lane(fr.rShift) | lane(fr.gShift) | lane(fr.bShift);
            };
            // soft halo around the core: wide and faint, then narrow and strong
            const auto stroke = [&](int32_t x, int32_t y, float k, int32_t width) {
                for (int32_t d = -2; d <= 1 + width; d++)
                {
                    const int32_t edge = d < 0 ? -d : (d >= width ? d - width + 1 : 0);
                    const float falloff = edge == 0 ? 1.0f : (edge == 1 ? 0.65f : (edge == 2 ? 0.32f : 0.14f));
                    plot(x + d, y, k * falloff);
                }
            };
            std::function<void(float, float, float, float, float, int32_t)> trace = [&](float x, float y, float endY, float lean, float strength, int32_t width) {
                while (y < endY)
                {
                    const int32_t seg = 3 + static_cast<int32_t>(rnd() * 6.0f);
                    const float nx = x + (lean + (rnd() - 0.5f) * 1.6f) * static_cast<float>(seg);
                    for (int32_t k = 0; k < seg && y + k < endY; k++)
                    {
                        const float px = x + (nx - x) * static_cast<float>(k) / static_cast<float>(seg);
                        stroke(static_cast<int32_t>(px), static_cast<int32_t>(y) + k, strength, width);
                    }
                    x = nx;
                    y += static_cast<float>(seg);
                    if (rnd() < 0.12f && strength > 0.4f)
                        trace(x, y, std::min(endY, y + (endY - y) * 0.5f * rnd() + 10.0f), (rnd() - 0.5f) * 1.2f, strength * 0.6f, 1);
                }
            };
            const float x0 = BoltOrigin(seed) * static_cast<float>(fr.width);
            r = seed * 1664525u + 1013904223u; // the origin used the first random value; the path continues from the next one
            // Compact bolts: half the former reach, with a one-pixel white core.
            trace(x0, 0.0f, static_cast<float>(fr.height) * (0.15f + 0.175f * rnd()), (rnd() - 0.5f) * 0.5f,
                std::min(1.0f, intensity * 2.4f), 1);
        }

        // Colour of a world pixel; swaying plants are shifted by fractions of a pixel by mixing two neighbouring palette
        // colours, so the movement is smooth instead of jumping from pixel to pixel.
        inline uint32_t SwayMix(const Frame& fr, const uint8_t* bits, int32_t x, int32_t y)
        {
            if (fr.groundColour != nullptr)
                return fr.groundColour[static_cast<size_t>(y) * fr.width + x];
            if (fr.waterColour != nullptr)
            {
                const uint32_t water = fr.waterColour[static_cast<size_t>(y) * fr.width + x];
                if (water != 0)
                    return water;
            }
            const uint32_t base = fr.worldPalette[bits[x]];
            if (fr.swayFrac == nullptr)
                return base;
            const size_t index = static_cast<size_t>(y) * fr.width + x;
            const uint32_t frac = fr.swayFrac[index];
            if (frac == 0)
                return base;
            const uint32_t other = fr.worldPalette[fr.swayAlt[index]];
            uint32_t result = base;
            for (const uint32_t shift : { fr.rShift, fr.gShift, fr.bShift })
            {
                const int32_t a = static_cast<int32_t>((base >> shift) & 0xFFu), c = static_cast<int32_t>((other >> shift) & 0xFFu);
                const uint32_t mixed = static_cast<uint32_t>(a + (((c - a) * static_cast<int32_t>(frac)) >> 8));
                result = (result & ~(0xFFu << shift)) | (mixed << shift);
            }
            return result;
        }

        // Stage 3: normalise by coverage (blur path) or convert directly (no blur), apply vignette, restore UI pixels.
        template<bool FromBlur, bool ApplyClouds = true, bool FuseComposite = false>
        void FinishRows(
            const Frame& fr, const uint32_t* blurred, const uint8_t* radius, const uint16_t* vignette,
            const uint32_t* coverageLut, const uint8_t* shadow, int32_t halfWidth, int32_t halfHeight, int32_t y0, int32_t y1,
            const uint32_t* halfBlur = nullptr)
        {
            const uint32_t freeMask = 0xFFu << fr.freeShift;
            // Shadow colour: cool tint per channel at full shadow strength.
            const bool cloudsActive = _cloudPlanes.shadow != nullptr || _cloudPlanes.cloud != nullptr || _cloudPlanes.fog != nullptr;
            const float strength = _settings.shadowStrength * (_time.active ? 1.0f - 0.85f * _time.night : 1.0f);
            constexpr float kTintR = 0.58f, kTintG = 0.62f, kTintB = 0.72f;
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* b = fr.bits + static_cast<size_t>(y) * fr.width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                uint32_t* o = fr.out + static_cast<size_t>(y) * fr.width;
                const uint32_t* a = FromBlur ? blurred + static_cast<size_t>(y) * fr.width : nullptr;
                const uint8_t* rad = FromBlur ? radius + static_cast<size_t>(y) * fr.width : nullptr;
                const uint16_t* m = vignette ? vignette + static_cast<size_t>(y) * fr.width : nullptr;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    // 1 = user interface, 2 = the exterior around the map (keeps its colour, but clouds drift over it)
                    if (ui[x] == 1)
                    {
                        o[x] = fr.palette[b[x]];
                        continue;
                    }
                    const bool exterior = ui[x] == 2;
                    uint32_t p;
                    if (exterior)
                    {
                        // Blank-tile sprites use palette 10 while the cleared viewport may use 0 or a
                        // weather-filtered index. They are the same empty backdrop, never visible terrain.
                        p = fr.palette[static_cast<uint8_t>(Drawing::PaletteIndex::pi10)];
                    }
                    else if constexpr (FromBlur)
                    {
                        p = a[x];
                        if constexpr (FuseComposite)
                            p = CompositeBlurPixel(p,rad[x],halfBlur,x,y,halfWidth,halfHeight);
                        const uint32_t cov = (p >> fr.freeShift) & 0xFFu;
                        if (cov == 0 || rad[x] == 0)
                            p = SwayMix(fr, b, x, y);
                            // untouched pixels keep their exact (graded) colour
                        else if (cov != 255)
                        {
                            const uint32_t scale = coverageLut[cov]; // 16.16, 255 / cov
                            uint32_t q = 0;
                            for (uint32_t lane = 0; lane < 32; lane += 8)
                            {
                                const uint32_t c = std::min(255u, (((p >> lane) & 0xFFu) * scale + 0x8000u) >> 16);
                                q |= c << lane;
                            }
                            p = q;
                        }
                    }
                    else
                    {
                        p = SwayMix(fr, b, x, y);
                    }
                    if (shadow != nullptr)
                    {
                        // bilinear sample of the half resolution shadow plane (9,3,3,1)/16
                        const int32_t hx = x >> 1, hy = y >> 1;
                        const int32_t hxo = std::clamp(hx + ((x & 1) ? 1 : -1), 0, halfWidth - 1);
                        const int32_t hyo = std::clamp(hy + ((y & 1) ? 1 : -1), 0, halfHeight - 1);
                        const uint32_t s0 = shadow[static_cast<size_t>(hy) * halfWidth + hx];
                        const uint32_t s1 = shadow[static_cast<size_t>(hy) * halfWidth + hxo];
                        const uint32_t s2 = shadow[static_cast<size_t>(hyo) * halfWidth + hx];
                        const uint32_t s3 = shadow[static_cast<size_t>(hyo) * halfWidth + hxo];
                        uint32_t amount = (9 * s0 + 3 * s1 + 3 * s2 + s3 + 8) >> 4; // 0..255
                        // Keep interpolated ground shadows off upright sprites and the exterior at full resolution.
                        // Receivers: only pixels that lie at the ground height of the casting object (ground, paths). The
                        // height of a pixel above that ground follows from its depth and its screen row.
                        const auto flatReceiver = [&]() {
                            // Explicit visible-sprite classification survives animation, tile seams and zoom.
                            // Neighbour-depth inference toggled when a peep covered the pixel above/below.
                            const auto& plane = Drawing::gDepthPlane;
                            return !exterior && fr.depth == plane.data && plane.shadowFlags != nullptr
                                && (plane.shadowFlags[static_cast<size_t>(y) * fr.width + x] & Drawing::kShadowReceiver) != 0;
                        };
                        const size_t shadowPixel = static_cast<size_t>(y) * fr.width + x;
                        const bool sceneryReceiver = !exterior && _sceneryShadow != nullptr
                            && fr.shadowFlags != nullptr && (fr.shadowFlags[shadowPixel] & Drawing::kSceneryReceiver) != 0;
                        bool sceneryFloor = false;
                        if (sceneryReceiver && flatReceiver() && _shadowZ != nullptr)
                        {
                            const float z = Drawing::DepthDecode(fr.depth[shadowPixel]) * 0.5f
                                - (y + _shadowOriginY) * _shadowWpp;
                            // Same-height paving gets precisely the path mask, including its stable filtering.
                            // Test the caster plane so a flat roof above a visitor cannot receive that mask.
                            for (const auto i : { static_cast<size_t>(hy) * halfWidth + hx,
                                    static_cast<size_t>(hy) * halfWidth + hxo,
                                    static_cast<size_t>(hyo) * halfWidth + hx,
                                    static_cast<size_t>(hyo) * halfWidth + hxo })
                                sceneryFloor |= _shadowZ[i] != INT16_MIN && std::abs(z - _shadowZ[i]) <= 4.0f;
                        }
                        if (sceneryReceiver && !sceneryFloor)
                            amount = _sceneryShadow[shadowPixel]; // never paste a ground-height shadow onto a roof
                        if (amount != 0 && (sceneryReceiver || flatReceiver()))
                        {
                            float shade = static_cast<float>(amount) * (1.0f / 255.0f) * strength;
                            // no direct sun under cloud shadows, and no ground shadow showing through the cloud layer itself
                            if (cloudsActive)
                            {
                                float sunBlock = 0.0f;
                                if (_cloudPlanes.shadow != nullptr)
                                    sunBlock = static_cast<float>(SampleHalfPlane(_cloudPlanes.shadow, x, y, halfWidth, halfHeight)) * (1.0f / 255.0f);
                                float veil = 0.0f;
                                if (_cloudPlanes.cloud != nullptr)
                                    veil = static_cast<float>(SampleHalfPlane(_cloudPlanes.cloud, x, y, halfWidth, halfHeight)) * (1.0f / 255.0f);
                                shade *= std::max(0.0f, 1.0f - std::min(1.0f, sunBlock * 2.5f)) * (1.0f - 0.9f * veil);
                            }
                            const auto lane = [&](uint32_t shift, float tint) {
                                const float factor = 1.0f - shade * (1.0f - tint);
                                return std::min(255u, static_cast<uint32_t>(static_cast<float>((p >> shift) & 0xFFu) * factor + 0.5f))
                                    << shift;
                            };
                            const uint32_t rs = fr.rShift, gs = fr.gShift, bs = fr.bShift;
                            const uint32_t colourMask = (0xFFu << rs) | (0xFFu << gs) | (0xFFu << bs);
                            p = (p & ~colourMask) | lane(rs, kTintR) | lane(gs, kTintG) | lane(bs, kTintB);
                        }
                    }
                    // A cloud is a layer in space, not paint clipped to visible map pixels. Its own map-space fade
                    // controls the overhang. UI remains protected by the early return above.
                    const bool cloudReceiver = cloudsActive;
                    if constexpr (ApplyClouds)
                    {
                    if (cloudReceiver
                        && _cloudPlanes.seg[static_cast<size_t>(y >> 1) * _cloudPlanes.segCols + (x >> 4)])
                    {
                        // Integer path: the planes are interpolated with fixed weights (9,3,3,1)/16 and all colour
                        // maths goes through per-frame tables.
                        const auto sample = [&](const uint8_t* plane) {
                            return SampleHalfPlane(plane, x, y, halfWidth, halfHeight);
                        };
                        uint32_t r = (p >> fr.rShift) & 0xFFu, g = (p >> fr.gShift) & 0xFFu, bl = (p >> fr.bShift) & 0xFFu;
                        if (!exterior && _cloudPlanes.shadow != nullptr)
                        {
                            const uint32_t amount = sample(_cloudPlanes.shadow);
                            if (amount != 0)
                            {
                                r = (r * _cloudPlanes.shadowFactor[0][amount]) >> 8;
                                g = (g * _cloudPlanes.shadowFactor[1][amount]) >> 8;
                                bl = (bl * _cloudPlanes.shadowFactor[2][amount]) >> 8;
                            }
                        }
                        if (_cloudPlanes.fog != nullptr)
                        {
                            const uint32_t amount = sample(_cloudPlanes.fog);
                            if (amount != 0)
                            {
                                const int32_t k = static_cast<int32_t>(amount + (amount >> 7));
                                r = static_cast<uint32_t>(static_cast<int32_t>(r) + (((_cloudPlanes.fogColour[0] - static_cast<int32_t>(r)) * k) >> 8));
                                g = static_cast<uint32_t>(static_cast<int32_t>(g) + (((_cloudPlanes.fogColour[1] - static_cast<int32_t>(g)) * k) >> 8));
                                bl = static_cast<uint32_t>(static_cast<int32_t>(bl) + (((_cloudPlanes.fogColour[2] - static_cast<int32_t>(bl)) * k) >> 8));
                            }
                        }
                        if (_cloudPlanes.cloud != nullptr)
                        {
                            const uint32_t amount = sample(_cloudPlanes.cloud);
                            if (amount != 0)
                            {
                                const uint32_t lit = _cloudPlanes.lit != nullptr ? sample(_cloudPlanes.lit) : 255u;
                                const int32_t k = static_cast<int32_t>(amount + (amount >> 7));
                                r = static_cast<uint32_t>(static_cast<int32_t>(r) + (((_cloudPlanes.cloudColour[0][lit] - static_cast<int32_t>(r)) * k) >> 8));
                                g = static_cast<uint32_t>(static_cast<int32_t>(g) + (((_cloudPlanes.cloudColour[1][lit] - static_cast<int32_t>(g)) * k) >> 8));
                                bl = static_cast<uint32_t>(static_cast<int32_t>(bl) + (((_cloudPlanes.cloudColour[2][lit] - static_cast<int32_t>(bl)) * k) >> 8));
                            }
                        }
                        const uint32_t colourMask = (0xFFu << fr.rShift) | (0xFFu << fr.gShift) | (0xFFu << fr.bShift);
                        p = (p & ~colourMask) | (std::min(r, 255u) << fr.rShift) | (std::min(g, 255u) << fr.gShift)
                            | (std::min(bl, 255u) << fr.bShift);
                    }
                    }
                    if (m != nullptr && m[x] < 256 && !exterior)
                        p = ScaleLanes(p, m[x]);
                    o[x] = p | freeMask;
                }
            }
        }

        // Depth of what is in focus: the median of the recorded depth along the focus row (robust against buildings).
        // Returns false if the plane has no data there. `focus` is in map units.
        bool MeasureFocusDepth(const uint16_t* depth, int32_t width, int32_t height, float& focus)
        {
            const int32_t row = std::clamp(static_cast<int32_t>(_settings.tiltFocus * height), 0, height - 1);
            std::vector<uint16_t> samples;
            samples.reserve(static_cast<size_t>(width / 4) * 3);
            for (int32_t dy = -4; dy <= 4; dy += 4)
            {
                const int32_t y = std::clamp(row + dy, 0, height - 1);
                for (int32_t x = 0; x < width; x += 4)
                {
                    const uint16_t d = depth[static_cast<size_t>(y) * width + x];
                    if (d != 0xFFFF)
                        samples.push_back(d);
                }
            }
            if (samples.empty())
                return false;
            const auto mid = samples.begin() + samples.size() / 2;
            std::nth_element(samples.begin(), mid, samples.end());
            focus = static_cast<float>((static_cast<int32_t>(*mid) - 32768) * 2);
            return true;
        }

        void DebugDepthView(const Frame& fr, const uint16_t* depth, float worldPerPixel)
        {
            float focus = 0.0f;
            const bool haveFocus = MeasureFocusDepth(depth, fr.width, fr.height, focus);
            const float scale = 2.0f * worldPerPixel * fr.height * 0.5f; // half a screen of ground distance
            {
                static int dumpCounter = 0;
                if (dumpCounter++ % 120 == 0)
                {
                    uint32_t mn = 0xFFFF, mx = 0, unknown = 0, total = 0;
                    for (int32_t y = 0; y < fr.height; y++)
                        for (int32_t x = 0; x < fr.width; x++)
                        {
                            const uint32_t d = depth[static_cast<size_t>(y) * fr.width + x];
                            total++;
                            if (d == 0xFFFF)
                            {
                                unknown++;
                                continue;
                            }
                            mn = std::min(mn, d);
                            mx = std::max(mx, d);
                        }
                    std::ofstream log(_settingsPath + ".depth.log", std::ios::app);
                    log << "focus=" << focus << (haveFocus ? "" : "(none)") << " wpp=" << worldPerPixel << " min=" << mn << " max=" << mx << " unknown=" << unknown << "/" << total;
                    for (int32_t row : { 40, 200, 360, 520, 680 })
                        log << " row" << row << "=" << depth[static_cast<size_t>(row) * fr.width + fr.width / 2];
                    log << "\n";
                }
            }
            for (int32_t y = 0; y < fr.height; y++)
            {
                uint32_t* o = fr.out + static_cast<size_t>(y) * fr.width;
                const uint16_t* d = depth + static_cast<size_t>(y) * fr.width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    if (ui[x])
                        continue;
                    uint32_t r = 255, g = 0, b = 255; // unknown = magenta
                    if (d[x] != 0xFFFF && haveFocus)
                    {
                        const float units = static_cast<float>((static_cast<int32_t>(d[x]) - 32768) * 2);
                        const float t = std::clamp((units - focus) / scale, -1.0f, 1.0f);
                        r = t > 0 ? static_cast<uint32_t>(255.0f * t) : 0;
                        b = t < 0 ? static_cast<uint32_t>(-255.0f * t) : 0;
                        g = static_cast<uint32_t>(255.0f * (1.0f - std::abs(t)));
                    }
                    o[x] = (r << fr.rShift) | (g << fr.gShift) | (b << fr.bShift) | (0xFFu << fr.freeShift);
                }
            }
        }

        // Prepares the depth -> radius table. Depths further from the focus than band + falloff all map to the maximum radius.
        void BuildRadiusLut(FrameState& f, float focus, float worldPerPixel)
        {
            const int32_t h = f.height;
            const float maxRadius = std::min(120.0f, 32.0f * _settings.tiltStrength * h / 1080.0f);
            const float unitsPerRow = 2.0f * worldPerPixel;
            const float bandD = _settings.tiltBand * h * unitsPerRow;
            const float fallD = std::max(1.0f, _settings.tiltFalloff * h * unitsPerRow);
            f.lutHalf = static_cast<int32_t>((bandD + fallD) * 0.5f) + 2;
            f.lutFocusStored = static_cast<int32_t>(std::lround(focus * 0.5f)) + 32768;
            f.lutFar = static_cast<uint8_t>(maxRadius + 0.5f);
            f.radiusLut.assign(static_cast<size_t>(f.lutHalf) * 2 + 1, 0);
            for (int32_t k = 0; k < static_cast<int32_t>(f.radiusLut.size()); k++)
            {
                const float units = static_cast<float>((k - f.lutHalf)) * 2.0f; // distance to the focus in map units
                float t = std::clamp((std::abs(units) - bandD) / fallD, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                f.radiusLut[k] = static_cast<uint8_t>(maxRadius * t + 0.5f);
            }
        }

        // Per-pixel blur radius: recorded depth where available, otherwise the deterministic screen-row radius.
        void BuildRadiusRows(const FrameState& f, const uint16_t* depth, bool useDepth, uint8_t* out, int32_t y0, int32_t y1)
        {
            const int32_t w = f.width;
            const int32_t lutSize = static_cast<int32_t>(f.radiusLut.size());
            const uint8_t* lut = f.radiusLut.data();
            for (int32_t y = y0; y < y1; y++)
            {
                uint8_t* o = out + static_cast<size_t>(y) * w;
                const uint16_t* d = depth != nullptr ? depth + static_cast<size_t>(y) * w : nullptr;
                for (int32_t x = 0; x < w; x++)
                {
                    if (!useDepth || d[x] == 0xFFFF)
                    {
                        // Never carry a flying sprite's radius horizontally or into a worker chunk below it.
                        o[x] = static_cast<uint8_t>(f.radius[y]);
                        continue;
                    }
                    const int32_t k = static_cast<int32_t>(d[x]) - f.lutFocusStored + f.lutHalf;
                    o[x] = (static_cast<uint32_t>(k) < static_cast<uint32_t>(lutSize)) ? lut[k] : f.lutFar;
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Sun shadows come from the actual opaque pixels painted this frame. Visible sprite coverage owns
        // the caster silhouette; paths/supports carry receiver-only flags, so no tile boxes enter this stage.
        // Projection deliberately has a smoothly bounded reach: tall scenery leaves a discreet nearby shadow
        // instead of an opaque city-sized slab. A separable soft kernel removes pixel stair-steps.
        struct ShadowProjection
        {
            float directionX = 0.0f, directionY = 0.0f;
            float invTan = 1.0f, reach = 40.0f, worldPerPixel = 1.0f;
        };

        struct ShadowPoint
        {
            float x, y;
        };

        ShadowProjection MakeShadowProjection(float azimuth, float elevation, float reach, int32_t rotation, float wpp)
        {
            constexpr float kPi = 3.14159265f;
            float rx = -std::cos(azimuth * kPi / 180.0f);
            float ry = -std::sin(azimuth * kPi / 180.0f);
            for (int32_t r = 0; r < (rotation & 3); r++)
            {
                const float previousX = rx;
                rx = ry;
                ry = -previousX;
            }
            return { ry - rx, (rx + ry) * 0.5f,
                1.0f / std::tan(std::clamp(elevation, 5.0f, 85.0f) * kPi / 180.0f),
                std::clamp(reach, 4.0f, 800.0f), std::max(0.25f, wpp) };
        }

        ShadowPoint ProjectShadowPixel(float x, float y, float height, const ShadowProjection& p)
        {
            const float rayLength = std::max(0.0f, height) * p.invTan;
            // Continuous compression retains the silhouette and does not pile every tall pixel onto a hard cap.
            const float distance = p.reach * rayLength / (p.reach + rayLength);
            return { (x + p.directionX * distance / p.worldPerPixel) * 0.5f,
                (y + height / p.worldPerPixel + p.directionY * distance / p.worldPerPixel) * 0.5f };
        }

        float ShadowEdgeDistanceSquared(ShadowPoint point, ShadowPoint a, ShadowPoint b)
        {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float lengthSquared = dx * dx + dy * dy;
            const float t = lengthSquared > 1e-8f
                ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f) : 0.0f;
            const float x = point.x - a.x - t * dx, y = point.y - a.y - t * dy;
            return x * x + y * y;
        }

        bool ShadowQuadContains(const ShadowPoint* quad, ShadowPoint point)
        {
            bool positive = false, negative = false;
            // Local edge vectors avoid cancellation for tiny quads far from the screen origin.
            const float area = (quad[1].x - quad[0].x) * (quad[2].y - quad[0].y)
                - (quad[1].y - quad[0].y) * (quad[2].x - quad[0].x)
                + (quad[2].x - quad[0].x) * (quad[3].y - quad[0].y)
                - (quad[2].y - quad[0].y) * (quad[3].x - quad[0].x);
            for (int32_t edge = 0; edge < 4; edge++)
            {
                const auto& a = quad[edge];
                const auto& b = quad[(edge + 1) & 3];
                const float cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
                positive |= cross > 1e-5f;
                negative |= cross < -1e-5f;
            }
            return std::abs(area) > 1e-5f && !(positive && negative);
        }

        void RasterShadowQuad(uint16_t* coverage, int32_t width, int32_t height, const ShadowPoint* quad)
        {
            // A half screen-pixel contact edge handles edge-on projections. No tile-sized dilation is used.
            constexpr float edgeRadius = 0.25f; // coordinates here are half-resolution pixels
            float left = quad[0].x, right = left, top = quad[0].y, bottom = top;
            for (int32_t corner = 1; corner < 4; corner++)
            {
                left = std::min(left, quad[corner].x); right = std::max(right, quad[corner].x);
                top = std::min(top, quad[corner].y); bottom = std::max(bottom, quad[corner].y);
            }
            if (right + edgeRadius < 0 || bottom + edgeRadius < 0 || left - edgeRadius >= width || top - edgeRadius >= height)
                return;
            const int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(left - edgeRadius)), 0, width - 1);
            const int32_t x1 = std::clamp(static_cast<int32_t>(std::floor(right + edgeRadius)), 0, width - 1);
            const int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(top - edgeRadius)), 0, height - 1);
            const int32_t y1 = std::clamp(static_cast<int32_t>(std::floor(bottom + edgeRadius)), 0, height - 1);
            const bool horizontalBand = quad[0].y == quad[1].y && quad[2].y == quad[3].y;
            const float innerLeft = std::max(quad[0].x, quad[3].x);
            const float innerRight = std::min(quad[1].x, quad[2].x);
            for (int32_t y = y0; y <= y1; y++)
                for (int32_t x = x0; x <= x1; x++)
                {
                    auto& mask = coverage[static_cast<size_t>(y) * width + x];
                    if (mask == 0xFFFF)
                        continue;
                    // Projected row spans have horizontal end edges. Away from their side boundaries,
                    // all four horizontal samples share exactly the same coverage at a given sample row.
                    if (horizontalBand && x >= innerLeft && x + 1 <= innerRight)
                    {
                        for (int32_t sy = 0; sy < 4; sy++)
                        {
                            const float sampleY = y + (sy + 0.5f) * 0.25f;
                            if (sampleY > top - edgeRadius && sampleY < bottom + edgeRadius)
                                mask |= static_cast<uint16_t>(0xFu << (sy * 4));
                        }
                        continue;
                    }
                    for (int32_t sy = 0; sy < 4; sy++)
                        for (int32_t sx = 0; sx < 4; sx++)
                        {
                            const uint16_t bit = static_cast<uint16_t>(1u << (sy * 4 + sx));
                            if (mask & bit)
                                continue;
                            const ShadowPoint point{ x + (sx + 0.5f) * 0.25f, y + (sy + 0.5f) * 0.25f };
                            bool inside = ShadowQuadContains(quad, point);
                            for (int32_t edge = 0; edge < 4 && !inside; edge++)
                                inside = ShadowEdgeDistanceSquared(point, quad[edge], quad[(edge + 1) & 3])
                                    < edgeRadius * edgeRadius;
                            if (inside)
                                mask |= bit;
                        }
                }
        }

        void RasterShadowSpan(
            uint16_t* coverage, int32_t width, int32_t height,
            float left, float right, float y, float altitude, const ShadowProjection& projection)
        {
            const float halfPixelHeight = projection.worldPerPixel * 0.5f;
            const ShadowPoint quad[4] = {
                ProjectShadowPixel(left - 0.5f, y - 0.5f, altitude + halfPixelHeight, projection),
                ProjectShadowPixel(right + 0.5f, y - 0.5f, altitude + halfPixelHeight, projection),
                ProjectShadowPixel(right + 0.5f, y + 0.5f, altitude - halfPixelHeight, projection),
                ProjectShadowPixel(left - 0.5f, y + 0.5f, altitude - halfPixelHeight, projection),
            };
            RasterShadowQuad(coverage, width, height, quad);
        }

        uint8_t ShadowCoverageValue(uint16_t coverage)
        {
            // Union the occupied samples; adjacent pixels complete coverage, duplicates never darken it.
            return static_cast<uint8_t>(std::popcount(coverage) * 12);
        }

        void BlurShadowRows(
            const uint8_t* source, uint8_t* destination, int32_t width, int32_t height,
            bool horizontal, int32_t radius, int32_t y0, int32_t y1)
        {
            const int32_t denominator = (radius + 1) * (radius + 1);
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    int32_t sum = 0;
                    for (int32_t tap = -radius; tap <= radius; tap++)
                    {
                        const int32_t sx = horizontal ? std::clamp(x + tap, 0, width - 1) : x;
                        const int32_t sy = horizontal ? y : std::clamp(y + tap, 0, height - 1);
                        sum += source[static_cast<size_t>(sy) * width + sx] * (radius + 1 - std::abs(tap));
                    }
                    destination[static_cast<size_t>(y) * width + x]
                        = static_cast<uint8_t>((sum + denominator / 2) / denominator);
                }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Object shadows for guests, staff, vehicles and track pieces. Instead of projecting every sprite pixel, every
        // object is one small soft capsule on the ground: from the point below it to the tip of its shadow. The capsules
        // go into a half resolution plane; the finish stage lets only ground / path pixels receive them (the receiver
        // flags are written while the world is painted), so shadows never land on walls, trees or rides.
        // Cost is proportional to the number of objects on screen, not to the number of pixels.
        // ---------------------------------------------------------------------------------------------------------
        // Uses the same local coordinates as Vehicle::UpdateTrackMotion; never joins independent elements.
        template<typename Point, typename Emit>
        void SampleTrackShadowPath(const Point* points, uint16_t count, Emit emit)
        {
            if (points == nullptr || count < 2) return;
            uint16_t previous = 0;
            for (uint16_t i = 1; i < count; i++)
            {
                const auto& a = points[previous]; const auto& b = points[i];
                if (a.x == kLocationNull || b.x == kLocationNull) { previous = i; continue; }
                const int32_t dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                const int32_t length2 = dx * dx + dy * dy + dz * dz;
                if (i + 1 < count && length2 < 16) continue;
                if (length2 > 0 && length2 <= 1024) emit(a, b);
                previous = i;
            }
        }

        struct TrackShadowCache
        {
            int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
            uint32_t revision = 0xFFFFFFFFu, ticks = 0;
            const TileElement* tileStorage = nullptr;
            size_t tileCount = 0;
            Clock::time_point collected{};
            struct Segment { CoordsXYZ a, b; RideId ride; };
            std::vector<Segment> segments;
        };
        TrackShadowCache _trackShadows;

        void CollectTrackShadowPieces(const ViewInfo& view)
        {
            auto& state = getGameState();
            if (state.mapSize.x <= 0 || state.mapSize.y <= 0)
            {
                _trackShadows.segments.clear();
                return;
            }
            int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
            for (const int32_t z : { 0, 1600 })
                for (int32_t corner = 0; corner < 4; corner++)
                {
                    const ScreenCoordsXY p{ static_cast<int32_t>((view.originX + ((corner & 1) ? view.viewportWidth : 0)) * view.worldPerPixel),
                                            static_cast<int32_t>((view.originY + ((corner & 2) ? view.viewportHeight : 0)) * view.worldPerPixel) };
                    const auto map = ViewportPosToMapPos(p, z, static_cast<uint8_t>(view.rotation));
                    minX = std::min(minX, map.x); minY = std::min(minY, map.y);
                    maxX = std::max(maxX, map.x); maxY = std::max(maxY, map.y);
                }
            // Includes the origin of multi-tile curves/loops and offscreen casters.
            const int32_t margin = 24 + static_cast<int32_t>(std::ceil(_settings.shadowReach / 32.0f));
            const int32_t x0 = std::clamp(minX / 32 - margin, 0, state.mapSize.x - 1);
            const int32_t y0 = std::clamp(minY / 32 - margin, 0, state.mapSize.y - 1);
            const int32_t x1 = std::clamp(maxX / 32 + margin, 0, state.mapSize.x - 1);
            const int32_t y1 = std::clamp(maxY / 32 + margin, 0, state.mapSize.y - 1);
            auto& cache = _trackShadows;
            const uint32_t revision = Drawing::gMapRevision.load(std::memory_order_relaxed);
            const auto now = Clock::now();
            if (x0 >= cache.x0 && y0 >= cache.y0 && x1 <= cache.x1 && y1 <= cache.y1
                && (revision == cache.revision || now - cache.collected < std::chrono::milliseconds(300))
                && state.currentTicks >= cache.ticks
                && cache.tileStorage == state.tileElements.data() && cache.tileCount == state.tileElements.size())
            {
                cache.ticks = state.currentTicks;
                return;
            }
            cache.x0 = x0; cache.y0 = y0; cache.x1 = x1; cache.y1 = y1;
            cache.revision = revision; cache.ticks = state.currentTicks; cache.collected = now;
            cache.tileStorage = state.tileElements.data(); cache.tileCount = state.tileElements.size();
            cache.segments.clear();
            for (int32_t ty = y0; ty <= y1; ty++)
                for (int32_t tx = x0; tx <= x1; tx++)
                {
                    const auto* element = MapGetFirstElementAt(TileCoordsXY{ tx, ty });
                    if (element == nullptr) continue;
                    do
                    {
                        if (element->isGhost() || element->isInvisible() || element->getType() != TileElementType::track)
                            continue;
                        const auto* track = element->asTrack();
                        // The movement table is relative to sequence zero, and already contains every curve tile.
                        if (track->getSequenceIndex() != 0) continue;
                        const Ride* ride = GetRide(track->getRideIndex());
                        if (ride == nullptr || ride->getRideTypeDescriptor().flags.has(RtdFlag::isFlatRide)
                            || ride->getRideTypeDescriptor().flags.has(RtdFlag::isShopOrFacility)) continue;
                        const auto type = track->getTrackType();
                        const uint8_t direction = track->getDirection();
                        const uint16_t count = VehicleGetMoveInfoSize(VehicleTrackSubposition::standard, type, direction);
                        if (count < 2) continue;
                        const auto* points = gTrackVehicleInfo[EnumValue(VehicleTrackSubposition::standard)]
                            [(EnumValue(type) << 2) | (direction & 3)]->info;
                        // Sample at <=4 map units of movement. No nearest-tile joins: crossings and parallel tracks stay separate.
                        SampleTrackShadowPath(points, count, [&](const auto& a, const auto& b) {
                            cache.segments.push_back({ { tx * 32 + a.x, ty * 32 + a.y, element->getBaseZ() + a.z },
                                                       { tx * 32 + b.x, ty * 32 + b.y, element->getBaseZ() + b.z }, track->getRideIndex() });
                        });
                    } while (!(element++)->isLastForTile());
                }
        }

        std::pair<float, float> ProjectPeepShadowPoint(
            float x, float y, float z, uint8_t rotation, float scale, float originX, float originY)
        {
            // Keep the sun offset fractional. Integer world/screen projection makes tiny shadows jump.
            float rx = x, ry = y;
            switch (rotation & 3)
            {
                case 1: rx = y; ry = -x; break;
                case 2: rx = -x; ry = -y; break;
                case 3: rx = -y; ry = x; break;
            }
            return { (ry - rx) * scale - originX * 0.5f,
                     ((rx + ry) * 0.5f - z) * scale - originY * 0.5f };
        }

        struct ShadowCapsule
        {
            float ax, ay, bx, by; // ground end points in half resolution pixels (a = below the object, b = tip of the shadow)
            float radius;         // half resolution pixels
            int16_t groundZ = 0;
            float opacity = 1.0f;
        };

        void RasterShadowCapsule(uint8_t* plane, int16_t* planeZ, int32_t width, int32_t height, const ShadowCapsule& c, float inner)
        {
            // iso ground: vertical distances count double (the ground is squashed by two), so the footprint is an ellipse
            const float rx = c.radius, ry = c.radius * 0.5f;
            const int32_t left = std::max(0, static_cast<int32_t>(std::floor(std::min(c.ax, c.bx) - rx)));
            const int32_t right = std::min(width - 1, static_cast<int32_t>(std::ceil(std::max(c.ax, c.bx) + rx)));
            const int32_t top = std::max(0, static_cast<int32_t>(std::floor(std::min(c.ay, c.by) - ry)));
            const int32_t bottom = std::min(height - 1, static_cast<int32_t>(std::ceil(std::max(c.ay, c.by) + ry)));
            if (left > right || top > bottom)
                return;
            const float dx = c.bx - c.ax, dy = (c.by - c.ay) * 2.0f;
            const float lengthSquared = dx * dx + dy * dy;
            const float invRxSquared = 1.0f / (rx * rx);
            const float invLength = lengthSquared > 1e-6f ? 1.0f / lengthSquared : 0.0f;
            // Evaluated on a coarser 2x2 grid (the shadow is soft anyway) and written as 2x2 blocks: a quarter of the cost.
            // Used for the track, which stands still; moving vehicles use the per pixel version below.
            for (int32_t y = top & ~1; y <= bottom; y += 2)
            {
                for (int32_t x = left & ~1; x <= right; x += 2)
                {
                    const float px = static_cast<float>(x) + 1.0f - c.ax, py = (static_cast<float>(y) + 1.0f - c.ay) * 2.0f;
                    const float t = std::clamp((px * dx + py * dy) * invLength, 0.0f, 1.0f);
                    const float ex = px - t * dx, ey = py - t * dy;
                    const float distanceSquared = (ex * ex + ey * ey) * invRxSquared;
                    if (distanceSquared >= 1.0f)
                        continue;
                    const float distance = std::sqrt(distanceSquared);
                    const float fade = distance <= inner ? 1.0f : 1.0f - (distance - inner) / (1.0f - inner);
                    const uint8_t v8 = static_cast<uint8_t>(fade * fade * (3.0f - 2.0f * fade) * 255.0f);
                    for (int32_t yy = y; yy < std::min(y + 2, height); yy++)
                    {
                        uint8_t* row = plane + static_cast<size_t>(yy) * width;
                        for (int32_t xx = x; xx < std::min(x + 2, width); xx++)
                            if (v8 >= row[xx])
                            {
                                row[xx] = v8;
                                planeZ[static_cast<size_t>(yy) * width + xx] = c.groundZ;
                            }
                    }
                }
            }
        }

        void RasterShadowCapsuleFine(uint8_t* plane, int16_t* planeZ, int32_t width, int32_t height, const ShadowCapsule& c, float inner)
        {
            // iso ground: vertical distances count double (the ground is squashed by two), so the footprint is an ellipse
            const float rx = c.radius, ry = c.radius * 0.5f;
            const int32_t left = std::max(0, static_cast<int32_t>(std::floor(std::min(c.ax, c.bx) - rx)));
            const int32_t right = std::min(width - 1, static_cast<int32_t>(std::ceil(std::max(c.ax, c.bx) + rx)));
            const int32_t top = std::max(0, static_cast<int32_t>(std::floor(std::min(c.ay, c.by) - ry)));
            const int32_t bottom = std::min(height - 1, static_cast<int32_t>(std::ceil(std::max(c.ay, c.by) + ry)));
            if (left > right || top > bottom)
                return;
            const float dx = c.bx - c.ax, dy = (c.by - c.ay) * 2.0f;
            const float lengthSquared = dx * dx + dy * dy;
            const float invRxSquared = 1.0f / (rx * rx);
            const float invLength = lengthSquared > 1e-6f ? 1.0f / lengthSquared : 0.0f;
            // Every half resolution pixel is evaluated at its centre: block wise evaluation made the edges jump when
            // vehicles move.
            for (int32_t y = top; y <= bottom; y++)
            {
                uint8_t* row = plane + static_cast<size_t>(y) * width;
                int16_t* rowZ = planeZ + static_cast<size_t>(y) * width;
                for (int32_t x = left; x <= right; x++)
                {
                    const float px = static_cast<float>(x) + 0.5f - c.ax, py = (static_cast<float>(y) + 0.5f - c.ay) * 2.0f;
                    const float t = std::clamp((px * dx + py * dy) * invLength, 0.0f, 1.0f);
                    const float ex = px - t * dx, ey = py - t * dy;
                    const float distanceSquared = (ex * ex + ey * ey) * invRxSquared;
                    if (distanceSquared >= 1.0f)
                        continue;
                    const float distance = std::sqrt(distanceSquared);
                    const float fade = distance <= inner ? 1.0f : 1.0f - (distance - inner) / (1.0f - inner);
                    const uint8_t v8 = static_cast<uint8_t>(fade * fade * (3.0f - 2.0f * fade) * 255.0f);
                    if (v8 >= row[x])
                    {
                        row[x] = v8;
                        rowZ[x] = c.groundZ;
                    }
                }
            }
        }

        void RasterPeepShadowCapsuleRows(
            uint8_t* plane, int16_t* planeZ, int32_t width, int32_t height, const ShadowCapsule& c, float inner, int32_t firstRow, int32_t lastRow)
        {
            if (width <= 0 || height <= 0 || c.radius <= 0.0f || !std::isfinite(c.radius)
                || !std::isfinite(c.ax) || !std::isfinite(c.ay) || !std::isfinite(c.bx) || !std::isfinite(c.by))
                return;
            const float rx = c.radius, ry = rx * 0.5f;
            const float minX = std::min(c.ax, c.bx) - rx, maxX = std::max(c.ax, c.bx) + rx;
            const float minY = std::min(c.ay, c.by) - ry, maxY = std::max(c.ay, c.by) + ry;
            if (maxX <= 0.0f || maxY <= 0.0f || minX >= width || minY >= height)
                return;
            const int32_t left = static_cast<int32_t>(std::floor(std::max(0.0f, minX)));
            const int32_t right = static_cast<int32_t>(std::floor(std::min(static_cast<float>(width - 1), maxX)));
            const int32_t top = std::max(firstRow, static_cast<int32_t>(std::floor(std::max(0.0f, minY))));
            const int32_t bottom = std::min(lastRow - 1, static_cast<int32_t>(std::floor(std::min(static_cast<float>(height - 1), maxY))));
            const float dx = c.bx - c.ax, dy = (c.by - c.ay) * 2.0f;
            const float lengthSquared = dx * dx + dy * dy;
            const float invLength = lengthSquared > 1e-6f ? 1.0f / lengthSquared : 0.0f;
            const float invRadiusSquared = 1.0f / (rx * rx);
            inner = std::clamp(inner, 0.0f, 0.99f);
            // Area coverage on each half-resolution pixel. A point sample on a 2x2 block
            // can miss the entire peep shadow at some movement phases, especially when zoomed out.
            constexpr int32_t samples = 4;
            for (int32_t y = top; y <= bottom; ++y)
                for (int32_t x = left; x <= right; ++x)
                {
                    float coverage = 0.0f;
                    for (int32_t sy = 0; sy < samples; ++sy)
                        for (int32_t sx = 0; sx < samples; ++sx)
                        {
                            const float px = x + (sx + 0.5f) / samples - c.ax;
                            const float py = (y + (sy + 0.5f) / samples - c.ay) * 2.0f;
                            const float t = std::clamp((px * dx + py * dy) * invLength, 0.0f, 1.0f);
                            const float ex = px - t * dx, ey = py - t * dy;
                            const float distanceSquared = (ex * ex + ey * ey) * invRadiusSquared;
                            if (distanceSquared >= 1.0f)
                                continue;
                            const float fade = std::clamp((1.0f - std::sqrt(distanceSquared)) / (1.0f - inner), 0.0f, 1.0f);
                            coverage += fade * fade * (3.0f - 2.0f * fade);
                        }
                    const auto value = static_cast<uint8_t>(std::lround(coverage * (255.0f / (samples * samples)) * c.opacity));
                    const size_t index = static_cast<size_t>(y) * width + x;
                    if (value != 0 && value >= plane[index])
                    {
                        plane[index] = value;
                        planeZ[index] = c.groundZ;
                    }
                }
        }

        void RasterPeepShadowCapsule(
            uint8_t* plane, int16_t* planeZ, int32_t width, int32_t height, const ShadowCapsule& c, float inner)
        {
            RasterPeepShadowCapsuleRows(plane, planeZ, width, height, c, inner, 0, height);
        }

        // Fills a convex polygon (half resolution coordinates) into the face planes.
        void RasterFaceQuad(FrameState& f, const float (*pts)[2], int32_t count, uint8_t value, int16_t depthValue)
        {
            f.hasFaceShading = true;
            const int32_t width = f.halfWidth, height = f.halfHeight;
            float top = pts[0][1], bottom = pts[0][1];
            for (int32_t i = 1; i < count; i++)
            {
                top = std::min(top, pts[i][1]);
                bottom = std::max(bottom, pts[i][1]);
            }
            const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top))), y1 = std::min(height - 1, static_cast<int32_t>(std::floor(bottom)));
            for (int32_t y = y0; y <= y1; y++)
            {
                const float sy = static_cast<float>(y) + 0.5f;
                float xl = 1e9f, xr = -1e9f;
                for (int32_t i = 0; i < count; i++)
                {
                    const float* a = pts[i];
                    const float* b = pts[(i + 1) % count];
                    if ((a[1] <= sy && b[1] > sy) || (b[1] <= sy && a[1] > sy))
                    {
                        const float x = a[0] + (sy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]);
                        xl = std::min(xl, x);
                        xr = std::max(xr, x);
                    }
                }
                if (xl > xr)
                    continue;
                const int32_t c0 = std::max(0, static_cast<int32_t>(std::floor(xl))), c1 = std::min(width - 1, static_cast<int32_t>(std::floor(xr)));
                for (int32_t x = c0; x <= c1; x++)
                {
                    f.faceVal[static_cast<size_t>(y) * width + x] = value;
                    f.faceDepth[static_cast<size_t>(y) * width + x] = depthValue;
                }
            }
        }

        // Soft edges: blur only coverage; receiving surfaces are classified at full resolution.
        void SoftenObjectShadows(FrameState& f, float wpp)
        {
            auto& pool = WorkerPool::Get();
            const int32_t hw = f.halfWidth, hh = f.halfHeight;
            const int32_t tasks = (hh + 31) / 32;
            const int32_t radius = _settings.shadowSoftness <= 0.0f ? 0
                : std::clamp(static_cast<int32_t>(std::lround((1.5f + 6.0f * _settings.shadowSoftness) / (2.0f * wpp))), 1, 5);
            if (radius > 0)
            {
                pool.ParallelFor(tasks, [&](int32_t task) {
                    BlurShadowRows(f.shadowRaw.data(), f.shadowSoft.data(), hw, hh, true, radius, task * 32, std::min(hh, task * 32 + 32));
                });
                pool.ParallelFor(tasks, [&](int32_t task) {
                    BlurShadowRows(f.shadowSoft.data(), f.shadowRaw.data(), hw, hh, false, radius, task * 32, std::min(hh, task * 32 + 32));
                });
            }
            // Carry the caster plane into the blur skirt, without changing its coverage. Flat scenery
            // needs this height to share the path mask while excluding roofs on another level.
            f.shadowZTmp.resize(f.shadowZ.size());
            const int spread = radius + 1;
            pool.ParallelFor(tasks, [&](int task) {
                for(int y=task*32;y<std::min(hh,task*32+32);y++) for(int x=0;x<hw;x++) {
                    const size_t i=static_cast<size_t>(y)*hw+x;
                    int16_t z=f.shadowZ[i];
                    for(int d=1;d<=spread && z==INT16_MIN;d++) {
                        if(x>=d)z=f.shadowZ[i-d];
                        if(z==INT16_MIN && x+d<hw)z=f.shadowZ[i+d];
                    }
                    f.shadowZTmp[i]=z;
                }
            });
            pool.ParallelFor(tasks, [&](int task) {
                for(int y=task*32;y<std::min(hh,task*32+32);y++) for(int x=0;x<hw;x++) {
                    const size_t i=static_cast<size_t>(y)*hw+x;
                    int16_t z=f.shadowZTmp[i];
                    for(int d=1;d<=spread && z==INT16_MIN;d++) {
                        if(y>=d)z=f.shadowZTmp[i-static_cast<size_t>(d)*hw];
                        if(z==INT16_MIN && y+d<hh)z=f.shadowZTmp[i+static_cast<size_t>(d)*hw];
                    }
                    f.shadowZ[i]=z;
                }
            });

        }

        // Sun side shading: wall quads of buildings (collected with the shadows) tell which pixels belong to a wall
        // facing the sun and which one faces away. Pixels are only changed where their depth fits the wall, so people
        // and objects in front of a wall keep their own light.
        void FaceShadingRows(const Frame& fr, const FrameState& f, const uint16_t* depth, const uint8_t* blurRadius, int32_t y0, int32_t y1)
        {
            constexpr float kPi = 3.14159265f;
            const float az = EffectiveSunAzimuth() * kPi / 180.0f;
            // direction to the sun in rotated map coordinates
            const CoordsXY sunRot = CoordsXY{ static_cast<int32_t>(std::cos(az) * 1000.0f), static_cast<int32_t>(std::sin(az) * 1000.0f) }.rotate(_faceRotation & 3);
            const float horizontal = std::cos(std::clamp(EffectiveSunElevation(), 5.0f, 85.0f) * kPi / 180.0f);
            const float sunX = static_cast<float>(sunRot.x) * 0.001f * horizontal, sunY = static_cast<float>(sunRot.y) * 0.001f * horizontal;
            const float k = _settings.faceShading * 0.45f * (_time.active ? 1.0f - 0.8f * _time.night : 1.0f);
            const int32_t gainLeft = static_cast<int32_t>(std::clamp(1.0f + k * sunX * 1.6f, 0.4f, 1.6f) * 256.0f);
            const int32_t gainRight = static_cast<int32_t>(std::clamp(1.0f + k * sunY * 1.6f, 0.4f, 1.6f) * 256.0f);
            const int32_t hw = f.halfWidth, hh = f.halfHeight;
            for (int32_t y = y0; y < y1; y++)
            {
                uint32_t* o = fr.out + static_cast<size_t>(y) * fr.width;
                const size_t row = static_cast<size_t>(std::min(y >> 1, hh - 1)) * hw;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    const size_t cell = row + (x >> 1);
                    const uint8_t face = f.faceVal[cell];
                    if (face == 0 || fr.ui[static_cast<size_t>(y) * fr.width + x])
                        continue;
                    if (blurRadius != nullptr && blurRadius[static_cast<size_t>(y) * fr.width + x] > 2)
                        continue; // blurred zones keep their soft look
                    const uint16_t d = depth[static_cast<size_t>(y) * fr.width + x];
                    if (d == 0xFFFF)
                        continue;
                    // only upright pixels (constant depth from row to row) are walls; ground has a steadily growing depth
                    const uint16_t dBelow = depth[static_cast<size_t>(std::min(y + 1, fr.height - 1)) * fr.width + x];
                    const uint16_t dAbove = depth[static_cast<size_t>(std::max(y - 1, 0)) * fr.width + x];
                    if (dBelow != d && dAbove != d)
                        continue;
                    const int32_t delta = Drawing::DepthDecode(d) / 4 - f.faceDepth[cell];
                    if (delta < -10 || delta > 10)
                        continue;
                    const int32_t gain = face == 1 ? gainLeft : gainRight;
                    uint32_t p = o[x];
                    uint32_t result = p;
                    for (const uint32_t shift : { fr.rShift, fr.gShift, fr.bShift })
                    {
                        const uint32_t c = std::min(255u, (((p >> shift) & 0xFFu) * static_cast<uint32_t>(gain)) >> 8);
                        result = (result & ~(0xFFu << shift)) | (c << shift);
                    }
                    o[x] = result;
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Water. The game paints water with the animated palette entries 230-239 (waves and sparkles). Where those
        // pixels are dense there is water: a coarse cell grid marks it. Waves are two slowly drifting sine layers in
        // world coordinates (so they stay on the map when the camera moves); glitter is a hashed field of single
        // sparkles that blink at random times.
        // ---------------------------------------------------------------------------------------------------------
        struct WaterState
        {
            int32_t cellsX = 0, cellsY = 0;
            std::vector<uint8_t> density; // per 8x8 pixel cell, 0..255
            std::vector<uint8_t> smooth;
            std::vector<uint8_t> shore; // distance to the nearest non-water cell (cells, capped), 0 = land
            uint32_t key = 0;
            int32_t age = 1000;
        };
        WaterState _water;

        void UpdateWaterZones(const Frame& fr, const ViewInfo& v)
        {
            auto& w = _water;
            const auto& native = Drawing::gDepthPlane;
            const uint8_t* surfaces = fr.shadowFlags;
            if (fr.shadowFlags == native.shadowFlags && native.receiverFlags != nullptr)
                surfaces = native.receiverFlags; // boats do not turn the water below them into land
            constexpr int32_t kCell = 8;
            const int32_t cx = (fr.width + kCell - 1) / kCell, cy = (fr.height + kCell - 1) / kCell;
            uint32_t key = 2166136261u;
            for (const int32_t value : { v.originX, v.originY, v.rotation, fr.width, fr.height, static_cast<int32_t>(v.worldPerPixel * 8.0f) })
                key = (key ^ static_cast<uint32_t>(value)) * 16777619u;
            key = (key ^ Drawing::gMapRevision.load(std::memory_order_relaxed)) * 16777619u;
            if (w.key == key && w.cellsX == cx && w.cellsY == cy && ++w.age < 8)
                return;
            w.key = key;
            w.age = 0;
            w.cellsX = cx;
            w.cellsY = cy;
            w.density.assign(static_cast<size_t>(cx) * cy, 0);
            w.smooth.assign(static_cast<size_t>(cx) * cy, 0);
            WorkerPool::Get().ParallelFor(cy, [&](int32_t j) {
                for (int32_t i = 0; i < cx; i++)
                {
                    int32_t count = 0, samples = 0;
                    const int32_t x1 = std::min(fr.width, (i + 1) * kCell), y1 = std::min(fr.height, (j + 1) * kCell);
                    for (int32_t y = j * kCell; y < y1; y += 2)
                    {
                        for (int32_t x = i * kCell; x < x1; x += 2)
                        {
                            samples++;
                            count += surfaces != nullptr
                                && (surfaces[static_cast<size_t>(y) * fr.width + x] & Drawing::kWaterSurface) != 0;
                        }
                    }
                    w.density[static_cast<size_t>(j) * cx + i] = static_cast<uint8_t>(count * 255 / std::max(1, samples));
                }
            });
            for (int32_t j = 0; j < cy; j++)
                for (int32_t i = 0; i < cx; i++)
                {
                    int32_t sum = 0, n = 0;
                    for (int32_t dj = -1; dj <= 1; dj++)
                        for (int32_t di = -1; di <= 1; di++)
                        {
                            const int32_t jj = j + dj, ii = i + di;
                            if (jj >= 0 && jj < cy && ii >= 0 && ii < cx)
                            {
                                sum += w.density[static_cast<size_t>(jj) * cx + ii];
                                n++;
                            }
                        }
                    w.smooth[static_cast<size_t>(j) * cx + i] = static_cast<uint8_t>(sum / n);
                }
            // distance to the shore: two chamfer passes
            w.shore.assign(static_cast<size_t>(cx) * cy, 0);
            // Stable native water coverage, widened by two cells for a gradual shore transition.
            for (int32_t j = 0; j < cy; j++)
                for (int32_t i = 0; i < cx; i++)
                {
                    bool any = false;
                    for (int32_t dj = -2; dj <= 2 && !any; dj++)
                        for (int32_t di = -2; di <= 2; di++)
                        {
                            const int32_t jj = j + dj, ii = i + di;
                            if (jj >= 0 && jj < cy && ii >= 0 && ii < cx && w.density[static_cast<size_t>(jj) * cx + ii] >= 12)
                            {
                                any = true;
                                break;
                            }
                        }
                    w.shore[static_cast<size_t>(j) * cx + i] = any ? 40 : 0;
                }
            const auto at = [&](int32_t i, int32_t j) -> uint8_t& { return w.shore[static_cast<size_t>(j) * cx + i]; };
            for (int32_t j = 0; j < cy; j++)
                for (int32_t i = 0; i < cx; i++)
                    if (at(i, j) != 0)
                    {
                        int32_t best = at(i, j);
                        if (i > 0)
                            best = std::min<int32_t>(best, at(i - 1, j) + 1);
                        if (j > 0)
                            best = std::min<int32_t>(best, at(i, j - 1) + 1);
                        if (i > 0 && j > 0)
                            best = std::min<int32_t>(best, at(i - 1, j - 1) + 1);
                        if (i + 1 < cx && j > 0)
                            best = std::min<int32_t>(best, at(i + 1, j - 1) + 1);
                        at(i, j) = static_cast<uint8_t>(best);
                    }
            for (int32_t j = cy - 1; j >= 0; j--)
                for (int32_t i = cx - 1; i >= 0; i--)
                    if (at(i, j) != 0)
                    {
                        int32_t best = at(i, j);
                        if (i + 1 < cx)
                            best = std::min<int32_t>(best, at(i + 1, j) + 1);
                        if (j + 1 < cy)
                            best = std::min<int32_t>(best, at(i, j + 1) + 1);
                        if (i + 1 < cx && j + 1 < cy)
                            best = std::min<int32_t>(best, at(i + 1, j + 1) + 1);
                        if (i > 0 && j + 1 < cy)
                            best = std::min<int32_t>(best, at(i - 1, j + 1) + 1);
                        at(i, j) = static_cast<uint8_t>(best);
                    }
            // soften the distance field (no diamond shaped steps): three 3x3 blurs
            {
                std::vector<float> a(w.shore.begin(), w.shore.end()), b(a.size());
                for (int32_t pass = 0; pass < 3; pass++)
                {
                    for (int32_t j = 0; j < cy; j++)
                        for (int32_t i = 0; i < cx; i++)
                        {
                            float sum = 0.0f, wsum = 0.0f;
                            for (int32_t dj = -1; dj <= 1; dj++)
                                for (int32_t di = -1; di <= 1; di++)
                                {
                                    const int32_t jj = std::clamp(j + dj, 0, cy - 1), ii = std::clamp(i + di, 0, cx - 1);
                                    const float k = (di == 0 ? 2.0f : 1.0f) * (dj == 0 ? 2.0f : 1.0f);
                                    sum += a[static_cast<size_t>(jj) * cx + ii] * k;
                                    wsum += k;
                                }
                            b[static_cast<size_t>(j) * cx + i] = sum / wsum;
                        }
                    a.swap(b);
                }
                for (size_t i = 0; i < a.size(); i++)
                    w.shore[i] = static_cast<uint8_t>(std::clamp(a[i] + 0.5f, 0.0f, 255.0f));
            }
        }

        // Water has no reliable per pixel depth (blended sprites): inside water zones the blur radius of the screen row
        // is used for every pixel, which removes the dotted and streaky blur on water.
        void ApplyWaterRadiusRows(const FrameState& f, uint8_t* radius, int32_t y0, int32_t y1)
        {
            constexpr int32_t kCell = 8;
            if (_water.cellsX <= 0)
                return;
            for (int32_t y = y0; y < y1; y++)
            {
                uint8_t* r = radius + static_cast<size_t>(y) * f.width;
                const uint8_t rowRadius = static_cast<uint8_t>(f.radius[y]);
                const uint8_t* zone = _water.smooth.data() + static_cast<size_t>(y / kCell) * _water.cellsX;
                for (int32_t x = 0; x < f.width; x++)
                    if (zone[x / kCell] >= 40)
                        r[x] = rowRadius;
            }
        }

        inline float WaterNoise(float x, float y)
        {
            const auto h = [](int32_t i, int32_t j) {
                uint32_t n = static_cast<uint32_t>(i) * 374761393u + static_cast<uint32_t>(j) * 668265263u;
                n = (n ^ (n >> 13)) * 1274126177u;
                return static_cast<float>((n ^ (n >> 16)) & 0xFFFFu) * (1.0f / 65535.0f);
            };
            const float fx = std::floor(x), fy = std::floor(y);
            const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
            float tx = x - fx, ty = y - fy;
            tx = tx * tx * (3.0f - 2.0f * tx);
            ty = ty * ty * (3.0f - 2.0f * ty);
            return (h(ix, iy) * (1 - tx) + h(ix + 1, iy) * tx) * (1 - ty) + (h(ix, iy + 1) * (1 - tx) + h(ix + 1, iy + 1) * tx) * ty;
        }

        // One clock and one wave field for the displacement of the water pixels (applied to the index buffer, like the swaying
        // plants) and for the light on the crests (applied to the finished colours), so light and movement stay in step.
        inline float WaterClockNow()
        {
            static const auto start = Clock::now();
            return std::chrono::duration<float>(Clock::now() - start).count();
        }

        // Displacement of the water surface at world position (wx, wy) in pixels at amplitude 1: dx, dy; slope = crest light -1..1.
        inline void WaterWaveField(float wx, float wy, float t, float& dx, float& dy, float& slope)
        {
            const float a1 = wx * 0.016f + wy * 0.037f + t * 1.1f;
            const float a2 = wx * 0.041f - wy * 0.027f + t * 0.8f + 1.7f;
            const float a3 = wx * 0.083f + wy * 0.061f - t * 1.5f + 4.0f;
            dx = 0.55f * std::sin(a1) + 0.30f * std::sin(a2) + 0.15f * std::sin(a3);
            dy = 0.5f * (0.5f * std::cos(wx * 0.026f - wy * 0.044f + t * 0.9f) + 0.5f * std::sin(wx * 0.07f + wy * 0.05f - t * 1.2f));
            // the slope of the surface (derivative of the displacement along the direction of travel) lights the crests
            slope = 0.55f * std::cos(a1) + 0.30f * std::cos(a2) + 0.15f * std::cos(a3);
        }

        uint32_t SampleWaterColour(const Frame& fr, int32_t x, int32_t y, float dx, float dy)
        {
            const float sx = static_cast<float>(x) - dx, sy = static_cast<float>(y) + dy;
            const int32_t x0 = static_cast<int32_t>(std::floor(sx)), y0 = static_cast<int32_t>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float channels[3]{}, total = 0.0f;
            const uint32_t shifts[3]{ fr.rShift, fr.gShift, fr.bShift };
            for (int32_t j = 0; j < 2; j++)
                for (int32_t i = 0; i < 2; i++)
                {
                    const int32_t px = x0 + i, py = y0 + j;
                    if (px < 0 || py < 0 || px >= fr.width || py >= fr.height)
                        continue;
                    const size_t index = static_cast<size_t>(py) * fr.width + px;
                    if (fr.ui[index] || fr.shadowFlags == nullptr
                        || (fr.shadowFlags[index] & Drawing::kWaterSurface) == 0)
                        continue;
                    const float weight = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                    const uint32_t colour = fr.worldPalette[fr.bits[index]];
                    for (int c = 0; c < 3; c++)
                        channels[c] += static_cast<float>((colour >> shifts[c]) & 255u) * weight;
                    total += weight;
                }
            if (total <= 0.0001f)
                return 0;
            uint32_t result = 255u << fr.freeShift;
            for (int c = 0; c < 3; c++)
                result |= static_cast<uint32_t>(std::clamp(channels[c] / total + 0.5f, 0.0f, 255.0f)) << shifts[c];
            return result;
        }

        void WaterRows(const Frame& fr, const uint16_t* depth, const uint8_t* blurRadius, const ViewInfo& v, float time, int32_t y0, int32_t y1)
        {
            const float waves = _settings.waterWaves, glitter = _settings.waterGlitter;
            const float sunVisible = std::clamp(1.0f - _sunBlockByWeather, 0.0f, 1.0f);
            const float wpp = v.worldPerPixel;
            const int32_t slot = static_cast<int32_t>(time * 2.5f);
            const float slotFrac = time * 2.5f - static_cast<float>(slot);
            const float threshold = 0.006f + 0.02f * glitter; // probability of a sparkle being active in a slot
            constexpr int32_t kCell = 8;
            // direction of the sun on the screen (towards the sun), for the sparkle density
            constexpr float kPi = 3.14159265f;
            const float sunAz = EffectiveSunAzimuth() * kPi / 180.0f;
            const CoordsXY sunRot = CoordsXY{ static_cast<int32_t>(std::cos(sunAz) * 1000.0f), static_cast<int32_t>(std::sin(sunAz) * 1000.0f) }.rotate(v.rotation);
            float sunX = static_cast<float>(sunRot.y - sunRot.x), sunY = static_cast<float>(sunRot.x + sunRot.y) * 0.5f;
            const float sunLen = std::max(1.0f, std::sqrt(sunX * sunX + sunY * sunY));
            sunX /= sunLen;
            sunY /= sunLen;
            const float halfDiag = 0.5f * std::sqrt(static_cast<float>(fr.width) * fr.width + static_cast<float>(fr.height) * fr.height);
            for (int32_t y = y0; y < y1; y++)
            {
                uint32_t* o = fr.out + static_cast<size_t>(y) * fr.width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                const float wy = (static_cast<float>(y) + static_cast<float>(v.originY)) * wpp;
                const int32_t rowCj = std::clamp(static_cast<int32_t>(std::floor((static_cast<float>(y) + 0.5f) / kCell - 0.5f)), 0, _water.cellsY - 1);
                const int32_t rowCj2 = std::min(rowCj + 1, _water.cellsY - 1);
                for (int32_t x = 0; x < fr.width; x++)
                {
                    if ((x & 7) == 0)
                    {
                        // blocks of 8 pixels whose surrounding cells hold no water are skipped at once
                        const int32_t k = x >> 3;
                        uint8_t best = 0;
                        for (int32_t i = std::max(0, k - 1); i <= std::min(_water.cellsX - 1, k + 1); i++)
                            best = std::max({ best, _water.smooth[static_cast<size_t>(rowCj) * _water.cellsX + i], _water.smooth[static_cast<size_t>(rowCj2) * _water.cellsX + i] });
                        if (best < 20)
                        {
                            x += 7;
                            continue;
                        }
                    }
                    if (ui[x])
                        continue;
                    if (fr.shadowFlags == nullptr
                        || (fr.shadowFlags[static_cast<size_t>(y) * fr.width + x] & Drawing::kWaterSurface) == 0)
                        continue;
                    if (fr.depth != nullptr && depth != nullptr
                        && fr.depth[static_cast<size_t>(y) * fr.width + x] != depth[static_cast<size_t>(y) * fr.width + x])
                        continue; // a visible leaf occupies this pixel above the water
                    // bilinear between cell centres: no block edges
                    const float gx = (static_cast<float>(x) + 0.5f) / kCell - 0.5f, gy = (static_cast<float>(y) + 0.5f) / kCell - 0.5f;
                    const int32_t ci = std::clamp(static_cast<int32_t>(std::floor(gx)), 0, _water.cellsX - 1), cj = std::clamp(static_cast<int32_t>(std::floor(gy)), 0, _water.cellsY - 1);
                    const int32_t ci2 = std::min(ci + 1, _water.cellsX - 1), cj2 = std::min(cj + 1, _water.cellsY - 1);
                    const float fx = std::clamp(gx - static_cast<float>(ci), 0.0f, 1.0f), fy = std::clamp(gy - static_cast<float>(cj), 0.0f, 1.0f);
                    const auto zs = [&](int32_t i, int32_t j) { return static_cast<float>(_water.smooth[static_cast<size_t>(j) * _water.cellsX + i]); };
                    const float zoneF = (zs(ci, cj) * (1 - fx) + zs(ci2, cj) * fx) * (1 - fy) + (zs(ci, cj2) * (1 - fx) + zs(ci2, cj2) * fx) * fy;
                    if (zoneF < 20.0f)
                        continue;
                    const uint8_t zone = static_cast<uint8_t>(std::min(255.0f, zoneF));
                    // inside blurred (tilt-shift) zones the sharp details fade out smoothly; the soft tints stay
                    const float sharp = blurRadius != nullptr ? std::clamp(1.0f - static_cast<float>(blurRadius[static_cast<size_t>(y) * fr.width + x]) * (1.0f / 6.0f), 0.0f, 1.0f) : 1.0f;
                    // only the flat surface of the water (objects on it have constant depth up and down)
                    if (depth != nullptr)
                    {
                        const size_t index = static_cast<size_t>(y) * fr.width + x;
                        const uint16_t here = depth[index];
                        // unknown depth = blended water pixels, they are flat by definition
                        if (here != 0xFFFF && depth[static_cast<size_t>(std::min(y + 1, fr.height - 1)) * fr.width + x] == here
                            && depth[static_cast<size_t>(std::max(y - 1, 0)) * fr.width + x] == here)
                            continue;
                    }
                    const float weight = std::clamp((static_cast<float>(zone) - 26.0f) * (1.0f / 60.0f), 0.0f, 1.0f);
                    const float wx = (static_cast<float>(x) + static_cast<float>(v.originX)) * wpp;
                    uint32_t p = o[x];
                    float gain = 1.0f;
                    if (waves > 0.0f)
                    {
                        float fdx, fdy, slope;
                        WaterWaveField(wx, wy, time, fdx, fdy, slope);
                        gain += waves * weight * sharp * 0.16f * slope;
                    }
                    float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
                    if (_settings.waterDepth > 0.0f)
                    {
                        // far from the shore = deep: darker and bluer; close to it = shallow: lighter and greener
                        const auto sh = [&](int32_t i, int32_t j) { return static_cast<float>(_water.shore[static_cast<size_t>(j) * _water.cellsX + i]); };
                        const float shoreF = (sh(ci, cj) * (1 - fx) + sh(ci2, cj) * fx) * (1 - fy) + (sh(ci, cj2) * (1 - fx) + sh(ci2, cj2) * fx) * fy;
                        const float deep = std::clamp((shoreF - 1.0f) * (1.0f / 14.0f), 0.0f, 1.0f);
                        const float k = _settings.waterDepth * weight;
                        tintR = 1.0f + k * (0.14f - 0.50f * deep);
                        tintG = 1.0f + k * (0.10f - 0.26f * deep);
                        tintB = 1.0f + k * (0.04f - 0.10f * deep);
                    }
                    if (_settings.waterPatches > 0.0f)
                    {
                        // big soft patches (reflections of clouds, caustics) drifting slowly
                        const float n = 0.65f * WaterNoise(wx / 260.0f + time * 0.012f, wy / 190.0f - time * 0.007f)
                            + 0.35f * WaterNoise(wx / 95.0f - time * 0.02f, wy / 70.0f + time * 0.011f + 7.0f);
                        const float patch = std::clamp((n - 0.45f) * 2.4f, -1.0f, 1.0f) * _settings.waterPatches * weight;
                        tintR *= 1.0f + 0.22f * patch;
                        tintG *= 1.0f + 0.16f * patch;
                        tintB *= 1.0f + 0.10f * patch;
                    }
                    float add = 0.0f;
                    if (glitter > 0.0f)
                    {
                        // chunky 2x2 sparkles; hash decides position, a hash based time offset decides when they blink
                        uint32_t hsh = static_cast<uint32_t>((x + v.originX) >> 1) * 73856093u ^ static_cast<uint32_t>(static_cast<int32_t>(wy / std::max(1.0f, wpp)) >> 1) * 19349663u;
                        hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
                        hsh ^= hsh >> 16;
                        // denser towards the sun (the side of the picture the sun is on)
                        const float towardSun = std::clamp(0.5f + 0.8f * ((static_cast<float>(x) - 0.5f * fr.width) * sunX + (static_cast<float>(y) - 0.5f * fr.height) * sunY) / halfDiag, 0.0f, 1.0f);
                        const uint32_t sparkleLimit = 4u + static_cast<uint32_t>(26.0f * _settings.waterSun * towardSun * towardSun);
                        if ((hsh & 255u) < sparkleLimit)
                        {
                            const uint32_t when = static_cast<uint32_t>(slot) + ((hsh >> 8) & 7u);
                            uint32_t h2 = (when * 2654435761u) ^ hsh;
                            h2 ^= h2 >> 15;
                            if (static_cast<float>(h2 & 1023u) * (1.0f / 1024.0f) < threshold * 30.0f)
                            {
                                const float tri = 1.0f - std::abs(slotFrac * 2.0f - 1.0f);
                                add = 150.0f * glitter * weight * sharp * tri;
                            }
                        }
                    }
                    if (gain == 1.0f && add == 0.0f && tintR == 1.0f && tintG == 1.0f && tintB == 1.0f)
                        continue;
                    // The water is below the clouds: where a cloud (or fog) covers the pixel, nothing of the surface light
                    // may shine through it, and with an overcast sky there is no sun to glitter.
                    const float veil = 1.0f - std::min(1.0f, CloudCoverAt(fr, x, y) * 1.05f);
                    add *= veil * (0.15f + 0.85f * sunVisible);
                    gain = 1.0f + (gain - 1.0f) * veil;
                    tintR = 1.0f + (tintR - 1.0f) * veil;
                    tintG = 1.0f + (tintG - 1.0f) * veil;
                    tintB = 1.0f + (tintB - 1.0f) * veil;
                    const uint32_t colourMask = (0xFFu << fr.rShift) | (0xFFu << fr.gShift) | (0xFFu << fr.bShift);
                    const auto lane = [&](uint32_t shift, float tint) {
                        const float value = static_cast<float>((p >> shift) & 0xFFu) * gain * tint + add;
                        return static_cast<uint32_t>(std::clamp(value, 0.0f, 255.0f)) << shift;
                    };
                    o[x] = (p & ~colourMask) | lane(fr.rShift, tintR) | lane(fr.gShift, tintG) | lane(fr.bShift, tintB);
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Life. Leaves originate at real crown pixels, keep map XY/Z and are occluded against scene geometry.
        // Their colour and depth enter the world image before depth-of-field, rather than a final overlay.
        // ---------------------------------------------------------------------------------------------------------
        std::vector<Leaves::Leaf> _leaves;
        float _leafAcc = 0.0f;
        uint32_t _lifeRng = 0x9E3779B9u;
        float _lifeTime = 0.0f;

        inline float LifeRand()
        {
            _lifeRng ^= _lifeRng << 13;
            _lifeRng ^= _lifeRng >> 17;
            _lifeRng ^= _lifeRng << 5;
            return static_cast<float>(_lifeRng & 0xFFFFFFu) * (1.0f / 16777216.0f);
        }
        inline float LifeRange(float a, float b)
        {
            return a + (b - a) * LifeRand();
        }
        // blend one pixel (a = 0..1) unless it belongs to the interface
        uint32_t* _lifeTarget = nullptr; // buffer the sprites are drawn into (set per frame by DrawLife)
        uint16_t* _leafDepthTarget = nullptr;
        float _drawingLeafDepth = 0.0f;

        inline void LifePlot(const Frame& fr, int32_t x, int32_t y, float r, float g, float b, float a)
        {
            if (x < 0 || y < 0 || x >= fr.width || y >= fr.height || a <= 0.0f)
                return;
            const size_t index = static_cast<size_t>(y) * fr.width + x;
            if (fr.ui[index])
                return;
            if (fr.depth != nullptr && !Leaves::Visible(fr.depth[index], _drawingLeafDepth, fr.worldPerPixel))
                return; // the source tree/leaf is behind the visible building at this pixel
            if (_leafDepthTarget != nullptr)
                _leafDepthTarget[index] = Drawing::DepthEncode(static_cast<int32_t>(_drawingLeafDepth));
            uint32_t& p = _lifeTarget[index];
            const uint32_t colourMask = (0xFFu << fr.rShift) | (0xFFu << fr.gShift) | (0xFFu << fr.bShift);
            const auto mix = [&](uint32_t shift, float c) {
                const float old = static_cast<float>((p >> shift) & 0xFFu);
                return static_cast<uint32_t>(std::clamp(old + (std::clamp(c, 0.0f, 255.0f) - old) * a, 0.0f, 255.0f)) << shift;
            };
            p = (p & ~colourMask) | mix(fr.rShift, r) | mix(fr.gShift, g) | mix(fr.bShift, b);
        }

        inline void LifeRect(const Frame& fr, int32_t x, int32_t y, int32_t w, int32_t h, float r, float g, float b, float a)
        {
            for (int32_t j = 0; j < h; j++)
                for (int32_t i = 0; i < w; i++)
                    LifePlot(fr, x + i, y + j, r, g, b, a);
        }

        void DrawLife(const Frame& fr, const ViewInfo& view, const uint8_t* bitsOriginal, const uint8_t* sway,
            uint32_t* target, uint16_t* leafDepth)
        {
            if (fr.width < 32 || fr.height < 32)
                return;
            _lifeTarget = target;
            _leafDepthTarget = leafDepth;
            static auto lastClock = Clock::now();
            const auto now = Clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - lastClock).count(), 0.0f, 0.08f);
            lastClock = now;
            _lifeTime += dt;
            static const TileElement* previousMap = nullptr;
            static uint32_t previousTick = 0;
            const auto& state = getGameState();
            if (previousMap != state.tileElements.data() || state.currentTicks < previousTick)
                _leaves.clear();
            previousMap = state.tileElements.data();
            previousTick = state.currentTicks;
            const float wpp = std::max(0.2f, view.worldPerPixel);
            const float night = _time.active ? _time.night : 0.0f;
            const float light = std::clamp((1.0f - 0.7f * night), 0.25f, 1.0f);

            // ---- leaves ----
            const float leafSetting = _settings.leaves;
            if (leafSetting > 0.0f && sway != nullptr && bitsOriginal != nullptr && fr.worldPalette != nullptr
                && fr.depth != nullptr)
            {
                const float gust = 0.75f + 0.25f * std::sin(_lifeTime * 0.37f) + 0.1f * std::sin(_lifeTime * 1.13f + 2.0f);
                const float wind = std::clamp(_settings.windStrength * gust * (1.0f + 0.9f * _precipLevelNow), 0.0f, 2.0f);
                const float windAngle = _settings.cloudWind * 0.01745329252f;
                const float windX = std::cos(windAngle) * wind * 3.0f;
                const float windY = std::sin(windAngle) * wind * 3.0f;
                _leafAcc = wpp < 3.2f ? std::min(6.0f, _leafAcc + dt * leafSetting * (12.0f + 28.0f * wind)) : 0.0f;
                while (_leafAcc >= 1.0f && _leaves.size() < 600)
                {
                    _leafAcc -= 1.0f;
                    for (int32_t tries = 0; tries < 80; tries++)
                    {
                        const int32_t x = static_cast<int32_t>(LifeRange(2.0f, static_cast<float>(fr.width - 2)));
                        const int32_t y = static_cast<int32_t>(LifeRange(2.0f, static_cast<float>(fr.height - 2)));
                        const size_t index = static_cast<size_t>(y) * fr.width + x;
                        if ((sway[index] >> 4) < 6 || fr.ui[index] || fr.depth[index] == Drawing::kDepthUnknown)
                            continue;
                        const uint32_t colour = fr.worldPalette[bitsOriginal[index]];
                        auto leaf = Leaves::FromPixel((static_cast<float>(x) + view.originX) * wpp,
                            (static_cast<float>(y) + view.originY) * wpp, fr.depth[index], view.rotation);
                        if (!MapIsLocationValid(CoordsXY{ static_cast<int32_t>(leaf.x), static_cast<int32_t>(leaf.y) }))
                            continue;
                        leaf.groundZ = static_cast<float>(TileElementHeight(CoordsXY{
                            static_cast<int32_t>(leaf.x), static_cast<int32_t>(leaf.y) }));
                        if (leaf.z < leaf.groundZ + 8.0f)
                            continue;
                        // the colour of the crown it falls from, with a little variation
                        const float vary = LifeRange(0.9f, 1.12f);
                        const auto lane = [&](uint32_t shift) {
                            return static_cast<uint8_t>(std::min(255.0f, static_cast<float>((colour >> shift) & 0xFFu) * vary));
                        };
                        leaf.r = lane(fr.rShift);
                        leaf.g = lane(fr.gShift);
                        leaf.b = lane(fr.bShift);
                        leaf.speed = LifeRange(6.0f, 11.0f);
                        leaf.drift = LifeRange(-1.0f, 1.0f);
                        leaf.phase = LifeRange(0.0f, 6.2831853f);
                        leaf.life = std::clamp((leaf.z - leaf.groundZ) / leaf.speed + 1.5f, 2.0f, 25.0f);
                        _leaves.push_back(leaf);
                        break;
                    }
                }
                const int32_t sc = std::clamp(static_cast<int32_t>(std::lround(1.0f / wpp)), 1, 3);
                for (auto it = _leaves.begin(); it != _leaves.end();)
                {
                    auto& l = *it;
                    Leaves::Advance(l, dt, windX, windY);
                    if (l.age >= l.life || l.z <= l.groundZ + 1.0f)
                    {
                        it = _leaves.erase(it);
                        continue;
                    }
                    if (wpp >= 3.2f)
                    {
                        ++it;
                        continue;
                    }
                    const auto [px, py] = Leaves::Project(l, view.rotation, wpp,
                        static_cast<float>(view.originX), static_cast<float>(view.originY));
                    _drawingLeafDepth = Leaves::Depth(l, view.rotation);
                    const float fadeIn = std::min(1.0f, l.age / 0.7f);
                    const float fadeOut = std::clamp((l.z - l.groundZ) / 12.0f, 0.0f, 1.0f);
                    const float a = std::min(fadeIn, fadeOut);
                    const bool flat = std::sin(l.age * 1.6f + l.phase) > 0.0f;
                    const int32_t lx = static_cast<int32_t>(std::lround(px)), ly = static_cast<int32_t>(std::lround(py));
                    for (int32_t j = 0; j < 3; j++)
                        for (int32_t i = 0; i < 3; i++)
                        {
                            const int32_t tone = Leaves::kShape[j][i];
                            if (tone < 0)
                                continue;
                            const float shade = light * Leaves::kShade[tone];
                            LifeRect(fr, lx + (flat ? i : j) * sc, ly + (flat ? j : i) * sc, sc, sc,
                                l.r * shade, l.g * shade, l.b * shade, a * Leaves::kOpacity[tone]);
                        }
                    ++it;
                }
            }
            else if (leafSetting <= 0.0f)
                _leaves.clear();
            _leafDepthTarget = nullptr;
        }

        // Convex hull of up to 8 points (half resolution pixel coordinates), filled into the shadow plane. A building
        // footprint swept along the shadow direction is exactly such a hull, so every tile of a building leaves
        // its own shadow and the structure of the building shows in the shadow.
        void RasterShadowHull(uint8_t* plane, int16_t* planeZ, int32_t width, int32_t height, const float (*pts)[2], int32_t count, int16_t groundZ)
        {
            std::array<std::array<float, 2>, 8> p{};
            for (int32_t i = 0; i < count; i++)
                p[i] = { pts[i][0], pts[i][1] };
            std::sort(p.begin(), p.begin() + count, [](const auto& a, const auto& b) { return a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]); });
            const auto cross = [](const auto& o, const auto& a, const auto& b) {
                return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
            };
            std::array<std::array<float, 2>, 16> hull{};
            int32_t n = 0;
            for (int32_t i = 0; i < count; i++)
            {
                while (n >= 2 && cross(hull[n - 2], hull[n - 1], p[i]) <= 0.0f)
                    n--;
                hull[n++] = p[i];
            }
            for (int32_t i = count - 2, t = n + 1; i >= 0; i--)
            {
                while (n >= t && cross(hull[n - 2], hull[n - 1], p[i]) <= 0.0f)
                    n--;
                hull[n++] = p[i];
            }
            n--; // last point equals the first
            if (n < 3)
                return;
            float top = hull[0][1], bottom = hull[0][1];
            for (int32_t i = 1; i < n; i++)
            {
                top = std::min(top, hull[i][1]);
                bottom = std::max(bottom, hull[i][1]);
            }
            const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top))), y1 = std::min(height - 1, static_cast<int32_t>(std::floor(bottom)));
            for (int32_t y = y0; y <= y1; y++)
            {
                const float sy = static_cast<float>(y) + 0.5f;
                float xl = 1e9f, xr = -1e9f;
                for (int32_t i = 0; i < n; i++)
                {
                    const auto& a = hull[i];
                    const auto& b = hull[(i + 1) % n];
                    if ((a[1] <= sy && b[1] > sy) || (b[1] <= sy && a[1] > sy))
                    {
                        const float x = a[0] + (sy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]);
                        xl = std::min(xl, x);
                        xr = std::max(xr, x);
                    }
                }
                if (xl > xr)
                    continue;
                const int32_t c0 = std::max(0, static_cast<int32_t>(std::floor(xl))), c1 = std::min(width - 1, static_cast<int32_t>(std::floor(xr)));
                uint8_t* row = plane + static_cast<size_t>(y) * width;
                int16_t* rowZ = planeZ + static_cast<size_t>(y) * width;
                for (int32_t x = c0; x <= c1; x++)
                {
                    row[x] = std::max<uint8_t>(row[x], 190); // building blocks stay lighter than capsules
                    rowZ[x] = groundZ;
                }
            }
        }

        bool BuildObjectShadows(FrameState& f, const ViewInfo& view)
        {
            if (view.viewportWidth <= 0 || view.viewportHeight <= 0)
                return false;
            auto& state = getGameState();
            std::fill(f.shadowRaw.begin(), f.shadowRaw.end(), static_cast<uint8_t>(0));
            std::fill(f.shadowZ.begin(), f.shadowZ.end(), static_cast<int16_t>(INT16_MIN));
            f.hasFaceShading = false;
            std::fill(f.faceVal.begin(), f.faceVal.end(), static_cast<uint8_t>(0));
            constexpr float kPi = 3.14159265f;
            const float azimuth = EffectiveSunAzimuth() * kPi / 180.0f;
            const float awayX = -std::cos(azimuth), awayY = -std::sin(azimuth);
            const float invTan = 1.0f / std::tan(std::clamp(EffectiveSunElevation(), 8.0f, 85.0f) * kPi / 180.0f);
            const float reach = std::max(8.0f, _settings.shadowReach);
            const float wpp = std::max(0.25f, view.worldPerPixel);
            const float guestZoom = ShadowDistance::ZoomFade(_settings.shadowGuestDistance,wpp,true);
            const float vehicleZoom = RideShadows::kRenderingEnabled ? ShadowDistance::ZoomFade(_settings.shadowVehicleDistance,wpp,false) : 0.0f;
            const float trackZoom = RideShadows::kRenderingEnabled ? ShadowDistance::ZoomFade(_settings.shadowTrackDistance,wpp,false) : 0.0f;
            if (guestZoom <= 0 && vehicleZoom <= 0 && trackZoom <= 0)
                return false;
            const float scale = 0.5f / wpp;
            const uint8_t rotation = static_cast<uint8_t>(view.rotation);
            const auto project = [&](float x, float y, float z) {
                return ProjectPeepShadowPoint(x, y, z, rotation, scale,
                    static_cast<float>(view.originX), static_cast<float>(view.originY));
            };
            // length of the shadow of something that is `h` units high (compressed so very tall things stay bounded)
            const auto shadowLength = [&](float h) {
                const float ray = std::max(0.0f, h) * invTan;
                return reach * ray / (reach + ray);
            };
            const float centreScreenX = (view.originX + f.width * 0.5f) * wpp;
            const float centreScreenY = (view.originY + f.height * 0.5f) * wpp;
            float centreZ = 0;
            const auto& native = Drawing::gDepthPlane;
            std::vector<float> surfaceHeights;
            if (native.data != nullptr && native.size >= static_cast<size_t>(f.width) * f.height) {
                for (int dy : {-16,0,16}) for (int dx : {-16,0,16}) {
                    const int px=std::clamp(f.width/2+dx,0,f.width-1), py=std::clamp(f.height/2+dy,0,f.height-1);
                    const size_t i=static_cast<size_t>(py)*f.width+px;
                    const auto d = native.receiverDepth != nullptr ? native.receiverDepth[i]
                        : (native.shadowFlags != nullptr && (native.shadowFlags[i]&Drawing::kShadowReceiver)!=0
                            ? native.data[i] : Drawing::kDepthUnknown);
                    if(d!=Drawing::kDepthUnknown)
                        surfaceHeights.push_back(Drawing::DepthDecode(d)*0.5f-(py+view.originY)*wpp);
                }
            }
            if (!surfaceHeights.empty()) {
                std::sort(surfaceHeights.begin(),surfaceHeights.end());
                centreZ=surfaceHeights[surfaceHeights.size()/2];
            } else {
                const auto base = Coordinates::Unproject(centreScreenX,centreScreenY,0,rotation);
                if(base.first>=0 && base.second>=0 && base.first<state.mapSize.x*32 && base.second<state.mapSize.y*32)
                    centreZ=static_cast<float>(TileElementHeight({static_cast<int>(base.first),static_cast<int>(base.second)}));
            }
            const auto centre = Coordinates::Unproject(centreScreenX,centreScreenY,centreZ,rotation);
            const auto visibility = [&](float distance,float x,float y,float radius,float zoom) {
                return zoom * ShadowDistance::RangeFade(distance,x,y,centre.first,centre.second,
                    reach+radius*3.0f+4.0f*wpp+4.0f);
            };
            const float inner = 0.9f - 0.8f * _settings.shadowSoftness;
            const int32_t w = f.halfWidth, h = f.halfHeight;
            std::vector<ShadowCapsule> groundCasters;
            groundCasters.reserve(state.entities.getEntityList(EntityType::guest).size() + state.entities.getEntityList(EntityType::staff).size());
            const auto addCapsule = [&](float x, float y, float groundZ, float riseFrom, float riseTo, float radiusWorld, bool peep = false,
                                        bool fine = false, float opacity = 1.0f) {
                // point on the ground below the object, shadow of the part between riseFrom and riseTo above it
                const float push = radiusWorld * (peep ? 0.45f : 0.9f); // a guest's shadow starts at the feet
                const float d0 = shadowLength(riseFrom) + push, d1 = std::max(d0 + 2.0f, shadowLength(riseTo) + push);
                const auto projectPoint = [&](float px, float py) { return project(px, py, groundZ); };
                const auto a = projectPoint(x + awayX * d0, y + awayY * d0);
                const auto b = projectPoint(x + awayX * d1, y + awayY * d1);
                const float radius = std::max(0.9f, radiusWorld * scale);
                if (!ShadowDistance::Intersects(a.first,a.second,b.first,b.second,radius,w,h)) return;
                (void)fine;
                groundCasters.push_back(
                    { a.first, a.second, b.first, b.second, radius, static_cast<int16_t>(groundZ), opacity });
            };

            // Use the visible sprite depth only for scenery. The persistent ground receiver remains untouched,
            // keeping cloud/water sampling independent of moving visitors and vehicles.
            const auto& plane = Drawing::gDepthPlane;
            f.sceneryShadow.assign(static_cast<size_t>(f.width) * f.height, 0);
            struct SceneryCaster { float cx, cy, bottom, top, radius, low, opacity; int x0, x1, y0, y1; };
            std::vector<SceneryCaster> sceneryCasters;
            const auto addSceneryShadow = [&](float cx, float cy, float bottom, float top, float radius, float opacity) {
                if (plane.data == nullptr || plane.shadowFlags == nullptr || plane.size < f.sceneryShadow.size())
                    return;
                // A walking person's reception stays close to the walking surface, including raised paving.
                // Extending to terrain under a raised invisible path made tall, stretched scenery shadows.
                const float low = bottom - 16.0f;
                float left = 1e9f, right = -1e9f, upper = 1e9f, lower = -1e9f;
                for (float z : { low, top })
                    for (float d : { 0.0f, reach })
                    {
                        const auto p = project(cx + awayX * d, cy + awayY * d, z);
                        left = std::min(left, p.first * 2.0f); right = std::max(right, p.first * 2.0f);
                        upper = std::min(upper, p.second * 2.0f); lower = std::max(lower, p.second * 2.0f);
                    }
                const float pad = (radius * 2.0f + wpp) / wpp;
                const int x0 = std::max(0, static_cast<int>(std::floor(left - pad)));
                const int x1 = std::min(f.width - 1, static_cast<int>(std::ceil(right + pad)));
                const int y0 = std::max(0, static_cast<int>(std::floor(upper - pad)));
                const int y1 = std::min(f.height - 1, static_cast<int>(std::ceil(lower + pad)));
                if (x0 <= x1 && y0 <= y1)
                    sceneryCasters.push_back({ cx, cy, bottom, top, radius, low, opacity, x0, x1, y0, y1 });
            };

            const auto profStart = Clock::now();
            // guests and staff stand on the surface they walk on: the ground is their own height
            for (const auto type : { EntityType::guest, EntityType::staff })
                for (const EntityId id : state.entities.getEntityList(type))
                {
                    const auto* entity = state.entities.tryGetEntity(id);
                    if (entity == nullptr || entity->x < 0 || entity->y < 0 || entity->x >= state.mapSize.x * 32 || entity->y >= state.mapSize.y * 32)
                        continue;
                    if (entity->type != type)
                        continue; // never interpret free balloons, birds or particle entities as people
                    const auto* peep = static_cast<const Peep*>(entity);
                    if (peep->state == PeepState::picked || peep->state == PeepState::onRide || peep->state == PeepState::falling)
                        continue;
                    const float radius = type == EntityType::guest ? 4.0f : 4.4f;
                    const float opacity=visibility(_settings.shadowGuestDistance,entity->x,entity->y,radius,guestZoom);
                    if(opacity<=0)continue;
                    addSceneryShadow(entity->x, entity->y, entity->z, entity->z + (type == EntityType::guest ? 11.0f : 13.0f),
                        std::max(radius * 0.70710678f, 1.2727922f * wpp),opacity); // same projected radius/minimum as path mask
                    addCapsule(static_cast<float>(entity->x), static_cast<float>(entity->y), static_cast<float>(entity->z), 0.0f,
                        type == EntityType::guest ? 11.0f : 13.0f, radius, true, false, opacity);
                }
            // vehicles move above the ground: their shadow falls on the surface below
            if constexpr (RideShadows::kRenderingEnabled)
            for (const EntityId id : state.entities.getEntityList(EntityType::vehicle))
            {
                const auto* vehicle = state.entities.tryGetEntity(id);
                if (vehicle == nullptr || vehicle->x < 0 || vehicle->y < 0 || vehicle->x >= state.mapSize.x * 32
                    || vehicle->y >= state.mapSize.y * 32)
                    continue;
                if (!RideShadows::IsEnabled(static_cast<const Vehicle*>(vehicle)->ride))
                    continue;
                const float opacity=visibility(_settings.shadowVehicleDistance,vehicle->x,vehicle->y,11.0f,vehicleZoom);
                if(opacity<=0)continue;
                const float ground = static_cast<float>(TileElementHeight(CoordsXY{ vehicle->x, vehicle->y }));
                const float rise = std::max(0.0f, static_cast<float>(vehicle->z) - ground);
                addSceneryShadow(vehicle->x, vehicle->y, vehicle->z, vehicle->z + 15.0f, 11.0f,opacity);
                addCapsule(static_cast<float>(vehicle->x), static_cast<float>(vehicle->y), ground, rise, rise + 15.0f, 11.0f, false, true,opacity);
            }
            // Each visible scenery pixel is unprojected once, not once per nearby guest. Independent
            // row ownership lets workers write disjoint output regions with the exact same max union.
            f.sceneryRows.resize(f.height);
            const bool sameSceneryView = f.sceneryCacheValid && f.sceneryCacheWidth == f.width
                && f.sceneryCacheHeight == f.height && f.sceneryCacheX == view.originX && f.sceneryCacheY == view.originY
                && f.sceneryCacheRotation == rotation && f.sceneryCacheWpp == wpp;
            f.sceneryInputFlags.resize(f.sceneryShadow.size());
            f.sceneryInputDepth.resize(f.sceneryShadow.size());
            auto& pool = WorkerPool::Get();
            const int sceneryTasks = (f.height + 31) / 32;
            if (!sceneryCasters.empty()) pool.ParallelFor(sceneryTasks, [&](int task) {
                const int first = task * 32, last = std::min(f.height, first + 32);
                for (int py = first; py < last; py++) {
                    const size_t row = static_cast<size_t>(py) * f.width;
                    // Exact input comparison, never a timer/revision guess: moving sprites or changed
                    // scenery invalidate only affected rows; camera and zoom invalidate the whole view.
                    if (sameSceneryView
                        && std::memcmp(f.sceneryInputFlags.data()+row,plane.shadowFlags+row,f.width)==0
                        && std::memcmp(f.sceneryInputDepth.data()+row,plane.data+row,f.width*sizeof(uint16_t))==0)
                        continue;
                    std::memcpy(f.sceneryInputFlags.data()+row,plane.shadowFlags+row,f.width);
                    std::memcpy(f.sceneryInputDepth.data()+row,plane.data+row,f.width*sizeof(uint16_t));
                    auto& points = f.sceneryRows[py]; points.clear();
                    for (int px = 0; px < f.width; px++) {
                        const size_t i = static_cast<size_t>(py) * f.width + px;
                        if ((plane.shadowFlags[i] & Drawing::kSceneryReceiver) == 0 || plane.data[i] == Drawing::kDepthUnknown)
                            continue;
                        const float d = static_cast<float>(Drawing::DepthDecode(plane.data[i]));
                        const float z = d * 0.5f - (py + view.originY) * wpp;
                        const float sx = (px + view.originX) * wpp;
                        const float rx = (d - sx) * 0.5f, ry = (d + sx) * 0.5f;
                        float x = rx, y = ry;
                        switch (rotation & 3) {
                            case 1: x = -ry; y = rx; break;
                            case 2: x = -rx; y = -ry; break;
                            case 3: x = ry; y = -rx; break;
                        }
                        points.push_back({px,x,y,z});
                    }
                }
                for (const auto& caster : sceneryCasters) {
                    const int y0 = std::max(first,caster.y0), y1 = std::min(last-1,caster.y1);
                    for (int py = y0; py <= y1; py++) {
                        const auto& points = f.sceneryRows[py];
                        auto point = std::lower_bound(points.begin(),points.end(),caster.x0,
                            [](const auto& p,int x){return p.px<x;});
                        for (;point!=points.end() && point->px<=caster.x1;++point) {
                            if (point->z < caster.low || point->z >= caster.top) continue;
                            const auto value = static_cast<uint8_t>(std::lround(255.0f * SceneryShadows::Coverage(
                                point->x,point->y,point->z,caster.cx,caster.cy,caster.bottom,caster.top,caster.radius,
                                awayX,awayY,invTan,reach,_settings.shadowSoftness,wpp)*caster.opacity));
                            const size_t i=static_cast<size_t>(py)*f.width+point->px;
                            f.sceneryShadow[i]=std::max(f.sceneryShadow[i],value);
                        }
                    }
                }
            });
            f.sceneryCacheWidth=f.width; f.sceneryCacheHeight=f.height;
            f.sceneryCacheX=view.originX; f.sceneryCacheY=view.originY;
            f.sceneryCacheRotation=rotation; f.sceneryCacheWpp=wpp;
            f.sceneryCacheValid=!sceneryCasters.empty();
            // Opt-in regression oracle: the former dense scalar raster must produce identical bytes.
            static const bool verifyScenery = std::getenv("ATMOS_QA_VERIFY_SCENERY") != nullptr;
            if (verifyScenery) {
                std::vector<uint8_t> reference(f.sceneryShadow.size(),0);
                for (const auto& c : sceneryCasters)
                    for (int py=c.y0;py<=c.y1;py++) for(int px=c.x0;px<=c.x1;px++) {
                        const size_t i=static_cast<size_t>(py)*f.width+px;
                        if((plane.shadowFlags[i]&Drawing::kSceneryReceiver)==0 || plane.data[i]==Drawing::kDepthUnknown)continue;
                        const float d=static_cast<float>(Drawing::DepthDecode(plane.data[i]));
                        const float z=d*.5f-(py+view.originY)*wpp;
                        if(z<c.low || z>=c.top)continue;
                        const float sx=(px+view.originX)*wpp;
                        const float rx=(d-sx)*.5f,ry=(d+sx)*.5f;
                        float x=rx,y=ry;
                        switch(rotation&3){case 1:x=-ry;y=rx;break;case 2:x=-rx;y=-ry;break;case 3:x=ry;y=-rx;break;}
                        const auto value=static_cast<uint8_t>(std::lround(255.0f*SceneryShadows::Coverage(
                            x,y,z,c.cx,c.cy,c.bottom,c.top,c.radius,awayX,awayY,invTan,reach,_settings.shadowSoftness,wpp)*c.opacity));
                        reference[i]=std::max(reference[i],value);
                    }
                if(reference!=f.sceneryShadow)throw std::runtime_error("Scenery shadow optimization differs from scalar reference");
            }
            const auto profEntities = Clock::now();
            // track pieces (cached in map space): thin ribbons that follow the track
            if constexpr (RideShadows::kRenderingEnabled) {
            CollectTrackShadowPieces(view);
            for (const auto& segment : _trackShadows.segments)
            {
                if (!RideShadows::IsEnabled(segment.ride)) continue;
                const float opacity=visibility(_settings.shadowTrackDistance,(segment.a.x+segment.b.x)*.5f,
                    (segment.a.y+segment.b.y)*.5f,6.0f,trackZoom);
                if(opacity<=0)continue;
                const auto tip = [&](const CoordsXYZ& point) {
                    float ground = static_cast<float>(TileElementHeight(CoordsXY{ point.x, point.y }));
                    // Re-evaluate the landing surface after the horizontal sun displacement.
                    float distance = shadowLength(std::max(0.0f, point.z + 3.0f - ground));
                    const CoordsXY landing{ static_cast<int32_t>(point.x + awayX * distance),
                                            static_cast<int32_t>(point.y + awayY * distance) };
                    ground = static_cast<float>(TileElementHeight(landing));
                    distance = shadowLength(std::max(0.0f, point.z + 3.0f - ground));
                    return project(point.x + awayX * distance, point.y + awayY * distance, ground);
                };
                const auto a = tip(segment.a), b = tip(segment.b);
                const float radius=std::max(0.65f,3.0f*scale);
                if(!ShadowDistance::Intersects(a.first,a.second,b.first,b.second,radius,w,h))continue;
                // Narrow rail envelope leaves the broader car shadows visible above it.
                groundCasters.push_back(
                    { a.first, a.second, b.first, b.second, radius, 0, opacity });
            }
            }
            const auto profTracks = Clock::now();
            // Bin by output rows; each worker owns disjoint bytes, so no locks or atomic blending.
            // Preserve caster order inside each bin, including equal-coverage height ties.
            const int groundTasks = (h + 31) / 32;
            std::vector<std::vector<size_t>> groundBins(groundTasks);
            for (size_t i = 0; i < groundCasters.size(); i++) {
                const auto& c = groundCasters[i];
                const float top = std::min(c.ay,c.by)-c.radius*.5f;
                const float bottom = std::max(c.ay,c.by)+c.radius*.5f;
                if (bottom <= 0 || top >= h || std::max(c.ax,c.bx)+c.radius <= 0
                    || std::min(c.ax,c.bx)-c.radius >= w) continue;
                const int first = static_cast<int>(std::floor(std::max(0.0f,top))) / 32;
                const int last = static_cast<int>(std::floor(std::min(static_cast<float>(h-1),bottom))) / 32;
                for (int bin=first;bin<=last;bin++) groundBins[bin].push_back(i);
            }
            pool.ParallelFor(groundTasks,[&](int bin) {
                for (const auto i : groundBins[bin])
                    RasterPeepShadowCapsuleRows(f.shadowRaw.data(),f.shadowZ.data(),w,h,groundCasters[i],inner,
                        bin*32,std::min(h,bin*32+32));
            });
            static const bool verifyGround = std::getenv("ATMOS_QA_VERIFY_GROUND") != nullptr;
            if (verifyGround) {
                std::vector<uint8_t> reference(f.shadowRaw.size(),0);
                std::vector<int16_t> heights(f.shadowZ.size(),INT16_MIN);
                for (const auto& c : groundCasters)
                    RasterPeepShadowCapsule(reference.data(),heights.data(),w,h,c,inner);
                if(reference!=f.shadowRaw || heights!=f.shadowZ)
                    throw std::runtime_error("Parallel ground shadows differ from serial reference");
            }
            SoftenObjectShadows(f, wpp);
            {
                static double tE = 0, tT = 0, tS = 0;
                static int n = 0;
                const auto profEnd = Clock::now();
                tE += std::chrono::duration<double, std::milli>(profEntities - profStart).count();
                tT += std::chrono::duration<double, std::milli>(profTracks - profEntities).count();
                tS += std::chrono::duration<double, std::milli>(profEnd - profTracks).count();
                if (++n >= 300 && _settings.devPerfLog)
                {
                    std::ofstream prof(_settingsPath + ".shadow.log", std::ios::app);
                    prof << "shadow avg ms: entities " << tE / n << " tracks " << tT / n << " soften " << tS / n << " pieces " << _trackShadows.segments.size() << "\n";
                    tE = tT = tS = 0;
                    n = 0;
                }
            }
            return true;
        }

        constexpr int16_t kNoShadowZ = INT16_MIN;

        // Silhouette shadows. Every opaque pixel of a shadow casting sprite (buildings, trees, rides, tracks, guests,
        // staff, vehicles) knows its height above the ground (recorded while the sprite was painted). It is moved along
        // the fixed sun direction by that height, so the shadow has the exact outline of the object, sheared onto the
        // ground, and it turns with the sun and the camera rotation. Rows of the same sprite are filled as segments.
        // Receivers are checked in the finish stage by ground height, so shadows only land on the ground.
        bool BuildSpriteShadows(FrameState& f, const uint16_t* depth, const ViewInfo& view)
        {
            const auto& plane = Drawing::gDepthPlane;
            if (view.viewportWidth <= 0 || view.viewportHeight <= 0 || depth != plane.data
                || plane.shadowFlags == nullptr || plane.shadowHeight == nullptr
                || plane.size < static_cast<size_t>(f.width) * f.height)
                return false;
            if (view.worldPerPixel > 4.5f)
                return false; // far zoom: no shadows, nothing calculated
            const auto projection = MakeShadowProjection(
                EffectiveSunAzimuth(), EffectiveSunElevation(), _settings.shadowReach, view.rotation, view.worldPerPixel);
            std::fill(f.shadowRaw.begin(), f.shadowRaw.end(), static_cast<uint8_t>(0));
            std::fill(f.shadowZ.begin(), f.shadowZ.end(), kNoShadowZ);
            const int32_t hw = f.halfWidth, hh = f.halfHeight;
            const float wpp = view.worldPerPixel;
            for (int32_t y = 0; y < f.height; y++)
            {
                const uint8_t* flags = plane.shadowFlags + static_cast<size_t>(y) * f.width;
                const float screenY = (static_cast<float>(y) + static_cast<float>(view.originY)) * wpp;
                for (int32_t x = 0; x < f.width; x++)
                {
                    if ((flags[x] & Drawing::kShadowCaster) == 0)
                        continue;
                    const size_t index = static_cast<size_t>(y) * f.width + x;
                    if (f.uiMask[index] || depth[index] == Drawing::kDepthUnknown)
                        continue;
                    const uint16_t height = plane.shadowHeight[index];
                    // flat ground made of scenery (tiles) only reaches a few units above its own ground: no shadow
                    if (height <= 10)
                        continue;
                    const int32_t left = x;
                    while (x + 1 < f.width)
                    {
                        const size_t next = index + static_cast<size_t>(x + 1 - left);
                        if ((flags[x + 1] & Drawing::kShadowCaster) == 0 || f.uiMask[next] || plane.shadowHeight[next] != height)
                            break;
                        x++;
                    }
                    const auto p0 = ProjectShadowPixel(static_cast<float>(left) - 0.5f, static_cast<float>(y), static_cast<float>(height), projection);
                    const auto p1 = ProjectShadowPixel(static_cast<float>(x) + 0.5f, static_cast<float>(y), static_cast<float>(height), projection);
                    const int32_t rowA = static_cast<int32_t>(std::floor(p0.y - 0.5f));
                    const int32_t c0 = std::max(0, static_cast<int32_t>(std::floor(std::min(p0.x, p1.x))) - 1);
                    const int32_t c1 = std::min(hw - 1, static_cast<int32_t>(std::floor(std::max(p0.x, p1.x))) + 1);
                    if (c0 > c1)
                        continue;
                    const float D = static_cast<float>(Drawing::DepthDecode(depth[index]));
                    const int16_t groundZ = static_cast<int16_t>(std::clamp(D * 0.5f - screenY - static_cast<float>(height), -2000.0f, 2000.0f));
                    // two rows and one pixel of overlap on each side: consecutive sprite rows never leave gaps
                    for (int32_t row = std::max(0, rowA); row <= std::min(hh - 1, rowA + 1); row++)
                    {
                        uint8_t* out = f.shadowRaw.data() + static_cast<size_t>(row) * hw;
                        int16_t* outZ = f.shadowZ.data() + static_cast<size_t>(row) * hw;
                        for (int32_t cx = c0; cx <= c1; cx++)
                        {
                            out[cx] = 255;
                            outZ[cx] = groundZ;
                        }
                    }
                }
            }
            auto& pool = WorkerPool::Get();
            const int32_t tasks = (hh + 31) / 32;
            const int32_t radius = _settings.shadowSoftness <= 0.0f ? 0
                : std::clamp(static_cast<int32_t>(std::lround((0.8f + 3.0f * _settings.shadowSoftness) / (2.0f * wpp))), 1, 3);
            if (radius > 0)
            {
                pool.ParallelFor(tasks, [&](int32_t task) {
                    BlurShadowRows(f.shadowRaw.data(), f.shadowSoft.data(), hw, hh, true, radius, task * 32, std::min(hh, task * 32 + 32));
                });
                pool.ParallelFor(tasks, [&](int32_t task) {
                    BlurShadowRows(f.shadowSoft.data(), f.shadowRaw.data(), hw, hh, false, radius, task * 32, std::min(hh, task * 32 + 32));
                });
            }
            // the ground height spreads into the soft edge and closes one pixel gaps between rows
            f.shadowZTmp.resize(f.shadowZ.size());
            const int32_t zr = radius + 1;
            pool.ParallelFor(tasks, [&](int32_t task) {
                for (int32_t y = task * 32; y < std::min(hh, task * 32 + 32); y++)
                    for (int32_t x = 0; x < hw; x++)
                    {
                        int16_t z = kNoShadowZ;
                        for (int32_t dx = -zr; dx <= zr && z == kNoShadowZ; dx++)
                        {
                            const int32_t xx = x + dx;
                            if (xx >= 0 && xx < hw)
                                z = f.shadowZ[static_cast<size_t>(y) * hw + xx];
                        }
                        f.shadowZTmp[static_cast<size_t>(y) * hw + x] = z;
                    }
            });
            pool.ParallelFor(tasks, [&](int32_t task) {
                for (int32_t y = task * 32; y < std::min(hh, task * 32 + 32); y++)
                    for (int32_t x = 0; x < hw; x++)
                    {
                        int16_t z = kNoShadowZ;
                        for (int32_t dy = -zr; dy <= zr && z == kNoShadowZ; dy++)
                        {
                            const int32_t yy = y + dy;
                            if (yy >= 0 && yy < hh)
                                z = f.shadowZTmp[static_cast<size_t>(yy) * hw + x];
                        }
                        f.shadowZ[static_cast<size_t>(y) * hw + x] = z;
                    }
            });
            return true;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Rain and snow. Drawn by Atmosphere instead of the overlay of the game: tiles with streaks / flakes are sampled
        // along the fall direction (slanted by the wind) in three layers; a mask built from the cloud field decides where
        // it rains or snows, so there is no precipitation below a clear sky.
        // ---------------------------------------------------------------------------------------------------------
        struct PrecipState
        {
            const uint8_t* mask = nullptr;
            bool snow = false;
            float time = 0.0f, slant = 0.0f, zoomFade = 1.0f;
            float level = 0.0f; // strength of the current weather (0..1)
            float drift = 0.0f; // integral of the wind slant over time: snow drifts sideways by this much (no rotation of the pattern)
            float originX = 0.0f, originY = 0.0f; // view origin: rain and snow are anchored to the world, not to the screen
        };
        PrecipState _precip;
        uint8_t _rainTile[65536];
        uint8_t _snowTile[65536];
        bool _precipTilesReady = false;

        inline uint32_t XorShift(uint32_t& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }

        void BuildPrecipTiles()
        {
            std::memset(_rainTile, 0, sizeof(_rainTile));
            std::memset(_snowTile, 0, sizeof(_snowTile));
            uint32_t rng = 0x9E3779B9u;
            for (int32_t n = 0; n < 95; n++)
            {
                const int32_t v = static_cast<int32_t>(XorShift(rng) & 255), u0 = static_cast<int32_t>(XorShift(rng) & 255);
                const int32_t length = 10 + static_cast<int32_t>(XorShift(rng) % 20);
                const float peak = 110.0f + static_cast<float>(XorShift(rng) % 100);
                for (int32_t i = 0; i < length; i++)
                {
                    const float k = 1.0f - static_cast<float>(i) / static_cast<float>(length);
                    const uint8_t value = static_cast<uint8_t>(peak * k * k);
                    uint8_t& a = _rainTile[(v << 8) | ((u0 - i) & 255)];
                    a = std::max(a, value);
                    uint8_t& b = _rainTile[(((v + 1) & 255) << 8) | ((u0 - i) & 255)];
                    b = std::max(b, static_cast<uint8_t>(value * 0.12f));
                }
            }
            for (int32_t n = 0; n < 150; n++)
            {
                const int32_t cx = static_cast<int32_t>(XorShift(rng) & 255), cy = static_cast<int32_t>(XorShift(rng) & 255);
                const float radius = 0.6f + static_cast<float>(XorShift(rng) % 3) * 0.4f;
                for (int32_t dy = -3; dy <= 3; dy++)
                    for (int32_t dx = -3; dx <= 3; dx++)
                    {
                        const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                        const float value = 255.0f * std::clamp(1.0f - d / (radius + 0.7f), 0.0f, 1.0f);
                        uint8_t& a = _snowTile[(((cy + dy) & 255) << 8) | ((cx + dx) & 255)];
                        a = std::max(a, static_cast<uint8_t>(value));
                    }
            }
            _precipTilesReady = true;
        }

        void PrecipRows(const Frame& fr, const FrameState& f, float amount, int32_t y0, int32_t y1, uint32_t* dst, bool layer)
        {
            const bool snow = _precip.snow;
            const uint8_t* tile = snow ? _snowTile : _rainTile;
            const float slant = _precip.slant;
            const float norm = 1.0f / std::sqrt(1.0f + slant * slant);
            // Rain streaks lean with the wind. Snow always falls straight down and only drifts sideways: turning the
            // whole pattern with every gust would swirl it around the origin and make flakes jump (even upwards).
            const float fs = snow ? 0.0f : slant * norm, fc = snow ? 1.0f : norm; // fall direction on the screen (x right, y down)
            const float scale[3] = { snow ? 0.62f : 0.85f, snow ? 0.9f : 1.15f, snow ? 1.3f : 1.6f };
            const float speed[3] = { snow ? 52.0f : 900.0f, snow ? 36.0f : 700.0f, snow ? 25.0f : 520.0f };
            const float layerAlpha[3] = { snow ? 0.62f : 0.62f, snow ? 0.52f : 0.5f, snow ? 0.42f : 0.38f };
            const int32_t alphaQ[3] = { static_cast<int32_t>(layerAlpha[0] * 256.0f * _precip.zoomFade),
                                        static_cast<int32_t>(layerAlpha[1] * 256.0f * _precip.zoomFade),
                                        static_cast<int32_t>(layerAlpha[2] * 256.0f * _precip.zoomFade) };
            const int32_t amountQ = static_cast<int32_t>(std::min(amount, 1.5f) * 256.0f);
            const int32_t target[3] = { snow ? 250 : 196, snow ? 252 : 210, snow ? 255 : 230 };
            const int32_t width = fr.width, hw = f.halfWidth, hh = f.halfHeight;
            for (int32_t y = y0; y < y1; y++)
            {
                uint32_t* o = dst + static_cast<size_t>(y) * width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * width;
                const int32_t hy = y >> 1;
                const size_t i0 = static_cast<size_t>(hy) * hw;
                const size_t i1 = static_cast<size_t>(std::clamp(hy + ((y & 1) ? 1 : -1), 0, hh - 1)) * hw;
                int32_t uStart[3], vStart[3], du[3], dv[3];
                for (int32_t l = 0; l < 3; l++)
                {
                    const float sway = snow ? std::sin(_precip.time * 0.45f + static_cast<float>(y) * 0.01f + static_cast<float>(l) * 1.9f) * 7.0f : 0.0f;
                    const float wy = static_cast<float>(y) + _precip.originY;
                    const float u = (wy * fc + _precip.originX * fs - _precip.time * speed[l]) * scale[l];
                    const float vv = (-wy * fs + _precip.originX * fc + sway) * scale[l] + static_cast<float>(l) * 91.0f
                        - (snow ? _precip.drift * speed[l] : 0.0f);
                    uStart[l] = static_cast<int32_t>(std::fmod(u, 256.0f) * 65536.0f);
                    vStart[l] = static_cast<int32_t>(std::fmod(vv, 256.0f) * 65536.0f);
                    du[l] = static_cast<int32_t>(fs * scale[l] * 65536.0f);
                    dv[l] = static_cast<int32_t>(fc * scale[l] * 65536.0f);
                }
                for (int32_t x = 0; x < width; x++)
                {
                    if (ui[x])
                        continue;
                    const int32_t hx = x >> 1;
                    const int32_t hxo = std::clamp(hx + ((x & 1) ? 1 : -1), 0, hw - 1);
                    const uint32_t m = (9u * _precip.mask[i0 + hx] + 3u * _precip.mask[i0 + hxo] + 3u * _precip.mask[i1 + hx]
                                        + _precip.mask[i1 + hxo] + 8u)
                        >> 4;
                    if (m == 0)
                        continue;
                    int32_t sum = 0;
                    for (int32_t l = 0; l < 3; l++)
                    {
                        const uint32_t uu = static_cast<uint32_t>((uStart[l] + x * du[l]) >> 16) & 255u;
                        const uint32_t vv = static_cast<uint32_t>((vStart[l] + x * dv[l]) >> 16) & 255u;
                        sum += (tile[(vv << 8) | uu] * alphaQ[l]) >> 8;
                    }
                    int32_t a = (sum * static_cast<int32_t>(m) >> 8) * amountQ >> 8;
                    if (a <= 0)
                        continue;
                    a = std::min(a, 255);
                    const int32_t k = a + (a >> 7);
                    uint32_t p = o[x];
                    const uint32_t shifts[3] = { fr.rShift, fr.gShift, fr.bShift };
                    if (layer)
                    {
                        // own layer: premultiplied colour + coverage, blurred on its own and laid over the finished frame
                        p = static_cast<uint32_t>(std::min(a, 255)) << fr.freeShift;
                        for (int32_t c = 0; c < 3; c++)
                            p |= static_cast<uint32_t>(target[c] * k >> 8) << shifts[c];
                        o[x] = p;
                        continue;
                    }
                    for (int32_t c = 0; c < 3; c++)
                    {
                        const int32_t old = static_cast<int32_t>((p >> shifts[c]) & 0xFFu);
                        const int32_t value = std::clamp(old + (((target[c] - old) * k) >> 8), 0, 255);
                        p = (p & ~(0xFFu << shifts[c])) | (static_cast<uint32_t>(value) << shifts[c]);
                    }
                    o[x] = p;
                }
            }
        }

        bool WeatherIsSnowing()
        {
            switch (getGameState().weatherCurrent.weatherType)
            {
                case Weather::Type::snow:
                case Weather::Type::heavySnow:
                case Weather::Type::blizzard:
                    return true;
                default:
                    return false;
            }
        }

        bool WeatherHasPrecipitation()
        {
            switch (getGameState().weatherCurrent.weatherType)
            {
                case Weather::Type::rain:
                case Weather::Type::heavyRain:
                case Weather::Type::thunder:
                case Weather::Type::snow:
                case Weather::Type::heavySnow:
                case Weather::Type::blizzard:
                    return true;
                default:
                    return false;
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Clouds. A drifting density field in map space gives (a) the visible cloud layer, seen on a plane high above the
        // map, and (b) the cloud shadows on the ground, which are the same field shifted along the sun direction by the
        // cloud altitude. Everything is evaluated on a coarse grid of nodes and then interpolated to half resolution.
        // ---------------------------------------------------------------------------------------------------------
        inline float SmoothStep01(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        inline float LatticeValue(int32_t x, int32_t y)
        {
            uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return static_cast<float>(h & 0xFFFFFFu) * (1.0f / 16777215.0f);
        }

        inline float ValueNoise(float x, float y)
        {
            const float fx = std::floor(x), fy = std::floor(y);
            const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
            float tx = x - fx, ty = y - fy;
            tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
            ty = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
            const float a = LatticeValue(ix, iy), b = LatticeValue(ix + 1, iy);
            const float c = LatticeValue(ix, iy + 1), d = LatticeValue(ix + 1, iy + 1);
            return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
        }

        // Continuous inverse of Translate3DTo2DWithZ; altitude belongs to the sampled layer.
        // Keeping fractional map coordinates avoids rotation-dependent truncation near the map edge.
        std::pair<float, float> CloudMapPoint(float screenX, float screenY, float altitude, int32_t rotation)
        {
            const float x = screenY + altitude - screenX * 0.5f;
            const float y = screenY + altitude + screenX * 0.5f;
            switch (rotation & 3)
            {
                default: return { x, y };
                case 1: return { -y, x };
                case 2: return { -x, -y };
                case 3: return { y, -x };
            }
        }

        float WeatherCloudCoverage(float configured, float multiplier, float precipitation, bool weatherEffects)
        {
            // Weather changes thickness, light and precipitation, never the user's selected bank footprint.
            // In particular a blizzard cannot push moderate cover across the broad-noise overcast threshold.
            (void)precipitation;
            (void)weatherEffects;
            if (configured >= 1.0f) return 1.0f;
            return std::clamp(configured * std::clamp(multiplier, 0.55f, 1.0f), 0.0f, 1.0f);
        }

        struct CloudFrame
        {
            float coverage = 0.0f, size = 1800.0f, windX = 0.0f, windY = 0.0f;
            float weight[5] = {};
            float minWavelength = 0.0f; // octaves finer than this are faded out (they would alias on the coarse node grid)
        };

        // Five octaves; each moves at a slightly different speed, so the clouds slowly change shape while they drift.
        inline float CloudFbm(const CloudFrame& c, float x, float y, float sizeScale, float seed)
        {
            float sum = 0.0f, used = 0.0f;
            float freq = 1.0f / (c.size * sizeScale);
            for (int32_t i = 0; i < 5; i++)
            {
                const float wavelength = 1.0f / freq;
                const float fade = c.minWavelength > 0.0f ? SmoothStep01(c.minWavelength, 2.0f * c.minWavelength, wavelength) : 1.0f;
                const float weight = c.weight[i] * fade;
                if (weight > 0.0f)
                {
                    const float drift = 1.0f + 0.18f * static_cast<float>(i);
                    sum += weight
                        * ValueNoise(
                              (x - c.windX * drift) * freq + seed + 17.3f * static_cast<float>(i),
                              (y - c.windY * drift) * freq - seed * 0.7f + 9.1f * static_cast<float>(i));
                    used += weight;
                }
                freq *= 2.0f;
            }
            // keep the mean level and contrast of the full sum
            return used > 0.0f ? 0.5f + (sum / used - 0.5f) * std::min(1.0f, 0.6f + used) : 0.5f;
        }

        inline float CloudShape(float n, float coverage)
        {
            // A finite support gives actual clear sky between cloud banks. The narrow soft skirt grows into a dense
            // body, rather than lifting the entire field into a translucent veil. At full coverage it is overcast.
            if (coverage >= 1.0f) return 1.0f;
            const float threshold = 0.71f - coverage * 0.38f;
            return SmoothStep01(threshold - 0.035f, threshold + 0.18f, n) * SmoothStep01(0.0f, 0.06f, coverage);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Snow on the ground. A coarse map of the snow depth per tile grows under the snow clouds and melts away when it
        // stops snowing (or rains). A half resolution plane looks the map up for every pixel; the final pass turns up
        // facing surfaces (flat sprites in the depth plane, tops of trees) white in an ordered dither, which keeps the
        // pixel look of the game.
        // ---------------------------------------------------------------------------------------------------------
        struct SnowGrid
        {
            int32_t w = 0, h = 0;
            std::vector<uint16_t> v;
            uint16_t maxValue = 0;
            float accumulated = 0.0f;
        };
        SnowGrid _snow;

        // Snow on the ground needs the depth and tree marks only while there is snow (or it is snowing).
        bool SnowWanted()
        {
            return false; // snow that stays on the ground is switched off for now (looked wrong)
        }


        void UpdateSnowGrid(
            const ViewInfo& view, const CloudFrame& cf, float snowAmount, float rainAmount, float dt, float windDirX, float windDirY,
            float windNow, float altitude);

        void SnowCoverRows(
            const FrameState& f, const uint16_t* depth, const uint8_t* ui, const ViewInfo& v, uint8_t* out, int32_t y0, int32_t y1)
        {
            const int32_t w = f.width, h = f.height, hw = f.halfWidth;
            const float wpp = v.worldPerPixel;
            const int32_t inverseRotation = (4 - v.rotation) & 3;
            for (int32_t y = y0; y < y1; y++)
            {
                const int32_t py = std::min(2 * y, h - 1);
                uint8_t* o = out + static_cast<size_t>(y) * hw;
                for (int32_t x = 0; x < hw; x++)
                {
                    o[x] = 0;
                    const int32_t px = std::min(2 * x, w - 1);
                    const size_t index = static_cast<size_t>(py) * w + px;
                    if (ui[index])
                        continue;
                    const uint16_t d = depth[index];
                    if (d == 0xFFFF)
                        continue;
                    const float D = static_cast<float>((static_cast<int32_t>(d) - 32768) * 2);
                    const float sx = (static_cast<float>(px) + static_cast<float>(v.originX)) * wpp;
                    const CoordsXY rotated{ static_cast<int32_t>((D - sx) * 0.5f), static_cast<int32_t>((D + sx) * 0.5f) };
                    const CoordsXY map = rotated.rotate(inverseRotation);
                    // bilinear between the four tiles around the position
                    const float fx = static_cast<float>(map.x) / 32.0f - 0.5f, fy = static_cast<float>(map.y) / 32.0f - 0.5f;
                    const int32_t tx = static_cast<int32_t>(std::floor(fx)), ty = static_cast<int32_t>(std::floor(fy));
                    if (tx < 0 || ty < 0 || tx + 1 >= _snow.w || ty + 1 >= _snow.h)
                        continue;
                    const float ax = fx - static_cast<float>(tx), ay = fy - static_cast<float>(ty);
                    const uint16_t* g = _snow.v.data() + static_cast<size_t>(ty) * _snow.w + tx;
                    const float top = static_cast<float>(g[0]) + (static_cast<float>(g[1]) - static_cast<float>(g[0])) * ax;
                    const float bottom = static_cast<float>(g[_snow.w]) + (static_cast<float>(g[_snow.w + 1]) - static_cast<float>(g[_snow.w])) * ax;
                    o[x] = static_cast<uint8_t>(std::clamp((top + (bottom - top) * ay) * (1.0f / 257.0f), 0.0f, 255.0f));
                }
            }
        }

        void SnowRows(
            const Frame& fr, const FrameState& f, const uint16_t* depth, const uint8_t* sway, float wpp, float maxCover,
            int32_t y0, int32_t y1)
        {
            const int32_t width = fr.width, height = fr.height, hw = f.halfWidth, hh = f.halfHeight;
            const int32_t flatMax = static_cast<int32_t>(2.0f * wpp + 1.0f);
            const uint8_t* plane = f.snowPlane.data();
            for (int32_t y = y0; y < y1; y++)
            {
                uint32_t* o = fr.out + static_cast<size_t>(y) * width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * width;
                const uint16_t* dr = depth + static_cast<size_t>(y) * width;
                const uint16_t* dn = depth + static_cast<size_t>(std::min(y + 1, height - 1)) * width;
                const uint8_t* sw = sway != nullptr ? sway + static_cast<size_t>(y) * width : nullptr;
                const int32_t hy = y >> 1;
                const size_t i0 = static_cast<size_t>(hy) * hw;
                const size_t i1 = static_cast<size_t>(std::clamp(hy + ((y & 1) ? 1 : -1), 0, hh - 1)) * hw;
                for (int32_t x = 0; x < width; x++)
                {
                    if (ui[x])
                        continue;
                    const int32_t hx = x >> 1;
                    const int32_t hxo = std::clamp(hx + ((x & 1) ? 1 : -1), 0, hw - 1);
                    const uint32_t cover = (9u * plane[i0 + hx] + 3u * plane[i0 + hxo] + 3u * plane[i1 + hx] + plane[i1 + hxo] + 8u) >> 4;
                    if (cover < 6)
                        continue;
                    float weight = 0.0f; // how much of the cover this pixel receives
                    if (dr[x] != 0xFFFF && dn[x] != 0xFFFF)
                    {
                        const int32_t delta = static_cast<int32_t>(dn[x]) - static_cast<int32_t>(dr[x]);
                        if (delta >= 1 && delta <= flatMax)
                            weight = 1.0f; // upward facing: ground, paths, flat roofs
                    }
                    if (weight == 0.0f && sw != nullptr && (sw[x] >> 4) >= 7)
                        weight = 0.55f; // upper part of trees and foliage
                    if (weight == 0.0f)
                        continue;
                    const float amount = static_cast<float>(cover) * (1.0f / 255.0f) * maxCover * weight;
                    // powdery look: a pixel hash decides, not a regular pattern
                    uint32_t hsh = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
                    hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
                    if (static_cast<float>((hsh >> 8) & 255u) * (1.0f / 256.0f) > amount * 0.85f)
                        continue;
                    uint32_t p = o[x];
                    const int32_t r = static_cast<int32_t>((p >> fr.rShift) & 0xFFu), g = static_cast<int32_t>((p >> fr.gShift) & 0xFFu),
                                  b = static_cast<int32_t>((p >> fr.bShift) & 0xFFu);
                    if (b > r + 22 && g > r + 8)
                        continue; // water stays water
                    const int32_t lum = (r * 3 + g * 6 + b) / 10;
                    const int32_t sr = 226 + (lum >> 4), sg = 233 + (lum >> 5), sb = 244 + (lum >> 6);
                    const int32_t nr = r + (((sr - r) * 224) >> 8), ng = g + (((sg - g) * 224) >> 8), nb = b + (((sb - b) * 224) >> 8);
                    p = (p & ~((0xFFu << fr.rShift) | (0xFFu << fr.gShift) | (0xFFu << fr.bShift)))
                        | (static_cast<uint32_t>(std::clamp(nr, 0, 255)) << fr.rShift) | (static_cast<uint32_t>(std::clamp(ng, 0, 255)) << fr.gShift)
                        | (static_cast<uint32_t>(std::clamp(nb, 0, 255)) << fr.bShift);
                    o[x] = p;
                }
            }
        }

        void UpdateSnowGrid(
            const ViewInfo& view, const CloudFrame& cf, float snowAmount, float rainAmount, float dt, float windDirX, float windDirY,
            float windNow, float altitude)
        {
            _snow.accumulated += dt;
            if (_snow.accumulated < 0.12f)
                return;
            const float step = _snow.accumulated;
            _snow.accumulated = 0.0f;
            const auto mapSize = getGameState().mapSize;
            if (_snow.w != mapSize.x || _snow.h != mapSize.y)
            {
                _snow.w = mapSize.x;
                _snow.h = mapSize.y;
                _snow.v.assign(static_cast<size_t>(_snow.w) * _snow.h, 0);
                _snow.maxValue = 0;
            }
            // melting: slowly when it is simply not snowing, quickly in rain
            float melt = 0.0f;
            if (snowAmount < 0.05f)
                melt = 65535.0f / 240.0f;
            if (rainAmount > 0.05f)
                melt += 65535.0f / 50.0f * rainAmount;
            if (melt > 0.0f && _snow.maxValue > 0)
            {
                const int32_t sub = static_cast<int32_t>(melt * step) + 1;
                uint16_t peak = 0;
                for (auto& value : _snow.v)
                {
                    value = value > sub ? static_cast<uint16_t>(value - sub) : 0;
                    peak = std::max(peak, value);
                }
                _snow.maxValue = peak;
            }
            if (snowAmount < 0.05f)
                return;
            // snowing: tiles under dense cloud collect snow
            const float wpp = view.worldPerPixel;
            const float left = static_cast<float>(view.originX) * wpp, top = static_cast<float>(view.originY) * wpp;
            const float right = left + static_cast<float>(view.viewportWidth) * wpp;
            const float bottom = top + static_cast<float>(view.viewportHeight) * wpp;
            int32_t minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
            for (const float zValue : { 0.0f, 300.0f })
                for (int32_t corner = 0; corner < 4; corner++)
                {
                    const ScreenCoordsXY p{ static_cast<int32_t>((corner & 1) ? right : left), static_cast<int32_t>((corner & 2) ? bottom : top) };
                    const CoordsXY map = ViewportPosToMapPos(p, static_cast<int32_t>(zValue), static_cast<uint8_t>(view.rotation));
                    minX = std::min(minX, map.x);
                    maxX = std::max(maxX, map.x);
                    minY = std::min(minY, map.y);
                    maxY = std::max(maxY, map.y);
                }
            const int32_t x0 = std::max(0, minX / 32 - 1), y0 = std::max(0, minY / 32 - 1);
            const int32_t x1 = std::min(_snow.w - 1, maxX / 32 + 1), y1 = std::min(_snow.h - 1, maxY / 32 + 1);
            if (x1 < x0 || y1 < y0)
                return;
            const float add = 65535.0f / 100.0f * snowAmount * step; // a heavy snowfall covers the ground in about 100 s
            const float drop = altitude * 0.3f * windNow;
            for (int32_t ty = y0; ty <= y1; ty++)
                for (int32_t tx = x0; tx <= x1; tx++)
                {
                    const float X = static_cast<float>(tx * 32 + 16), Y = static_cast<float>(ty * 32 + 16);
                    const float density = SmoothStep01(0.3f, 0.75f, CloudShape(CloudFbm(cf, X - windDirX * drop, Y - windDirY * drop, 1.0f, 0.0f), cf.coverage));
                    if (density <= 0.0f)
                        continue;
                    uint16_t& value = _snow.v[static_cast<size_t>(ty) * _snow.w + tx];
                    value = static_cast<uint16_t>(std::min(65535.0f, static_cast<float>(value) + add * density));
                    _snow.maxValue = std::max(_snow.maxValue, value);
                }
        }

        void BuildCloudPlanes(FrameState& f, const uint16_t* depth, [[maybe_unused]] const uint8_t* ui, const ViewInfo& v)
        {
            static const auto startTime = Clock::now();
            static auto lastTime = startTime;
            static bool weatherInit = false;
            static float coverageMul = 1.0f, gloom = 0.0f, fogAdd = 0.0f, rainAmount = 0.0f, snowAmount = 0.0f;
            const auto now = Clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - lastTime).count(), 0.0f, 0.25f);
            lastTime = now;
            const float t = std::chrono::duration<float>(now - startTime).count();

            // Weather of the game -> cloud amount, darkness, fog and precipitation (eased, the game switches abruptly)
            float targetMul = 1.0f, targetGloom = 0.0f, targetFog = 0.0f, targetRain = 0.0f, targetSnow = 0.0f;
            if (_settings.cloudWeather)
            {
                switch (getGameState().weatherCurrent.weatherType)
                {
                    case Weather::Type::sunny:
                        targetMul = 0.55f;
                        break;
                    case Weather::Type::partiallyCloudy:
                        break;
                    case Weather::Type::cloudy:
                        targetMul = 1.5f, targetGloom = 0.15f;
                        break;
                    case Weather::Type::rain:
                        targetMul = 1.9f, targetGloom = 0.4f, targetFog = 0.06f, targetRain = 0.5f;
                        break;
                    case Weather::Type::heavyRain:
                        targetMul = 2.1f, targetGloom = 0.55f, targetFog = 0.1f, targetRain = 0.8f;
                        break;
                    case Weather::Type::thunder:
                        targetMul = 2.3f, targetGloom = 0.7f, targetFog = 0.1f, targetRain = 1.0f;
                        break;
                    case Weather::Type::snow:
                        targetMul = 1.8f, targetGloom = 0.25f, targetFog = 0.04f, targetSnow = 0.5f;
                        break;
                    case Weather::Type::heavySnow:
                        targetMul = 2.0f, targetGloom = 0.35f, targetFog = 0.06f, targetSnow = 0.8f;
                        break;
                    case Weather::Type::blizzard:
                        targetMul = 2.2f, targetGloom = 0.45f, targetFog = 0.08f, targetSnow = 1.0f;
                        break;
                    default:
                        break;
                }
            }
            const float ease = weatherInit ? 1.0f - std::exp(-dt * 0.035f) : 1.0f; // about 30 seconds
            weatherInit = true;
            coverageMul += (targetMul - coverageMul) * ease;
            gloom += (targetGloom - gloom) * ease;
            fogAdd += (targetFog - fogAdd) * ease;
            rainAmount += (targetRain - rainAmount) * ease;
            snowAmount += (targetSnow - snowAmount) * ease;
            const float precipLevel = std::max(rainAmount, snowAmount);

            const float wpp = v.worldPerPixel;
            constexpr float kPi = 3.14159265f;
            constexpr float kBaseWind = 14.0f; // map units per second at speed 1
            CloudFrame cf;
            const float precipMin = _settings.weatherEffects ? precipLevel * 0.30f : 0.0f;
            cf.coverage = WeatherCloudCoverage(_settings.cloudCoverage, coverageMul, precipLevel, _settings.weatherEffects);
            _sunBlockByWeather = std::clamp(std::max(cf.coverage * 0.9f, precipMin), 0.0f, 1.0f);
            cf.size = _settings.cloudSize; // constant: a changing scale would make the whole pattern slide across the map
            cf.minWavelength = 30.0f * wpp;
            const float windAngle = _settings.cloudWind * kPi / 180.0f;
            // The cloud position is integrated over time: changing the speed changes the pace smoothly (no jump), and
            // the weather type never speeds the clouds up.
            static float windPosition = 0.0f;
            windPosition += kBaseWind * _settings.cloudSpeed * dt;
            const float windDistance = windPosition;
            cf.windX = std::cos(windAngle) * windDistance;
            cf.windY = std::sin(windAngle) * windDistance;
            // Zoomed out the large structures count more and the finest detail would be sub-pixel anyway.
            const float zoomOut = std::clamp(std::log2(std::max(wpp, 1.0f)), 0.0f, 3.0f);
            const float base[5] = { 0.52f * (1.0f + 0.35f * zoomOut), 0.27f, 0.14f, 0.07f * std::max(0.2f, 1.0f - 0.3f * zoomOut),
                                    0.03f * std::max(0.2f, 1.0f - 0.3f * zoomOut) };
            float total = 0.0f;
            for (float w : base)
                total += w;
            for (int32_t i = 0; i < 5; i++)
                cf.weight[i] = base[i] / total;

            const float fogAmount = std::clamp(_settings.fog + fogAdd, 0.0f, 1.0f);
            // wind with gusts, in map space (unit vector scaled by strength)
            const float gust = 0.75f + 0.25f * std::sin(t * 0.37f) + 0.1f * std::sin(t * 1.13f + 2.0f);
            const float windNow = _settings.windStrength * gust * (1.0f + 0.3f * snowAmount * snowAmount);
            const float windDirX = std::cos(windAngle), windDirY = std::sin(windAngle);
            if (_settings.snowCover > 0.0f)
                UpdateSnowGrid(v, cf, snowAmount, rainAmount, dt, windDirX, windDirY, windNow, _settings.cloudHeight);
            const bool wantClouds = cf.coverage > 0.0f;
            const float detailAmount = 0.4f * (1.0f - SmoothStep01(1.0f, 6.0f, wpp)); // finer than a node would alias
            const float altitude = _settings.cloudHeight;
            const float az = EffectiveSunAzimuth() * kPi / 180.0f;
            const float shiftLength = altitude / std::tan(std::clamp(EffectiveSunElevation(), 10.0f, 85.0f) * kPi / 180.0f);
            const float sunDirX = std::cos(az), sunDirY = std::sin(az);
            const float sunX = sunDirX * shiftLength, sunY = sunDirY * shiftLength;
            // Lighting distances scale with the cloud bank, so changing size does not turn the shading into speckles.
            const float lightStep = std::max(cf.minWavelength, cf.size * 0.065f)
                / std::max(0.35f, std::sin(EffectiveSunElevation() * kPi / 180.0f));
            // zoom: the visible layer thins out when zooming in, the centre of the screen is kept clear
            // targets by zoom (log2 of the world units per pixel: 0 = closest), eased over time
            static float zoomOpacityS = -1.0f, holeFactorS = 1.0f, precipFadeS = 1.0f;
            const float zl = std::log2(std::max(wpp, 1.0f));
            const float zoomOpacityT = 0.7f + 0.3f * SmoothStep01(0.0f, 1.0f, zl);
            // the clear centre shrinks quickly when zooming out: 100 %, 55 %, 12 % and none at the highest level
            const float holeFactorT = zl <= 1.0f ? 1.0f - 0.45f * zl : (zl <= 2.0f ? 0.55f - 0.43f * (zl - 1.0f) : std::max(0.0f, 0.12f * (3.0f - zl)));
            const float precipFadeT = 1.0f - 0.9f * SmoothStep01(1.0f, 3.0f, zl);
            const float zoomEase = zoomOpacityS < 0.0f ? 1.0f : 1.0f - std::exp(-dt * 4.0f);
            if (zoomOpacityS < 0.0f)
                zoomOpacityS = zoomOpacityT;
            zoomOpacityS += (zoomOpacityT - zoomOpacityS) * zoomEase;
            holeFactorS += (holeFactorT - holeFactorS) * zoomEase;
            precipFadeS += (precipFadeT - precipFadeS) * zoomEase;
            const float zoomOpacity = zoomOpacityS;
            const float holeRadius = _settings.cloudFocus ? _settings.cloudClear * holeFactorS : 0.0f;
            const float opacity = _settings.cloudOpacity * zoomOpacity;
            const float shadowStrength = _settings.cloudShadow;
            const bool precipActive = _settings.weatherEffects && precipLevel > 0.01f;
            const float opacityRaw = std::min(1.0f, _settings.cloudOpacity + 0.5f * precipLevel); // rain falls from thick cloud even if it is drawn thin

            // The planes only change slowly: rebuild them when the view or a setting changes, otherwise every few frames.
            uint32_t keyHash = 2166136261u;
            const auto mix = [&](float value, float scale) {
                const int32_t q = static_cast<int32_t>(std::lround(value * scale));
                keyHash = (keyHash ^ static_cast<uint32_t>(q)) * 16777619u;
            };
            mix(static_cast<float>(v.originX), 1.0f);
            mix(static_cast<float>(v.originY), 1.0f);
            mix(static_cast<float>(v.rotation), 1.0f);
            mix(static_cast<float>(f.width), 1.0f);
            mix(static_cast<float>(f.height), 1.0f);
            mix(static_cast<float>(getGameState().mapSize.x), 1.0f);
            mix(static_cast<float>(getGameState().mapSize.y), 1.0f);
            mix(wpp, 16.0f);
            mix(cf.coverage, 200.0f);
            mix(cf.size, 0.1f);
            mix(altitude, 1.0f);
            mix(opacity, 200.0f);
            mix(opacityRaw, 200.0f);
            mix(shadowStrength, 200.0f);
            mix(fogAmount, 200.0f);
            mix(precipLevel, 100.0f);
            mix(holeRadius, 200.0f);
            mix(_settings.cloudFocus ? 1.0f : 0.0f, 1.0f);
            mix(windNow, 60.0f);
            mix(EffectiveSunAzimuth(), 4.0f);
            mix(EffectiveSunElevation(), 4.0f);
            mix(_settings.cloudSpeed, 100.0f);
            mix(_settings.cloudWind, 1.0f);
            mix(wantClouds ? 1.0f : 0.0f, 1.0f);
            mix(precipActive ? 1.0f : 0.0f, 1.0f);
            mix(snowAmount > rainAmount ? 1.0f : 0.0f, 1.0f);
            mix(depth != nullptr ? 1.0f : 0.0f, 1.0f);
            keyHash = (keyHash ^ Drawing::gMapRevision.load(std::memory_order_relaxed)) * 16777619u;
            static uint32_t lastKey = 0;
            static int32_t framesSinceBuild = 1000;
            const bool rebuild = keyHash != lastKey || ++framesSinceBuild >= 5;
            if (rebuild)
            {
                lastKey = keyHash;
                framesSinceBuild = 0;
            }

            constexpr int32_t kNodeStep = 8;
            const int32_t w = f.width, h = f.height;
            f.nodesX = (w + kNodeStep - 1) / kNodeStep + 1;
            f.nodesY = (h + kNodeStep - 1) / kNodeStep + 1;
            const size_t nodeCount = static_cast<size_t>(f.nodesX) * f.nodesY;
            f.nodeCloud.resize(nodeCount);
            f.nodeLit.resize(nodeCount);
            f.nodeShadow.resize(nodeCount);
            f.nodeFog.resize(nodeCount);

            auto& pool = WorkerPool::Get();
            if (rebuild)
            {
            pool.ParallelFor(f.nodesY, [&](int32_t j) {
                const int32_t py = std::min(j * kNodeStep, h - 1);
                const float screenY = (static_cast<float>(py) + static_cast<float>(v.originY)) * wpp;
                for (int32_t i = 0; i < f.nodesX; i++)
                {
                    const int32_t px = std::min(i * kNodeStep, w - 1);
                    const float sx = (static_cast<float>(px) + static_cast<float>(v.originX)) * wpp;
                    const size_t index = static_cast<size_t>(j) * f.nodesX + i;

                    // visible layer: the point of the cloud plane that appears at this screen position
                    const auto [cmx, cmy] = CloudMapPoint(sx, screenY, altitude, v.rotation);
                    const float shape = wantClouds ? CloudShape(CloudFbm(cf, cmx, cmy, 1.0f, 0.0f), cf.coverage) : 0.0f;
                    // Thickness is optical depth, independent of edge coverage. Dense cores can hide the park while
                    // the finite-support density leaves clear gaps and a smooth, transparent perimeter.
                    const float detail = detailAmount <= 0.0f ? 0.5f : 0.6f * ValueNoise((cmx - cf.windX * 1.3f) / (cf.size * 0.16f) + 71.0f, (cmy - cf.windY * 1.3f) / (cf.size * 0.16f))
                        + 0.4f * ValueNoise((cmx - cf.windX * 1.6f) / (cf.size * 0.07f), (cmy - cf.windY * 1.6f) / (cf.size * 0.07f) + 43.0f);
                    const float thickness = ValueNoise((cmx - cf.windX * 0.8f) / (cf.size * 0.45f) + 13.0f, (cmy - cf.windY * 0.8f) / (cf.size * 0.45f) + 5.0f);
                    const float body = 3.0f + 1.8f * thickness + 0.9f * precipLevel;
                    f.nodeCloud[index] = shape * body * (1.0f + detailAmount * (detail - 0.5f));
                    // Two samples along the sun approximate self-occlusion in the bank. The local density slope
                    // brightens the sun-facing rim; a diffuse floor keeps the interiors luminous rather than black.
                    const float towardSun = wantClouds
                        ? CloudShape(CloudFbm(cf, cmx + sunDirX * lightStep, cmy + sunDirY * lightStep, 1.0f, 0.0f), cf.coverage)
                        : 0.0f;
                    const float fartherSun = wantClouds
                        ? CloudShape(CloudFbm(cf, cmx + sunDirX * lightStep * 2.5f, cmy + sunDirY * lightStep * 2.5f, 1.0f, 0.0f), cf.coverage)
                        : 0.0f;
                    const float directLight = std::exp(-(0.75f * towardSun + 0.35f * fartherSun));
                    f.nodeLit[index] = std::clamp(0.55f + 0.45f * directLight + 0.65f * (shape - towardSun), 0.0f, 1.0f);

                    // Project through the persistent receiver below sprites, never a guest/boat's footprint.
                    // Visible scene depth changes as they cross an 8px node and used to drag a whole patch of
                    // cloud shadow/fog with them, then retain that deformation for five frames.
                    float dGround = 2.0f * screenY;
                    if (depth != nullptr)
                    {
                        const size_t pixel = static_cast<size_t>(py) * w + px;
                        const auto& plane = Drawing::gDepthPlane;
                        uint16_t d = Drawing::kDepthUnknown;
                        if (depth == plane.data && plane.receiverDepth != nullptr)
                            d = plane.receiverDepth[pixel];
                        else if (depth == plane.data && plane.shadowFlags != nullptr
                            && (plane.shadowFlags[pixel] & Drawing::kShadowReceiver) != 0)
                            d = depth[pixel];
                        if (d != 0xFFFF)
                            dGround = static_cast<float>((static_cast<int32_t>(d) - 32768) * 2);
                    }
                    const auto [gx, gy] = CloudMapPoint(sx, dGround * 0.5f, 0.0f, v.rotation);
                    // the cloud that shades this ground point hangs towards the sun
                    const float shadowDensity = wantClouds && shadowStrength > 0.0f
                        ? CloudShape(CloudFbm(cf, gx + sunX, gy + sunY, 1.0f, 0.0f), cf.coverage)
                        : 0.0f;
                    f.nodeShadow[index] = 1.0f - std::exp(-shadowDensity * 3.9f);
                    const float fogNoise = fogAmount > 0.0f ? CloudFbm(cf, gx, gy, 0.55f, 31.0f) : 0.5f;
                    f.nodeFog[index] = 0.35f + 0.65f * SmoothStep01(0.35f, 0.70f, fogNoise);
                }
            });

            const int32_t hw = f.halfWidth, hh = f.halfHeight;
            const CoordsXY mapExtent{ getGameState().mapSize.x * 32, getGameState().mapSize.y * 32 };
            pool.ParallelFor((hh + 15) / 16, [&](int32_t task) {
                for (int32_t y = task * 16; y < std::min(hh, task * 16 + 16); y++)
                {
                    const int32_t py = std::min(2 * y, h - 1);
                    const float vy = static_cast<float>(py) / static_cast<float>(kNodeStep);
                    const int32_t j0 = std::min(static_cast<int32_t>(vy), f.nodesY - 2);
                    const float fy = vy - static_cast<float>(j0);
                    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(hh) * 2.0f - 1.0f;
                    for (int32_t x = 0; x < hw; x++)
                    {
                        const int32_t px = std::min(2 * x, w - 1);
                        const float vx = static_cast<float>(px) / static_cast<float>(kNodeStep);
                        const int32_t i0 = std::min(static_cast<int32_t>(vx), f.nodesX - 2);
                        const float fx = vx - static_cast<float>(i0);
                        const size_t n00 = static_cast<size_t>(j0) * f.nodesX + i0;
                        const size_t n10 = n00 + 1, n01 = n00 + f.nodesX, n11 = n01 + 1;
                        auto bilinear = [&](const std::vector<float>& a) {
                            const float top = a[n00] + (a[n10] - a[n00]) * fx;
                            const float bottom = a[n01] + (a[n11] - a[n01]) * fx;
                            return top + (bottom - top) * fy;
                        };
                        const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(hw) * 2.0f - 1.0f;
                        const float r = std::sqrt(nx * nx + ny * ny) * 0.85f;
                        const float clear = holeRadius > 0.001f ? SmoothStep01(holeRadius * 0.9f, holeRadius * 0.9f + 0.55f, r) : 1.0f;
                        const size_t out = static_cast<size_t>(y) * hw + x;
                        const auto toByte = [](float value) {
                            return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                        };
                        // sky = outside of the map: the ground position of the pixel is beyond the map edge
                        bool skyHere = false;
                        float edgeFade = 1.0f;
                        float groundEdgeFade = 1.0f;
                        {
                            const float sxw = (static_cast<float>(px) + static_cast<float>(v.originX)) * wpp;
                            const float syw = (static_cast<float>(py) + static_cast<float>(v.originY)) * wpp;
                            // ground plane position from the screen position only (smooth; the depth of upright sprites would make steps)
                            const auto [groundX, groundY] = CloudMapPoint(sxw, syw, 0.0f, v.rotation);
                            skyHere = groundX < 0 || groundY < 0 || groundX >= mapExtent.x || groundY >= mapExtent.y;
                            const float groundEx = std::max({ -groundX, groundX - mapExtent.x, 0.0f });
                            const float groundEy = std::max({ -groundY, groundY - mapExtent.y, 0.0f });
                            groundEdgeFade = 1.0f - SmoothStep01(150.0f, 900.0f,
                                std::sqrt(groundEx * groundEx + groundEy * groundEy));
                            const auto [cloudX, cloudY] = CloudMapPoint(sxw, syw, altitude, v.rotation);
                            const float ex = std::max({ -cloudX, cloudX - mapExtent.x, 0.0f });
                            const float ey = std::max({ -cloudY, cloudY - mapExtent.y, 0.0f });
                            // clouds and their shadows drift a little over the map edge and then fade out
                            edgeFade = 1.0f - SmoothStep01(150.0f, 900.0f, std::sqrt(ex * ex + ey * ey));
                        }
                        // Beer-Lambert transmittance: wisps remain translucent and increasing thickness saturates into
                        // a white core. Apply focus clearing afterwards so it stays useful even for very thick clouds.
                        const float cloudAlpha = (1.0f - std::exp(-bilinear(f.nodeCloud) * opacity)) * clear * edgeFade;
                        // dense cloud is opaque: nothing of the world below (shadows, dark objects) shines through
                        const float dense = std::clamp(cloudAlpha * 1.25f, 0.0f, 1.0f);
                        f.cloudAlphaPlane[out] = wantClouds ? toByte(dense * dense * (3.0f - 2.0f * dense)) : 0;
                        f.skyPlane[out] = wantClouds && skyHere ? 255 : 0;
                        f.cloudLitPlane[out] = wantClouds ? toByte(bilinear(f.nodeLit)) : 255;
                        f.cloudShadowPlane[out] = wantClouds ? toByte(bilinear(f.nodeShadow) * shadowStrength * groundEdgeFade) : 0;
                        f.cloudRawPlane[out] = precipActive
                            ? toByte(SmoothStep01(0.3f, 0.8f, 1.0f - std::exp(-bilinear(f.nodeCloud) * opacityRaw)))
                            : 0;
                        f.fogPlane[out] = fogAmount > 0.0f ? toByte(bilinear(f.nodeFog) * fogAmount * (0.5f + 0.5f * clear) * groundEdgeFade) : 0;
                    }
                }
            });

            if (precipActive)
            {
                // Rain and snow start at the clouds and fall straight down the screen (slanted by the wind) over the
                // height of the cloud layer, fading out on the way: the mask is the cloud shape smeared downwards.
                const float slantNow = [&]() {
                    const CoordsXY screenWind = CoordsXY{ static_cast<int32_t>(windDirX * 1000.0f), static_cast<int32_t>(windDirY * 1000.0f) }
                                                    .rotate(v.rotation & 3);
                    return static_cast<float>(screenWind.y - screenWind.x) * 0.001f * windNow * (snowAmount > rainAmount ? 0.4f : 0.5f);
                }();
                const float fallRows = std::max(24.0f, altitude / wpp * 0.5f); // half resolution rows
                const int32_t decay = std::max(1, static_cast<int32_t>(150.0f / fallRows));
                const int32_t shift = static_cast<int32_t>(std::lround(slantNow));
                const uint8_t* raw = f.cloudRawPlane.data();
                uint8_t* mask = f.precipPlane.data();
                for (int32_t x = 0; x < hw; x++)
                    mask[x] = raw[x];
                for (int32_t y = 1; y < hh; y++)
                {
                    const uint8_t* above = mask + static_cast<size_t>(y - 1) * hw;
                    const uint8_t* r = raw + static_cast<size_t>(y) * hw;
                    uint8_t* m = mask + static_cast<size_t>(y) * hw;
                    for (int32_t x = 0; x < hw; x++)
                    {
                        const int32_t sx = std::clamp(x - shift, 0, hw - 1);
                        // average with the neighbours so the edges of the rain curtain stay soft
                        const int32_t a = (above[sx] * 2 + above[std::max(0, sx - 1)] + above[std::min(hw - 1, sx + 1)]) >> 2;
                        m[x] = static_cast<uint8_t>(std::max<int32_t>(r[x], a - decay));
                    }
                }
                // The curtain starts at the underside of the cloud: inside the visible cloud the flakes are hidden behind
                // it, so they emerge from its lower edge instead of being painted over the cloud.
                if (wantClouds && opacity > 0.0f)
                {
                    const uint8_t* cloudAlpha = f.cloudAlphaPlane.data();
                    for (size_t i = 0; i < static_cast<size_t>(hw) * hh; i++)
                        mask[i] = static_cast<uint8_t>((mask[i] * (255 - cloudAlpha[i]) + 127) / 255);
                }
            }


                // cells that contain any cloud, shadow or fog (dilated by one cell) - everything else skips the cloud maths
                const int32_t cols = (hw + 7) / 8;
                f.cloudSeg.assign(static_cast<size_t>(cols) * hh, 0);
                f.cloudSegTmp.assign(static_cast<size_t>(cols) * hh, 0);
                const uint8_t* planesToCheck[4] = { wantClouds ? f.skyPlane.data() : nullptr,
                                                    wantClouds && shadowStrength > 0.0f ? f.cloudShadowPlane.data() : nullptr,
                                                    wantClouds && opacity > 0.0f ? f.cloudAlphaPlane.data() : nullptr,
                                                    fogAmount > 0.0f ? f.fogPlane.data() : nullptr };
                for (int32_t y = 0; y < hh; y++)
                    for (int32_t sx = 0; sx < cols; sx++)
                    {
                        bool any = false;
                        const int32_t xEnd = std::min(hw, sx * 8 + 8);
                        for (const uint8_t* plane : planesToCheck)
                        {
                            if (plane == nullptr || any)
                                continue;
                            const uint8_t* row = plane + static_cast<size_t>(y) * hw;
                            for (int32_t x = sx * 8; x < xEnd; x++)
                                if (row[x])
                                {
                                    any = true;
                                    break;
                                }
                        }
                        f.cloudSegTmp[static_cast<size_t>(y) * cols + sx] = any ? 1 : 0;
                    }
                for (int32_t y = 0; y < hh; y++)
                    for (int32_t sx = 0; sx < cols; sx++)
                    {
                        uint8_t value = 0;
                        for (int32_t dy = -1; dy <= 1 && !value; dy++)
                            for (int32_t dx = -1; dx <= 1; dx++)
                            {
                                const int32_t yy = y + dy, xx = sx + dx;
                                if (yy >= 0 && yy < hh && xx >= 0 && xx < cols && f.cloudSegTmp[static_cast<size_t>(yy) * cols + xx])
                                {
                                    value = 1;
                                    break;
                                }
                            }
                        f.cloudSeg[static_cast<size_t>(y) * cols + sx] = value;
                    }
            }

            _cloudPlanes.seg = f.cloudSeg.data();
            _cloudPlanes.segCols = (f.halfWidth + 7) / 8;
            _cloudPlanes.cloud = wantClouds && opacity > 0.0f ? f.cloudAlphaPlane.data() : nullptr;
            _cloudPlanes.shadow = wantClouds && shadowStrength > 0.0f ? f.cloudShadowPlane.data() : nullptr;
            _cloudPlanes.fog = fogAmount > 0.0f ? f.fogPlane.data() : nullptr;
            _cloudPlanes.lit = f.cloudLitPlane.data();
            _cloudPlanes.sky = nullptr; // the background around the map stays as the game draws it (black)

            // precipitation pass parameters
            _precip.mask = precipActive ? f.precipPlane.data() : nullptr;
            _precip.snow = snowAmount > rainAmount;
            _precip.time = t;
            _precip.level = precipLevel;
            _precipLevelNow = precipLevel;
            _precip.originX = static_cast<float>(v.originX);
            _precip.originY = static_cast<float>(v.originY);
            {
                const CoordsXY screenWind = CoordsXY{ static_cast<int32_t>(windDirX * 1000.0f), static_cast<int32_t>(windDirY * 1000.0f) }
                                                .rotate(v.rotation & 3);
                // horizontal screen movement per unit of falling (isometric: screen x = yr - xr)
                _precip.slant = static_cast<float>(screenWind.y - screenWind.x) * 0.001f * windNow * (_precip.snow ? 0.4f : 0.5f);
            }
            _precip.zoomFade = precipFadeS;
            _precip.drift += std::clamp(_precip.slant, -0.5f, 0.5f) * dt;
            _cloudPlanes.gloom = gloom;
            const float daylight = _time.active ? 1.0f - _time.night : 1.0f;
            const float sunWarmth = _settings.sunWarmth * daylight;
            const float timeR = _time.active ? _time.tint[0] : 1.0f;
            const float timeG = _time.active ? _time.tint[1] : 1.0f;
            const float timeB = _time.active ? _time.tint[2] : 1.0f;
            for (int32_t i = 0; i < 256; i++)
            {
                const float lit = static_cast<float>(i) * (1.0f / 255.0f);
                const float shade = lit * (0.15f + 0.85f * daylight);
                _cloudPlanes.shadowFactor[0][i] = static_cast<uint32_t>((1.0f - shade * 0.36f) * 256.0f);
                _cloudPlanes.shadowFactor[1][i] = static_cast<uint32_t>((1.0f - shade * 0.32f) * 256.0f);
                _cloudPlanes.shadowFactor[2][i] = static_cast<uint32_t>((1.0f - shade * 0.22f) * 256.0f);
                const float level = (202.0f + 53.0f * lit) * (1.0f - gloom * 0.40f);
                // Neutral white cores, subtly cool shaded interiors and a warm sun-facing edge.
                _cloudPlanes.cloudColour[0][i] = static_cast<int32_t>(
                    std::min(255.0f, level * (0.97f + 0.04f * lit + 0.025f * sunWarmth * lit) * timeR));
                _cloudPlanes.cloudColour[1][i] = static_cast<int32_t>(
                    std::min(255.0f, level * (0.985f + 0.015f * lit) * timeG));
                _cloudPlanes.cloudColour[2][i] = static_cast<int32_t>(
                    std::min(255.0f, level * (1.025f - 0.035f * lit - 0.06f * sunWarmth * lit) * timeB));
            }
            const float fogK = 1.0f - gloom * 0.35f;
            _cloudPlanes.fogColour[0] = static_cast<int32_t>(std::min(255.0f, 214.0f * fogK * timeR));
            _cloudPlanes.fogColour[1] = static_cast<int32_t>(std::min(255.0f, 220.0f * fogK * timeG));
            _cloudPlanes.fogColour[2] = static_cast<int32_t>(std::min(255.0f, 228.0f * fogK * timeB));
        }

        float Hash01(uint32_t n)
        {
            n = (n ^ 61u) ^ (n >> 16);
            n *= 9u;
            n ^= n >> 4;
            n *= 0x27d4eb2du;
            n ^= n >> 15;
            return static_cast<float>(n & 0xFFFFFFu) * (1.0f / 16777216.0f);
        }

        float ValueNoise(float x, uint32_t seed)
        {
            const float fl = std::floor(x);
            const uint32_t i = static_cast<uint32_t>(static_cast<int32_t>(fl));
            const float f = x - fl, s2 = f * f * (3.0f - 2.0f * f);
            const float a = Hash01(i * 747796405u + seed), b = Hash01((i + 1u) * 747796405u + seed);
            return a + (b - a) * s2;
        }

        double LightClock()
        {
            static const auto start = Clock::now();
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        // Animation of one light: brightness factor, position shake (world units) and radius factor.
        struct LightPhase
        {
            float level = 1.0f, dx = 0.0f, dy = 0.0f, radius = 1.0f;
        };

        LightPhase AnimateLight(const LightObject& light, double now)
        {
            LightPhase p;
            const uint32_t seed = static_cast<uint32_t>(light.id) * 2654435761u;
            const float t = static_cast<float>(std::fmod(now, 4096.0)), sp = std::max(0.05f, light.speed);
            const float seed01 = Hash01(seed);
            switch (light.anim)
            {
                case LightAnim::Flicker:
                    p.level = 0.5f + 0.5f * ValueNoise(t * sp * 14.0f, seed);
                    break;
                case LightAnim::Pulse:
                    p.level = 0.4f + 0.6f * (0.5f + 0.5f * std::sin(6.2831853f * (t * sp * 0.5f + seed01)));
                    break;
                case LightAnim::Vibrate:
                    p.dx = (ValueNoise(t * sp * 30.0f, seed) - 0.5f) * 8.0f;
                    p.dy = (ValueNoise(t * sp * 30.0f, seed + 99u) - 0.5f) * 8.0f;
                    p.level = 0.88f + 0.12f * ValueNoise(t * sp * 22.0f, seed + 5u);
                    break;
                case LightAnim::Strobe:
                    p.level = std::fmod(t * sp * 3.0f + seed01, 1.0f) < 0.12f ? 1.0f : 0.0f;
                    break;
                case LightAnim::Blink:
                    p.level = std::fmod(t * sp * 0.5f + seed01, 1.0f) < 0.5f ? 1.0f : 0.0f;
                    break;
                case LightAnim::Candle:
                    p.level = 0.65f + 0.35f * ValueNoise(t * sp * 9.0f, seed);
                    p.radius = 0.92f + 0.16f * ValueNoise(t * sp * 6.0f, seed + 7u);
                    p.dx = (ValueNoise(t * sp * 7.0f, seed + 13u) - 0.5f) * 3.0f;
                    break;
                case LightAnim::Flash:
                {
                    const float slotTime = t * sp * 2.0f;
                    const uint32_t slot = static_cast<uint32_t>(slotTime);
                    const float frac = slotTime - std::floor(slotTime);
                    const bool fires = Hash01(slot * 2246822519u + seed) < 0.3f;
                    p.level = 0.06f + (fires ? std::pow(1.0f - frac, 3.0f) : 0.0f);
                    break;
                }
                default:
                    break;
            }
            return p;
        }

        void DrawLightMarkers(const Frame& fr, const ViewInfo& view)
        {
            const float wpp = std::max(0.01f, view.worldPerPixel);
            const uint32_t mask = (255u << fr.rShift) | (255u << fr.gShift) | (255u << fr.bShift);
            const auto put = [&](int32_t x, int32_t y, uint32_t colour) {
                if (x < 0 || y < 0 || x >= fr.width || y >= fr.height || fr.ui[static_cast<size_t>(y) * fr.width + x])
                    return;
                uint32_t& p = fr.out[static_cast<size_t>(y) * fr.width + x];
                p = (p & ~mask) | colour;
            };
            for (int32_t i = 0; i < static_cast<int32_t>(_lights.size()); i++)
            {
                const auto& light = _lights[i];
                const float ground = static_cast<float>(TileElementHeight(CoordsXY{ light.x, light.y }));
                const CoordsXY rot = CoordsXY{ light.x, light.y }.rotate(view.rotation);
                const int32_t px = static_cast<int32_t>(static_cast<float>(rot.y - rot.x) / wpp) - view.originX,
                              py = static_cast<int32_t>(LightPlacement::PlaneY(rot.x,rot.y,ground+light.height) / wpp) - view.originY;
                const bool selected = i == _lightSelected;
                const int32_t r = selected ? 7 : 4;
                const uint32_t colour = selected ? ((255u << fr.rShift) | (240u << fr.gShift) | (80u << fr.bShift))
                                                 : ((static_cast<uint32_t>(light.r) << fr.rShift) | (static_cast<uint32_t>(light.g) << fr.gShift)
                                                    | (static_cast<uint32_t>(light.b) << fr.bShift));
                const uint32_t dark = 0;
                // a straight green line down to the ground point below the light: shows where it really stands
                {
                    const uint32_t green = (60u << fr.rShift) | (255u << fr.gShift) | (90u << fr.bShift);
                    const int32_t footY = static_cast<int32_t>(LightPlacement::PlaneY(rot.x,rot.y,ground) / wpp) - view.originY;
                    const int32_t top = std::min(py, footY), bottom = std::max(py, footY);
                    const int32_t width = selected ? 2 : 1;
                    for (int32_t y = top; y <= bottom; y++)
                    {
                        put(px - 1, y, dark);
                        put(px + width, y, dark);
                        for (int32_t w = 0; w < width; w++)
                            put(px + w, y, green);
                    }
                    // foot: a small flat ring on the ground
                    const int32_t fr2 = selected ? 8 : 5;
                    for (int32_t k = -fr2; k <= fr2; k++)
                    {
                        const int32_t e = static_cast<int32_t>(std::lround(std::sqrt(std::max(0, fr2 * fr2 - k * k)) * 0.5f));
                        put(px + k, footY + e, green);
                        put(px + k, footY - e, green);
                    }
                }
                for (int32_t k = -r - 1; k <= r + 1; k++)
                {
                    const int32_t e = r + 1 - std::abs(k);
                    for (const int32_t sgn : { -1, 1 })
                    {
                        put(px + k, py + sgn * e, dark);
                    }
                }
                for (int32_t k = -r; k <= r; k++)
                {
                    const int32_t e = r - std::abs(k);
                    put(px + k, py + e, colour);
                    put(px + k, py - e, colour);
                }
                if (selected)
                    for (int32_t k = -r - 3; k <= r + 3; k++)
                    {
                        put(px + k, py, colour);
                        put(px, py + k, colour);
                    }
                if (!light.on)
                    put(px, py, 0);
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Sun rays. A coarse height grid of the map (terrain and everything that stands on it) lets a ray go from any
        // point in the air towards the sun; the point is lit when nothing is in the way. A pixel of the shaft layer
        // adds up a few such points along its line of sight. Beams stay in the gaps between buildings and stop where
        // something tall is in the way.
        // ---------------------------------------------------------------------------------------------------------
        struct HeightGrid
        {
            int32_t x0 = 0, y0 = 0, w = 0, h = 0;
            std::vector<int16_t> top;
            int32_t maxTop = 0;
            uint32_t revision = 0xFFFFFFFFu;
            const TileElement* storage = nullptr;
            size_t count = 0;
            Clock::time_point collected{};
        };
        HeightGrid _heights;

        void UpdateHeightGrid(const ViewInfo& view)
        {
            auto& state = getGameState();
            if (state.mapSize.x <= 0 || state.mapSize.y <= 0)
                return;
            const float wpp = view.worldPerPixel;
            int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
            for (const int32_t z : { 0, 700 })
                for (int32_t corner = 0; corner < 4; corner++)
                {
                    const ScreenCoordsXY p{ static_cast<int32_t>((view.originX + ((corner & 1) ? view.viewportWidth : 0)) * wpp),
                                            static_cast<int32_t>((view.originY + ((corner & 2) ? view.viewportHeight : 0)) * wpp) };
                    const auto map = ViewportPosToMapPos(p, z, static_cast<uint8_t>(view.rotation));
                    minX = std::min(minX, map.x);
                    minY = std::min(minY, map.y);
                    maxX = std::max(maxX, map.x);
                    maxY = std::max(maxY, map.y);
                }
            const int32_t x0 = std::clamp(minX / 32 - 14, 0, state.mapSize.x - 1);
            const int32_t y0 = std::clamp(minY / 32 - 14, 0, state.mapSize.y - 1);
            const int32_t x1 = std::clamp(maxX / 32 + 14, 0, state.mapSize.x - 1);
            const int32_t y1 = std::clamp(maxY / 32 + 14, 0, state.mapSize.y - 1);
            auto& g = _heights;
            const uint32_t revision = Drawing::gMapRevision.load(std::memory_order_relaxed);
            const auto now = Clock::now();
            const bool outside = x0 < g.x0 || y0 < g.y0 || x1 >= g.x0 + g.w || y1 >= g.y0 + g.h;
            const bool replaced = g.storage != state.tileElements.data() || g.count != state.tileElements.size();
            if (!(outside || replaced || (revision != g.revision && now - g.collected > std::chrono::milliseconds(400))))
                return;
            g.x0 = std::max(0, x0 - 6);
            g.y0 = std::max(0, y0 - 6);
            g.w = std::min(state.mapSize.x - 1, x1 + 6) - g.x0 + 1;
            g.h = std::min(state.mapSize.y - 1, y1 + 6) - g.y0 + 1;
            g.revision = revision;
            g.storage = state.tileElements.data();
            g.count = state.tileElements.size();
            g.collected = now;
            g.top.assign(static_cast<size_t>(g.w) * g.h, 0);
            g.maxTop = 0;
            for (int32_t ty = 0; ty < g.h; ty++)
                for (int32_t tx = 0; tx < g.w; tx++)
                {
                    const auto* element = MapGetFirstElementAt(TileCoordsXY{ g.x0 + tx, g.y0 + ty });
                    if (element == nullptr)
                        continue;
                    int32_t top = 0;
                    do
                    {
                        if (element->isGhost() || element->isInvisible())
                            continue;
                        switch (element->getType())
                        {
                            case TileElementType::surface:
                                top = std::max(top, element->getBaseZ());
                                break;
                            case TileElementType::smallScenery:
                                if (element->getClearanceZ() - element->getBaseZ() >= 24)
                                    top = std::max(top, element->getClearanceZ());
                                break;
                            case TileElementType::largeScenery:
                            case TileElementType::track:
                            case TileElementType::entrance:
                                top = std::max(top, element->getClearanceZ());
                                break;
                            default:
                                break;
                        }
                    } while (!(element++)->isLastForTile());
                    g.top[static_cast<size_t>(ty) * g.w + tx] = static_cast<int16_t>(std::min(top, 32000));
                    g.maxTop = std::max(g.maxTop, top);
                }
        }

        // 1 when the point (map units) can see the sun, 0 when something stands in the way.
        inline float SunLit(const HeightGrid& g, float x, float y, float z, float dirX, float dirY, float tanE)
        {
            constexpr float kStep = 28.0f;
            const auto topAt = [&](float px, float py) {
                const int32_t tx = static_cast<int32_t>(std::floor(px / 32.0f)) - g.x0, ty = static_cast<int32_t>(std::floor(py / 32.0f)) - g.y0;
                if (tx < 0 || ty < 0 || tx >= g.w || ty >= g.h)
                    return -1;
                return static_cast<int32_t>(g.top[static_cast<size_t>(ty) * g.w + tx]);
            };
            if (topAt(x, y) > z)
                return 0.0f;
            for (int32_t i = 1; i <= 16; i++)
            {
                const float d = kStep * static_cast<float>(i);
                const float zr = z + d * tanE;
                if (zr > static_cast<float>(g.maxTop))
                    return 1.0f;
                if (topAt(x + dirX * d, y + dirY * d) > zr)
                    return 0.0f;
            }
            return 1.0f;
        }

        // Straight beams: the stripes run along the sun direction as it is projected on the screen, so they are parallel,
        // straight lines. How much light a pixel gets comes from a smooth visibility plane (quarter resolution, blurred),
        // which lets the beams stop behind tall buildings without falling apart into blocks.
        struct RayStyle
        {
            float nx = 0.0f, ny = 1.0f; // unit vector across the beams (map space)
            float spacing = 80.0f;      // distance between beams in map units
            float drift = 0.0f;         // slow sideways movement
            float dx = 0.0f, dy = 1.0f; // unit vector along the beams (direction of travel, towards their end)
            float lengthScale = 1.0f;
        };
        float _rayStripe[4096];

        void BuildRayStyle(RayStyle& style)
        {
            constexpr float kPi = 3.14159265f;
            const float az = EffectiveSunAzimuth() * kPi / 180.0f;
            // Pattern and extent belong to the map. Normalising a projected screen vector changes both
            // stripe spacing and phase on rotation, even when the world-space sun has not moved.
            style.dx = -std::cos(az);
            style.dy = -std::sin(az);
            style.nx = -style.dy;
            style.ny = style.dx;
            style.lengthScale = Coordinates::RayLengthScale(EffectiveSunElevation());
            style.spacing = 80.0f;
            style.drift = static_cast<float>(std::fmod(LightClock() * 0.05, 4096.0));
            // table of the beam pattern: 16 entries per spacing, repeating
            for (int32_t i = 0; i < 4096; i++)
            {
                const float u = static_cast<float>(i) * (1.0f / 16.0f);
                const float n = 0.6f * ValueNoise(u, 3.7f) + 0.4f * ValueNoise(u * 2.3f + 11.0f, 8.1f);
                _rayStripe[i] = std::clamp((n - 0.36f) * 2.6f, 0.0f, 1.0f);
            }
        }

        // Light of the sun per tile and height: 1 = the sky is open towards the sun, 0 = something tall is in the way.
        // Depends only on the map and the sun (not on the camera), is smoothed a little and cached, so the beams are
        // stable: the same places are lit whatever the camera does.
        struct SunLitGrid
        {
            int32_t x0 = -1, y0 = -1, w = 0, h = 0;
            std::vector<float> lit[3];
            uint32_t key = 0;
        };
        SunLitGrid _sunLit;

        // Centre and half size of the built-up part of the map (anything but bare ground), for the beam group.
        struct ParkArea
        {
            float cx = 0, cy = 0, half = 1500;
            float rayHeight = 110.0f;
            const TileElement* storage = nullptr;
            uint32_t revision = 0xFFFFFFFFu;
            Clock::time_point checked{};
        };
        ParkArea _parkArea;

        void UpdateParkArea()
        {
            auto& state = getGameState();
            const uint32_t revision = Drawing::gMapRevision.load(std::memory_order_relaxed);
            const auto now = Clock::now();
            const bool replaced = _parkArea.storage != state.tileElements.data();
            if (!replaced && (_parkArea.revision == revision || now - _parkArea.checked < std::chrono::seconds(3)))
                return;
            _parkArea.storage = state.tileElements.data();
            _parkArea.checked = now;
            _parkArea.revision = revision;
            int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = -1, maxY = -1;
            int32_t maxTop = 0;
            for (int32_t ty = 1; ty < state.mapSize.y - 1; ty++)
                for (int32_t tx = 1; tx < state.mapSize.x - 1; tx++)
                {
                    const auto* element = MapGetFirstElementAt(TileCoordsXY{ tx, ty });
                    if (element == nullptr)
                        continue;
                    bool built = false;
                    do
                    {
                        const auto type = element->getType();
                        if (!element->isGhost() && !element->isInvisible())
                            maxTop = std::max(maxTop, element->getClearanceZ());
                        if (type != TileElementType::surface && !element->isGhost())
                        {
                            built = true;
                        }
                    } while (!(element++)->isLastForTile());
                    if (built)
                    {
                        minX = std::min(minX, tx);
                        maxX = std::max(maxX, tx);
                        minY = std::min(minY, ty);
                        maxY = std::max(maxY, ty);
                    }
                }
            // Camera-local height maxima changed the sampling planes on rotation/panning.
            _parkArea.rayHeight = std::clamp(0.7f * static_cast<float>(maxTop), 110.0f, 420.0f);
            if (maxX < 0)
            {
                _parkArea.cx = static_cast<float>(state.mapSize.x * 16);
                _parkArea.cy = static_cast<float>(state.mapSize.y * 16);
                _parkArea.half = 1500.0f;
                return;
            }
            _parkArea.cx = static_cast<float>((minX + maxX + 1) * 16);
            _parkArea.cy = static_cast<float>((minY + maxY + 1) * 16);
            _parkArea.half = std::max(900.0f, 16.0f * static_cast<float>(std::max(maxX - minX, maxY - minY) + 1));
        }
        constexpr int32_t kRayLevels = 3;

        void UpdateSunLit(const ViewInfo& view)
        {
            UpdateHeightGrid(view);
            const auto& g = _heights;
            if (g.w <= 0)
                return;
            constexpr float kPi = 3.14159265f;
            uint32_t key = 2166136261u;
            for (const int32_t value : { g.x0, g.y0, g.w, g.h, static_cast<int32_t>(EffectiveSunAzimuth() * 4.0f), static_cast<int32_t>(EffectiveSunElevation() * 4.0f),
                                         static_cast<int32_t>(Drawing::gMapRevision.load(std::memory_order_relaxed)), g.maxTop,
                                         static_cast<int32_t>(_parkArea.rayHeight * 10.0f) })
                key = (key ^ static_cast<uint32_t>(value)) * 16777619u;
            if (key == _sunLit.key && _sunLit.w == g.w && _sunLit.h == g.h)
                return;
            _sunLit.key = key;
            _sunLit.x0 = g.x0;
            _sunLit.y0 = g.y0;
            _sunLit.w = g.w;
            _sunLit.h = g.h;
            const float az = EffectiveSunAzimuth() * kPi / 180.0f;
            const float dirX = std::cos(az), dirY = std::sin(az);
            const float tanE = std::tan(std::clamp(EffectiveSunElevation(), 12.0f, 85.0f) * kPi / 180.0f);
            const float spawnZ = _parkArea.rayHeight;
            const float levels[kRayLevels] = { spawnZ * 0.15f, spawnZ * 0.45f, spawnZ * 0.85f };
            std::vector<float> raw(static_cast<size_t>(g.w) * g.h);
            for (int32_t k = 0; k < kRayLevels; k++)
            {
                for (int32_t ty = 0; ty < g.h; ty++)
                    for (int32_t tx = 0; tx < g.w; tx++)
                        raw[static_cast<size_t>(ty) * g.w + tx] = SunLit(
                            g, static_cast<float>((g.x0 + tx) * 32 + 16), static_cast<float>((g.y0 + ty) * 32 + 16), levels[k], dirX, dirY, tanE);
                // 3x3 blur, twice
                auto& out = _sunLit.lit[k];
                out.assign(raw.size(), 0.0f);
                for (int32_t pass = 0; pass < 2; pass++)
                {
                    for (int32_t ty = 0; ty < g.h; ty++)
                        for (int32_t tx = 0; tx < g.w; tx++)
                        {
                            float sum = 0.0f;
                            for (int32_t dy = -1; dy <= 1; dy++)
                                for (int32_t dx = -1; dx <= 1; dx++)
                                    sum += raw[static_cast<size_t>(std::clamp(ty + dy, 0, g.h - 1)) * g.w + std::clamp(tx + dx, 0, g.w - 1)]
                                        * ((dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f));
                            out[static_cast<size_t>(ty) * g.w + tx] = sum * (1.0f / 16.0f);
                        }
                    raw = out;
                }
            }
        }

        inline float SunLitAt(int32_t level, float mx, float my)
        {
            const auto& g = _sunLit;
            const float fx = mx * (1.0f / 32.0f) - 0.5f - static_cast<float>(g.x0), fy = my * (1.0f / 32.0f) - 0.5f - static_cast<float>(g.y0);
            const int32_t ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
            const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
            const auto at = [&](int32_t cx, int32_t cy) {
                cx = std::clamp(cx, 0, g.w - 1);
                cy = std::clamp(cy, 0, g.h - 1);
                return g.lit[level][static_cast<size_t>(cy) * g.w + cx];
            };
            return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
        }

        void BuildSunRays(FrameState& f, const Frame& fr, const ViewInfo& view, const RayStyle& style, float strength)
        {
            if (strength <= 0.0f || view.worldPerPixel > 3.2f || view.viewportWidth <= 0)
            {
                std::fill(f.rayPlane.begin(), f.rayPlane.end(), static_cast<uint8_t>(0));
                return;
            }
            UpdateParkArea(); // main thread, before workers read the shared park bounds
            UpdateSunLit(view);
            if (_sunLit.w <= 0)
            {
                std::fill(f.rayPlane.begin(), f.rayPlane.end(), static_cast<uint8_t>(0));
                return;
            }
            const float wpp = view.worldPerPixel;
            const float spawnZ = _parkArea.rayHeight;
            const float heights[kRayLevels] = { spawnZ * 0.15f, spawnZ * 0.45f, spawnZ * 0.85f };
            static const float kWeights[kRayLevels] = { 0.22f, 0.34f, 0.44f };
            const float mapW = static_cast<float>(getGameState().mapSize.x * 32), mapH = static_cast<float>(getGameState().mapSize.y * 32);
            auto& pool = WorkerPool::Get();
            const int32_t qw = f.rayW, qh = f.rayH;
            pool.ParallelFor((qh + 3) / 4, [&](int32_t task) {
                for (int32_t qy = task * 4; qy < std::min(qh, task * 4 + 4); qy++)
                    for (int32_t qx = 0; qx < qw; qx++)
                    {
                        uint8_t& cell = f.rayPlane[static_cast<size_t>(qy) * qw + qx];
                        cell = 0;
                        const int32_t X = std::min(fr.width - 1, qx * 4 + 2), Y = std::min(fr.height - 1, qy * 4 + 2);
                        if (fr.ui[static_cast<size_t>(Y) * fr.width + X] == 1)
                            continue;
                        const float sx = (static_cast<float>(X) + static_cast<float>(view.originX)) * wpp;
                        const float sy = (static_cast<float>(Y) + static_cast<float>(view.originY)) * wpp;
                        float sum = 0.0f;
                        for (int32_t k = 0; k < kRayLevels; k++)
                        {
                            const auto [mx, my] = Coordinates::Unproject(sx, sy, heights[k], view.rotation);
                            // fade out over the edge of the map (the beams reach a little beyond it)
                            const float edge = std::min(std::min(mx, my), std::min(mapW - mx, mapH - my));
                            const float edgeFade = std::clamp((edge + 320.0f) * (1.0f / 640.0f), 0.0f, 1.0f);
                            // outside the area the grid covers the sky is open
                            const bool inside = mx >= 0.0f && my >= 0.0f && mx < mapW && my < mapH;
                            const float lit = inside ? SunLitAt(k, mx, my) : 1.0f;
                            const float vx = mx - _parkArea.cx, vy = my - _parkArea.cy;
                            const float across = vx * style.nx + vy * style.ny;
                            const float along = (vx * style.dx + vy * style.dy) / style.lengthScale;
                            const float half = _parkArea.half;
                            const auto smooth = [](float a, float b, float v) {
                                const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
                                return t * t * (3.0f - 2.0f * t);
                            };
                            const float window = (1.0f - smooth(0.30f * half, 0.75f * half, std::abs(across)))
                                * smooth(-1.25f * half, -1.05f * half, along)
                                * (1.0f - 0.7f * smooth(-0.9f * half, 0.9f * half, along))
                                * (1.0f - smooth(0.9f * half, 1.3f * half, along));
                            const float u = (across / style.spacing + style.drift) * 16.0f;
                            const int32_t i = static_cast<int32_t>(std::floor(u));
                            const float t = u - std::floor(u);
                            const float stripe = _rayStripe[static_cast<uint32_t>(i) & 4095u] * (1.0f - t)
                                + _rayStripe[static_cast<uint32_t>(i + 1) & 4095u] * t;
                            // A world-space envelope varies beam lengths and brightness without changing their direction.
                            const float envelope = 0.55f + 0.45f * ValueNoise(across / (style.spacing * 2.5f),
                                along / std::max(1.0f, half * 0.35f) + 19.0f);
                            sum += kWeights[k] * edgeFade * edgeFade * (3.0f - 2.0f * edgeFade)
                                * lit * window * stripe * envelope;
                        }
                        cell = static_cast<uint8_t>(std::clamp(sum * 255.0f, 0.0f, 255.0f));
                    }
            });
        }

        void SunRayRows(const Frame& fr, const FrameState& f, float strength, float warmth, int32_t y0, int32_t y1)
        {
            const int32_t qw = f.rayW, qh = f.rayH;
            const uint32_t mask = (255u << fr.rShift) | (255u << fr.gShift) | (255u << fr.bShift);
            const int32_t strengthQ = static_cast<int32_t>(std::clamp(strength, 0.0f, 4.0f) * 256.0f);
            const int32_t tintR = 255, tintG = static_cast<int32_t>((0.93f - 0.12f * warmth) * 255.0f), tintB = static_cast<int32_t>((0.78f - 0.28f * warmth) * 255.0f);
            thread_local std::vector<int32_t> qxA, qxB, wt;
            if (static_cast<int32_t>(qxA.size()) != fr.width)
            {
                qxA.resize(fr.width);
                qxB.resize(fr.width);
                wt.resize(fr.width);
            }
            for (int32_t x = 0; x < fr.width; x++)
            {
                const float u = std::max(0.0f, (static_cast<float>(x) - 1.5f) * 0.25f);
                const int32_t q0 = std::min(qw - 1, static_cast<int32_t>(u));
                qxA[x] = q0;
                qxB[x] = std::min(qw - 1, q0 + 1);
                wt[x] = static_cast<int32_t>((u - static_cast<float>(q0)) * 256.0f);
            }
            thread_local std::vector<int32_t> line;
            line.resize(qw);
            for (int32_t y = y0; y < y1; y++)
            {
                const float fy = std::max(0.0f, (static_cast<float>(y) - 1.5f) * 0.25f);
                const int32_t qy0 = std::min(qh - 1, static_cast<int32_t>(fy)), qy1 = std::min(qh - 1, qy0 + 1);
                const int32_t wy = static_cast<int32_t>((fy - static_cast<float>(qy0)) * 256.0f);
                const uint8_t* r0 = f.rayPlane.data() + static_cast<size_t>(qy0) * qw;
                const uint8_t* r1 = f.rayPlane.data() + static_cast<size_t>(qy1) * qw;
                int32_t any = 0;
                for (int32_t i = 0; i < qw; i++)
                {
                    line[i] = (r0[i] * (256 - wy) + r1[i] * wy) >> 8;
                    any |= line[i];
                }
                if (any == 0)
                    continue;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                uint32_t* out = fr.out + static_cast<size_t>(y) * fr.width;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    const int32_t la = line[qxA[x]], lb = line[qxB[x]];
                    if ((la | lb) == 0 || ui[x] == 1)
                        continue;
                    const int32_t v = (la * (256 - wt[x]) + lb * wt[x]) >> 8;
                    const int32_t k = std::min(120, (v * strengthQ) >> 8);
                    if (k < 3)
                        continue;
                    uint32_t p = out[x];
                    const auto lane = [&](uint32_t shift, int32_t tint) {
                        const int32_t value = static_cast<int32_t>((p >> shift) & 255u);
                        return static_cast<uint32_t>(value + (((255 - value) * k * tint) >> 16)) << shift;
                    };
                    out[x] = (p & ~mask) | lane(fr.rShift, tintR) | lane(fr.gShift, tintG) | lane(fr.bShift, tintB);
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Light objects, lit per pixel in 3D. Every pixel of the frame knows its depth D = xr + yr (rotated map
        // frame), so with its screen position the full world position of the visible surface follows:
        //   z = D / 2 - sy,  xr = (D - sx) / 2,  yr = (D + sx) / 2   (sx, sy in viewport units)
        // A light is a point in the same frame; the light on a pixel falls off with the real 3D distance. The ground
        // under a lamp, walls next to it and roofs above it are therefore lit where they really are, the light
        // follows the camera rotation, and heights are respected. Between pixel and light the height grid of the map
        // tells whether a building is in the way. The glowing bulb itself only shows on pixels that are not in front
        // of the light.
        // ---------------------------------------------------------------------------------------------------------
        struct LightScreen
        {
            float lxr = 0, lyr = 0, lz = 0;  // the light in the rotated map frame and its absolute height
            float bx = 0, by = 0;            // bulb in full resolution pixels
            float radius = 0, haloRadius = 0, level = 0;
            float cr = 1, cg = 1, cb = 1;
            float depthLight = 0;            // D of the light
            int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            int32_t tileX = 0, tileY = 0;
            bool showBulb = true;
            float fx = 0, fy = 0, groundZ = 0; // the foot of the light (point on the ground below it), screen pixels
        };
        std::vector<LightScreen> _lightScreens;

        // Light planes at quarter resolution (RGB, float). Everything is computed here and blurred a little, so edges of
        // sprites and tiles never show up as outlines; the final pass only upsamples and adds the light.
        std::vector<float> _low[2][3]; // [0] = accumulated, [1] = blurred
        int32_t _lowW = 0, _lowH = 0;
        std::vector<int32_t> _lowMin, _lowMax;
        int32_t _lowDirtyY0 = 0, _lowDirtyY1 = -1;

        bool BuildLightList(const Frame& fr, const ViewInfo& view)
        {
            _lightScreens.clear();
            if (!_settings.lightsEnabled || _settings.lightsStrength <= 0.0f || view.viewportWidth <= 0 || view.viewportHeight <= 0
                || fr.depth == nullptr)
                return false;
            const float night = _time.active ? _time.night : 0.0f;
            const float gloom = std::clamp(static_cast<float>(getGameState().weatherCurrent.weatherGloom) * 0.12f, 0.0f, 0.3f);
            const float master = _settings.lightsStrength * std::min(1.0f, 0.9f + 0.1f * night + gloom);
            const float wpp = std::max(0.01f, view.worldPerPixel);
            const double now = LightClock();
            for (const auto& light : _lights)
            {
                if (!light.on || light.intensity <= 0.0f)
                    continue;
                const auto phase = AnimateLight(light, now);
                const float level = phase.level * light.intensity * master;
                if (level <= 0.003f)
                    continue;
                LightScreen L;
                L.radius = std::max(8.0f, light.radius * phase.radius);
                L.level = level;
                const float ground = static_cast<float>(TileElementHeight(CoordsXY{ light.x, light.y }));
                const CoordsXY rot = CoordsXY{ light.x + static_cast<int32_t>(phase.dx), light.y + static_cast<int32_t>(phase.dy) }.rotate(view.rotation);
                L.lxr = static_cast<float>(rot.x);
                L.lyr = static_cast<float>(rot.y);
                L.lz = ground + light.height;
                L.depthLight = L.lxr + L.lyr;
                L.bx = (L.lyr - L.lxr) / wpp - static_cast<float>(view.originX);
                L.by = LightPlacement::PlaneY(rot.x,rot.y,L.lz) / wpp - static_cast<float>(view.originY);
                L.groundZ = ground;
                L.fx = L.bx;
                L.fy = L.by + light.height / wpp;
                const float halfW = L.radius * 1.35f / wpp + 8.0f;
                const float top = L.by - L.radius * 0.95f / wpp - 8.0f, bottom = L.fy + L.radius * 0.7f / wpp + 8.0f;
                if (L.bx + halfW < 0 || L.bx - halfW >= fr.width || bottom < 0 || top >= fr.height)
                    continue;
                L.x0 = std::max(0, static_cast<int32_t>(L.bx - halfW));
                L.x1 = std::min(fr.width - 1, static_cast<int32_t>(L.bx + halfW));
                L.y0 = std::max(0, static_cast<int32_t>(top));
                L.y1 = std::min(fr.height - 1, static_cast<int32_t>(bottom));
                L.haloRadius = std::max(4.0f, L.radius * 0.16f / wpp);
                L.cr = light.r / 255.0f;
                L.cg = light.g / 255.0f;
                L.cb = light.b / 255.0f;
                L.tileX = light.x / 32;
                L.tileY = light.y / 32;
                L.showBulb = light.bulb;
                _lightScreens.push_back(L);
            }
            return !_lightScreens.empty();
        }

        // Light that reaches the surface seen at one pixel (0 .. about 1), including the glow of the bulb.
        inline float Smooth01(float e0, float e1, float v)
        {
            const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Height of what stands on the map at (x, y): bilinear between the tile centres, so there are no tile blocks.
        inline float TopSmooth(const HeightGrid& g, float x, float y)
        {
            const float fx = x * (1.0f / 32.0f) - 0.5f - static_cast<float>(g.x0), fy = y * (1.0f / 32.0f) - 0.5f - static_cast<float>(g.y0);
            const int32_t ix = static_cast<int32_t>(std::floor(fx)), iy = static_cast<int32_t>(std::floor(fy));
            const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
            const auto at = [&](int32_t cx, int32_t cy) {
                cx = std::clamp(cx, 0, g.w - 1);
                cy = std::clamp(cy, 0, g.h - 1);
                return static_cast<float>(g.top[static_cast<size_t>(cy) * g.w + cx]);
            };
            return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
        }

        // The light of one lamp at one pixel (0 .. about 1). Three clean shapes that look the same in every direction:
        //  - a round glow in the air around the bulb,
        //  - a soft elliptical pool on the ground below it (a circle on the ground, seen in the iso view),
        //  - the small bulb itself.
        // The depth of the game picture is only used for soft gates: things clearly in front of the lamp or a roof above
        // it dim the glow, and the pool stays on surfaces near ground level. No tile or sprite shapes appear in the light.
        float LightAtPixel(const LightScreen& L, const Frame& fr, const ViewInfo& view, const HeightGrid&, int32_t, int32_t x, int32_t y)
        {
            const uint16_t* depth = fr.depth;
            x = std::clamp(x, 0, fr.width - 1);
            y = std::clamp(y, 0, fr.height - 1);
            const float wpp = std::max(0.01f, view.worldPerPixel);
            uint16_t dv = depth[static_cast<size_t>(y) * fr.width + x];
            int32_t sx = x, sy = y;
            if (dv == Drawing::kDepthUnknown)
            {
                static const int32_t kOffsets[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 } };
                for (const auto& o : kOffsets)
                {
                    const int32_t nx = x + o[0], ny = y + o[1];
                    if (nx < 0 || ny < 0 || nx >= fr.width || ny >= fr.height)
                        continue;
                    const uint16_t v = depth[static_cast<size_t>(ny) * fr.width + nx];
                    if (v != Drawing::kDepthUnknown)
                    {
                        dv = v;
                        sx = nx;
                        sy = ny;
                        break;
                    }
                }
            }
            const bool known = dv != Drawing::kDepthUnknown;
            float D = 0.0f, z = 0.0f;
            bool flat = false;
            if (known)
            {
                D = static_cast<float>(Drawing::DepthDecode(dv));
                z = D * 0.5f - (static_cast<float>(y) + static_cast<float>(view.originY)) * wpp;
                const uint16_t up = depth[static_cast<size_t>(std::max(sy - 1, 0)) * fr.width + sx];
                const uint16_t dn = depth[static_cast<size_t>(std::min(sy + 1, fr.height - 1)) * fr.width + sx];
                flat = !(up == dv && dn == dv);
            }
            const float px = static_cast<float>(x), py = static_cast<float>(y);
            float amount = 0.0f;

            // gates: in front of the lamp (nearer to the camera) and, for top sides, above the lamp
            float gate = 1.0f;
            if (known)
            {
                gate *= 1.0f - 0.6f * Smooth01(L.depthLight + 12.0f, L.depthLight + 70.0f, D);
                if (flat)
                    gate *= 1.0f - 0.85f * Smooth01(-4.0f, 26.0f, z - L.lz);
            }

            // 1. round glow around the bulb
            {
                const float rr = std::max(6.0f, L.radius * 0.9f / wpp);
                const float d2 = ((px - L.bx) * (px - L.bx) + (py - L.by) * (py - L.by)) / (rr * rr);
                if (d2 < 1.0f)
                {
                    const float e = 1.0f - d2;
                    amount += 0.26f * L.level * e * e * gate;
                }
            }
            // 2. pool on the ground
            {
                const float rx = std::max(8.0f, L.radius * 1.3f / wpp), ry = rx * 0.5f;
                const float ddx = (px - L.fx) / rx, ddy = (py - L.fy) / ry;
                const float d2 = ddx * ddx + ddy * ddy;
                if (d2 < 1.0f)
                {
                    const float e = 1.0f - d2;
                    // only surfaces near the ground level of the lamp (not roofs and tall things standing in the pool)
                    const float near = known ? 1.0f - Smooth01(18.0f, 64.0f, std::abs(z - L.groundZ)) : 1.0f;
                    amount += 0.48f * L.level * e * e * (0.25f + 0.75f * near);
                }
            }
            // 3. the bulb
            if (L.showBulb)
            {
                const float hx = px - L.bx, hy = py - L.by;
                const float hd = (hx * hx + hy * hy) / (L.haloRadius * L.haloRadius);
                if (hd < 1.0f)
                {
                    const float e = 1.0f - hd;
                    const float visible = known ? 1.0f - Smooth01(L.depthLight + 8.0f, L.depthLight + 40.0f, D) : 1.0f;
                    amount += 0.9f * L.level * e * e * e * visible;
                }
            }
            return amount;
        }

        // Accumulates all lights into the quarter resolution planes (rows qy0..qy1 of the low planes).
        void LightAccumulateRows(const Frame& fr, const ViewInfo& view, int32_t qy0, int32_t qy1)
        {
            const int32_t inverse = (4 - view.rotation) & 3;
            const auto& g = _heights;
            for (int32_t qy = qy0; qy < qy1; qy++)
            {
                _lowMin[qy] = INT32_MAX;
                _lowMax[qy] = -1;
                for (const auto& L : _lightScreens)
                {
                    const int32_t lqy0 = L.y0 >> 2, lqy1 = L.y1 >> 2;
                    if (qy < lqy0 || qy > lqy1)
                        continue;
                    for (int32_t qx = L.x0 >> 2; qx <= (L.x1 >> 2) && qx < _lowW; qx++)
                    {
                        const int32_t X = qx * 4, Y = qy * 4;
                        const float amount = 0.25f * (LightAtPixel(L, fr, view, g, inverse, X + 1, Y + 1) + LightAtPixel(L, fr, view, g, inverse, X + 3, Y + 1)
                            + LightAtPixel(L, fr, view, g, inverse, X + 1, Y + 3) + LightAtPixel(L, fr, view, g, inverse, X + 3, Y + 3));
                        if (amount < 0.002f)
                            continue;
                        const size_t i = static_cast<size_t>(qy) * _lowW + qx;
                        _low[0][0][i] += amount * L.cr;
                        _low[0][1][i] += amount * L.cg;
                        _low[0][2][i] += amount * L.cb;
                        _lowMin[qy] = std::min(_lowMin[qy], qx);
                        _lowMax[qy] = std::max(_lowMax[qy], qx);
                    }
                }
            }
        }

        // 3x3 blur of the accumulated planes (soft, clean edges).
        void LightBlurRows(int32_t qy0, int32_t qy1, int32_t src, int32_t dst)
        {
            for (int32_t qy = qy0; qy < qy1; qy++)
            {
                int32_t lo = INT32_MAX, hi = -1;
                for (int32_t k = std::max(0, qy - 1); k <= std::min(_lowH - 1, qy + 1); k++)
                    if (_lowMax[k] >= 0)
                    {
                        lo = std::min(lo, _lowMin[k]);
                        hi = std::max(hi, _lowMax[k]);
                    }
                if (hi < 0)
                    continue;
                lo = std::max(0, lo - 1);
                hi = std::min(_lowW - 1, hi + 1);
                for (int32_t qx = lo; qx <= hi; qx++)
                    for (int32_t c = 0; c < 3; c++)
                    {
                        float sum = 0.0f;
                        for (int32_t dy = -1; dy <= 1; dy++)
                        {
                            const int32_t yy = std::clamp(qy + dy, 0, _lowH - 1);
                            for (int32_t dx = -1; dx <= 1; dx++)
                            {
                                const int32_t xx = std::clamp(qx + dx, 0, _lowW - 1);
                                sum += _low[src][c][static_cast<size_t>(yy) * _lowW + xx] * ((dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f));
                            }
                        }
                        _low[dst][c][static_cast<size_t>(qy) * _lowW + qx] = sum * (1.0f / 16.0f);
                    }
            }
        }

        // Upsamples the light planes and adds the light to the picture.
        void LightApplyRows(const Frame& fr, int32_t y0, int32_t y1)
        {
            const uint32_t mask = (255u << fr.rShift) | (255u << fr.gShift) | (255u << fr.bShift);
            thread_local std::vector<int32_t> qxA, qxB;
            thread_local std::vector<float> wt;
            if (static_cast<int32_t>(qxA.size()) != fr.width)
            {
                qxA.resize(fr.width);
                qxB.resize(fr.width);
                wt.resize(fr.width);
            }
            for (int32_t x = 0; x < fr.width; x++)
            {
                const float u = std::max(0.0f, (static_cast<float>(x) - 1.5f) * 0.25f);
                const int32_t q0 = std::min(_lowW - 1, static_cast<int32_t>(u));
                qxA[x] = q0;
                qxB[x] = std::min(_lowW - 1, q0 + 1);
                wt[x] = u - static_cast<float>(q0);
            }
            for (int32_t y = y0; y < y1; y++)
            {
                const float fy = std::max(0.0f, (static_cast<float>(y) - 1.5f) * 0.25f);
                const int32_t qy0 = std::min(_lowH - 1, static_cast<int32_t>(fy)), qy1 = std::min(_lowH - 1, qy0 + 1);
                const float ty = fy - static_cast<float>(qy0);
                int32_t lo = INT32_MAX, hi = -1;
                for (int32_t k = std::max(0, qy0 - 1); k <= std::min(_lowH - 1, qy1 + 1); k++)
                    if (_lowMax[k] >= 0)
                    {
                        lo = std::min(lo, _lowMin[k]);
                        hi = std::max(hi, _lowMax[k]);
                    }
                if (hi < 0)
                    continue;
                const int32_t xBegin = std::max(0, (lo - 1) * 4 - 2), xEnd = std::min(fr.width, (hi + 2) * 4 + 3);
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                uint32_t* out = fr.out + static_cast<size_t>(y) * fr.width;
                const size_t r0 = static_cast<size_t>(qy0) * _lowW, r1 = static_cast<size_t>(qy1) * _lowW;
                for (int32_t x = xBegin; x < xEnd; x++)
                {
                    if (ui[x] != 0)
                        continue;
                    const size_t a = qxA[x], b = qxB[x];
                    const float tx = wt[x];
                    float l[3];
                    for (int32_t c = 0; c < 3; c++)
                    {
                        const float* plane = _low[0][c].data();
                        l[c] = ((plane[r0 + a] * (1 - tx) + plane[r0 + b] * tx) * (1 - ty)) + ((plane[r1 + a] * (1 - tx) + plane[r1 + b] * tx) * ty);
                    }
                    if (l[0] < 0.004f && l[1] < 0.004f && l[2] < 0.004f)
                        continue;
                    // lights glow beneath the clouds: they show through a cloud, but veiled
                    const float shine = 1.0f - 0.55f * CloudCoverAt(fr, x, y);
                    l[0] *= shine;
                    l[1] *= shine;
                    l[2] *= shine;
                    uint32_t p = out[x];
                    const auto lane = [&](uint32_t shift, float light) {
                        const float value = static_cast<float>((p >> shift) & 255u);
                        return static_cast<uint32_t>(value + (255.0f - value) * std::min(0.92f, light)) << shift;
                    };
                    out[x] = (p & ~mask) | lane(fr.rShift, l[0]) | lane(fr.gShift, l[1]) | lane(fr.bShift, l[2]);
                }
            }
        }

        // Runs the whole light pass.
        void RunLights(const Frame& fr, const ViewInfo& view, WorkerPool& pool)
        {
            if (!BuildLightList(fr, view))
                return;
            const int32_t lw = (fr.width + 3) / 4, lh = (fr.height + 3) / 4;
            if (lw != _lowW || lh != _lowH)
            {
                _lowW = lw;
                _lowH = lh;
                for (auto& set : _low)
                    for (auto& plane : set)
                        plane.assign(static_cast<size_t>(lw) * lh, 0.0f);
                _lowMin.assign(lh, INT32_MAX);
                _lowMax.assign(lh, -1);
                _lowDirtyY0 = 0;
                _lowDirtyY1 = lh - 1;
            }
            // clear the rows that held light last frame
            for (int32_t y = std::max(0, _lowDirtyY0); y <= std::min(lh - 1, _lowDirtyY1); y++)
                for (auto& set : _low)
                    for (auto& plane : set)
                        std::fill_n(plane.begin() + static_cast<size_t>(y) * lw, lw, 0.0f);
            int32_t rowLo = lh, rowHi = -1;
            for (const auto& L : _lightScreens)
            {
                rowLo = std::min(rowLo, L.y0 >> 2);
                rowHi = std::max(rowHi, L.y1 >> 2);
            }
            rowLo = std::max(0, rowLo - 1);
            rowHi = std::min(lh - 1, rowHi + 1);
            std::fill(_lowMin.begin(), _lowMin.end(), INT32_MAX);
            std::fill(_lowMax.begin(), _lowMax.end(), -1);
            _lowDirtyY0 = rowLo;
            _lowDirtyY1 = rowHi;
            const int32_t rows = rowHi - rowLo + 1;
            pool.ParallelFor((rows + 1) / 2, [&](int32_t t) {
                LightAccumulateRows(fr, view, rowLo + t * 2, std::min(rowHi + 1, rowLo + t * 2 + 2));
            });
            pool.ParallelFor((rows + 1) / 2, [&](int32_t t) { LightBlurRows(rowLo + t * 2, std::min(rowHi + 1, rowLo + t * 2 + 2), 0, 1); });
            // widen the valid column range by one for the second blur
            for (int32_t y = 0; y < _lowH; y++)
                if (_lowMax[y] >= 0)
                {
                    _lowMin[y] = std::max(0, _lowMin[y] - 1);
                    _lowMax[y] = std::min(_lowW - 1, _lowMax[y] + 1);
                }
            pool.ParallelFor((rows + 1) / 2, [&](int32_t t) { LightBlurRows(rowLo + t * 2, std::min(rowHi + 1, rowLo + t * 2 + 2), 1, 0); });
            for (int32_t y = 0; y < _lowH; y++)
                if (_lowMax[y] >= 0)
                {
                    _lowMin[y] = std::max(0, _lowMin[y] - 1);
                    _lowMax[y] = std::min(_lowW - 1, _lowMax[y] + 1);
                }
            pool.ParallelFor((rows + 1) / 2, [&](int32_t t) { LightBlurRows(rowLo + t * 2, std::min(rowHi + 1, rowLo + t * 2 + 2), 0, 1); });
            std::swap(_low[0], _low[1]);
            const int32_t y0 = std::max(0, rowLo * 4 - 4), y1 = std::min(fr.height, (rowHi + 1) * 4 + 4);
            pool.ParallelFor((y1 - y0 + 7) / 8, [&](int32_t t) { LightApplyRows(fr, y0 + t * 8, std::min(y1, y0 + t * 8 + 8)); });
        }

        // Near-field blur, part 1: per grid cell, the strongest blurred pixel (radius >= 2) and its depth.
        void BuildCellsRows(FrameState& f, const uint8_t* radius, const uint16_t* depth, int32_t cy0, int32_t cy1)
        {
            const int32_t w = f.width, h = f.height, c = f.cell;
            for (int32_t cy = cy0; cy < cy1; cy++)
            {
                for (int32_t cx = 0; cx < f.cellsX; cx++)
                {
                    uint8_t best = 0;
                    uint16_t bestDepth = 0;
                    const int32_t xEnd = std::min(w, (cx + 1) * c), yEnd = std::min(h, (cy + 1) * c);
                    for (int32_t y = cy * c; y < yEnd; y++)
                    {
                        const uint8_t* r = radius + static_cast<size_t>(y) * w;
                        const uint16_t* d = depth + static_cast<size_t>(y) * w;
                        for (int32_t x = cx * c; x < xEnd; x++)
                        {
                            if (r[x] < 2 || d[x] == 0xFFFF)
                                continue;
                            if (r[x] > best || (r[x] == best && d[x] > bestDepth))
                            {
                                best = r[x];
                                bestDepth = d[x];
                            }
                        }
                    }
                    const size_t idx = static_cast<size_t>(cy) * f.cellsX + cx;
                    f.cellRadius.data()[idx] = best;
                    f.cellDepth.data()[idx] = bestDepth;
                }
            }
        }

        // Near-field blur, part 2: chamfer propagation of (radius, source depth) over the grid. The radius falls off by
        // one pixel per pixel of distance (cone), so the blur of a near object reaches about its own radius around it.
        void PropagateCells(FrameState& f)
        {
            const int32_t cx = f.cellsX, cy = f.cellsY, c = f.cell;
            const int32_t orth = c, diag = (c * 3) / 2;
            uint8_t* r = f.cellRadius.data();
            uint16_t* d = f.cellDepth.data();
            auto relax = [&](size_t to, size_t from, int32_t cost) {
                const int32_t cand = static_cast<int32_t>(r[from]) - cost;
                if (cand > r[to])
                {
                    r[to] = static_cast<uint8_t>(cand);
                    d[to] = d[from];
                }
            };
            for (int32_t y = 0; y < cy; y++)
                for (int32_t x = 0; x < cx; x++)
                {
                    const size_t i = static_cast<size_t>(y) * cx + x;
                    if (x > 0)
                        relax(i, i - 1, orth);
                    if (y > 0)
                    {
                        relax(i, i - cx, orth);
                        if (x > 0)
                            relax(i, i - cx - 1, diag);
                        if (x + 1 < cx)
                            relax(i, i - cx + 1, diag);
                    }
                }
            for (int32_t y = cy - 1; y >= 0; y--)
                for (int32_t x = cx - 1; x >= 0; x--)
                {
                    const size_t i = static_cast<size_t>(y) * cx + x;
                    if (x + 1 < cx)
                        relax(i, i + 1, orth);
                    if (y + 1 < cy)
                    {
                        relax(i, i + cx, orth);
                        if (x + 1 < cx)
                            relax(i, i + cx + 1, diag);
                        if (x > 0)
                            relax(i, i + cx - 1, diag);
                    }
                }
        }

        // Near-field blur, part 3: a pixel takes the (bilinearly interpolated) spread radius of the grid when the source
        // is clearly nearer than the pixel itself. Sharp objects next to far blurred ones are therefore left alone.
        void ApplyNearRows(
            const FrameState& f, const uint8_t* radius, const uint16_t* depth, int32_t marginStored, uint8_t* out, int32_t y0,
            int32_t y1)
        {
            const int32_t w = f.width, cx = f.cellsX;
            const uint8_t* cr = f.cellRadius.data();
            const uint16_t* cd = f.cellDepth.data();
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* r = radius + static_cast<size_t>(y) * w;
                const uint16_t* d = depth + static_cast<size_t>(y) * w;
                uint8_t* o = out + static_cast<size_t>(y) * w;
                const int32_t wy = f.tableWY[y];
                const size_t row0 = static_cast<size_t>(f.tableY0[y]) * cx, row1 = static_cast<size_t>(f.tableY1[y]) * cx;
                for (int32_t x = 0; x < w; x++)
                {
                    int32_t v = r[x];
                    const int32_t dp = d[x];
                    if (dp != 0xFFFF)
                    {
                        const size_t i00 = row0 + f.tableX0[x], i10 = row0 + f.tableX1[x];
                        const size_t i01 = row1 + f.tableX0[x], i11 = row1 + f.tableX1[x];
                        // The spread can never exceed the strongest surrounding cell: nothing to do if the pixel is already
                        // at least that blurred.
                        if (v >= std::max(std::max<int32_t>(cr[i00], cr[i10]), std::max<int32_t>(cr[i01], cr[i11])))
                        {
                            o[x] = static_cast<uint8_t>(v);
                            continue;
                        }
                        const int32_t limit = dp + marginStored;
                        const int32_t c00 = (cr[i00] != 0 && cd[i00] > limit) ? cr[i00] : 0;
                        const int32_t c10 = (cr[i10] != 0 && cd[i10] > limit) ? cr[i10] : 0;
                        const int32_t c01 = (cr[i01] != 0 && cd[i01] > limit) ? cr[i01] : 0;
                        const int32_t c11 = (cr[i11] != 0 && cd[i11] > limit) ? cr[i11] : 0;
                        if ((c00 | c10 | c01 | c11) != 0)
                        {
                            const int32_t wx = f.tableWX[x];
                            const int32_t top = (c00 * (256 - wx) + c10 * wx) >> 8;
                            const int32_t bottom = (c01 * (256 - wx) + c11 * wx) >> 8;
                            const int32_t spread = (top * (256 - wy) + bottom * wy + 128) >> 8;
                            v = std::max(v, spread);
                        }
                    }
                    o[x] = static_cast<uint8_t>(v);
                }
            }
        }

        // 3-tap max/min along a row (dilate / erode), SSE2 on the interior.
        void Morph3Row(const uint8_t* in, uint8_t* out, int32_t w, bool useMax)
        {
            auto op = [useMax](uint8_t a, uint8_t b, uint8_t c) {
                return useMax ? std::max({ a, b, c }) : std::min({ a, b, c });
            };
            out[0] = op(in[0], in[0], in[1]);
            int32_t x = 1;
            for (; x + 16 < w; x += 16)
            {
                const __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x - 1));
                const __m128i m = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x));
                const __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x + 1));
                const __m128i v = useMax ? _mm_max_epu8(_mm_max_epu8(l, m), r) : _mm_min_epu8(_mm_min_epu8(l, m), r);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), v);
            }
            for (; x < w; x++)
                out[x] = op(in[x - 1], in[x], in[std::min(x + 1, w - 1)]);
        }

        void Morph3Rows(const uint8_t* src, uint8_t* dst, int32_t w, bool useMax, int32_t y0, int32_t y1)
        {
            for (int32_t y = y0; y < y1; y++)
                Morph3Row(src + static_cast<size_t>(y) * w, dst + static_cast<size_t>(y) * w, w, useMax);
        }

        // Vertical 3-tap max/min: combines three whole rows, so memory access stays contiguous.
        void Morph3RowsVertical(const uint8_t* src, uint8_t* dst, int32_t w, int32_t h, bool useMax, int32_t y0, int32_t y1)
        {
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* up = src + static_cast<size_t>(std::max(y - 1, 0)) * w;
                const uint8_t* mid = src + static_cast<size_t>(y) * w;
                const uint8_t* dn = src + static_cast<size_t>(std::min(y + 1, h - 1)) * w;
                uint8_t* out = dst + static_cast<size_t>(y) * w;
                int32_t x = 0;
                for (; x + 16 <= w; x += 16)
                {
                    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(up + x));
                    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mid + x));
                    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dn + x));
                    const __m128i v = useMax ? _mm_max_epu8(_mm_max_epu8(a, b), c) : _mm_min_epu8(_mm_min_epu8(a, b), c);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), v);
                }
                for (; x < w; x++)
                    out[x] = useMax ? std::max({ up[x], mid[x], dn[x] }) : std::min({ up[x], mid[x], dn[x] });
            }
        }

        // Horizontal box smoothing of a byte plane (running sum), rows y0..y1.
        void SmoothRows(const uint8_t* src, uint8_t* dst, int32_t w, int32_t s, int32_t y0, int32_t y1)
        {
            for (int32_t y = y0; y < y1; y++)
            {
                const uint8_t* in = src + static_cast<size_t>(y) * w;
                uint8_t* out = dst + static_cast<size_t>(y) * w;
                int32_t sum = 0, hi = -1;
                for (int32_t x = 0; x < w; x++)
                {
                    const int32_t newHi = std::min(x + s, w - 1);
                    while (hi < newHi)
                        sum += in[++hi];
                    if (x - s - 1 >= 0)
                        sum -= in[x - s - 1];
                    out[x] = static_cast<uint8_t>(sum / (newHi - std::max(x - s, 0) + 1));
                }
            }
        }

        constexpr int32_t kChunkWidth = 128;

        // Vertical box smoothing of a byte plane for one wide column chunk; every row access is a contiguous run.
        void SmoothChunk(const uint8_t* src, uint8_t* dst, int32_t w, int32_t h, int32_t s, int32_t chunk)
        {
            const int32_t x0 = chunk * kChunkWidth;
            const int32_t cw = std::min(kChunkWidth, w - x0);
            int32_t sum[kChunkWidth] = {};
            int32_t hi = -1;
            for (int32_t y = 0; y < h; y++)
            {
                const int32_t newHi = std::min(y + s, h - 1);
                while (hi < newHi)
                {
                    ++hi;
                    const uint8_t* row = src + static_cast<size_t>(hi) * w + x0;
                    for (int32_t i = 0; i < cw; i++)
                        sum[i] += row[i];
                }
                if (y - s - 1 >= 0)
                {
                    const uint8_t* row = src + static_cast<size_t>(y - s - 1) * w + x0;
                    for (int32_t i = 0; i < cw; i++)
                        sum[i] -= row[i];
                }
                const int32_t count = newHi - std::max(y - s, 0) + 1;
                const int32_t recip = 65536 / count + 1;
                uint8_t* out = dst + static_cast<size_t>(y) * w + x0;
                for (int32_t i = 0; i < cw; i++)
                    out[i] = static_cast<uint8_t>(std::min(255, (sum[i] * recip) >> 16));
            }
        }

        // Grey visualisation of the final blur radius (debug_depth = 2).
        void DebugRadiusView(const Frame& fr, const uint8_t* radius, float maxRadius)
        {
            for (int32_t y = 0; y < fr.height; y++)
            {
                uint32_t* o = fr.out + static_cast<size_t>(y) * fr.width;
                const uint8_t* r = radius + static_cast<size_t>(y) * fr.width;
                const uint8_t* ui = fr.ui + static_cast<size_t>(y) * fr.width;
                for (int32_t x = 0; x < fr.width; x++)
                {
                    if (ui[x])
                        continue;
                    const uint32_t g = std::min(255.0f, 255.0f * r[x] / std::max(1.0f, maxRadius));
                    o[x] = (g << fr.rShift) | (g << fr.gShift) | (g << fr.bShift) | (0xFFu << fr.freeShift);
                }
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Colour grading: applied to the 256 palette entries only (world pixels), the UI keeps the raw palette.
        // ---------------------------------------------------------------------------------------------------------
        inline float Luma(float r, float g, float b)
        {
            return 0.299f * r + 0.587f * g + 0.114f * b;
        }

        void GradeColour(const ColourGrade& g, float& r, float& gr, float& b)
        {
            // White balance (gamma-encoded values, simple channel gains)
            r *= 1.0f + 0.25f * g.temperature - 0.04f * g.tint;
            gr *= 1.0f - 0.18f * g.tint;
            b *= 1.0f - 0.25f * g.temperature - 0.04f * g.tint;

            // Exposure in linear light
            if (g.exposure != 0.0f)
            {
                const float gain = std::exp2(g.exposure);
                r = std::pow(std::pow(std::max(r, 0.0f), 2.2f) * gain, 1.0f / 2.2f);
                gr = std::pow(std::pow(std::max(gr, 0.0f), 2.2f) * gain, 1.0f / 2.2f);
                b = std::pow(std::pow(std::max(b, 0.0f), 2.2f) * gain, 1.0f / 2.2f);
            }
            if (g.gamma != 1.0f)
            {
                const float inv = 1.0f / g.gamma;
                r = std::pow(std::max(r, 0.0f), inv);
                gr = std::pow(std::max(gr, 0.0f), inv);
                b = std::pow(std::max(b, 0.0f), inv);
            }

            // Highlights / shadows follow the luma of the colour
            if (g.highlights != 0.0f || g.shadows != 0.0f)
            {
                const float l = std::clamp(Luma(r, gr, b), 0.0f, 1.0f);
                const float add = g.highlights * 0.3f * l * l + g.shadows * 0.3f * (1.0f - l) * (1.0f - l);
                r += add;
                gr += add;
                b += add;
            }

            // Contrast around mid grey
            if (g.contrast != 0.0f)
            {
                const float k = 1.0f + g.contrast;
                r = (r - 0.5f) * k + 0.5f;
                gr = (gr - 0.5f) * k + 0.5f;
                b = (b - 0.5f) * k + 0.5f;
            }

            // Saturation and vibrance around the luma
            r = std::clamp(r, 0.0f, 1.0f);
            gr = std::clamp(gr, 0.0f, 1.0f);
            b = std::clamp(b, 0.0f, 1.0f);
            float factor = 1.0f + g.saturation;
            if (g.vibrance != 0.0f)
            {
                const float chroma = std::max({ r, gr, b }) - std::min({ r, gr, b });
                factor *= 1.0f + g.vibrance * (1.0f - chroma);
            }
            if (factor != 1.0f)
            {
                const float l = Luma(r, gr, b);
                r = l + (r - l) * factor;
                gr = l + (gr - l) * factor;
                b = l + (b - l) * factor;
            }
            r = std::clamp(r, 0.0f, 1.0f);
            gr = std::clamp(gr, 0.0f, 1.0f);
            b = std::clamp(b, 0.0f, 1.0f);
        }

        // Builds the palette used for world pixels. Entries keep their non-colour byte (alpha / padding).
        void BuildWorldPalette(const uint32_t* palette, const PixelLayout& layout, const ColourGrade& g, uint32_t* out)
        {
            if (g.IsNeutral() && !_time.active && _cube.size == 0 && _settings.sunlight == 0.0f && _settings.sunWarmth == 0.0f)
            {
                std::memcpy(out, palette, sizeof(uint32_t) * 256);
                return;
            }
            const uint32_t rs = layout.redShift, gs = layout.greenShift, bs = layout.blueShift;
            const uint32_t colourMask = (0xFFu << rs) | (0xFFu << gs) | (0xFFu << bs);
            for (int32_t i = 0; i < 256; i++)
            {
                const uint32_t p = palette[i];
                float r = static_cast<float>((p >> rs) & 0xFFu) / 255.0f;
                float gr = static_cast<float>((p >> gs) & 0xFFu) / 255.0f;
                float b = static_cast<float>((p >> bs) & 0xFFu) / 255.0f;
                if (_time.active)
                {
                    const float lum = 0.3f * r + 0.59f * gr + 0.11f * b;
                    _time.emissive[i] = 0;
                    (void)lum;
                }
                GradeColour(g, r, gr, b);
                if (_cube.size > 0)
                    ApplyCube(r, gr, b);
                // Bounded daylight lift: black stays black; bright pixel-art colours retain highlight detail.
                const float daylight = _time.active ? 1.0f - _time.night : 1.0f;
                const float lift = _settings.sunlight * daylight * 0.55f;
                const float warmth = _settings.sunWarmth * daylight;
                r += lift * r * (1.0f - r);
                gr += lift * gr * (1.0f - gr);
                b += lift * b * (1.0f - b);
                r += warmth * 0.16f * r * (1.0f - r);
                gr += warmth * 0.035f * gr * (1.0f - gr);
                b *= 1.0f - warmth * 0.18f;
                if (_time.active)
                {
                    // lamps and lit windows keep their brightness when it gets dark
                    const float keep = 0.0f; // no palette colour glows at night any more (lights are light objects)
                    r = r * (_time.tint[0] + (1.0f - _time.tint[0]) * keep);
                    gr = gr * (_time.tint[1] + (1.0f - _time.tint[1]) * keep);
                    b = b * (_time.tint[2] + (1.0f - _time.tint[2]) * keep);
                }
                const uint32_t R = static_cast<uint32_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint32_t G = static_cast<uint32_t>(std::clamp(gr, 0.0f, 1.0f) * 255.0f + 0.5f);
                const uint32_t B = static_cast<uint32_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
                out[i] = (p & ~colourMask) | (R << rs) | (G << gs) | (B << bs);
            }
        }
    } // namespace

    // ---------------------------------------------------------------------------------------------------------
    // LUTs
    // ---------------------------------------------------------------------------------------------------------
    namespace
    {
        constexpr const char* kLutExtension = ".atmolut";

        std::string SanitiseLutName(std::string name)
        {
            for (auto& c : name)
            {
                const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '
                    || c == '_' || c == '-' || c == '(' || c == ')' || c == '.';
                if (!allowed)
                    c = '_';
            }
            const auto first = name.find_first_not_of(" .");
            if (first == std::string::npos)
                return {};
            const auto last = name.find_last_not_of(" .");
            name = name.substr(first, last - first + 1);
            if (name.size() > 48)
                name.resize(48);
            return name;
        }
    } // namespace

    // ---- Light objects: storage, persistence, editing API ----------------------------------------------------------
    namespace
    {
        const char* const kAnimNames[] = { "None", "Flicker", "Pulse", "Vibrate", "Strobe", "Blink", "Candle", "Flash" };
        struct ColourPreset
        {
            const char* name;
            uint8_t r, g, b;
        };
        const ColourPreset kColourPresets[] = {
            { "Warm white", 255, 200, 128 }, { "Candle", 255, 150, 60 },  { "White", 255, 255, 255 },
            { "Cool white", 200, 225, 255 }, { "Lightning blue", 150, 205, 255 }, { "Red", 255, 40, 30 },
            { "Orange", 255, 120, 20 },      { "Yellow", 255, 235, 70 }, { "Green", 60, 255, 90 },
            { "Cyan", 40, 230, 255 },        { "Blue", 60, 90, 255 },    { "Purple", 170, 70, 255 },
            { "Pink", 255, 90, 200 },
        };

        std::string SafeFileName(std::string name)
        {
            for (auto& c : name)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_')
                    c = '_';
            if (name.empty())
                name = "park";
            if (name.size() > 60)
                name.resize(60);
            return name;
        }

        std::filesystem::path LightsPath(const std::string& park)
        {
            auto userDir = GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user);
            const auto dir = fs::u8path(userDir) / "atmosphere" / "lights";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / (SafeFileName(park) + ".lights");
        }

        void SaveLights()
        {
            if (_lightsPark.empty() && _lights.empty())
                return;
            std::ofstream out(LightsPath(_lightsPark), std::ios::trunc);
            if (!out)
                return;
            out << "# Atmosphere light objects: x y height r g b intensity radius animation speed on\n";
            for (const auto& l : _lights)
                out << "L " << l.x << ' ' << l.y << ' ' << l.height << ' ' << static_cast<int>(l.r) << ' ' << static_cast<int>(l.g)
                    << ' ' << static_cast<int>(l.b) << ' ' << l.intensity << ' ' << l.radius << ' ' << static_cast<int>(l.anim)
                    << ' ' << l.speed << ' ' << (l.on ? 1 : 0) << ' ' << (l.bulb ? 1 : 0) << "\n";
            _lightsDirty = false;
        }

        void LoadLightsFor(const std::string& park)
        {
            _lights.clear();
            _lightSelected = -1;
            _lightsPark = park;
            _lightNextId = 1;
            std::ifstream in(LightsPath(park));
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] != 'L')
                    continue;
                LightObject l;
                int r = 255, g = 200, b = 128, anim = 0, on = 1;
                std::istringstream ss(line.substr(1));
                if (!(ss >> l.x >> l.y >> l.height >> r >> g >> b >> l.intensity >> l.radius >> anim >> l.speed >> on))
                    continue;
                l.r = static_cast<uint8_t>(std::clamp(r, 0, 255));
                l.g = static_cast<uint8_t>(std::clamp(g, 0, 255));
                l.b = static_cast<uint8_t>(std::clamp(b, 0, 255));
                l.anim = static_cast<LightAnim>(std::clamp(anim, 0, static_cast<int>(LightAnim::Count) - 1));
                l.on = on != 0;
                int bulbFlag = 1;
                ss >> bulbFlag;
                l.bulb = bulbFlag != 0;
                l.id = _lightNextId++;
                _lights.push_back(l);
            }
            _lightsDirty = false;
        }

        void MarkLightsDirty()
        {
            _lightsDirty = true;
            _lightsDirtySince = Clock::now();
            _settingsRevision++;
        }

        // Called every frame from IsActive: switches the light set when another park was loaded, writes edits.
        void TickLights()
        {
            const std::string& name = getGameState().park.name;
            static bool loadedOnce = false;
            if (!loadedOnce || name != _lightsPark)
            {
                if (_lightsDirty)
                    SaveLights();
                loadedOnce = true;
                LoadLightsFor(name);
                _settingsRevision++;
            }
            else if (_lightsDirty && Clock::now() - _lightsDirtySince > std::chrono::milliseconds(400))
                SaveLights();
            static int32_t seenDemo = 0;
            if (_settings.devLightDemo != 0 && _settings.devLightDemo != seenDemo)
            {
                seenDemo = _settings.devLightDemo;
                auto* main = WindowGetMain();
                if (main != nullptr && main->viewport != nullptr)
                {
                    Viewport* found = nullptr;
                    const auto c = ScreenGetMapXY({ main->viewport->pos.x + main->viewport->width / 2, main->viewport->pos.y + main->viewport->height / 2 }, &found);
                    if (c.has_value())
                    {
                        static const uint8_t cols[6][3] = { { 255, 200, 128 }, { 255, 60, 40 }, { 60, 255, 120 }, { 80, 120, 255 }, { 150, 205, 255 }, { 255, 230, 80 } };
                        for (int32_t i = 0; i < 12; i++)
                        {
                            LightObject l;
                            l.id = _lightNextId++;
                            l.x = c->x + ((i % 4) - 2) * 90;
                            l.y = c->y + ((i / 4) - 1) * 90;
                            l.height = 30.0f + 10.0f * (i % 3);
                            l.r = cols[i % 6][0]; l.g = cols[i % 6][1]; l.b = cols[i % 6][2];
                            l.anim = static_cast<LightAnim>(i % static_cast<int>(LightAnim::Count));
                            l.radius = 80.0f + 20.0f * (i % 4);
                            _lights.push_back(l);
                        }
                    }
                }
            }
        }
    } // namespace

    void LightSnap(int32_t& x, int32_t& y)
    {
        if (_settings.lightSnap == 2)
        {
            x = (x >> 5) * 32 + 16;
            y = (y >> 5) * 32 + 16;
        }
        else if (_settings.lightSnap == 1)
        {
            x = (x >> 3) * 8 + 4;
            y = (y >> 3) * 8 + 4;
        }
    }

    int32_t LightPickAtScreen(int32_t screenX, int32_t screenY)
    {
        auto* main = WindowGetMain();
        if (main == nullptr || main->viewport == nullptr)
            return -1;
        const auto* vp = main->viewport;
        if (ViewportFindFromPoint(ScreenCoordsXY{ screenX, screenY }) != vp)
            return -1;
        const auto p0 = vp->ScreenToViewportCoord(ScreenCoordsXY{ screenX, screenY });
        const float scale = static_cast<float>(vp->zoom.ApplyTo(1024)) / 1024.0f;
        const float px = static_cast<float>(p0.x), py = static_cast<float>(p0.y);
        int32_t best = -1;
        float bestDistance = (22.0f * scale) * (22.0f * scale);
        for (int32_t i = 0; i < LightCount(); i++)
        {
            const auto& l = _lights[i];
            const float ground = static_cast<float>(TileElementHeight(CoordsXY{ l.x, l.y }));
            const CoordsXY rot = CoordsXY{ l.x, l.y }.rotate(vp->rotation);
            const float sx = static_cast<float>(rot.y - rot.x);
            const float depthHalf = LightPlacement::PlaneY(rot.x,rot.y,0);
            const float yBulb = depthHalf - (ground + l.height), yFoot = depthHalf - ground;
            // distance to the vertical line from the bulb down to the foot
            const float cy = std::clamp(py, std::min(yBulb, yFoot), std::max(yBulb, yFoot));
            const float d = (px - sx) * (px - sx) + (py - cy) * (py - cy);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    bool PickSurfacePoint(int32_t screenX, int32_t screenY, int32_t& mapX, int32_t& mapY, int32_t& z)
    {
        auto* main = WindowGetMain();
        const auto& plane = Drawing::gDepthPlane;
        if (main == nullptr || main->viewport == nullptr || plane.data == nullptr || _lastFrameWidth <= 0 || _lastFrameHeight <= 0)
            return false;
        if (screenX < 0 || screenY < 0 || screenX >= _lastFrameWidth || screenY >= _lastFrameHeight)
            return false;
        // the depth of the surface under the cursor (a few pixels of tolerance for blended edges)
        uint16_t dv = Drawing::kDepthUnknown;
        for (int32_t r = 0; r <= 3 && dv == Drawing::kDepthUnknown; r++)
            for (int32_t dy = -r; dy <= r && dv == Drawing::kDepthUnknown; dy++)
                for (int32_t dx = -r; dx <= r; dx++)
                {
                    const int32_t x = screenX + dx, y = screenY + dy;
                    if (x < 0 || y < 0 || x >= _lastFrameWidth || y >= _lastFrameHeight)
                        continue;
                    const size_t index = static_cast<size_t>(y) * _lastFrameWidth + x;
                    if (index < plane.size && plane.data[index] != Drawing::kDepthUnknown)
                    {
                        dv = plane.data[index];
                        break;
                    }
                }
        if (dv == Drawing::kDepthUnknown)
            return false;
        const auto vc = main->viewport->ScreenToViewportCoord(ScreenCoordsXY{ screenX, screenY });
        const float D = static_cast<float>(Drawing::DepthDecode(dv));
        const float fz = D * 0.5f - static_cast<float>(vc.y);
        const float xr = (D - static_cast<float>(vc.x)) * 0.5f, yr = (D + static_cast<float>(vc.x)) * 0.5f;
        const CoordsXY m = CoordsXY{ static_cast<int32_t>(xr), static_cast<int32_t>(yr) }.rotate((4 - main->viewport->rotation) & 3);
        mapX = m.x;
        mapY = m.y;
        z = static_cast<int32_t>(fz);
        return true;
    }

    void LightPlaceAt(int32_t index, int32_t x, int32_t y, int32_t surfaceZ, float lift)
    {
        if (index < 0 || index >= LightCount())
            return;
        if (!MapIsLocationValid(CoordsXY{x,y})) return;
        const float height=static_cast<float>(surfaceZ-TileElementHeight(CoordsXY{x,y}))+lift;
        if(height<0.0f || height>640.0f) return;
        auto& l = _lights[index];
        l.x = x;
        l.y = y;
        l.height = height;
        LightLoadEdit();
        MarkLightsDirty();
    }

    int32_t LightCount()
    {
        return static_cast<int32_t>(_lights.size());
    }

    const LightObject* LightAt(int32_t index)
    {
        return index >= 0 && index < LightCount() ? &_lights[index] : nullptr;
    }

    int32_t LightSelected()
    {
        return _lightSelected < LightCount() ? _lightSelected : -1;
    }

    void LightSelect(int32_t index)
    {
        _lightSelected = index >= 0 && index < LightCount() ? index : -1;
        LightLoadEdit();
    }

    static LightObject LightTemplate(bool useClipboard)
    {
        if (useClipboard && _lightHasClipboard) return _lightClipboard;
        if (_lightSelected >= 0 && _lightSelected < LightCount()) return _lights[_lightSelected];
        return LightObject{};
    }

    float LightPlacementHeight(bool useClipboard)
    {
        return LightTemplate(useClipboard).height;
    }

    int32_t LightAdd(int32_t x, int32_t y, bool useClipboard)
    {
        auto l = LightTemplate(useClipboard);
        l.id = _lightNextId++;
        l.x = x;
        l.y = y;
        _lights.push_back(l);
        _lightSelected = static_cast<int32_t>(_lights.size()) - 1;
        LightLoadEdit();
        MarkLightsDirty();
        return _lightSelected;
    }

    void LightDelete(int32_t index)
    {
        if (index < 0 || index >= LightCount())
            return;
        _lights.erase(_lights.begin() + index);
        _lightSelected = _lights.empty() ? -1 : std::min(index, LightCount() - 1);
        LightLoadEdit();
        MarkLightsDirty();
    }

    void LightMove(int32_t index, int32_t x, int32_t y)
    {
        if (index < 0 || index >= LightCount())
            return;
        _lights[index].x = x;
        _lights[index].y = y;
        MarkLightsDirty();
    }

    int32_t LightNearest(int32_t x, int32_t y, int32_t maxDistance)
    {
        int32_t best = -1;
        int64_t bestD = static_cast<int64_t>(maxDistance) * maxDistance;
        for (int32_t i = 0; i < LightCount(); i++)
        {
            const int64_t dx = _lights[i].x - x, dy = _lights[i].y - y;
            if (dx * dx + dy * dy <= bestD)
            {
                bestD = dx * dx + dy * dy;
                best = i;
            }
        }
        return best;
    }

    void LightCopy()
    {
        if (_lightSelected >= 0 && _lightSelected < LightCount())
        {
            _lightClipboard = _lights[_lightSelected];
            _lightHasClipboard = true;
        }
    }

    bool LightHasClipboard()
    {
        return _lightHasClipboard;
    }

    void LightLoadEdit()
    {
        const auto* l = LightAt(_lightSelected);
        if (l == nullptr)
            return;
        _settings.editHeight = l->height;
        _settings.editRadius = l->radius;
        _settings.editIntensity = l->intensity;
        _settings.editR = l->r / 255.0f;
        _settings.editG = l->g / 255.0f;
        _settings.editB = l->b / 255.0f;
        _settings.editSpeed = l->speed;
    }

    void LightApplyEdit()
    {
        if (_lightSelected < 0 || _lightSelected >= LightCount())
            return;
        auto& l = _lights[_lightSelected];
        l.height = _settings.editHeight;
        l.radius = _settings.editRadius;
        l.intensity = _settings.editIntensity;
        l.r = static_cast<uint8_t>(std::clamp(_settings.editR, 0.0f, 1.0f) * 255.0f + 0.5f);
        l.g = static_cast<uint8_t>(std::clamp(_settings.editG, 0.0f, 1.0f) * 255.0f + 0.5f);
        l.b = static_cast<uint8_t>(std::clamp(_settings.editB, 0.0f, 1.0f) * 255.0f + 0.5f);
        l.speed = _settings.editSpeed;
        MarkLightsDirty();
    }

    void LightSetOn(int32_t index, bool on)
    {
        if (index >= 0 && index < LightCount())
        {
            _lights[index].on = on;
            MarkLightsDirty();
        }
    }

    void LightSetBulb(int32_t index, bool bulb)
    {
        if (index >= 0 && index < LightCount())
        {
            _lights[index].bulb = bulb;
            MarkLightsDirty();
        }
    }

    void LightSetAnim(int32_t index, LightAnim anim)
    {
        if (index >= 0 && index < LightCount())
        {
            _lights[index].anim = anim;
            MarkLightsDirty();
        }
    }

    int32_t LightColourPresetCount()
    {
        return static_cast<int32_t>(std::size(kColourPresets));
    }

    const char* LightColourPresetName(int32_t index)
    {
        return index >= 0 && index < LightColourPresetCount() ? kColourPresets[index].name : "";
    }

    void LightApplyColourPreset(int32_t index)
    {
        if (index < 0 || index >= LightColourPresetCount() || _lightSelected < 0 || _lightSelected >= LightCount())
            return;
        auto& l = _lights[_lightSelected];
        l.r = kColourPresets[index].r;
        l.g = kColourPresets[index].g;
        l.b = kColourPresets[index].b;
        LightLoadEdit();
        MarkLightsDirty();
    }

    const char* LightAnimName(int32_t index)
    {
        return index >= 0 && index < static_cast<int32_t>(std::size(kAnimNames)) ? kAnimNames[index] : "";
    }

    void LightsEditPing()
    {
        _lightsPing = Clock::now();
    }

    bool LightsEditing()
    {
        return LightsEditingNow();
    }

    std::string LightsFileName()
    {
        return LightsPath(_lightsPark).string();
    }

    std::string LutDirectory()
    {
        auto userDir = GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user);
        const auto dir = fs::u8path(userDir) / "atmosphere" / "luts";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }

    std::vector<LutInfo> ListLuts()
    {
        std::vector<LutInfo> result;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(LutDirectory(), ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            auto extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension == kLutExtension)
                result.push_back({ false, entry.path().stem().string(), entry.path().string() });
            else if (extension == ".cube")
                result.push_back({ true, entry.path().stem().string() + ".cube", entry.path().string() });
        }
        std::sort(result.begin(), result.end(), [](const LutInfo& a, const LutInfo& b) {
            return std::lexicographical_compare(
                a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
        });
        return result;
    }

    std::string SaveLut(const std::string& name, const ColourGrade& grade)
    {
        const std::string clean = SanitiseLutName(name);
        if (clean.empty())
            return {};
        const auto path = std::filesystem::path(LutDirectory()) / (clean + kLutExtension);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return {};
        out << "# Atmosphere LUT: a saved colour setting. Copy this file into the 'atmosphere/luts' folder of another\n";
        out << "# OpenRCT2 (Atmosphere build) user directory to share it.\n";
        out << "[atmosphere-lut]\n";
        out << "version = 1\n";
        out << "name = " << clean << "\n";
        out << "exposure = " << grade.exposure << "\n";
        out << "contrast = " << grade.contrast << "\n";
        out << "saturation = " << grade.saturation << "\n";
        out << "vibrance = " << grade.vibrance << "\n";
        out << "temperature = " << grade.temperature << "\n";
        out << "tint = " << grade.tint << "\n";
        out << "highlights = " << grade.highlights << "\n";
        out << "shadows = " << grade.shadows << "\n";
        out << "gamma = " << grade.gamma << "\n";
        return out ? clean : std::string{};
    }

    bool LoadCubeLut(const std::string& path)
    {
        std::ifstream in(path);
        if (!in)
            return false;
        CubeLut lut;
        std::string line;
        size_t expected = 0;
        while (std::getline(in, line))
        {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == '#')
                continue;
            if (line.compare(first, 11, "LUT_3D_SIZE") == 0)
            {
                lut.size = static_cast<int32_t>(ParseFloat(Trim(line.substr(first + 11)), 0.0f));
                if (lut.size < 2 || lut.size > 128)
                    return false;
                expected = static_cast<size_t>(lut.size) * lut.size * lut.size * 3;
                lut.data.reserve(expected);
                continue;
            }
            if (line.compare(first, 11, "LUT_1D_SIZE") == 0)
                return false; // only 3D tables are supported
            if (std::isalpha(static_cast<unsigned char>(line[first])))
                continue; // TITLE, DOMAIN_MIN, DOMAIN_MAX ...
            if (expected == 0)
                return false;
            std::istringstream values(line);
            float v[3];
            if (!(values >> v[0] >> v[1] >> v[2]))
                continue;
            for (float f : v)
                lut.data.push_back(f);
        }
        if (expected == 0 || lut.data.size() != expected)
            return false;
        _cube = std::move(lut);
        _cubeName = std::filesystem::path(path).filename().string();
        NotifySettingsChanged();
        return true;
    }

    void ClearCubeLut()
    {
        if (_cube.size != 0)
        {
            _cube = CubeLut{};
            _cubeName.clear();
            NotifySettingsChanged();
        }
    }

    bool CubeLutActive()
    {
        return _cube.size > 0;
    }

    bool LoadLut(const std::string& path, ColourGrade& grade)
    {
        std::ifstream in(path);
        if (!in)
            return false;
        ColourGrade g;
        bool any = false;
        std::string line;
        while (std::getline(in, line))
        {
            const auto hash = line.find('#');
            if (hash != std::string::npos)
                line.resize(hash);
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const auto key = Trim(line.substr(0, eq));
            const auto val = Trim(line.substr(eq + 1));
            const auto number = [&](float fallback) { return ParseFloat(val, fallback); };
            if (key == "exposure")
                g.exposure = std::clamp(number(0.0f), -3.0f, 3.0f);
            else if (key == "contrast")
                g.contrast = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "saturation")
                g.saturation = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "vibrance")
                g.vibrance = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "temperature")
                g.temperature = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "tint")
                g.tint = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "highlights")
                g.highlights = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "shadows")
                g.shadows = std::clamp(number(0.0f), -1.0f, 1.0f);
            else if (key == "gamma")
                g.gamma = std::clamp(number(1.0f), 0.5f, 2.0f);
            else
                continue;
            any = true;
        }
        if (!any)
            return false;
        grade = g;
        return true;
    }

    bool DeleteLut(const std::string& path)
    {
        std::error_code ec;
        return std::filesystem::remove(path, ec);
    }

    Settings& GetSettings()
    {
        return _settings;
    }

    void ResetEffectSettings()
    {
        const Settings defaults;
        _settings.vignetteStrength = defaults.vignetteStrength;
        _settings.vignetteInner = defaults.vignetteInner;
        _settings.tiltStrength = defaults.tiltStrength;
        _settings.tiltFocus = defaults.tiltFocus;
        _settings.tiltBand = defaults.tiltBand;
        _settings.tiltFalloff = defaults.tiltFalloff;
        _settings.tiltIterations = defaults.tiltIterations;
        _settings.tiltDepth = defaults.tiltDepth;
        _settings.tiltSmooth = defaults.tiltSmooth;
        _settings.tiltNear = defaults.tiltNear;
        NotifySettingsChanged();
    }

    namespace
    {
        struct PresetDef
        {
            const char* name;
            void (*apply)(Settings&);
        };

        void ClearAll(Settings& s)
        {
            s.vignetteStrength = 0.0f;
            s.tiltStrength = 0.0f;
            s.cloudCoverage = 0.0f;
            s.fog = 0.0f;
            s.shadowStrength = 0.0f;
            s.lightningStrength = 0.0f;
            s.sunlight = 0.0f;
            s.sunWarmth = 0.0f;
            s.timeEnabled = false;
            s.timeCycle = false;
            s.swayTrees = false;
            s.snowCover = 0.0f;
            s.weatherEffects = false;
            s.grade = ColourGrade{};
            s.activeLut.clear();
        }

        const PresetDef kPresets[] = {
            { "Soft clouds and sunlight", [](Settings& s) {
                  ClearAll(s);
                  s.sunlight = 0.45f; s.sunWarmth = 0.35f;
                  s.shadowStrength = 0.5f; s.shadowSoftness = 0.65f;
                  s.sunAzimuth = 260.0f; s.sunElevation = 45.0f; s.shadowReach = 120.0f;
                  s.lightningStrength = 0.4f; s.lightningInterval = 20.0f;
                  s.cloudCoverage = 0.55f; s.cloudSize = 3000.0f; s.cloudHeight = 700.0f;
                  s.cloudOpacity = 0.95f; s.cloudShadow = 0.4f; s.cloudWeather = false;
                  s.cloudFocus = true; s.cloudClear = 0.5f; s.cloudSpeed = 1.0f;
              } },
            { "Atmosphere (recommended)", [](Settings& s) {
                  const Settings d;
                  ClearAll(s);
                  s.vignetteStrength = d.vignetteStrength;
                  s.vignetteInner = d.vignetteInner;
                  s.tiltStrength = d.tiltStrength;
                  s.tiltBand = d.tiltBand;
                  s.tiltFalloff = d.tiltFalloff;
                  s.tiltIterations = d.tiltIterations;
                  s.tiltDepth = true;
                  s.tiltNear = true;
                  s.cloudCoverage = d.cloudCoverage;
                  s.cloudShadow = d.cloudShadow;
                  s.cloudWeather = true;
                  s.weatherEffects = true;
                  s.swayTrees = true;
                  
                  s.shadowStrength = 0.4f;
                  s.grade.saturation = 0.08f;
              } },
            { "Miniature", [](Settings& s) {
                  ClearAll(s);
                  s.tiltStrength = 0.7f; s.tiltBand = 0.18f; s.tiltFalloff = 0.45f; s.tiltIterations = 3;
                  s.tiltDepth = true; s.tiltNear = true;
                  s.vignetteStrength = 0.3f; s.vignetteInner = 0.45f;
                  s.grade.saturation = 0.22f; s.grade.vibrance = 0.15f; s.grade.contrast = 0.1f;
                  s.cloudCoverage = 0.3f; s.cloudShadow = 0.4f; s.shadowStrength = 0.4f;
              } },
            { "Golden hour", [](Settings& s) {
                  ClearAll(s);
                  s.timeEnabled = true; s.timeHour = 18.3f; s.timeCycle = false;
                  s.vignetteStrength = 0.3f;
                  s.tiltStrength = 0.35f; s.tiltDepth = true; s.tiltNear = true;
                  s.cloudCoverage = 0.3f; s.cloudShadow = 0.5f; s.shadowStrength = 0.5f;
                  s.grade.temperature = 0.18f; s.grade.contrast = 0.1f; s.grade.vibrance = 0.12f;
              } },
            { "Foggy morning", [](Settings& s) {
                  ClearAll(s);
                  s.timeEnabled = true; s.timeHour = 6.9f;
                  s.fog = 0.45f; s.cloudCoverage = 0.3f; s.cloudOpacity = 0.6f; s.cloudShadow = 0.3f;
                  s.vignetteStrength = 0.3f; s.tiltStrength = 0.25f; s.tiltDepth = true; s.tiltNear = true;
                  s.grade.saturation = -0.1f; s.grade.temperature = -0.08f;
              } },
            { "Storm", [](Settings& s) {
                  ClearAll(s);
                  s.cloudCoverage = 0.50f; s.cloudOpacity = 0.80f; s.cloudShadow = 0.6f; s.cloudWeather = true;
                  s.weatherEffects = true; s.windStrength = 0.8f; s.swayTrees = true; s.swayAmount = 0.8f;
                  s.vignetteStrength = 0.35f;
                  s.grade.saturation = -0.15f; s.grade.contrast = 0.12f; s.grade.temperature = -0.1f;
              } },
            { "Winter night", [](Settings& s) {
                  ClearAll(s);
                  s.timeEnabled = true; s.timeHour = 22.0f; s.nightDarkness = 0.5f;
                  s.weatherEffects = true; s.cloudWeather = true; s.cloudCoverage = 0.35f;
                  s.vignetteStrength = 0.35f; s.tiltStrength = 0.35f; s.tiltDepth = true; s.tiltNear = true;
                  s.grade.temperature = -0.15f;
              } },
            { "Everything off", [](Settings& s) { ClearAll(s); } },
        };
    } // namespace

    int32_t PresetCount()
    {
        return static_cast<int32_t>(std::size(kPresets));
    }

    const char* PresetName(int32_t index)
    {
        return index >= 0 && index < PresetCount() ? kPresets[index].name : "";
    }

    void ApplyPreset(int32_t index)
    {
        if (index < 0 || index >= PresetCount())
            return;
        ClearCubeLut();
        kPresets[index].apply(_settings);
        NotifySettingsChanged();
    }

    void ResetLightSettings()
    {
        const Settings defaults;
        _settings.shadowStrength = defaults.shadowStrength;
        _settings.shadowSoftness = defaults.shadowSoftness;
        _settings.shadowGuestDistance = defaults.shadowGuestDistance;
        _settings.shadowVehicleDistance = defaults.shadowVehicleDistance;
        _settings.shadowTrackDistance = defaults.shadowTrackDistance;
        _settings.faceShading = defaults.faceShading;
        _settings.sunAzimuth = defaults.sunAzimuth;
        _settings.sunElevation = defaults.sunElevation;
        _settings.sunlight = defaults.sunlight;
        _settings.sunWarmth = defaults.sunWarmth;
        _settings.shadowReach = defaults.shadowReach;
        _settings.timeEnabled = defaults.timeEnabled;
        _settings.timeHour = defaults.timeHour;
        _settings.timeCycle = defaults.timeCycle;
        _settings.timeCycleMinutes = defaults.timeCycleMinutes;
        _settings.nightDarkness = defaults.nightDarkness;
        NotifySettingsChanged();
    }

    void ResetWeatherSettings()
    {
        const Settings defaults;
        _settings.weatherEffects = defaults.weatherEffects;
        _settings.precipitation = defaults.precipitation;
        _settings.windStrength = defaults.windStrength;
        _settings.swayTrees = defaults.swayTrees;
        _settings.snowCover = defaults.snowCover;
        _settings.swayAmount = defaults.swayAmount;
        _settings.waterWaves = defaults.waterWaves;
        _settings.waterGlitter = defaults.waterGlitter;
        _settings.leaves = defaults.leaves;
        _settings.waterDepth = defaults.waterDepth;
        _settings.waterPatches = defaults.waterPatches;
        _settings.waterSun = defaults.waterSun;
        _settings.lightningStrength = defaults.lightningStrength;
        _settings.lightningInterval = defaults.lightningInterval;
        NotifySettingsChanged();
    }

    bool ReplacesWeather()
    {
        return _settings.enabled;
    }

    void ResetCloudSettings()
    {
        const Settings defaults;
        _settings.cloudCoverage = defaults.cloudCoverage;
        _settings.cloudSize = defaults.cloudSize;
        _settings.cloudSpeed = defaults.cloudSpeed;
        _settings.cloudWind = defaults.cloudWind;
        _settings.cloudOpacity = defaults.cloudOpacity;
        _settings.cloudShadow = defaults.cloudShadow;
        _settings.cloudClear = defaults.cloudClear;
        _settings.cloudFocus = defaults.cloudFocus;
        _settings.cloudHeight = defaults.cloudHeight;
        _settings.fog = defaults.fog;
        _settings.cloudWeather = defaults.cloudWeather;
        NotifySettingsChanged();
    }

    void ResetColourGrade()
    {
        _settings.grade = ColourGrade{};
        NotifySettingsChanged();
    }

    void NotifySettingsChanged()
    {
        if (!_settingsDirty)
            _dirtySince = Clock::now();
        _settingsDirty = true;
        _settingsRevision++;
    }

    std::string DefaultSettingsFile()
    {
        auto userDir = GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user);
        return (fs::u8path(userDir) / "atmosphere.ini").string();
    }

    void RestoreCubeFromSettings();

    void SetSettingsFile(const char* settingsFile)
    {
        if (!_settingsPath.empty())
            return;
        _settingsPath = settingsFile;
        _settings = LoadSettings(_settingsPath);
        RestoreCubeFromSettings();
        _knownMtime = FileTime(_settingsPath);
        _lastSettingsCheck = Clock::now();
    }

    bool OpaqueInterface()
    {
        return _lastActive;
    }

    float CurrentHourOfDay()
    {
        return CurrentHour();
    }

    void RestoreCubeFromSettings()
    {
        const auto& name = _settings.activeLut;
        const bool wantsCube = name.size() > 5 && name.compare(name.size() - 5, 5, ".cube") == 0;
        if (wantsCube && _cubeName != name)
            LoadCubeLut((std::filesystem::path(LutDirectory()) / name).string());
        else if (!wantsCube && _cube.size != 0)
            _cube = CubeLut{}, _cubeName.clear();
    }

    bool CinemaActive()
    {
        return _settings.cinemaActive;
    }

    void SetCinemaActive(bool active)
    {
        _settings.cinemaActive = active;
        if (!active)
            Drawing::gSmoothCameraRate.store(0.0f, std::memory_order_relaxed);
    }

    float CinemaBarFraction()
    {
        const bool smooth = _settings.cinemaActive && _settings.cinemaSmooth;
        // smoothness 0..1 -> glide rate 14..3 per second (lower = slower, gentler)
        Drawing::gSmoothCameraRate.store(smooth ? 14.0f - 11.0f * _settings.cinemaSmoothness : 0.0f, std::memory_order_relaxed);
        return _settings.cinemaActive && _settings.cinemaBars ? _settings.cinemaBarHeight : 0.0f;
    }

    bool PerfLogEnabled()
    {
        return _settings.devPerfLog;
    }

    uint32_t SettingsRevision()
    {
        return _settingsRevision;
    }

    bool IsAnimated()
    {
        if (_settings.leaves > 0.0f)
            return true;
        if (ObjectLightsWanted() || LightningPossible() || _settings.waterWaves > 0.0f || (_settings.waterGlitter > 0.0f || _settings.waterDepth > 0.0f || _settings.waterPatches > 0.0f || _settings.waterSun > 0.0f) || _settings.sunRays > 0.0f)
            return true;
        if (_settings.cloudCoverage > 0.0f || _settings.fog > 0.0f || (_settings.swayTrees && _settings.swayAmount > 0.0f)
            || SnowWanted() || _settings.debugDepth || _settings.debugMask || (_settings.timeEnabled && _settings.timeCycle))
            return true;
        return _settings.weatherEffects && (WeatherHasPrecipitation() || _precipLevelNow > 0.005f);
    }

    bool IsActive(const char* settingsFile)
    {
        if (Drawing::AtmosphereFoliage::SyncProfile(GetContext()->GetObjectManager())) Drawing::GfxInvalidateScreen();

        SetSettingsFile(settingsFile);
        TickLights();
        const auto now = Clock::now();
        if (_settingsDirty && now - _dirtySince > std::chrono::milliseconds(250))
        {
            // Menu changes are written shortly after the last edit; our own write must not trigger a reload.
            SaveSettings(_settingsPath, _settings);
            _knownMtime = FileTime(_settingsPath);
            _settingsDirty = false;
        }
        if (!_settingsDirty && now - _lastSettingsCheck > std::chrono::milliseconds(500))
        {
            _lastSettingsCheck = now;
            const auto mtime = FileTime(_settingsPath);
            if (mtime != _knownMtime)
            {
                const bool wasCinema = _settings.cinemaActive;
                _settings = LoadSettings(_settingsPath);
                _settings.cinemaActive = wasCinema || _settings.cinemaActive;
                _knownMtime = mtime;
                _settingsRevision++;
                RestoreCubeFromSettings();
            }
        }
        _lastActive = _settings.enabled
            && (_settings.vignetteStrength > 0.0f || _settings.tiltStrength > 0.0f || !_settings.grade.IsNeutral()
                || _settings.shadowStrength > 0.0f || _settings.sunlight > 0.0f || _settings.sunWarmth > 0.0f || _settings.sunRays > 0.0f
                || _settings.cloudCoverage > 0.0f || _settings.fog > 0.0f
                || ObjectLightsWanted() || LightningPossible() || _settings.leaves > 0.0f
                || _settings.weatherEffects || _settings.swayTrees || _settings.timeEnabled || _settings.waterWaves > 0.0f || (_settings.waterGlitter > 0.0f || _settings.waterDepth > 0.0f || _settings.waterPatches > 0.0f || _settings.waterSun > 0.0f) || _settings.debugMask || _settings.debugDepth);
        // Atmosphere owns precipitation even when its rain/snow switch is off.
        // Disabling that switch must not bring back the legacy patterned rain.
        Drawing::gAtmosphereReplacesWeather.store(ReplacesWeather(), std::memory_order_relaxed);
        Drawing::gAtmosphereReplacesLightning.store(_lastActive, std::memory_order_relaxed);
        Drawing::gAtmosphereOpaqueUi.store(_lastActive, std::memory_order_relaxed);
        Drawing::gSwayEnabled.store(
            _lastActive && ((_settings.swayTrees && _settings.swayAmount > 0.0f) || SnowWanted() || _settings.leaves > 0.0f),
            std::memory_order_relaxed);
        return _lastActive;
    }

    bool ProcessStill(
        const uint8_t* bits, int32_t width, int32_t height, const uint32_t* palette, int32_t viewX, int32_t viewY, int32_t zoom,
        int32_t rotation, uint32_t* out)
    {
        if (!_lastActive || !_settings.enabled || bits == nullptr || out == nullptr || width < 2 || height < 2)
            return false;
        // Strips of about ten million pixels, with a margin so the blur does not show seams.
        constexpr int32_t kMargin = 200;
        const int32_t rows = std::clamp<int32_t>(static_cast<int32_t>(10000000ll / width), 2 * kMargin + 64, height);
        const PixelLayout layout{ 0, 8, 16 };
        const std::vector<ExcludeRect> none;
        std::vector<uint32_t> strip;
        const float wpp = zoom >= 0 ? static_cast<float>(1 << zoom) : 1.0f / static_cast<float>(1 << -zoom);
        for (int32_t y0 = 0; y0 < height; y0 += rows)
        {
            const int32_t y1 = std::min(height, y0 + rows);
            const int32_t ya = std::max(0, y0 - kMargin), yb = std::min(height, y1 + kMargin);
            strip.assign(static_cast<size_t>(width) * (yb - ya), 0);
            _still = { true, height, ya };
            _frame.mapStrength = -1.0f; // the vignette map depends on the strip
            ViewInfo view;
            view.worldPerPixel = wpp;
            view.originX = viewX;
            view.originY = viewY + ya;
            view.rotation = rotation;
            view.viewportWidth = width;
            view.viewportHeight = yb - ya;
            ProcessFrame(bits + static_cast<size_t>(ya) * width, palette, strip.data(), width, yb - ya, layout, none, nullptr, view, nullptr, nullptr);
            std::memcpy(
                out + static_cast<size_t>(y0) * width, strip.data() + static_cast<size_t>(y0 - ya) * width,
                static_cast<size_t>(y1 - y0) * width * sizeof(uint32_t));
        }
        _still = {};
        _frame.mapStrength = -1.0f;
        return true;
    }

    bool TakeDevZoom(int32_t& value)
    {
        static int32_t last = 100;
        if (_settings.devZoom == 100 || (_settings.devZoom == last && !_settings.devLock))
            return false;
        last = _settings.devZoom;
        value = last;
        return true;
    }

    bool TakeDevOpenOptions(int32_t& sub)
    {
        static int32_t applied = 0;
        if (_settings.devOpenOptions == 0 || _settings.devOpenOptions == applied)
            return false;
        applied = _settings.devOpenOptions;
        sub = _settings.devOptionsSub;
        return true;
    }

    namespace
    {
        int32_t _pendingOptionsSub = -1;
    }

    void SetPendingOptionsSubPage(int32_t sub)
    {
        _pendingOptionsSub = sub;
    }

    bool ConsumePendingOptionsSubPage(int32_t& sub)
    {
        if (_pendingOptionsSub < 0)
            return false;
        sub = _pendingOptionsSub;
        _pendingOptionsSub = -1;
        return true;
    }

    bool TakeDevWeather(int32_t& type)
    {
        static auto last = Clock::time_point{};
        if (_settings.devWeather < 0 || Clock::now() - last < std::chrono::seconds(1))
            return false;
        last = Clock::now();
        type = _settings.devWeather;
        return true;
    }

    void DumpFrame(const uint32_t* pixels, int32_t width, int32_t height, const PixelLayout& layout)
    {
        static int32_t frames = 0, written = 0;
        if (_settings.devDump <= 0 || written >= _settings.devDump || ++frames < _settings.devDumpAfter)
            return;
        if ((frames - _settings.devDumpAfter) % 90 != 0)
            return;
        std::ofstream file(_settingsPath + ".frame" + std::to_string(written++) + ".bmp", std::ios::binary);
        if (!file)
            return;
        const uint32_t rowBytes = (static_cast<uint32_t>(width) * 3u + 3u) & ~3u;
        const uint32_t dataSize = rowBytes * static_cast<uint32_t>(height);
        uint8_t head[54] = { 'B', 'M' };
        const auto put32 = [&](int offset, uint32_t value) {
            for (int i = 0; i < 4; i++)
                head[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        };
        put32(2, 54 + dataSize);
        put32(10, 54);
        put32(14, 40);
        put32(18, static_cast<uint32_t>(width));
        put32(22, static_cast<uint32_t>(height));
        head[26] = 1;
        head[28] = 24;
        put32(34, dataSize);
        file.write(reinterpret_cast<const char*>(head), sizeof(head));
        std::vector<uint8_t> row(rowBytes, 0);
        for (int32_t y = height - 1; y >= 0; y--)
        {
            const uint32_t* s = pixels + static_cast<size_t>(y) * width;
            for (int32_t x = 0; x < width; x++)
            {
                row[static_cast<size_t>(x) * 3 + 0] = static_cast<uint8_t>(s[x] >> layout.blueShift);
                row[static_cast<size_t>(x) * 3 + 1] = static_cast<uint8_t>(s[x] >> layout.greenShift);
                row[static_cast<size_t>(x) * 3 + 2] = static_cast<uint8_t>(s[x] >> layout.redShift);
            }
            file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(rowBytes));
        }
    }

    bool TakeDevScreenshot(int32_t& kind)
    {
        static int32_t applied = 0;
        if (_settings.devScreenshot == 0 || _settings.devScreenshot == applied)
            return false;
        applied = _settings.devScreenshot;
        kind = applied;
        return true;
    }

    bool TakeDevRotate(int32_t& steps)
    {
        static int32_t applied = 0;
        if (_settings.devRotate == applied)
            return false;
        steps = _settings.devRotate - applied;
        applied = _settings.devRotate;
        return true;
    }

    bool WantsSway()
    {
        return _settings.enabled && ((_settings.swayTrees && _settings.swayAmount > 0.0f) || SnowWanted() || _settings.leaves > 0.0f);
    }

    bool WantsDepth()
    {
        return _settings.enabled
            && (_settings.debugDepth || _settings.shadowStrength > 0.0f || SnowWanted() || _settings.leaves > 0.0f
                || (_settings.swayTrees && _settings.swayAmount > 0.0f)
                || _settings.waterWaves > 0.0f || (_settings.waterGlitter > 0.0f || _settings.waterDepth > 0.0f || _settings.waterPatches > 0.0f || _settings.waterSun > 0.0f)
                || ObjectLightsWanted() || LightningPossible()
                || (_settings.cloudCoverage > 0.0f && (_settings.cloudShadow > 0.0f || _settings.cloudOpacity > 0.0f))
                || _settings.fog > 0.0f || _settings.sunRays > 0.0f
                || (_settings.weatherEffects && WeatherHasPrecipitation())
                || _settings.tiltStrength > 0.0f);
    }

    void ProcessFrame(
        const uint8_t* bits, const uint32_t* palette, uint32_t* out, int32_t width, int32_t height, const PixelLayout& layout,
        const std::vector<ExcludeRect>& excluded, const uint16_t* depth, const ViewInfo& view, const uint8_t* world,
        const uint8_t* sway)
    {
        const float worldPerPixel = view.worldPerPixel;
        if (bits == nullptr || palette == nullptr || out == nullptr || width < 2 || height < 2)
            return;
        static uint64_t foliageRevision = 0;
        const auto currentFoliageRevision=Drawing::AtmosphereFoliage::revision.load(std::memory_order_relaxed);
        if (foliageRevision!=currentFoliageRevision) {
            _leaves.clear();_leafAcc=0;foliageRevision=currentFoliageRevision;
        }
        const auto start = Clock::now();
        auto lapStart = start;
        auto lap = [&](Stage stage) {
            const auto now = Clock::now();
            _stageMs[stage] += std::chrono::duration<double, std::milli>(now - lapStart).count();
            lapStart = now;
        };

        // The one byte lane that is not a colour channel carries the coverage weight.
        const bool aligned = (layout.redShift % 8 == 0) && (layout.greenShift % 8 == 0) && (layout.blueShift % 8 == 0);
        const uint32_t laneSum = layout.redShift / 8 + layout.greenShift / 8 + layout.blueShift / 8;
        const bool tilt = _settings.tiltStrength > 0.0f && aligned && laneSum <= 6 && laneSum >= 3;
        const uint32_t freeShift = aligned && laneSum <= 6 ? (6 - laneSum) * 8 : 24;

        auto& f = _frame;
        if (f.width != width || f.height != height)
        {
            f.width = width;
            f.height = height;
            f.mapStrength = -1.0f;
            f.bufA.assign(static_cast<size_t>(width) * height, 0);
            f.bufB.assign(static_cast<size_t>(width) * height, 0);
            f.lifeA.assign(static_cast<size_t>(width) * height, 0);
            f.lifeB.assign(static_cast<size_t>(width) * height, 0);
            f.lifeRadius.assign(static_cast<size_t>(width) * height, 0);
            f.lifeRowMax.assign(height, 0);
            f.radiusRaw.assign(static_cast<size_t>(width) * height, 0);
            f.radiusSmooth.assign(static_cast<size_t>(width) * height, 0);
            f.radiusFinal.assign(static_cast<size_t>(width) * height, 0);
            f.uiMask.assign(static_cast<size_t>(width) * height, 0);
            f.halfWidth = (width + 1) / 2;
            f.halfHeight = (height + 1) / 2;
            f.halfA.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.halfB.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.radiusHalf.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.rowMaxHalf.assign(f.halfHeight, 0);
            f.shadowRaw.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.shadowZ.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.faceVal.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.faceDepth.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.shadowSoft.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.shadowCoverage.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.cloudShadowPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.cloudAlphaPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.fogPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.cloudLitPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.precipPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.cloudRawPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.skyPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.snowPlane.assign(static_cast<size_t>(f.halfWidth) * f.halfHeight, 0);
            f.rayW = (width + 3) / 4;
            f.rayH = (height + 3) / 4;
            if (f.rayPlane.size() != static_cast<size_t>(f.rayW) * f.rayH)
            {
                f.rayPlane.assign(static_cast<size_t>(f.rayW) * f.rayH, 0);
                f.rayValid = false;
            }
            f.cell = std::clamp((height + 67) / 135, 4, 16);
            f.cellsX = (width + f.cell - 1) / f.cell;
            f.cellsY = (height + f.cell - 1) / f.cell;
            f.cellRadius.assign(static_cast<size_t>(f.cellsX) * f.cellsY, 0);
            f.cellDepth.assign(static_cast<size_t>(f.cellsX) * f.cellsY, 0);
            auto buildTable = [&](int32_t count, int32_t cells, std::vector<int32_t>& i0, std::vector<int32_t>& i1, std::vector<uint16_t>& wt) {
                i0.assign(count, 0);
                i1.assign(count, 0);
                wt.assign(count, 0);
                for (int32_t p = 0; p < count; p++)
                {
                    const float fp = (p + 0.5f) / static_cast<float>(f.cell) - 0.5f;
                    const int32_t a = std::clamp(static_cast<int32_t>(std::floor(fp)), 0, cells - 1);
                    i0[p] = a;
                    i1[p] = std::min(a + 1, cells - 1);
                    wt[p] = static_cast<uint16_t>(std::clamp(fp - static_cast<float>(a), 0.0f, 1.0f) * 256.0f);
                }
            };
            buildTable(width, f.cellsX, f.tableX0, f.tableX1, f.tableWX);
            buildTable(height, f.cellsY, f.tableY0, f.tableY1, f.tableWY);
            f.rowMax.assign(height, 0);
            f.focusValid = false;
        }
        const bool vignette = _settings.vignetteStrength > 0.0f;
        if (vignette && (f.mapStrength != _settings.vignetteStrength || f.mapInner != _settings.vignetteInner))
            RebuildVignetteMap(f);
        BuildRadii(f);

        static uint32_t coverageLut[256];
        static bool lutReady = false;
        if (!lutReady)
        {
            for (uint32_t c = 1; c < 256; c++)
                coverageLut[c] = static_cast<uint32_t>((255.0 * 65536.0) / c);
            lutReady = true;
        }

        UpdateTimeState();
        uint32_t worldPalette[256];
        BuildWorldPalette(palette, layout, _settings.grade, worldPalette);
        const Frame frBase{ bits,   palette,          out,        width,           height,           freeShift,
                            &excluded, layout.redShift, layout.greenShift, layout.blueShift, f.uiMask.data(), worldPalette, depth, worldPerPixel,
                            depth != nullptr ? Drawing::gDepthPlane.shadowFlags : nullptr };
        auto& pool = WorkerPool::Get();
        const int32_t rowTasks = (height + kRowChunk - 1) / kRowChunk;
        auto rowRange = [&](int32_t task, int32_t& y0, int32_t& y1) {
            y0 = task * kRowChunk;
            y1 = std::min(height, y0 + kRowChunk);
        };

        pool.ParallelFor(rowTasks, [&](int32_t t) {
            int32_t y0, y1;
            rowRange(t, y0, y1);
            BuildUiMaskRows(frBase, world, f.uiMask.data(), y0, y1, view);
        });
        lap(kStageMask);

        // Trees and foliage bend in the wind and the water surface moves: displaced copy of the frame (the original frame
        // buffer is never touched).
        const uint8_t* effectiveBits = bits;
        const uint8_t* swayAltPlane = nullptr;
        const uint8_t* swayFracPlane = nullptr;
        const bool doSway = sway != nullptr && _settings.swayTrees && _settings.swayAmount > 0.0f;
        const bool doWater = _settings.waterWaves > 0.0f;
        if (doSway || doWater)
        {
            static const auto swayStart = Clock::now();
            const float st = std::chrono::duration<float>(Clock::now() - swayStart).count();
            const float gust = 0.75f + 0.25f * std::sin(st * 0.37f) + 0.1f * std::sin(st * 1.13f + 2.0f);
            const float wind = _settings.windStrength * gust * (1.0f + 0.9f * _precipLevelNow);
            const float ampPx = 3.91f * _settings.swayAmount * (0.3f + 1.2f * wind) / std::max(1.0f, worldPerPixel);
            // the water moves in bigger swells than the plants (up to eight pixels at full strength)
            const float waterT = WaterClockNow();
            const float waterAmp = 8.0f * _settings.waterWaves / std::max(1.0f, worldPerPixel);
            float wave[16];
            for (int32_t i = 0; i < 16; i++)
                wave[i] = (0.7f * std::sin(st * 1.6f + static_cast<float>(i) * 0.7f) + 0.3f * std::sin(st * 2.9f + static_cast<float>(i) * 1.9f)) * ampPx;
            if (doSway) {
                f.swayBits.resize(static_cast<size_t>(width) * height);
                f.swayAltBits.resize(static_cast<size_t>(width) * height);
                f.swayFracBits.resize(static_cast<size_t>(width) * height);
            }
            uint8_t* swayOut = f.swayBits.data();
            uint8_t* swayAlt = f.swayAltBits.data();
            uint8_t* swayFrac = f.swayFracBits.data();
            if (doWater)
                f.waterColour.resize(static_cast<size_t>(width) * height);
            uint32_t* waterOut = doWater ? f.waterColour.data() : nullptr;
            const uint8_t* uiMask = f.uiMask.data();
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                for (int32_t y = y0; y < y1; y++)
                {
                    const size_t row = static_cast<size_t>(y) * width;
                    if (doSway) {
                        std::memcpy(swayOut + row, bits + row, static_cast<size_t>(width));
                        std::memset(swayFrac + row, 0, static_cast<size_t>(width));
                    }
                    if (doWater)
                        std::fill_n(waterOut + row, width, 0u);
                    if (doWater)
                    {
                        // Surface flags remain stable through palette animation. Both the pixel and
                        // its source must be water, so shores, boats and the interface never get pulled into the surface.
                        const auto isWater = [&](size_t index) {
                            return frBase.shadowFlags != nullptr && (frBase.shadowFlags[index] & Drawing::kWaterSurface) != 0;
                        };
                        bool anyWater = false;
                        for (int32_t x = 0; x < width && !anyWater; x++)
                            anyWater = isWater(row + x);
                        if (anyWater)
                        {
                            thread_local std::vector<float> fx, fy;
                            const int32_t steps = width / 4 + 2;
                            fx.resize(static_cast<size_t>(steps));
                            fy.resize(static_cast<size_t>(steps));
                            const float wyRow = (static_cast<float>(y) + static_cast<float>(view.originY)) * worldPerPixel;
                            for (int32_t k = 0; k < steps; k++)
                            {
                                float fdx, fdy, slope;
                                WaterWaveField((static_cast<float>(k * 4) + static_cast<float>(view.originX)) * worldPerPixel, wyRow, waterT, fdx, fdy, slope);
                                fx[static_cast<size_t>(k)] = fdx * waterAmp;
                                fy[static_cast<size_t>(k)] = fdy * waterAmp;
                            }
                            for (int32_t x = 0; x < width; x++)
                            {
                                if (!isWater(row + x) || uiMask[row + x])
                                    continue;
                                const int32_t k = x >> 2;
                                const float u = static_cast<float>(x & 3) * 0.25f;
                                const float sdx = fx[static_cast<size_t>(k)] * (1.0f - u) + fx[static_cast<size_t>(k) + 1] * u;
                                const float sdy = fy[static_cast<size_t>(k)] * (1.0f - u) + fy[static_cast<size_t>(k) + 1] * u;
                                waterOut[row + x] = SampleWaterColour(frBase, x, y, sdx, sdy);
                            }
                        }
                    }
                    if (!doSway)
                        continue;
                    const uint8_t* sw = sway + row;
                    int32_t first = -1, last = -1;
                    for (int32_t x = 0; x < width; x++)
                        if (sw[x] >> 4)
                        {
                            if (first < 0)
                                first = x;
                            last = x;
                        }
                    if (first < 0)
                        continue;
                    // prefix count of swaying pixels: windows without any are skipped in O(1)
                    thread_local std::vector<int32_t> prefix;
                    prefix.resize(static_cast<size_t>(width) + 1);
                    prefix[0] = 0;
                    for (int32_t x = 0; x < width; x++)
                        prefix[x + 1] = prefix[x] + ((sw[x] >> 4) != 0 ? 1 : 0);
                    for (int32_t x = std::max(0, first - 4); x <= std::min(width - 1, last + 4); x++)
                    {
                        if (prefix[std::min(width, x + 5)] - prefix[std::max(0, x - 4)] == 0)
                            continue;
                        // strongest bending within reach of this pixel decides its displacement
                        uint8_t best = 0;
                        for (int32_t k = std::max(0, x - 4); k <= std::min(width - 1, x + 4); k++)
                            if ((sw[k] >> 4) > (best >> 4))
                                best = sw[k];
                        if ((best >> 4) == 0)
                            continue;
                        const float shift = wave[best & 15] * static_cast<float>(best >> 4) * (1.0f / 15.0f);
                        const int32_t whole = static_cast<int32_t>(std::floor(shift));
                        const float frac = shift - static_cast<float>(whole);
                        const int32_t sx = x - whole;
                        if (sx < 1 || sx >= width || uiMask[row + x] || uiMask[row + sx] || uiMask[row + sx - 1])
                            continue;
                        if (waterOut != nullptr)
                            waterOut[row + x] = 0; // foreground foliage owns the displaced pixel
                        swayOut[row + x] = bits[row + sx];
                        swayAlt[row + x] = bits[row + sx - 1];
                        swayFrac[row + x] = static_cast<uint8_t>(frac * 255.0f);
                    }
                }
            });
            if (doSway) {
                effectiveBits = swayOut;
                swayAltPlane = swayAlt;
                swayFracPlane = swayFrac;
            }
        }
        lap(kStageSway);
        Frame fr{ effectiveBits, palette, out, width, height, freeShift,
                        &excluded, layout.redShift, layout.greenShift, layout.blueShift, f.uiMask.data(), worldPalette, depth, worldPerPixel,
                        depth != nullptr ? Drawing::gDepthPlane.shadowFlags : nullptr, swayAltPlane, swayFracPlane };

        fr.waterColour = doWater ? f.waterColour.data() : nullptr;

        _lastFrameWidth = width;
        _lastFrameHeight = height;
        const uint8_t* shadowPlane = nullptr;
        _sceneryShadow = nullptr;
        if (_settings.shadowStrength > 0.0f && depth != nullptr && BuildObjectShadows(f, view))
        {
            shadowPlane = f.shadowRaw.data();
            _sceneryShadow = f.sceneryShadow.data();
            _shadowZ = f.shadowZ.data();
            _faceRotation = view.rotation;
            _shadowOriginY = static_cast<float>(view.originY);
            _shadowWpp = view.worldPerPixel;
            lap(kStageShadow);
        }

        _cloudPlanes = CloudPlanes{};
        _precip.mask = nullptr;
        if (!_precipTilesReady)
            BuildPrecipTiles();
        if (_settings.cloudCoverage > 0.0f || _settings.fog > 0.0f || (_settings.weatherEffects && WeatherHasPrecipitation())
            )
            BuildCloudPlanes(f, depth, f.uiMask.data(), view);
        lap(kStageCloud);

        // Ground shadows are part of the world image. Filtering them with the world avoids both crisp overlays
        // on blurred ground and their former disappearance as soon as the blur radius reached three pixels.
        std::vector<uint32_t>& grounded = f.groundColour;
        const bool leavesNow = _settings.leaves > 0.0f && !_still.active && depth != nullptr && sway != nullptr;
        const uint16_t* blurDepth = depth;
        if (!leavesNow)
            _leaves.clear();
        if (shadowPlane != nullptr || leavesNow || doWater)
        {
            grounded.resize(static_cast<size_t>(width) * height);
            Frame groundFrame = fr;
            groundFrame.out = grounded.data();
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                FinishRows<false, false>(groundFrame, nullptr, nullptr, nullptr, nullptr,
                    shadowPlane, f.halfWidth, f.halfHeight, y0, y1);
            });
            if (leavesNow)
            {
                // Leaves are world sprites with their own depth, not screen-space weather. Occlude them against
                // the original scene, then filter their colour with the world using their actual source depth.
                f.leafDepth.assign(depth, depth + static_cast<size_t>(width) * height);
                DrawLife(groundFrame, view, bits, sway, grounded.data(), f.leafDepth.data());
                blurDepth = f.leafDepth.data();
            }
            fr.groundColour = grounded.data();
            fr.depth = blurDepth;
        }

        lap(kStageGround);
        const uint16_t* vmap = vignette ? f.vignetteMap.data() : nullptr;
        const uint8_t* finalRadius = nullptr;
        float maxRadiusForDebug = 1.0f;
        // Rain and snow are a layer of their own. With the tilt-shift on the layer gets its own blur (a smooth
        // radius per screen row, independent of the depth of the pixels behind it, so flakes are never cut along the
        // outlines of objects) and is laid over the finished frame.
        const auto runPrecip = [&](uint32_t* dst, bool layer) {
            if (_precip.mask == nullptr)
                return;
            const float amountNow = std::min(1.5f, _settings.precipitation) * (0.55f + 0.45f * _precip.level);
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                PrecipRows(fr, f, amountNow, y0, y1, dst, layer);
            });
        };
        if (tilt)
        {
            uint32_t* A = f.bufA.data();

            // Focus depth: median along the focus row, lightly smoothed over time so passing buildings do not pump it.
            bool useDepth = false;
            float focus = 0.0f;
            if (depth != nullptr && _settings.tiltDepth)
            {
                float measured = 0.0f;
                if (MeasureFocusDepth(depth, width, height, measured))
                {
                    const float unitsPerRow = 2.0f * worldPerPixel;
                    if (!f.focusValid || f.lastWorldPerPixel != worldPerPixel
                        || std::abs(measured - f.focusSmoothed) > 12.0f * height * unitsPerRow)
                        f.focusSmoothed = measured;
                    else
                        f.focusSmoothed += (measured - f.focusSmoothed) * 0.3f;
                    f.focusValid = true;
                    f.lastWorldPerPixel = worldPerPixel;
                    focus = f.focusSmoothed;
                    useDepth = true;
                }
            }

            lap(kStageFocus);
            // Radius plane: raw -> horizontal smooth -> vertical smooth (hides tile steps and hard silhouettes).
            const int32_t smoothRadius = static_cast<int32_t>(5.0f * _settings.tiltSmooth * height / 1080.0f + 0.5f);
            maxRadiusForDebug = std::min(120.0f, 32.0f * _settings.tiltStrength * height / 1080.0f);
            if (useDepth)
                BuildRadiusLut(f, focus, worldPerPixel);
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                BuildRadiusRows(f, blurDepth, useDepth, f.radiusRaw.data(), y0, y1);
            });
            lap(kStageRadBuild);
            // Morphological closing (3x3 dilate then erode) fills isolated one/two pixel holes in the radius plane.
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                Morph3Rows(f.radiusRaw.data(), f.radiusSmooth.data(), width, true, y0, y1);
            });
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                Morph3RowsVertical(f.radiusSmooth.data(), f.radiusFinal.data(), width, height, true, y0, y1);
            });
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                Morph3Rows(f.radiusFinal.data(), f.radiusSmooth.data(), width, false, y0, y1);
            });
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                Morph3RowsVertical(f.radiusSmooth.data(), f.radiusRaw.data(), width, height, false, y0, y1);
            });
            lap(kStageRadClose);
            // Near-field blur: foreground blur spreads over sharper pixels behind it (e.g. through the gaps of a lattice).
            const bool smoothing = smoothRadius > 0;
            uint8_t* nearOut = smoothing ? f.radiusSmooth.data() : f.radiusFinal.data();
            if (useDepth && _settings.tiltNear && depth != nullptr)
            {
                pool.ParallelFor((f.cellsY + 7) / 8, [&](int32_t t) {
                    BuildCellsRows(f, f.radiusRaw.data(), blurDepth, t * 8, std::min(f.cellsY, t * 8 + 8));
                });
                lap(kStageRadNear);
                PropagateCells(f);
                lap(kStageRadSmooth);
                const int32_t marginStored = std::max(2, static_cast<int32_t>(4.0f * worldPerPixel));
                pool.ParallelFor(rowTasks, [&](int32_t t) {
                    int32_t y0, y1;
                    rowRange(t, y0, y1);
                    ApplyNearRows(f, f.radiusRaw.data(), blurDepth, marginStored, nearOut, y0, y1);
                });
                lap(kStageRadMax);
            }
            else
            {
                std::memcpy(nearOut, f.radiusRaw.data(), f.radiusRaw.size());
            }
            lap(kStageRadNear);
            if (smoothing)
            {
                pool.ParallelFor(rowTasks, [&](int32_t t) {
                    int32_t y0, y1;
                    rowRange(t, y0, y1);
                    SmoothRows(f.radiusSmooth.data(), f.radiusRaw.data(), width, smoothRadius, y0, y1);
                });
                pool.ParallelFor((width + kChunkWidth - 1) / kChunkWidth, [&](int32_t c) {
                    SmoothChunk(f.radiusRaw.data(), f.radiusFinal.data(), width, height, smoothRadius, c);
                });
            }
            lap(kStageRadSmooth);
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                for (int32_t y = y0; y < y1; y++)
                {
                    const uint8_t* r = f.radiusFinal.data() + static_cast<size_t>(y) * width;
                    f.rowMax[y] = *std::max_element(r, r + width);
                }
            });
            finalRadius = f.radiusFinal.data();
            lap(kStageRadMax);

            // Blur at half resolution: downsample, iterate box blurs with fractional radii, upsample and blend.
            const int32_t hw = f.halfWidth, hh = f.halfHeight;
            const int32_t halfRowTasks = (hh + kRowChunk - 1) / kRowChunk;
            const int32_t halfStripTasks = (hw + kStripWidth - 1) / kStripWidth;
            pool.ParallelFor(halfRowTasks, [&](int32_t t) {
                const int32_t y0 = t * kRowChunk, y1 = std::min(hh, y0 + kRowChunk);
                // This task owns exactly the full-resolution rows consumed by its half-resolution rows.
                ConvertRows(fr, f.radiusFinal.data(), A, y0 * 2, std::min(height, y1 * 2));
                DownsampleRows(fr, f, A, f.radiusFinal.data(), f.halfA.data(), f.radiusHalf.data(), y0, y1);
                for (int32_t y = y0; y < y1; y++)
                {
                    const uint8_t* r = f.radiusHalf.data() + static_cast<size_t>(y) * hw;
                    f.rowMaxHalf[y] = *std::max_element(r, r + hw);
                }
            });
            static const bool verifyBlur = std::getenv("ATMOS_QA_VERIFY_BLUR") != nullptr;
            if (verifyBlur) {
                std::vector<uint32_t> converted(static_cast<size_t>(width) * height), half(f.halfA.size());
                std::vector<uint8_t> radii(f.radiusHalf.size());
                ConvertRows(fr,f.radiusFinal.data(),converted.data(),0,height);
                DownsampleRows(fr,f,converted.data(),f.radiusFinal.data(),half.data(),radii.data(),0,hh);
                if(!std::equal(converted.begin(),converted.end(),A) || half!=f.halfA || radii!=f.radiusHalf)
                    throw std::runtime_error("Fused blur preparation differs from separate passes");
            }
            lap(kStageDown);
            for (int32_t it = 0; it < _settings.tiltIterations; it++)
            {
                pool.ParallelFor(halfRowTasks, [&](int32_t t) {
                    const int32_t y0 = t * kRowChunk, y1 = std::min(hh, y0 + kRowChunk);
                    BlurRows(hw, f.radiusHalf.data(), f.rowMaxHalf.data(), f.halfA.data(), f.halfB.data(), y0, y1);
                });
                lap(kStageBlurH);
                pool.ParallelFor(halfStripTasks, [&](int32_t st) {
                    BlurStrip(hw, hh, f.radiusHalf.data(), f.halfB.data(), f.halfA.data(), st);
                });
                lap(kStageBlurV);
            }
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                FinishRows<true,true,true>(fr, A, f.radiusFinal.data(), vmap, coverageLut, nullptr,
                    f.halfWidth, f.halfHeight, y0, y1, f.halfA.data());
            });
            if (verifyBlur) {
                std::vector<uint32_t> composite(A,A+static_cast<size_t>(width)*height), reference(composite.size());
                CompositeRows(fr,f,f.radiusFinal.data(),f.halfA.data(),composite.data(),0,height);
                Frame oracle=fr;oracle.out=reference.data();
                FinishRows<true>(oracle,composite.data(),f.radiusFinal.data(),vmap,coverageLut,nullptr,f.halfWidth,f.halfHeight,0,height);
                if(!std::equal(reference.begin(),reference.end(),out))
                    throw std::runtime_error("Fused blur finish differs from separate composite and finish passes");
            }
            lap(kStageFinish);
        }
        else
        {
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                FinishRows<false>(fr, nullptr, nullptr, vmap, coverageLut, nullptr, f.halfWidth, f.halfHeight, y0, y1);
            });
            lap(kStageFinish);
        }


        if ((_settings.waterWaves > 0.0f || (_settings.waterGlitter > 0.0f || _settings.waterDepth > 0.0f || _settings.waterPatches > 0.0f || _settings.waterSun > 0.0f)) && depth != nullptr)
        {
            const float waterTime = WaterClockNow();
            UpdateWaterZones(fr, view);
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                WaterRows(fr, depth, finalRadius, view, waterTime, y0, y1);
            });
        }

        lap(kStageWater);
        if (shadowPlane != nullptr && f.hasFaceShading && _settings.faceShading > 0.0f && depth != nullptr)
        {
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                FaceShadingRows(fr, f, depth, finalRadius, y0, y1);
            });
        }

        lap(kStageFaces);
        if (_settings.sunRays > 0.0f && !_still.active)
        {
            const float night = _time.active ? _time.night : 0.0f;
            const float rayStrength = _settings.sunRays * (1.0f - night) * (1.0f - _sunBlockByWeather)
                * Coordinates::RayDayStrength(EffectiveSunElevation());
            if (rayStrength > 0.01f)
            {
                RayStyle rayStyle;
                BuildRayStyle(rayStyle);
                BuildSunRays(f, fr, view, rayStyle, rayStrength);
                const float warm = std::clamp(1.0f - (EffectiveSunElevation() - 12.0f) / 40.0f, 0.0f, 1.0f);
                pool.ParallelFor(rowTasks, [&](int32_t t) {
                    int32_t y0, y1;
                    rowRange(t, y0, y1);
                    SunRayRows(fr, f, rayStrength * 2.6f, warm, y0, y1);
                });
            }
        }

        lap(kStageRays);
        // Light objects.
        if (ObjectLightsWanted())
        {
            RunLights(fr, view, pool);
            if (LightsEditingNow())
                DrawLightMarkers(fr, view);
            lap(kStageLights);
        }

        if (_settings.snowCover > 0.0f && _snow.maxValue > 0 && depth != nullptr)
        {
            const int32_t hh2 = f.halfHeight;
            pool.ParallelFor((hh2 + 15) / 16, [&](int32_t t) {
                SnowCoverRows(f, depth, f.uiMask.data(), view, f.snowPlane.data(), t * 16, std::min(hh2, t * 16 + 16));
            });
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                SnowRows(fr, f, depth, sway, worldPerPixel, _settings.snowCover, y0, y1);
            });
        }

        const bool layerWanted = tilt && f.radius.size() == static_cast<size_t>(height)
            && *std::max_element(f.radius.begin(), f.radius.end()) > 0 && _precip.mask != nullptr;
        static double lifeMs[4] = {}; static int lifeN = 0;
        const auto lifeT0 = Clock::now();
        if (layerWanted)
        {
            std::fill(f.lifeA.begin(), f.lifeA.end(), 0u);
            runPrecip(f.lifeA.data(), true);
            const auto lifeT1 = Clock::now();
            const auto lifeT2 = Clock::now();
            lifeMs[0] += std::chrono::duration<double, std::milli>(lifeT1 - lifeT0).count();
            lifeMs[1] += std::chrono::duration<double, std::milli>(lifeT2 - lifeT1).count();
            // radius per row in quarter pixels (full resolution), the same soft screen-row profile as the tilt-shift band
            for (int32_t y = 0; y < height; y++)
            {
                const uint8_t rq = static_cast<uint8_t>(std::min(255, f.radius[y] * 4));
                std::memset(f.lifeRadius.data() + static_cast<size_t>(y) * width, rq, static_cast<size_t>(width));
                f.lifeRowMax[y] = rq;
            }
            const int32_t stripTasks = (width + kStripWidth - 1) / kStripWidth;
            for (int32_t it = 0; it < 1; it++) // one box pass is enough for flakes and leaves: 1.2 ms instead of 2.5 ms at 1080p
            {
                pool.ParallelFor(rowTasks, [&](int32_t t) {
                    int32_t y0, y1;
                    rowRange(t, y0, y1);
                    BlurRows(width, f.lifeRadius.data(), f.lifeRowMax.data(), f.lifeA.data(), f.lifeB.data(), y0, y1);
                });
                pool.ParallelFor(stripTasks, [&](int32_t st) {
                    BlurStrip(width, height, f.lifeRadius.data(), f.lifeB.data(), f.lifeA.data(), st);
                });
            }
            const auto lifeT3 = Clock::now();
            lifeMs[2] += std::chrono::duration<double, std::milli>(lifeT3 - lifeT2).count();
            const uint32_t layerMask = (0xFFu << fr.rShift) | (0xFFu << fr.gShift) | (0xFFu << fr.bShift);
            pool.ParallelFor(rowTasks, [&](int32_t t) {
                int32_t y0, y1;
                rowRange(t, y0, y1);
                for (int32_t y = y0; y < y1; y++)
                {
                    const uint32_t* l = f.lifeA.data() + static_cast<size_t>(y) * width;
                    uint32_t* o = out + static_cast<size_t>(y) * width;
                    const uint8_t* ui = fr.ui + static_cast<size_t>(y) * width;
                    for (int32_t x = 0; x < width; x++)
                    {
                        const uint32_t cover = (l[x] >> fr.freeShift) & 0xFFu;
                        if (cover == 0 || ui[x])
                            continue;
                        const uint32_t keep = 256u - (cover + (cover >> 7));
                        const auto lane = [&](uint32_t shift) {
                            const uint32_t v = (((o[x] >> shift) & 0xFFu) * keep >> 8) + ((l[x] >> shift) & 0xFFu);
                            return std::min(255u, v) << shift;
                        };
                        o[x] = (o[x] & ~layerMask) | lane(fr.rShift) | lane(fr.gShift) | lane(fr.bShift);
                    }
                }
            });
        }
        else
            runPrecip(out, false);
        if (layerWanted)
        {
            lifeMs[3] += std::chrono::duration<double, std::milli>(Clock::now() - lifeT0).count();
            if (++lifeN >= 300 && _settings.devPerfLog)
            {
                std::ofstream lg(_settingsPath + ".life.log", std::ios::app);
                lg << "life avg ms: precip " << lifeMs[0] / lifeN << " leaves " << lifeMs[1] / lifeN << " blur " << lifeMs[2] / lifeN << " total " << lifeMs[3] / lifeN << std::endl;
                lifeMs[0] = lifeMs[1] = lifeMs[2] = lifeMs[3] = 0;
                lifeN = 0;
            }
        }

        lap(kStagePrecip);
        const double lightningSeconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        const bool thunder = !_still.active && _settings.lightningStrength > 0.0f
            && !Config::Get().general.disableLightningEffect
            && (getGameState().weatherCurrent.weatherType == Weather::Type::thunder
                || getGameState().weatherCurrent.weatherType == Weather::Type::heavyRain);
        float flash = UpdateLightning(_lightning, lightningSeconds, getGameState().currentTicks,
            thunder, _settings.lightningInterval) * _settings.lightningStrength * 1.1f;
        if (_settings.devLightningHold && thunder)
            flash = 0.8f * _settings.lightningStrength * 1.1f;
        pool.ParallelFor(rowTasks, [&](int32_t t) {
            int32_t y0, y1;
            rowRange(t, y0, y1);
            LightningRows(fr, flash, _lightning.boltSeed, y0, y1);
        });
        if (flash > 0.02f)
            DrawLightningBolt(fr, _lightning.boltSeed, flash * 1.6f);
        lap(kStageLightning);


        if (_settings.debugDepth == 4 && depth != nullptr)
        {
            // Heights rebuilt from depth and screen position (z = D / 2 - sy): flat ground must show one constant grey.
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    const size_t i = static_cast<size_t>(y) * width + x;
                    if (f.uiMask[i] || depth[i] == 0xFFFF)
                        continue;
                    const float D = static_cast<float>((static_cast<int32_t>(depth[i]) - 32768) * 2);
                    const float sy = (static_cast<float>(y) + static_cast<float>(view.originY)) * worldPerPixel;
                    const float z = D * 0.5f - sy;
                    const uint32_t g = static_cast<uint32_t>(std::clamp(z * 2.0f + 40.0f, 0.0f, 255.0f));
                    out[i] = (g << fr.rShift) | (g << fr.gShift) | (g << fr.bShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 5 && sway != nullptr)
        {
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    const uint32_t g = static_cast<uint32_t>(sway[static_cast<size_t>(y) * width + x] >> 4) * 17u;
                    out[static_cast<size_t>(y) * width + x] = (g << fr.rShift) | (g << fr.gShift) | (g << fr.bShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 7 && depth != nullptr)
        {
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    const size_t index = static_cast<size_t>(y) * width + x;
                    const uint16_t here = depth[index], below = depth[static_cast<size_t>(std::min(y + 1, height - 1)) * width + x];
                    uint32_t rr = 0, gg = 0, bb = 0;
                    if (here == 0xFFFF)
                        bb = 255;
                    else if (below != 0xFFFF)
                    {
                        const int32_t delta = static_cast<int32_t>(below) - static_cast<int32_t>(here);
                        if (delta >= 1 && delta <= static_cast<int32_t>(2.0f * worldPerPixel + 1.0f))
                            rr = 255;
                        else if (delta == 0)
                            gg = 255;
                        else
                            gg = rr = 120;
                    }
                    out[index] = (rr << fr.rShift) | (gg << fr.gShift) | (bb << fr.bShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 6 && fr.shadowFlags != nullptr)
        {
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    const uint8_t fl = fr.shadowFlags[static_cast<size_t>(y) * width + x];
                    const uint32_t rr = (fl & 2) ? 255u : 0u, gg = (fl & 1) ? 255u : 0u;
                    out[static_cast<size_t>(y) * width + x] = (rr << fr.rShift) | (gg << fr.gShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 3 && shadowPlane != nullptr)
        {
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    if (f.uiMask[static_cast<size_t>(y) * width + x])
                        continue;
                    const uint32_t g = shadowPlane[static_cast<size_t>(y >> 1) * f.halfWidth + (x >> 1)];
                    out[static_cast<size_t>(y) * width + x] = (g << fr.rShift) | (g << fr.gShift) | (g << fr.bShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 8 && _water.cellsX > 0)
        {
            for (int32_t y = 0; y < height; y++)
                for (int32_t x = 0; x < width; x++)
                {
                    const uint32_t g = _water.smooth[static_cast<size_t>(y / 8) * _water.cellsX + (x / 8)];
                    out[static_cast<size_t>(y) * width + x] = (g << fr.rShift) | (g << fr.gShift) | (g << fr.bShift) | (0xFFu << fr.freeShift);
                }
        }
        else if (_settings.debugDepth == 2 && finalRadius != nullptr)
            DebugRadiusView(fr, finalRadius, maxRadiusForDebug);
        else if (_settings.debugDepth >= 1 && depth != nullptr)
            DebugDepthView(fr, depth, worldPerPixel);

        if (_settings.debugMask)
        {
            const uint32_t red = 0xFFu << layout.redShift;
            for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
            {
                if (f.uiMask[i])
                    out[i] |= red;
            }
        }

        _accumMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (++_accumFrames >= 300)
        {
            std::ofstream perf;
            if (_settings.devPerfLog)
                perf.open(_settingsPath + ".perf.log", std::ios::app);
            if (perf)
                perf << "avg " << (_accumMs / _accumFrames) << " ms/frame over " << _accumFrames << " frames (" << width << "x"
                     << height << ", tilt=" << (tilt ? 1 : 0) << ", vignette=" << (vignette ? 1 : 0) << ")";
            if (perf)
            {
                perf << "  stages(ms):";
                for (int i = 0; i < kStageCount; i++)
                    perf << " " << kStageNames[i] << "=" << (_stageMs[i] / _accumFrames);
                perf << "\n";
            }
            std::fill(std::begin(_stageMs), std::end(_stageMs), 0.0);
            _accumMs = 0;
            _accumFrames = 0;
        }
    }
} // namespace OpenRCT2::Ui::Atmosphere
