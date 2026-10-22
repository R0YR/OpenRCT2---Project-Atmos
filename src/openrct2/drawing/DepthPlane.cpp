/*****************************************************************************
 * Depth plane globals. See DepthPlane.h.
 *****************************************************************************/

#include "DepthPlane.h"

namespace OpenRCT2::Drawing
{
    DepthPlane gDepthPlane;
    std::atomic<uint32_t> gMapRevision{ 0 };
    std::atomic<bool> gAtmosphereReplacesWeather{ false };
    std::atomic<bool> gAtmosphereReplacesLightning{ false };
    std::atomic<bool> gSwayEnabled{ false };
    std::atomic<bool> gAtmosphereOpaqueUi{ false };
    std::atomic<float> gSmoothCameraRate{ 0.0f };
    std::atomic<uint64_t> gPaintNanos{ 0 };
    std::atomic<uint64_t> gPaintPixels{ 0 };
    WorldSnapshot gWorldSnapshot;
    thread_local DepthPaintState gDepthPaint;
} // namespace OpenRCT2::Drawing
