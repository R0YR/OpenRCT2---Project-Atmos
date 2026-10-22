/*****************************************************************************
 * "Atmosphere" page of the Options window. See OptionsAtmosphere.h.
 *****************************************************************************/

#include "OptionsAtmosphere.h"

#include "../drawing/Atmosphere.h"
#include "../drawing/AtmosphereLightPlacement.h"

#include <openrct2-ui/interface/Dropdown.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/Context.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/world/Map.h>
#include <openrct2/Input.h>
#include <openrct2/ui/UiContext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <openrct2/config/Config.h>
#include <openrct2/drawing/ColourMap.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/drawing/Rectangle.h>
#include <openrct2/drawing/Text.h>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;

namespace OpenRCT2::Ui::Windows
{
    using Atmosphere::Settings;

    namespace
    {
        enum SubPage : int32_t
        {
            kSubEffects = 0,
            kSubLight = 1,
            kSubTime = 2,
            kSubClouds = 3,
            kSubWeather = 4,
            kSubColour = 5,
            kSubLuts = 6,
            kSubCinema = 7,
            kSubLights = 8,
            kSubWater = 9,
            kSubPageCount
        };

        constexpr const char* kPageNames[kSubPageCount] = {
            "Effects", "Sun", "Time", "Sky", "Weather", "Colour", "LUTs", "Cinema", "Lights", "Water"
        };
        constexpr Colour kPageColours[kSubPageCount] = {
            Colour::mossGreen, Colour::yellow, Colour::lightOrange, Colour::icyBlue, Colour::lightPurple,
            Colour::salmonPink, Colour::brightPurple, Colour::white, Colour::brightYellow, Colour::lightWater
        };
        constexpr const char* kPageTips[kSubPageCount] = {
            "Overall look, presets and effect strength", "Sunlight, shadows and sun rays", "Time of day and day cycle",
            "Cloud coverage, height and fog", "Rain, snow, lightning and wind", "Exposure, contrast and colour grading",
            "Load and save colour looks", "Camera and cinematic presentation", "Place and edit park lights", "Water appearance"
        };
        // Compact 7 x 7 monochrome icons: sparkle, sun, clock, cloud, storm, palette, stack, film, bulb, waves.
        constexpr uint8_t kPageIcons[kSubPageCount][7] = {
            { 8, 8, 42, 28, 42, 8, 8 }, { 8, 42, 28, 127, 28, 42, 8 },
            { 28, 34, 73, 77, 65, 34, 28 }, { 0, 12, 30, 59, 127, 127, 0 },
            { 12, 30, 63, 127, 8, 24, 16 }, { 28, 62, 85, 127, 85, 60, 24 },
            { 62, 34, 62, 0, 62, 0, 62 }, { 127, 85, 127, 65, 127, 85, 127 },
            { 28, 34, 34, 28, 28, 28, 8 }, { 0, 50, 76, 0, 50, 76, 0 }
        };

        enum class Format
        {
            Percent,       // value * 100 followed by %
            SignedPercent, // -100 .. +100
            ExposureEv,    // +1.25 EV
            Number2,       // 1.00
            Passes,        // integer count
            Degrees,       // 260 deg
            Units,         // plain integer
            Hour,          // 18:30
        };

        struct SliderDef
        {
            const char* name;
            float& (*ref)(Settings&);
            int32_t& (*intRef)(Settings&);
            float minimum;
            float maximum;
            Format format;
        };

#define FLOAT_REF(expr) +[](Settings& s) -> float& { return expr; }
#define INT_REF(expr) +[](Settings& s) -> int32_t& { return expr; }

        // clang-format off
        const SliderDef kEffectSliders[] = {
            { "Strength",       FLOAT_REF(s.vignetteStrength), nullptr,                       0.0f,  1.0f,  Format::Percent },
            { "Start radius",   FLOAT_REF(s.vignetteInner),    nullptr,                       0.0f,  0.95f, Format::Percent },
            { "Strength",       FLOAT_REF(s.tiltStrength),     nullptr,                       0.0f,  1.0f,  Format::Percent },
            { "Focus position", FLOAT_REF(s.tiltFocus),        nullptr,                       0.0f,  1.0f,  Format::Percent },
            { "Sharp band",     FLOAT_REF(s.tiltBand),         nullptr,                       0.0f,  0.5f,  Format::Percent },
            { "Blur falloff",   FLOAT_REF(s.tiltFalloff),      nullptr,                       0.02f, 1.0f,  Format::Percent },
            { "Smoothness",     nullptr,                       INT_REF(s.tiltIterations),     1.0f,  3.0f,  Format::Passes  },
        };
        const SliderDef kLightSliders[] = {
            { "Sunlight",       FLOAT_REF(s.sunlight),       nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Sun warmth",     FLOAT_REF(s.sunWarmth),      nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Shadows",        FLOAT_REF(s.shadowStrength), nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Shadow softness", FLOAT_REF(s.shadowSoftness), nullptr, 0.0f, 1.0f, Format::Percent },
            { "Sun direction",  FLOAT_REF(s.sunAzimuth),     nullptr, 0.0f,  360.0f, Format::Degrees },
            { "Sun height",     FLOAT_REF(s.sunElevation),   nullptr, 15.0f, 80.0f,  Format::Degrees },
            { "Shadow length",  FLOAT_REF(s.shadowReach),    nullptr, 20.0f, 300.0f, Format::Units   },
            { "Sun rays",       FLOAT_REF(s.sunRays),        nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Guest range",    FLOAT_REF(s.shadowGuestDistance), nullptr, 0.0f, 256.0f, Format::Units },
            { "Time of day",    FLOAT_REF(s.timeHour),       nullptr, 0.0f,  24.0f,  Format::Hour    },
            { "Day length (min)", FLOAT_REF(s.timeCycleMinutes), nullptr, 1.0f, 60.0f, Format::Units },
            { "Night darkness", FLOAT_REF(s.nightDarkness),  nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Light objects",  FLOAT_REF(s.lightsStrength), nullptr, 0.0f, 1.5f, Format::Percent },
        };
        constexpr int32_t kSunSliderCount = 9;
        static_assert(kSunSliderCount <= 12, "A page supports at most 12 slider scroll areas");
        const SliderDef kLightObjSliders[] = {
            { "Height",         FLOAT_REF(s.editHeight),     nullptr, 0.0f,  640.0f, Format::Units   },
            { "Radius",         FLOAT_REF(s.editRadius),     nullptr, 24.0f, 400.0f, Format::Units   },
            { "Intensity",      FLOAT_REF(s.editIntensity),  nullptr, 0.0f,  2.0f,   Format::Percent },
            { "Red",            FLOAT_REF(s.editR),          nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Green",          FLOAT_REF(s.editG),          nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Blue",           FLOAT_REF(s.editB),          nullptr, 0.0f,  1.0f,   Format::Percent },
            { "Animation speed", FLOAT_REF(s.editSpeed),     nullptr, 0.1f,  4.0f,   Format::Number2 },
        };
        const SliderDef kCinemaSliders[] = {
            { "Bar height",       FLOAT_REF(s.cinemaBarHeight),   nullptr, 0.03f, 0.3f, Format::Percent },
            { "Camera smoothing", FLOAT_REF(s.cinemaSmoothness),  nullptr, 0.0f,  1.0f, Format::Percent },
        };
        const SliderDef kCloudSliders[] = {
            { "Clouds", FLOAT_REF(s.cloudCoverage), nullptr, 0.0f, 1.0f, Format::Percent },
            { "Cloud size",     FLOAT_REF(s.cloudSize),     nullptr, 600.0f, 8000.0f, Format::Units   },
            { "Cloud height",   FLOAT_REF(s.cloudHeight),   nullptr, 100.0f, 2000.0f, Format::Units   },
            { "Cloud speed",    FLOAT_REF(s.cloudSpeed),    nullptr, 0.0f,   4.0f,    Format::Number2 },
            { "Wind direction", FLOAT_REF(s.cloudWind),     nullptr, 0.0f,   360.0f,  Format::Degrees },
            { "Cloud opacity",  FLOAT_REF(s.cloudOpacity),  nullptr, 0.0f,   1.0f,    Format::Percent },
            { "Cloud shadows",  FLOAT_REF(s.cloudShadow),   nullptr, 0.0f,   1.0f,    Format::Percent },
            { "Clear centre",   FLOAT_REF(s.cloudClear),    nullptr, 0.0f,   1.0f,    Format::Percent },
            { "Fog",            FLOAT_REF(s.fog),           nullptr, 0.0f,   1.0f,    Format::Percent },
        };
        const SliderDef kWaterSliders[] = {
            { "Water waves",    FLOAT_REF(s.waterWaves),    nullptr, 0.0f, 1.0f, Format::Percent },
            { "Water glitter",  FLOAT_REF(s.waterGlitter),  nullptr, 0.0f, 1.0f, Format::Percent },
            { "Water depth",    FLOAT_REF(s.waterDepth),    nullptr, 0.0f, 1.0f, Format::Percent },
            { "Water patches",  FLOAT_REF(s.waterPatches),  nullptr, 0.0f, 1.0f, Format::Percent },
            { "Sun sparkle",    FLOAT_REF(s.waterSun),      nullptr, 0.0f, 1.0f, Format::Percent },
        };
        const SliderDef kWeatherSliders[] = {
            { "Rain and snow",  FLOAT_REF(s.precipitation), nullptr, 0.0f, 1.5f, Format::Percent },
            { "Wind strength",  FLOAT_REF(s.windStrength),  nullptr, 0.0f, 1.0f, Format::Percent },
            { "Tree sway",      FLOAT_REF(s.swayAmount),    nullptr, 0.0f, 1.0f, Format::Percent },
            { "Falling leaves", FLOAT_REF(s.leaves),        nullptr, 0.0f, 1.0f, Format::Percent },
            { "Lightning",      FLOAT_REF(s.lightningStrength), nullptr, 0.0f, 1.0f, Format::Percent },
            { "Flash interval (s)", FLOAT_REF(s.lightningInterval), nullptr, 5.0f, 90.0f, Format::Units },
        };
        const SliderDef kColourSliders[] = {
            { "Exposure",       FLOAT_REF(s.grade.exposure),    nullptr, -3.0f, 3.0f, Format::ExposureEv    },
            { "Contrast",       FLOAT_REF(s.grade.contrast),    nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Saturation",     FLOAT_REF(s.grade.saturation),  nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Vibrance",       FLOAT_REF(s.grade.vibrance),    nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Temperature",    FLOAT_REF(s.grade.temperature), nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Tint",           FLOAT_REF(s.grade.tint),        nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Highlights",     FLOAT_REF(s.grade.highlights),  nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Shadows",        FLOAT_REF(s.grade.shadows),     nullptr, -1.0f, 1.0f, Format::SignedPercent },
            { "Gamma",          FLOAT_REF(s.grade.gamma),       nullptr,  0.5f, 2.0f, Format::Number2       },
        };
        // clang-format on
#undef FLOAT_REF
#undef INT_REF

        constexpr int32_t kVignetteSliders = 2;

        struct CheckboxDef
        {
            const char* name;
            bool& (*ref)(Settings&);
        };

        const CheckboxDef kEffectCheckboxes[] = {
            { "Use scene depth", +[](Settings& s) -> bool& { return s.tiltDepth; } },
            { "Blur spreads over sharp pixels behind it", +[](Settings& s) -> bool& { return s.tiltNear; } },
            { "Follow the weather of the game", +[](Settings& s) -> bool& { return s.cloudWeather; } },
            { "Keep the screen centre free of clouds (focus)", +[](Settings& s) -> bool& { return s.cloudFocus; } },
            { "Draw rain and snow (only under clouds)", +[](Settings& s) -> bool& { return s.weatherEffects; } },
            { "Sway trees and foliage in the wind", +[](Settings& s) -> bool& { return s.swayTrees; } },
            { "Time of day (light, dusk, night)", +[](Settings& s) -> bool& { return s.timeEnabled; } },
            { "Let the day pass with the game", +[](Settings& s) -> bool& { return s.timeCycle; } },
            { "Cinema mode on (Esc leaves it)", +[](Settings& s) -> bool& { return s.cinemaActive; } },
            { "Black bars at the top and bottom", +[](Settings& s) -> bool& { return s.cinemaBars; } },
            { "Smooth camera (gentle start and stop)", +[](Settings& s) -> bool& { return s.cinemaSmooth; } },
            { "Show light objects", +[](Settings& s) -> bool& { return s.lightsEnabled; } },
        };
        constexpr int32_t kLightsBoxIndex = 11;
        constexpr int32_t kEffectBoxCount = 2; // the checkboxes after these belong to other pages
        constexpr int32_t kCloudBoxIndex = 2;
        constexpr int32_t kWeatherBoxIndex = 4; // two checkboxes: 4 and 5
        constexpr int32_t kTimeBoxIndex = 6;    // two checkboxes: 6 and 7
        constexpr int32_t kCinemaBoxIndex = 8;  // three checkboxes: 8, 9 and 10

        constexpr int32_t kRowHeight = 17;
        constexpr int32_t kLabelWidth = 126;
        constexpr int32_t kSliderX = 142;
        constexpr int32_t kSliderWidth = 156;

        const SliderDef* SlidersOf(int32_t subPage, int32_t& count)
        {
            if (subPage == kSubLuts)
            {
                count = 0;
                return nullptr;
            }
            if (subPage == kSubWater)
            {
                count = static_cast<int32_t>(std::size(kWaterSliders));
                return kWaterSliders;
            }
            if (subPage == kSubLights)
            {
                count = static_cast<int32_t>(std::size(kLightObjSliders));
                return kLightObjSliders;
            }
            if (subPage == kSubColour)
            {
                count = static_cast<int32_t>(std::size(kColourSliders));
                return kColourSliders;
            }
            // The first kSunSliderCount entries of kLightSliders belong to the Light page, the rest to the Time page
            // (a page can hold at most 12 sliders: the window has 12 scroll areas).
            if (subPage == kSubLight)
            {
                count = kSunSliderCount;
                return kLightSliders;
            }
            if (subPage == kSubTime)
            {
                count = static_cast<int32_t>(std::size(kLightSliders)) - kSunSliderCount;
                return kLightSliders + kSunSliderCount;
            }
            if (subPage == kSubCinema)
            {
                count = static_cast<int32_t>(std::size(kCinemaSliders));
                return kCinemaSliders;
            }
            if (subPage == kSubWeather)
            {
                count = static_cast<int32_t>(std::size(kWeatherSliders));
                return kWeatherSliders;
            }
            if (subPage == kSubClouds)
            {
                count = static_cast<int32_t>(std::size(kCloudSliders));
                return kCloudSliders;
            }
            count = static_cast<int32_t>(std::size(kEffectSliders));
            return kEffectSliders;
        }

        float GetValue(const SliderDef& def)
        {
            Settings& s = Atmosphere::GetSettings();
            return def.ref != nullptr ? def.ref(s) : static_cast<float>(def.intRef(s));
        }

        void SetValue(const SliderDef& def, float value)
        {
            Settings& s = Atmosphere::GetSettings();
            value = std::clamp(value, def.minimum, def.maximum);
            if (def.ref != nullptr)
                def.ref(s) = value;
            else
                def.intRef(s) = static_cast<int32_t>(std::lround(value));
        }

        int32_t ToPercent(const SliderDef& def, float value)
        {
            return static_cast<int32_t>(
                std::lround(std::clamp((value - def.minimum) / (def.maximum - def.minimum), 0.0f, 1.0f) * 100.0f));
        }

        float FromPercent(const SliderDef& def, int32_t percent)
        {
            // Land exactly on the neutral position when the bar sits (almost) in the middle of a signed range.
            float value = def.minimum + static_cast<float>(percent) * 0.01f * (def.maximum - def.minimum);
            if (def.minimum < 0.0f && std::abs(value) < 0.006f * (def.maximum - def.minimum))
                value = 0.0f;
            return value;
        }

        void FormatValue(char* buffer, size_t size, const SliderDef& def, float value)
        {
            switch (def.format)
            {
                case Format::Percent:
                    std::snprintf(buffer, size, "%s: %d%%", def.name, static_cast<int32_t>(std::lround(value * 100.0f)));
                    break;
                case Format::SignedPercent:
                    std::snprintf(buffer, size, "%s: %+d", def.name, static_cast<int32_t>(std::lround(value * 100.0f)));
                    break;
                case Format::ExposureEv:
                    std::snprintf(buffer, size, "%s: %+.2f EV", def.name, static_cast<double>(value));
                    break;
                case Format::Number2:
                    std::snprintf(buffer, size, "%s: %.2f", def.name, static_cast<double>(value));
                    break;
                case Format::Passes:
                    std::snprintf(buffer, size, "%s: %d", def.name, static_cast<int32_t>(std::lround(value)));
                    break;
                case Format::Degrees:
                    std::snprintf(buffer, size, "%s: %d deg", def.name, static_cast<int32_t>(std::lround(value)));
                    break;
                case Format::Units:
                    std::snprintf(buffer, size, "%s: %d", def.name, static_cast<int32_t>(std::lround(value)));
                    break;
                case Format::Hour:
                {
                    const int32_t minutes = static_cast<int32_t>(std::lround(value * 60.0f)) % (24 * 60);
                    std::snprintf(buffer, size, "%s: %02d:%02d", def.name, minutes / 60, minutes % 60);
                    break;
                }
            }
        }

        int32_t ScrollPercent(const Widget& widget, const ScrollArea& scroll)
        {
            const int32_t usable = scroll.contentWidth - (widget.width() - 2);
            if (usable <= 0)
                return -1; // scroll area not laid out yet
            return std::clamp(
                static_cast<int32_t>(std::lround(static_cast<float>(scroll.contentOffsetX) / static_cast<float>(usable) * 100.0f)),
                0, 100);
        }

        void SetScrollPercent(Window& w, WidgetIndex widgetIndex, int32_t scrollIndex, int32_t percent)
        {
            const auto& widget = w.widgets[widgetIndex];
            auto& scroll = w.scrolls[scrollIndex];
            const int32_t usable = scroll.contentWidth - (widget.width() - 2);
            if (usable <= 0)
                return;
            scroll.contentOffsetX = static_cast<int32_t>(std::ceil(static_cast<float>(percent) * 0.01f * static_cast<float>(usable)));
            widgetScrollUpdateThumbs(w, widgetIndex);
        }

        // Results of the "save LUT" text input: the callback can run after the window was closed, so it only touches these.
        bool _lutsDirty = true;
        std::string _lutStatus;

        std::string NameOf(const std::string& activeLut)
        {
            return activeLut.empty() ? std::string("") : activeLut;
        }

        bool EngineSupported()
        {
            return Config::Get().general.drawingEngine == DrawingEngine::softwareWithHardwareDisplay;
        }
    } // namespace

    const char* AtmosphereOptionsPage::TooltipFor(const char* name)
    {
        struct Tip
        {
            const char* name;
            const char* text;
        };
        static const Tip kTips[] = {
            { "Sun on walls", "Walls that face the sun get brighter, walls that face away get darker" },
            { "Shadow softness", "Softens projected sprite silhouettes without making them darker; paths never cast shadows" },
            { "Object lights", "Master brightness of lamps and vehicle lights. 0 % turns them off" },
            { "Day light strength", "Visibility of object lights in daylight, independent of their night brightness" },
            { "Night light strength", "Visibility of object lights at night and in dark weather" },
            { "Headlights", "Forward beams from supported moving vehicles. 0 % turns them off" },
            { "Tail lights", "Red rear lights on moving vehicles and the back of trains. 0 % turns them off" },
            { "Light radius", "Size of lamp halos and headlight pools in map units" },
            { "Lightning", "Short scene flashes during thunderstorms only. 0 % turns them off" },
            { "Flash interval (s)", "Average time between thunderstorm flashes; normal rain never flashes" },
            { "Sunlight", "Brightens world colours while preserving highlight detail. 0 % keeps the original brightness" },
            { "Sun warmth", "Adds warm daylight colour; night lighting is controlled by Time of day" },
            { "Cloud height", "Altitude above the map; sets cloud parallax and distance to cloud shadows" },
            { "Strength", "How strong the effect is. 0 % switches it off" },
            { "Start radius", "How far from the centre the dark corners begin" },
            { "Focus position", "Where the sharp zone sits (0 % top, 100 % bottom)" },
            { "Sharp band", "Width of the sharp zone" },
            { "Blur falloff", "How quickly things get blurry outside the sharp zone" },
            { "Smoothness", "How many blur passes are used. More looks softer and costs more speed" },
            { "Shadows", "Shadows of buildings and trees on the ground. 0 % switches them off" },
            { "Sun direction", "Where the sun is. Also decides where cloud shadows fall (ignored while time of day is on)" },
            { "Sun height", "How high the sun stands (ignored while time of day is on)" },
            { "Shadow length", "How far shadows reach at most" },
            { "Guest range", "Visitor and staff shadow distance from the view centre, in tiles. Fades through the outer 20%. 0 keeps unlimited distance and full quality at every zoom" },
            { "Vehicle range", "Vehicle shadow distance from the view centre, in tiles. Fades through the outer 20%. 0 keeps unlimited distance and full quality at every zoom" },
            { "Track range", "Track shadow distance from the view centre, in tiles. Fades through the outer 20%. 0 keeps unlimited distance and full quality at every zoom" },
            { "Time of day", "The hour of the day: dawn and dusk are warm, the night is dark and blue" },
            { "Day length (min)", "How many real minutes one day lasts when the day passes with the game" },
            { "Night darkness", "How dark the night gets" },
            { "Lamp glow", "Soft glow around lamps and lit windows at night" },
            { "Clouds", "Cloud coverage: weather stays below 60 %. Raise above 60 % for a denser sky; 100 % explicitly fills the sky" },
            { "Cloud size", "Size of the largest clouds" },
            { "Cloud speed", "How fast the clouds drift" },
            { "Wind direction", "The direction the wind blows (clouds, rain, snow)" },
            { "Cloud opacity", "How solid the clouds look" },
            { "Cloud shadows", "How dark the ground gets below the clouds" },
            { "Clear centre", "Keeps the middle of the screen free of clouds so you can see your park" },
            { "Fog", "Fog near the ground" },
            { "Bar height", "Height of each black bar in cinema mode" },
            { "Camera smoothing", "Higher = the camera glides slower and stops more gently" },
            { "Rain and snow", "Amount of rain and snow. They only fall under clouds" },
            { "Wind strength", "Slants rain and snow and bends the trees" },
            { "Snow on the ground", "How much snow stays on the ground and in the trees. 0 % switches it off" },
            { "Tree sway", "How much trees and plants move in the wind" },
            { "Water waves", "How strongly the surface of the water moves in swells; the light follows the waves" },
            { "Falling leaves", "How often leaves loosen from the trees and drift down" },
            { "Water glitter", "Random sparkles on the water" },
            { "Water depth", "Shallow water near the shore is light, deep water far from it is dark and blue" },
            { "Water patches", "Large soft light and dark patches that drift slowly over the water" },
            { "Sun sparkle", "More sparkles on the side of the picture where the sun is" },
            { "Exposure", "Overall brightness" },
            { "Contrast", "Difference between light and dark" },
            { "Saturation", "Colour intensity" },
            { "Vibrance", "Boosts muted colours more than strong ones" },
            { "Temperature", "Cool (blue) to warm (orange)" },
            { "Tint", "Green to magenta" },
            { "Highlights", "Brightness of the light areas only" },
            { "Gamma", "Brightness of the mid tones" },
        };
        static const struct { const char* name; const char* text; } kLightTips[] = {
            { "Height", "How high the light hangs above the ground" },
            { "Radius", "How far the light reaches" },
            { "Intensity", "Brightness of the selected light" },
            { "Red", "Red part of the light colour" },
            { "Green", "Green part of the light colour" },
            { "Blue", "Blue part of the light colour" },
            { "Animation speed", "How fast the animation runs" },
            { "Sun rays", "Light shafts between the buildings. They stop behind tall buildings and fade in clouds and rain. 0 % switches them off" },
            { "Light objects", "Master brightness of all placed lights. 0 % hides them" },
        };
        for (const auto& tip : kLightTips)
            if (std::strcmp(tip.name, name) == 0)
                return tip.text;
        for (const auto& tip : kTips)
            if (std::strcmp(tip.name, name) == 0)
                return tip.text;
        return nullptr;
    }

    AtmosphereOptionsPage::AtmosphereOptionsPage()
    {
        // Make sure the settings are loaded even before the first frame was rendered with the effects.
        Atmosphere::SetSettingsFile(Atmosphere::DefaultSettingsFile().c_str());
    }

    char* AtmosphereOptionsPage::NewText(const char* initial)
    {
        auto& slot = _texts[std::min(_textCount, kMaxTexts - 1)];
        _textCount++;
        std::snprintf(slot.data(), slot.size(), "%s", initial);
        return slot.data();
    }

    bool AtmosphereOptionsPage::ConsumeRebuild()
    {
        const bool value = _rebuild;
        _rebuild = false;
        return value;
    }

    void AtmosphereOptionsPage::AppendWidgets(std::vector<Widget>& list)
    {
        const size_t first = list.size();
        AppendWidgetsInner(list);
        if (_sideLayout)
        {
            // everything after the sub page buttons moves to the right of the button column
            constexpr int32_t kShift = 108;
            for (size_t i = first + kSubPageCount; i < list.size(); i++)
            {
                list[i].left += kShift;
                list[i].right += kShift;
            }
        }
    }

    void AtmosphereOptionsPage::AppendWidgetsInner(std::vector<Widget>& list)
    {
        _sliders.clear();
        _checkboxes.clear();
        _textCount = 0;

        auto add = [&](Widget widget) {
            list.push_back(widget);
            return static_cast<WidgetIndex>(list.size() - 1);
        };

        int32_t y = 48;

        // Sub page selector
        {
            // order of the buttons: Water sits next to Weather
            const int32_t order[kSubPageCount] = { kSubEffects, kSubLight, kSubTime, kSubClouds, kSubWeather, kSubWater, kSubColour, kSubLuts, kSubCinema, kSubLights };
            if (_sideLayout)
            {
                for (int32_t i = 0; i < kSubPageCount; i++)
                {
                    const int32_t page = order[i];
                    Widget tab = makeWidget({ 6, 22 + i * 22 + (i >= 1 ? 7 : 0) + (i >= 6 ? 7 : 0) + (i >= 9 ? 7 : 0) }, { 96, 20 }, WidgetType::button, WindowColour::secondary);
                    tab.setString(NewText(kPageNames[page]));
                    tab.setTooltip(kPageTips[page]);
                    _subTabs[page] = add(tab);
                }
                y = 24;
            }
            else
            {
                const int32_t tabWidths[kSubPageCount] = { 38, 30, 28, 34, 42, 36, 28, 38, 40, 36 };
                int32_t tabX = 8;
                for (int32_t i = 0; i < kSubPageCount; i++)
                {
                    Widget tab = makeWidget({ tabX, y }, { tabWidths[order[i]], 14 }, WidgetType::button, WindowColour::secondary);
                    tabX += tabWidths[order[i]] + 1;
                    tab.setString(NewText(kPageNames[order[i]]));
                    tab.setTooltip(kPageTips[order[i]]);
                    _subTabs[order[i]] = add(tab);
                }
                y += 18;
            }
        }

        // Master switch and a hint that is only visible when the active drawing engine cannot run the effects.
        Widget enable = makeWidget({ 10, y }, { 290, 12 }, WidgetType::checkbox, WindowColour::tertiary);
        enable.setString(NewText("Enable Atmosphere effects"));
        _enable = add(enable);
        y += 14;
        Widget hint = makeWidget({ 12, y }, { 286, 12 }, WidgetType::label, WindowColour::secondary);
        hint.setString(NewText("Needs drawing engine: Software (hardware display)"));
        _hint = add(hint);
        y += 14;

        auto addSlider = [&](int32_t definition, int32_t rowTop, const char* name) {
            Widget label = makeWidget({ 14, rowTop }, { kLabelWidth, 12 }, WidgetType::label, WindowColour::secondary);
            char* labelText = NewText(name);
            label.setString(labelText);
            const char* tip = TooltipFor(name);
            if (tip != nullptr)
                label.setTooltip(tip);
            const WidgetIndex labelIndex = add(label);
            Widget scroll = makeWidget(
                { kSliderX, rowTop - 1 }, { kSliderWidth, 13 }, WidgetType::scroll, WindowColour::secondary, SCROLL_HORIZONTAL);
            if (tip != nullptr)
                scroll.setTooltip(tip);
            const WidgetIndex scrollIndex = add(scroll);
            _sliders.push_back({ definition, labelIndex, scrollIndex, -1, labelText });
        };

        auto addGroup = [&](int32_t top, int32_t height, const char* title) {
            Widget group = makeWidget({ 5, top }, { 300, height }, WidgetType::groupbox, WindowColour::secondary);
            group.setString(NewText(title));
            add(group);
        };

        if (_subPage == kSubEffects)
        {
            // complete looks
            Widget caption = makeWidget({ 14, y + 1 }, { 60, 12 }, WidgetType::label, WindowColour::secondary);
            caption.setString(NewText("Look:"));
            caption.setTooltip("Sets many settings at once. You can change everything afterwards");
            add(caption);
            Widget presetBox = makeWidget({ 60, y }, { 240, 14 }, WidgetType::dropdownMenu, WindowColour::secondary);
            _presetText = NewText("Choose a look...");
            presetBox.setString(_presetText);
            _presetDropdown = add(presetBox);
            Widget presetGlyph = makeWidget({ 288, y + 1 }, { 11, 12 }, WidgetType::button, WindowColour::secondary, STR_DROPDOWN_GLYPH);
            _presetButton = add(presetGlyph);
            y += 18;

            const int32_t vignetteRows = kVignetteSliders;
            const int32_t tiltRows = static_cast<int32_t>(std::size(kEffectSliders)) - kVignetteSliders;
            const int32_t boxes = kEffectBoxCount;

            addGroup(y, 14 + vignetteRows * kRowHeight + 4, "Vignette");
            for (int32_t i = 0; i < vignetteRows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kEffectSliders[i].name);
            y += 14 + vignetteRows * kRowHeight + 4 + 4;

            addGroup(y, 14 + tiltRows * kRowHeight + boxes * 15 + 4, "Tilt-shift");
            for (int32_t i = 0; i < tiltRows; i++)
                addSlider(kVignetteSliders + i, y + 14 + i * kRowHeight, kEffectSliders[kVignetteSliders + i].name);
            int32_t boxY = y + 14 + tiltRows * kRowHeight;
            for (int32_t i = 0; i < boxes; i++)
            {
                Widget box = makeWidget({ 12, boxY }, { 288, 12 }, WidgetType::checkbox, WindowColour::tertiary);
                box.setString(NewText(kEffectCheckboxes[i].name));
                _checkboxes.push_back({ i, add(box) });
                boxY += 15;
            }
            y += 14 + tiltRows * kRowHeight + boxes * 15 + 4 + 6;
        }
        else if (_subPage == kSubLight)
        {
            const int32_t rows = kSunSliderCount;
            addGroup(y, 14 + rows * kRowHeight + 4, "Sun and shadows");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kLightSliders[i].name);
            y += 14 + rows * kRowHeight + 4 + 6;
        }
        else if (_subPage == kSubTime)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kLightSliders)) - kSunSliderCount;
            addGroup(y, 14 + rows * kRowHeight + 3 * 15 + 4, "Time of day and light objects");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kLightSliders[kSunSliderCount + i].name);
            const int32_t timeBoxes[3] = { kTimeBoxIndex, kTimeBoxIndex + 1, kLightsBoxIndex };
            for (int32_t i = 0; i < 3; i++)
            {
                Widget box = makeWidget(
                    { 12, y + 14 + rows * kRowHeight + i * 15 }, { 288, 12 }, WidgetType::checkbox, WindowColour::tertiary);
                box.setString(NewText(kEffectCheckboxes[timeBoxes[i]].name));
                _checkboxes.push_back({ timeBoxes[i], add(box) });
            }
            y += 14 + rows * kRowHeight + 3 * 15 + 4 + 6;
        }
        else if (_subPage == kSubClouds)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kCloudSliders));
            addGroup(y, 14 + rows * kRowHeight + 2 * 15 + 4, "Clouds, cloud shadows and fog");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kCloudSliders[i].name);
            for (int32_t i = 0; i < 2; i++)
            {
                Widget box = makeWidget(
                    { 12, y + 14 + rows * kRowHeight + i * 15 }, { 288, 12 }, WidgetType::checkbox, WindowColour::tertiary);
                box.setString(NewText(kEffectCheckboxes[kCloudBoxIndex + i].name));
                _checkboxes.push_back({ kCloudBoxIndex + i, add(box) });
            }
            y += 14 + rows * kRowHeight + 2 * 15 + 4 + 6;
        }
        else if (_subPage == kSubCinema)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kCinemaSliders));
            addGroup(y, 14 + rows * kRowHeight + 3 * 15 + 4, "Cinema mode");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kCinemaSliders[i].name);
            for (int32_t i = 0; i < 3; i++)
            {
                Widget box = makeWidget(
                    { 12, y + 14 + rows * kRowHeight + i * 15 }, { 288, 12 }, WidgetType::checkbox, WindowColour::tertiary);
                box.setString(NewText(kEffectCheckboxes[kCinemaBoxIndex + i].name));
                _checkboxes.push_back({ kCinemaBoxIndex + i, add(box) });
            }
            y += 14 + rows * kRowHeight + 3 * 15 + 4 + 6;
        }
        else if (_subPage == kSubLights)
        {
            // group 1: which light, tools
            const int32_t top1 = y - 2;
            addGroup(top1, 14 + 14 + 17 + 17 + 3, "Light objects");
            int32_t rowY = top1 + 13;
            Widget label = makeWidget({ 14, rowY }, { 196, 12 }, WidgetType::label, WindowColour::secondary);
            _lightLabelText = NewText("");
            label.setString(_lightLabelText);
            add(label);
            Widget bulbButton = makeWidget({ 214, rowY - 2 }, { 84, 14 }, WidgetType::button, WindowColour::secondary);
            _lightBulbText = NewText("Bulb: shown");
            bulbButton.setString(_lightBulbText);
            bulbButton.setTooltip("Shows or hides the glowing light source. Hidden: only the light around it is visible");
            _lightBulb = add(bulbButton);
            rowY += 14;
            auto button = [&](int32_t x, int32_t width, const char* text, const char* tip, int32_t top) {
                Widget b = makeWidget({ x, top }, { width, 14 }, WidgetType::button, WindowColour::secondary);
                b.setString(NewText(text));
                b.setTooltip(tip);
                return add(b);
            };
            _lightPrev = button(12, 22, "<", "Previous light", rowY);
            _lightNext = button(36, 22, ">", "Next light", rowY);
            {
                Widget snap = makeWidget({ 60, rowY }, { 44, 14 }, WidgetType::button, WindowColour::secondary);
                _lightSnapText = NewText("Tile");
                snap.setString(_lightSnapText);
                snap.setTooltip("Grid for placing lights: Tile = tile centre, 1/4 = quarter tile, Free = anywhere. Click to change");
                _lightGoto = add(snap);
            }
            _lightOn = button(106, 52, "On/Off", "Switches the selected light on or off", rowY);
            _lightDelete = button(160, 46, "Delete", "Removes the selected light", rowY);
            _lightCopy = button(208, 44, "Copy", "Remembers the selected light (colour, size, animation)", rowY);
            _lightPaste = button(254, 44, "Paste", "Then click the map: places copies of the remembered light", rowY);
            rowY += 17;
            _toolPlace = button(12, 94, "Place new", "Click the map to place a new light (same settings as the selected one)", rowY);
            _toolMove = button(108, 94, "Move", "Click the map to move the selected light there", rowY);
            _toolPick = button(204, 94, "Pick", "Click near a light on the map to select it", rowY);
            y = top1 + 14 + 14 + 17 + 17 + 3 + 3;

            // group 2: the selected light
            const int32_t rows = static_cast<int32_t>(std::size(kLightObjSliders));
            addGroup(y, 14 + rows * 16 + 2 * 17 + 3, "Selected light");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * 16, kLightObjSliders[i].name);
            rowY = y + 14 + rows * 16;
            Widget animCaption = makeWidget({ 14, rowY + 1 }, { 70, 12 }, WidgetType::label, WindowColour::secondary);
            animCaption.setString(NewText("Animation:"));
            add(animCaption);
            Widget animBox = makeWidget({ 84, rowY }, { 216, 14 }, WidgetType::dropdownMenu, WindowColour::secondary);
            _animText = NewText("None");
            animBox.setString(_animText);
            _animDropdown = add(animBox);
            Widget animGlyph = makeWidget({ 288, rowY + 1 }, { 11, 12 }, WidgetType::button, WindowColour::secondary, STR_DROPDOWN_GLYPH);
            _animButton = add(animGlyph);
            rowY += 17;
            Widget colourCaption = makeWidget({ 14, rowY + 1 }, { 70, 12 }, WidgetType::label, WindowColour::secondary);
            colourCaption.setString(NewText("Colour:"));
            add(colourCaption);
            Widget colourBox = makeWidget({ 84, rowY }, { 216, 14 }, WidgetType::dropdownMenu, WindowColour::secondary);
            colourBox.setString(NewText("Choose a colour..."));
            _colourDropdown = add(colourBox);
            Widget colourGlyph = makeWidget({ 288, rowY + 1 }, { 11, 12 }, WidgetType::button, WindowColour::secondary, STR_DROPDOWN_GLYPH);
            _colourButton = add(colourGlyph);
            y += 14 + rows * 16 + 2 * 17 + 3 + 4;
            _reset = 0;
            return;
        }
        else if (_subPage == kSubWater)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kWaterSliders));
            addGroup(y, 14 + rows * kRowHeight + 4, "Water");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kWaterSliders[i].name);
            y += 14 + rows * kRowHeight + 4 + 6;
        }
        else if (_subPage == kSubWeather)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kWeatherSliders));
            addGroup(y, 14 + rows * kRowHeight + 2 * 15 + 4, "Rain, snow and wind");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kWeatherSliders[i].name);
            for (int32_t i = 0; i < 2; i++)
            {
                Widget box = makeWidget(
                    { 12, y + 14 + rows * kRowHeight + i * 15 }, { 288, 12 }, WidgetType::checkbox, WindowColour::tertiary);
                box.setString(NewText(kEffectCheckboxes[kWeatherBoxIndex + i].name));
                _checkboxes.push_back({ kWeatherBoxIndex + i, add(box) });
            }
            y += 14 + rows * kRowHeight + 2 * 15 + 4 + 6;
        }
        else if (_subPage == kSubColour)
        {
            const int32_t rows = static_cast<int32_t>(std::size(kColourSliders));
            addGroup(y, 14 + rows * kRowHeight + 4, "Colour grade");
            for (int32_t i = 0; i < rows; i++)
                addSlider(i, y + 14 + i * kRowHeight, kColourSliders[i].name);
            y += 14 + rows * kRowHeight + 4 + 6;
        }
        else
        {
            // LUTs: named, saved colour settings
            addGroup(y, 14 + 19 + 19 + 19 + 19 + 16 + 16 + 4, "LUTs (saved colour settings)");
            int32_t rowY = y + 14;
            Widget caption = makeWidget({ 14, rowY + 1 }, { 70, 12 }, WidgetType::label, WindowColour::secondary);
            caption.setString(NewText("Active LUT:"));
            add(caption);
            Widget dropdown = makeWidget({ 84, rowY }, { 216, 14 }, WidgetType::dropdownMenu, WindowColour::secondary);
            _lutCurrentText = NewText("");
            dropdown.setString(_lutCurrentText);
            _lutDropdown = add(dropdown);
            Widget glyph = makeWidget({ 288, rowY + 1 }, { 11, 12 }, WidgetType::button, WindowColour::secondary, STR_DROPDOWN_GLYPH);
            _lutDropdownButton = add(glyph);
            rowY += 19;

            Widget save = makeWidget({ 12, rowY }, { 286, 14 }, WidgetType::button, WindowColour::secondary);
            save.setString(NewText("Save current colour as LUT..."));
            save.setTooltip("Stores the colour settings under a name. The file can be shared with other players.");
            _lutSave = add(save);
            rowY += 19;

            Widget folder = makeWidget({ 12, rowY }, { 140, 14 }, WidgetType::button, WindowColour::secondary);
            folder.setString(NewText("Open LUT folder"));
            folder.setTooltip("Drop LUT files (.atmolut) from other players into this folder");
            _lutFolder = add(folder);
            Widget refresh = makeWidget({ 158, rowY }, { 140, 14 }, WidgetType::button, WindowColour::secondary);
            refresh.setString(NewText("Reload list"));
            _lutRefresh = add(refresh);
            rowY += 19;

            Widget del = makeWidget({ 12, rowY }, { 286, 14 }, WidgetType::button, WindowColour::secondary);
            _lutDeleteText = NewText("Delete active LUT");
            del.setString(_lutDeleteText);
            _lutDelete = add(del);
            rowY += 19;

            Widget info = makeWidget({ 14, rowY }, { 284, 12 }, WidgetType::label, WindowColour::secondary);
            _lutInfoText = NewText("");
            info.setString(_lutInfoText);
            add(info);
            rowY += 16;
            Widget status = makeWidget({ 14, rowY }, { 284, 12 }, WidgetType::label, WindowColour::secondary);
            _lutStatusText = NewText("");
            status.setString(_lutStatusText);
            add(status);
            y += 14 + 19 + 19 + 19 + 19 + 16 + 16 + 4 + 6;
            _reset = 0;
            return;
        }

        if (_subPage == kSubCinema || _subPage == kSubLights || _subPage == kSubWater)
        {
            _reset = 0;
            return;
        }

        Widget reset = makeWidget({ 10, y }, { 290, 14 }, WidgetType::button, WindowColour::secondary);
        reset.setString(NewText(
            _subPage == kSubEffects ? "Reset effects to default"
                                    : ((_subPage == kSubLight || _subPage == kSubTime) ? "Reset light to default"
                                                             : (_subPage == kSubClouds ? "Reset clouds to default"
                                                                                       : (_subPage == kSubWeather ? "Reset weather to default"
                                                                                                                  : "Reset colour to neutral")))));
        reset.setTooltip(
            _subPage == kSubEffects ? "Restores the standard Atmosphere look (vignette and tilt-shift)"
                                    : ((_subPage == kSubLight || _subPage == kSubTime) ? "Restores the standard sun, shadow and time settings"
                                                             : (_subPage == kSubClouds ? "Restores the standard clouds and fog"
                                                                                       : (_subPage == kSubWeather ? "Restores the standard rain, snow and wind settings"
                                                                                                                  : "Sets all colour sliders back to their neutral position"))));
        _reset = add(reset);
    }

    void AtmosphereOptionsPage::RefreshLuts()
    {
        _luts = Atmosphere::ListLuts();
        _lutsDirty = false;
        // Remember what the active LUT looked like, so edits can be shown as "modified".
        _activeGradeKnown = false;
        const auto& active = Atmosphere::GetSettings().activeLut;
        for (const auto& lut : _luts)
        {
            if (lut.name == active)
            {
                _activeGradeKnown = Atmosphere::LoadLut(lut.path, _activeGrade);
                break;
            }
        }
    }

    // item 0 = neutral colour, items 1.. = LUTs
    void AtmosphereOptionsPage::ApplyLut(int32_t item)
    {
        Settings& s = Atmosphere::GetSettings();
        if (item <= 0)
        {
            Atmosphere::ClearCubeLut();
            s.grade = Atmosphere::ColourGrade{};
            s.activeLut.clear();
            _activeGradeKnown = false;
            _lutStatus = "Colour set to neutral";
        }
        else if (item - 1 < static_cast<int32_t>(_luts.size()))
        {
            const auto& lut = _luts[item - 1];
            Atmosphere::ColourGrade grade;
            if (lut.isCube)
            {
                if (Atmosphere::LoadCubeLut(lut.path))
                {
                    s.grade = Atmosphere::ColourGrade{};
                    s.activeLut = lut.name;
                    _activeGradeKnown = false;
                    _lutStatus = "Applied: " + lut.name;
                }
                else
                {
                    _lutStatus = "Could not read " + lut.name + " (only 3D .cube tables work)";
                }
            }
            else if (Atmosphere::LoadLut(lut.path, grade))
            {
                Atmosphere::ClearCubeLut();
                s.grade = grade;
                s.activeLut = lut.name;
                _activeGrade = grade;
                _activeGradeKnown = true;
                _lutStatus = "Applied: " + lut.name;
            }
            else
            {
                _lutStatus = "Could not read " + lut.name;
            }
        }
        _deleteArmed = false;
        Atmosphere::NotifySettingsChanged();
    }

    void AtmosphereOptionsPage::UpdateLutTexts()
    {
        const Settings& s = Atmosphere::GetSettings();
        if (_lutCurrentText != nullptr)
        {
            if (s.activeLut.empty())
                std::snprintf(_lutCurrentText, kTextLength, "%s", s.grade.IsNeutral() ? "(neutral colour)" : "(custom colour)");
            else
            {
                const bool modified = _activeGradeKnown && !(s.grade == _activeGrade);
                std::snprintf(_lutCurrentText, kTextLength, "%s%s", s.activeLut.c_str(), modified ? " (modified)" : "");
            }
        }
        if (_lutDeleteText != nullptr)
            std::snprintf(_lutDeleteText, kTextLength, "%s", _deleteArmed ? "Click again to delete the active LUT" : "Delete active LUT");
        if (_lutInfoText != nullptr)
            std::snprintf(_lutInfoText, kTextLength, "%d LUT file(s) found", static_cast<int32_t>(_luts.size()));
        if (_lutStatusText != nullptr)
            std::snprintf(_lutStatusText, kTextLength, "%s", _lutStatus.c_str());
    }

    void AtmosphereOptionsPage::OnPageOpened()
    {
        _deleteArmed = false;
        if (_subPage == kSubLuts)
            _lutsDirty = true;
        for (auto& slider : _sliders)
            slider.lastPercent = -1;
    }

    // The ground point under the cursor, exactly as the game picks the terrain for its own tools.
    static std::optional<CoordsXY> CursorLightPlane(const ScreenCoordsXY& screenCoords, int32_t z, Viewport*& viewport)
    {
        viewport = ViewportFindFromPoint(screenCoords);
        if (viewport == nullptr) return std::nullopt;
        return Atmosphere::LightPlacement::AtPlane(viewport->ScreenToViewportCoord(screenCoords),z,viewport->rotation);
    }

    static std::optional<CoordsXYZ> CursorNewLight(const ScreenCoordsXY& screenCoords, float height, Viewport*& viewport)
    {
        // Native terrain hit supplies only an initial height. Final XY is an unclamped ray/height intersection.
        const auto seed=ScreenGetMapXY(screenCoords,&viewport);
        if (!seed.has_value() || viewport==nullptr) return std::nullopt;
        return Atmosphere::LightPlacement::AtTerrainLift(viewport->ScreenToViewportCoord(screenCoords),height,
            viewport->rotation,*seed,[](const CoordsXY& xy)->std::optional<int32_t> {
                if(!MapIsLocationValid(xy))return std::nullopt;
                return TileElementHeight(xy);
            });
    }

    static int32_t SelectedBulbZ()
    {
        const auto* light=Atmosphere::LightAt(Atmosphere::LightSelected());
        return light==nullptr?0:TileElementHeight(CoordsXY{light->x,light->y})+static_cast<int32_t>(std::lround(light->height));
    }

    void AtmosphereOptionsPage::DragSelectedLight(const ScreenCoordsXY& screenCoords)
    {
        const int32_t selected=Atmosphere::LightSelected();
        if(selected<0)return;
        if(!_grabBulb) {
            _grabBulb=true;_dragZ=SelectedBulbZ();_dragOffsetX=_dragOffsetY=0;_dragRotation=-1;
        }
        Viewport* viewport=nullptr;
        const auto map=CursorLightPlane(screenCoords,_dragZ,viewport);
        if(!map.has_value())return;
        const int rotation=viewport->rotation,zoom=static_cast<int8_t>(viewport->zoom);
        if(_dragRotation>=0 && (_dragRotation!=rotation || _dragZoom!=zoom)) {
            const auto* light=Atmosphere::LightAt(selected);
            _dragOffsetX=light->x-map->x;_dragOffsetY=light->y-map->y;
            _dragRotation=rotation;_dragZoom=zoom;
            return; // Camera changes rebase the grab; they never move the stored world anchor.
        }
        _dragRotation=rotation;_dragZoom=zoom;
        int32_t x=map->x+_dragOffsetX,y=map->y+_dragOffsetY;
        if(!MapIsLocationValid(CoordsXY{x,y}))return;
        Atmosphere::LightSnap(x,y);
        Atmosphere::LightPlaceAt(selected,x,y,_dragZ,0.0f);
    }

    void AtmosphereOptionsPage::ToolDown(Window& w, WidgetIndex, const ScreenCoordsXY& screenCoords)
    {
        _grabBulb=false;_dragRotation=-1;
        switch(_tool) {
            case LightTool::Place:
            case LightTool::Paste: {
                const bool paste=_tool==LightTool::Paste;
                Viewport* viewport=nullptr;
                const auto anchor=CursorNewLight(screenCoords,Atmosphere::LightPlacementHeight(paste),viewport);
                if(!anchor.has_value())break;
                int x=anchor->x,y=anchor->y;Atmosphere::LightSnap(x,y);
                if(!MapIsLocationValid(CoordsXY{x,y}))break;
                const int selected=Atmosphere::LightAdd(x,y,paste);
                Atmosphere::LightPlaceAt(selected,x,y,anchor->z,0.0f);
                _grabBulb=true;_dragZ=anchor->z;_dragOffsetX=_dragOffsetY=0;
                _dragRotation=viewport->rotation;_dragZoom=static_cast<int8_t>(viewport->zoom);_dragging=true;
                break;
            }
            case LightTool::Move:
                if(Atmosphere::LightSelected()>=0) {
                    DragSelectedLight(screenCoords);_dragging=true;
                }
                break;
            case LightTool::Pick: {
                const int found=Atmosphere::LightPickAtScreen(screenCoords.x,screenCoords.y);
                if(found<0)break;
                Atmosphere::LightSelect(found);_grabBulb=true;_dragZ=SelectedBulbZ();
                Viewport* viewport=nullptr;
                const auto map=CursorLightPlane(screenCoords,_dragZ,viewport);
                const auto* light=Atmosphere::LightAt(found);
                if(map.has_value() && light!=nullptr) {
                    _dragOffsetX=light->x-map->x;_dragOffsetY=light->y-map->y;
                    _dragRotation=viewport->rotation;_dragZoom=static_cast<int8_t>(viewport->zoom);_dragging=true;
                }
                break;
            }
            default:break;
        }
        w.invalidate();
    }

    void AtmosphereOptionsPage::ToolDrag(Window& w, WidgetIndex, const ScreenCoordsXY& screenCoords)
    {
        if (!_dragging)
            return;
        DragSelectedLight(screenCoords);
        w.invalidate();
    }

    void AtmosphereOptionsPage::ToolUp()
    {
        _dragging = false;
        _dragRotation = -1;
    }

    void AtmosphereOptionsPage::ToolAbort()
    {
        _tool = LightTool::None;
        _dragging = false;
    }

    void AtmosphereOptionsPage::MouseUp(Window& w, WidgetIndex widgetIndex)
    {
        Settings& s = Atmosphere::GetSettings();

        if (_subPage == kSubLights)
        {
            const int32_t count = Atmosphere::LightCount();
            const int32_t selected = Atmosphere::LightSelected();
            const auto armTool = [&](LightTool tool, WidgetIndex index) {
                if (_tool == tool)
                {
                    ToolCancel();
                    _tool = LightTool::None;
                }
                else
                {
                    ToolCancel();
                    // ToolSet returns true only when it cancelled the tool of the same widget
                    if (!ToolSet(w, index, Tool::crosshair))
                        _tool = tool;
                }
            };
            if (widgetIndex == _lightPrev && count > 0)
                Atmosphere::LightSelect(selected <= 0 ? count - 1 : selected - 1);
            else if (widgetIndex == _lightNext && count > 0)
                Atmosphere::LightSelect(selected + 1 >= count ? 0 : selected + 1);
            else if (widgetIndex == _lightGoto)
            {
                s.lightSnap = (s.lightSnap + 2) % 3; // tile -> free -> quarter -> tile
                Atmosphere::NotifySettingsChanged();
            }
            else if (widgetIndex == _lightBulb && selected >= 0)
                Atmosphere::LightSetBulb(selected, !Atmosphere::LightAt(selected)->bulb);
            else if (widgetIndex == _lightOn && selected >= 0)
                Atmosphere::LightSetOn(selected, !Atmosphere::LightAt(selected)->on);
            else if (widgetIndex == _lightDelete && selected >= 0)
                Atmosphere::LightDelete(selected);
            else if (widgetIndex == _lightCopy)
                Atmosphere::LightCopy();
            else if (widgetIndex == _lightPaste && Atmosphere::LightHasClipboard())
                armTool(LightTool::Paste, widgetIndex);
            else if (widgetIndex == _toolPlace)
                armTool(LightTool::Place, widgetIndex);
            else if (widgetIndex == _toolMove)
                armTool(LightTool::Move, widgetIndex);
            else if (widgetIndex == _toolPick)
                armTool(LightTool::Pick, widgetIndex);
            for (auto& slider : _sliders)
                slider.lastPercent = -2;
            w.invalidate();
        }

        for (int32_t i = 0; i < kSubPageCount; i++)
        {
            if (widgetIndex == _subTabs[i])
            {
                if (_subPage != i)
                {
                    _subPage = i;
                    _rebuild = true;
                }
                return;
            }
        }
        if (widgetIndex == _enable)
        {
            if (EngineSupported())
            {
                s.enabled = !s.enabled;
                Atmosphere::NotifySettingsChanged();
                w.invalidate();
            }
            return;
        }
        if (_subPage == kSubLuts)
        {
            if (widgetIndex == _lutSave)
            {
                _deleteArmed = false;
                const std::string initial = s.activeLut.empty() ? std::string("My LUT") : s.activeLut;
                WindowTextInputOpen(
                    "Save colour as LUT", "Name of the LUT (an existing LUT with the same name is replaced):", initial, 40,
                    [](std::string_view text) {
                        const std::string saved = Atmosphere::SaveLut(std::string(text), Atmosphere::GetSettings().grade);
                        if (saved.empty())
                        {
                            _lutStatus = "Could not save the LUT";
                        }
                        else
                        {
                            Atmosphere::Settings& settings = Atmosphere::GetSettings();
                            settings.activeLut = saved;
                            Atmosphere::NotifySettingsChanged();
                            _lutStatus = "Saved: " + saved;
                        }
                        _lutsDirty = true;
                    },
                    [] {});
            }
            else if (widgetIndex == _lutFolder)
            {
                _deleteArmed = false;
                GetContext()->GetUiContext().OpenFolder(Atmosphere::LutDirectory());
            }
            else if (widgetIndex == _lutRefresh)
            {
                _deleteArmed = false;
                _lutsDirty = true;
                _lutStatus = "List reloaded";
            }
            else if (widgetIndex == _lutDelete)
            {
                const auto found = std::find_if(
                    _luts.begin(), _luts.end(), [&](const Atmosphere::LutInfo& lut) { return lut.name == s.activeLut; });
                if (found == _luts.end())
                {
                    _deleteArmed = false;
                    _lutStatus = "No saved LUT is active";
                }
                else if (!_deleteArmed)
                {
                    _deleteArmed = true;
                }
                else
                {
                    const std::string name = found->name;
                    _lutStatus = Atmosphere::DeleteLut(found->path) ? ("Deleted: " + name) : ("Could not delete " + name);
                    s.activeLut.clear();
                    Atmosphere::NotifySettingsChanged();
                    _deleteArmed = false;
                    _lutsDirty = true;
                }
            }
            w.invalidate();
            return;
        }
        if (widgetIndex == _reset)
        {
            if (_subPage == kSubEffects)
                Atmosphere::ResetEffectSettings();
            else if (_subPage == kSubLight || _subPage == kSubTime)
                Atmosphere::ResetLightSettings();
            else if (_subPage == kSubClouds)
                Atmosphere::ResetCloudSettings();
            else if (_subPage == kSubWeather)
                Atmosphere::ResetWeatherSettings();
            else
                Atmosphere::ResetColourGrade();
            // Sliders follow the settings again on the next update.
            for (auto& slider : _sliders)
                slider.lastPercent = -2;
            w.invalidate();
            return;
        }
        for (const auto& box : _checkboxes)
        {
            if (box.widget == widgetIndex)
            {
                bool& value = kEffectCheckboxes[box.definition].ref(s);
                value = !value;
                Atmosphere::NotifySettingsChanged();
                w.invalidate();
                return;
            }
        }
    }

    void AtmosphereOptionsPage::MouseDown(Window& w, WidgetIndex widgetIndex)
    {
        if (_subPage == kSubEffects && widgetIndex == _presetButton)
        {
            const Widget& presetBox = w.widgets[_presetDropdown];
            const int32_t presets = std::min(Atmosphere::PresetCount(), static_cast<int32_t>(Dropdown::kItemsMaxSize));
            for (int32_t i = 0; i < presets; i++)
                gDropdown.items[i] = Dropdown::MenuLabel(Atmosphere::PresetName(i));
            WindowDropdownShowTextCustomWidth(
                { w.windowPos.x + presetBox.left, w.windowPos.y + presetBox.top }, presetBox.height(), w.colours[1], 0, {}, presets,
                presetBox.width() - 4);
            return;
        }
        if (_subPage == kSubLights && (widgetIndex == _animButton || widgetIndex == _colourButton))
        {
            const bool anim = widgetIndex == _animButton;
            const Widget& box = w.widgets[anim ? _animDropdown : _colourDropdown];
            const int32_t items = anim ? static_cast<int32_t>(Atmosphere::LightAnim::Count) : Atmosphere::LightColourPresetCount();
            for (int32_t i = 0; i < items; i++)
                gDropdown.items[i] = Dropdown::MenuLabel(anim ? Atmosphere::LightAnimName(i) : Atmosphere::LightColourPresetName(i));
            WindowDropdownShowTextCustomWidth(
                { w.windowPos.x + box.left, w.windowPos.y + box.top }, box.height(), w.colours[1], 0, {}, items, box.width() - 4);
            return;
        }
        if (_subPage != kSubLuts || widgetIndex != _lutDropdownButton)
            return;
        if (_lutsDirty)
            RefreshLuts();

        const Widget& dropdown = w.widgets[_lutDropdown];
        const Settings& s = Atmosphere::GetSettings();
        int32_t count = 0;
        gDropdown.items[count++] = Dropdown::MenuLabel("Neutral (reset colour)");
        int32_t checked = s.activeLut.empty() ? 0 : -1;
        for (const auto& lut : _luts)
        {
            if (count >= Dropdown::kItemsMaxSize)
                break;
            if (lut.name == s.activeLut)
                checked = count;
            gDropdown.items[count++] = Dropdown::MenuLabel(lut.name.c_str());
        }
        WindowDropdownShowTextCustomWidth(
            { w.windowPos.x + dropdown.left, w.windowPos.y + dropdown.top }, dropdown.height(), w.colours[1], 0, {}, count,
            dropdown.width() - 4);
        if (checked >= 0)
            gDropdown.items[checked].setChecked(true);
    }

    void AtmosphereOptionsPage::Dropdown(Window& w, WidgetIndex widgetIndex, int32_t selectedIndex)
    {
        if (_subPage == kSubEffects && widgetIndex == _presetButton && selectedIndex >= 0)
        {
            Atmosphere::ApplyPreset(selectedIndex);
            for (auto& slider : _sliders)
                slider.lastPercent = -2;
            if (_presetText != nullptr)
                std::snprintf(_presetText, kTextLength, "%s", Atmosphere::PresetName(selectedIndex));
            w.invalidate();
            return;
        }
        if (_subPage == kSubLights && selectedIndex >= 0)
        {
            if (widgetIndex == _animButton)
                Atmosphere::LightSetAnim(Atmosphere::LightSelected(), static_cast<Atmosphere::LightAnim>(selectedIndex));
            else if (widgetIndex == _colourButton)
                Atmosphere::LightApplyColourPreset(selectedIndex);
            for (auto& slider : _sliders)
                slider.lastPercent = -2;
            w.invalidate();
            return;
        }
        if (_subPage != kSubLuts || widgetIndex != _lutDropdownButton || selectedIndex < 0)
            return;
        ApplyLut(selectedIndex);
        w.invalidate();
    }

    void AtmosphereOptionsPage::Update(Window& w)
    {
        if (_subPage == kSubLuts && _lutsDirty)
        {
            RefreshLuts();
            w.invalidate();
        }
        if (_subPage == kSubLights)
        {
            Atmosphere::LightsEditPing();
            if (_seenSelected != Atmosphere::LightSelected() || _seenCount != Atmosphere::LightCount())
            {
                _seenSelected = Atmosphere::LightSelected();
                _seenCount = Atmosphere::LightCount();
                Atmosphere::LightLoadEdit();
                for (auto& slider : _sliders)
                    slider.lastPercent = -2;
                w.invalidate();
            }
        }
        int32_t count = 0;
        const SliderDef* defs = SlidersOf(_subPage, count);
        for (size_t i = 0; i < _sliders.size(); i++)
        {
            auto& slider = _sliders[i];
            const SliderDef& def = defs[slider.definition];
            const int32_t scrollIndex = static_cast<int32_t>(i);
            const int32_t barPercent = ScrollPercent(w.widgets[slider.scroll], w.scrolls[scrollIndex]);
            if (barPercent < 0)
                continue; // scroll widgets are not laid out yet

            const int32_t settingPercent = ToPercent(def, GetValue(def));
            if (slider.lastPercent < 0 || (barPercent == slider.lastPercent && settingPercent != slider.lastPercent))
            {
                // First visit, a reset, or the value changed from outside (atmosphere.ini was edited): follow the setting.
                SetScrollPercent(w, slider.scroll, scrollIndex, settingPercent);
                slider.lastPercent = settingPercent;
                w.invalidateWidget(slider.label);
            }
            else if (barPercent != slider.lastPercent && InputGetState() == InputState::normal
                     && !ContextGetCursorState()->left)
            {
                // The bar moved without the mouse (layout reset the scroll offset): the setting is the truth.
                SetScrollPercent(w, slider.scroll, scrollIndex, settingPercent);
                slider.lastPercent = settingPercent;
                w.invalidateWidget(slider.label);
            }
            else if (barPercent != slider.lastPercent)
            {
                // The user dragged the bar.
                SetValue(def, FromPercent(def, barPercent));
                slider.lastPercent = barPercent;
                if (_subPage == kSubLights)
                    Atmosphere::LightApplyEdit();
                else
                    Atmosphere::NotifySettingsChanged();
                w.invalidateWidget(slider.label);
            }
        }
    }

    void AtmosphereOptionsPage::PrepareDraw(Window& w)
    {
        Settings& s = Atmosphere::GetSettings();
        const bool supported = EngineSupported();

        for (int32_t i = 0; i < kSubPageCount; i++)
            w.setWidgetPressed(_subTabs[i], i == _subPage);
        if (_subPage == kSubLuts)
            UpdateLutTexts();

        if (_subPage == kSubLights)
        {
            const int32_t selected = Atmosphere::LightSelected(), total = Atmosphere::LightCount();
            if (_lightLabelText != nullptr)
            {
                if (total == 0)
                    std::snprintf(_lightLabelText, kTextLength, "%s", "No light yet: use 'Place new'");
                else if (selected < 0)
                    std::snprintf(_lightLabelText, kTextLength, "%d lights. Click 'Pick' and click one on the map", total);
                else
                {
                    const auto* l = Atmosphere::LightAt(selected);
                    std::snprintf(_lightLabelText, kTextLength, "Light %d of %d   (tile %d, %d)   %s", selected + 1, total, l->x / 32,
                        l->y / 32, l->on ? "on" : "OFF");
                }
            }
            if (_lightSnapText != nullptr)
                std::snprintf(_lightSnapText, kTextLength, "%s", s.lightSnap == 2 ? "Tile" : (s.lightSnap == 1 ? "1/4" : "Free"));
            if (_lightBulbText != nullptr)
            {
                const auto* l = Atmosphere::LightAt(Atmosphere::LightSelected());
                std::snprintf(_lightBulbText, kTextLength, "%s", l != nullptr && !l->bulb ? "Bulb: hidden" : "Bulb: shown");
            }
            if (_animText != nullptr)
            {
                const auto* l = Atmosphere::LightAt(Atmosphere::LightSelected());
                std::snprintf(_animText, kTextLength, "%s", l != nullptr ? Atmosphere::LightAnimName(static_cast<int32_t>(l->anim)) : "-");
            }
            w.setWidgetPressed(_toolPlace, _tool == LightTool::Place);
            w.setWidgetPressed(_toolMove, _tool == LightTool::Move);
            w.setWidgetPressed(_toolPick, _tool == LightTool::Pick);
            w.setWidgetPressed(_lightPaste, _tool == LightTool::Paste);
        }
        w.setCheckboxValue(_enable, s.enabled && supported);
        w.setWidgetDisabled(_enable, !supported);
        w.widgets[_hint].setHidden(supported);

        int32_t count = 0;
        const SliderDef* defs = SlidersOf(_subPage, count);
        for (const auto& slider : _sliders)
        {
            const SliderDef& def = defs[slider.definition];
            FormatValue(slider.text, kTextLength, def, GetValue(def));
            w.setWidgetDisabled(slider.scroll, !supported);
        }
        for (const auto& box : _checkboxes)
        {
            w.setCheckboxValue(box.widget, kEffectCheckboxes[box.definition].ref(s));
            w.setWidgetDisabled(box.widget, !supported);
        }
    }

    void AtmosphereOptionsPage::DrawNavigation(Window& w, RenderTarget& rt) const
    {
        for (int32_t page = 0; page < kSubPageCount; ++page)
        {
            const auto& tab = w.widgets[_subTabs[page]];
            const auto shades = getColourMap(kPageColours[page]);
            const bool selected = page == _subPage;
            const auto origin = w.windowPos + ScreenCoordsXY{ tab.left, tab.top };
            auto fill = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2, PaletteIndex colour) {
                Rectangle::fill(rt, { origin + ScreenCoordsXY{ x1, y1 }, origin + ScreenCoordsXY{ x2, y2 } }, colour);
            };
            // Paint the entire bevel in the tab's own colour; no window-blue frame remains.
            fill(0, 0, tab.width() - 1, tab.height() - 1, shades.darkest);
            fill(0, 0, tab.width() - 2, 0, shades.lighter);
            fill(0, 0, 0, tab.height() - 2, shades.lighter);
            fill(1, 1, tab.width() - 2, tab.height() - 2, selected ? shades.lightest : shades.light);
            // Window "black" uses its light contrast shade. The explicit text token selects actual black ink.
            TextPaint label{ Colour::black };
            const std::string labelText = std::string("{BLACK}") + kPageNames[page];
            fill(2, 2, 4, tab.height() - 3, selected ? shades.darkest : shades.midDark);
            if (_sideLayout)
            {
                for (int32_t y = 0; y < 7; ++y)
                    for (int32_t x = 0; x < 7; ++x)
                        if (kPageIcons[page][y] & (1 << (6 - x)))
                            fill(9 + x, 6 + y, 9 + x, 6 + y, shades.darkest);
                drawTextEllipsised(rt, origin + ScreenCoordsXY{ 23, 5 }, tab.width() - 28, labelText, label);
                if (selected)
                    fill(tab.width() - 5, 3, tab.width() - 4, tab.height() - 4, shades.darkest);
            }
            else
            {
                label.alignment = TextAlignment::centre;
                drawTextEllipsised(rt, origin + ScreenCoordsXY{ tab.width() / 2, 2 }, tab.width() - 6,
                    labelText, label);
            }
        }
    }

    ScreenSize AtmosphereOptionsPage::ScrollGetSize(int32_t) const
    {
        return { 500, 0 };
    }

    void AtmosphereOptionsPage::DrawTabIcon(RenderTarget& rt, const ScreenCoordsXY& origin, bool selected, int32_t frame)
    {
        // Artist's palette, shared by the options tab and toolbar.
        const auto topLeft = origin + ScreenCoordsXY{ 3, 4 };
        const auto edge = getColourMap(Colour::darkBrown).darkest;
        const auto wood = getColourMap(Colour::lightBrown).light;
        const auto highlight = getColourMap(Colour::lightBrown).lightest;
        const auto put = [&](int32_t x, int32_t y, PaletteIndex colour) {
            Rectangle::fill(rt, { topLeft + ScreenCoordsXY{ x, y }, topLeft + ScreenCoordsXY{ x, y } }, colour);
        };
        for (int32_t y = 1; y < 19; y++)
            for (int32_t x = 1; x < 23; x++)
            {
                const float dx = (x - 11.f) / 10.f, dy = (y - 9.f) / 8.f;
                const float r = dx * dx + dy * dy;
                const int32_t hx = x - 18, hy = y - 13;
                if (r <= 1.f && hx * hx + hy * hy > 10)
                    put(x, y, r > .77f ? edge : (y < 5 ? highlight : wood));
            }
        const Colour paints[] = { Colour::brightRed, Colour::yellow, Colour::brightGreen, Colour::lightBlue, Colour::brightPurple };
        const ScreenCoordsXY dots[] = { { 5, 8 }, { 8, 4 }, { 13, 4 }, { 17, 7 }, { 10, 13 } };
        for (size_t i = 0; i < std::size(paints); i++)
            for (int32_t y = -1; y <= 1; y++)
                for (int32_t x = -1; x <= 1; x++)
                    put(dots[i].x + x, dots[i].y + y, getColourMap(paints[i]).midLight);
        (void)selected;
        (void)frame;
    }
    void AtmosphereOptionsPage::DrawCinemaIcon(RenderTarget& rt, const ScreenCoordsXY& origin, bool active)
    {
        const auto outline = Drawing::getColourMap(Drawing::Colour::black).darker;
        const auto body = Drawing::getColourMap(Drawing::Colour::grey).midDark;
        const auto bodyLight = Drawing::getColourMap(Drawing::Colour::grey).lighter;
        const auto lens = Drawing::getColourMap(Drawing::Colour::lightBlue).midLight;
        const auto flash = Drawing::getColourMap(Drawing::Colour::brightYellow).lightest;
        const auto fill = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, Drawing::PaletteIndex colour) {
            Drawing::Rectangle::fill(rt, { origin + ScreenCoordsXY{ x0, y0 }, origin + ScreenCoordsXY{ x1, y1 } }, colour);
        };
        // Film camera: twin reels, rectangular body and projecting lens hood.
        const auto reel = [&](int32_t cx) {
            for (int32_t y = 0; y <= 7; y++)
                for (int32_t x = cx - 4; x <= cx + 4; x++)
                {
                    const int32_t d = (x - cx) * (x - cx) + (y - 3) * (y - 3);
                    if (d <= 16)
                        fill(x, y, x, y, d > 8 ? outline : bodyLight);
                }
            fill(cx, 2, cx + 1, 4, body);
        };
        reel(5); reel(14);
        fill(1, 7, 17, 16, outline);
        fill(2, 8, 16, 15, body);
        fill(3, 8, 15, 9, bodyLight);
        fill(18, 9, 19, 14, outline);
        fill(20, 8, 21, 15, outline);
        fill(21, 7, 22, 16, outline);
        fill(18, 10, 20, 13, lens);
        fill(6, 17, 12, 18, outline);
        if (active)
            fill(13, 11, 14, 12, flash);

    }
} // namespace OpenRCT2::Ui::Windows
