/*****************************************************************************
 * Depth plane (fork addition for the Atmosphere post-processing effects).
 *
 * A 16-bit value per screen pixel that mirrors the 8bpp frame buffer 1:1 (same index). Whenever a world sprite
 * writes a pixel into the main frame buffer the blit also stores that sprite's depth here, so effects such as
 * tilt-shift can work with real scene depth instead of the screen row.
 *
 * Depth unit: "ground distance" D = xr + yr (map units, rotated by the view rotation, can be negative). One screen
 * row on flat ground equals 2 * 2^zoom units. Stored as (D / 2) + 32768 so the whole map fits in 16 bit.
 * 0xFFFF = no depth recorded.
 *
 * Licensed GPLv3 like OpenRCT2.
 *****************************************************************************/

#pragma once

#include "PaletteIndex.h"
#include "RenderTarget.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OpenRCT2::Drawing
{
    constexpr uint16_t kDepthUnknown = 0xFFFF;
    constexpr uint8_t kShadowCaster = 1;
    constexpr uint8_t kShadowReceiver = 2;
    constexpr uint8_t kSceneryReceiver = 16; // visible scenery; separate from stable ground/water receivers
    constexpr uint8_t kWaterSurface = 4; // visible water, independent of its animated/remapped palette colour
    constexpr uint8_t kExteriorSurface = 8; // native blank tile; dark pixels of actual scenery remain scene pixels

    // The plane mirrors the engine's main frame buffer. data == nullptr means depth recording is disabled.
    struct DepthPlane
    {
        uint16_t* data = nullptr;
        const PaletteIndex* base = nullptr;
        size_t size = 0;
        // Sway plane (same indexing): high nibble = how much the pixel bends in the wind, low nibble = phase of the plant.
        uint8_t* sway = nullptr;
        // Actual opaque sprite coverage, never tile bounding boxes. Heights are above the local terrain.
        uint8_t* shadowFlags = nullptr;
        uint16_t* shadowHeight = nullptr;
        // Last ground/water receiver under upright sprites, for stable world-space atmosphere sampling.
        uint16_t* receiverDepth = nullptr;
        uint8_t* receiverFlags = nullptr; // underlying receiver material, preserved together with receiverDepth
    };
    extern DepthPlane gDepthPlane;
    // True while the post-process wants plants to sway; the paint code then classifies tree and foliage sprites.
    extern std::atomic<bool> gSwayEnabled;
    // True while Atmosphere is active: translucent interface bars are drawn opaque.
    extern std::atomic<bool> gAtmosphereOpaqueUi;
    // Cinema mode: when > 0 the main viewport glides to its target position with this rate (1/s); 0 = normal camera.
    extern std::atomic<float> gSmoothCameraRate;
    // Test counters: time and pixel count of the world painting (ViewportPaint).
    extern std::atomic<uint64_t> gPaintNanos;
    extern std::atomic<uint64_t> gPaintPixels;

    // Incremented whenever tile elements change (building, removing, terrain edits). The Atmosphere shadow map is
    // rebuilt when this changes.
    extern std::atomic<uint32_t> gMapRevision;
    // Set by the Atmosphere post-process while it draws rain and snow itself (the vanilla weather overlay is then skipped).
    extern std::atomic<bool> gAtmosphereReplacesWeather;
    // Atmosphere owns scene-only lightning while active; vanilla palette flashes would brighten UI too.
    extern std::atomic<bool> gAtmosphereReplacesLightning;

    // Depth of the sprite that is currently being painted (set per paint struct, thread local because the
    // viewport is painted by several worker threads, each on its own screen column).
    struct DepthPaintState
    {
        bool active = false;
        bool flat = false;    // ground-like sprite: depth changes with the screen row (D = 2 * (worldY + z))
        int32_t value = 0;    // constant depth for upright sprites
        int32_t flatBase = 0; // 2 * z for flat sprites
        int8_t zoom = 0;      // zoom of the painting session (sprite blits may use a reduced zoom on a copy of the RT)
        bool sway = false;    // the sprite is a tree or foliage that bends in the wind
        int32_t swayBottom = 0; // world row (zoom 0) of the bottom of the sprite
        int32_t swayHeight = 1; // height of the sprite in world rows
        uint8_t swayPhase = 0;  // 0..15, differs between plants
        uint8_t shadowFlags = 0;
        int32_t shadowGroundZ = 0;
    };
    extern thread_local DepthPaintState gDepthPaint;

    // Copy of the 8bpp frame as it looked right after the world (main viewport) was drawn and before any other window
    // was drawn on top. Everything that differs from this copy in the final frame is user interface.
    struct WorldSnapshot
    {
        PaletteIndex* data = nullptr;
        const PaletteIndex* base = nullptr;
        size_t size = 0;
    };
    extern WorldSnapshot gWorldSnapshot;

    // Stores the given screen region of `rt` (a crop of the main frame buffer) into the world snapshot.
    inline void SnapshotWorldRegion(const RenderTarget& rt, int32_t left, int32_t top, int32_t right, int32_t bottom)
    {
        const auto& s = gWorldSnapshot;
        if (s.data == nullptr || rt.bits < s.base || rt.bits >= s.base + s.size)
            return;
        left = left < rt.x ? rt.x : left;
        top = top < rt.y ? rt.y : top;
        right = right > rt.x + rt.width ? rt.x + rt.width : right;
        bottom = bottom > rt.y + rt.height ? rt.y + rt.height : bottom;
        if (left >= right || top >= bottom)
            return;
        const int32_t stride = rt.LineStride();
        for (int32_t y = top; y < bottom; y++)
        {
            const PaletteIndex* src = rt.bits + static_cast<ptrdiff_t>(y - rt.y) * stride + (left - rt.x);
            PaletteIndex* dst = s.data + (src - s.base);
            for (int32_t x = 0; x < right - left; x++)
                dst[x] = src[x];
        }
    }

    // Map-unit depth D -> stored value (2 units resolution) and back.
    inline uint16_t DepthEncode(int32_t d)
    {
        const int32_t v = (d >> 1) + 32768;
        return v < 0 ? 0 : (v > 0xFFFE ? 0xFFFE : static_cast<uint16_t>(v));
    }
    inline int32_t DepthDecode(uint16_t stored)
    {
        return (static_cast<int32_t>(stored) - 32768) * 2;
    }

    // Per-blit view of the depth plane, created once per sprite draw call.
    struct DepthBlitCtx
    {
        uint16_t* plane = nullptr;
        const PaletteIndex* base = nullptr;
        bool flat = false;
        int32_t value = 0;
        int32_t flatBase = 0;
        int8_t zoom = 0;
        uint8_t* swayPlane = nullptr;
        bool sway = false;
        int32_t swayBottom = 0, swayHeight = 1;
        uint8_t swayPhase = 0;
        uint8_t* shadowFlagsPlane = nullptr;
        uint16_t* shadowHeightPlane = nullptr;
        uint8_t shadowFlags = 0;
        int32_t shadowGroundZ = 0;
        uint16_t* receiverDepthPlane = nullptr;
        uint8_t* receiverFlagsPlane = nullptr;

        static DepthBlitCtx Make(const RenderTarget& rt)
        {
            DepthBlitCtx c;
            const auto& p = gDepthPlane;
            const auto& s = gDepthPaint;
            if (s.active && p.data != nullptr && rt.bits >= p.base && rt.bits < p.base + p.size)
            {
                c.plane = p.data;
                c.base = p.base;
                c.flat = s.flat;
                c.value = s.value;
                c.flatBase = s.flatBase;
                c.zoom = s.zoom;
                c.swayPlane = p.sway;
                c.sway = s.sway;
                c.swayBottom = s.swayBottom;
                c.swayHeight = s.swayHeight;
                c.swayPhase = s.swayPhase;
                c.shadowFlagsPlane = p.shadowFlags;
                c.shadowHeightPlane = nullptr; // silhouette shadows are disabled
                c.shadowFlags = s.shadowFlags;
                c.shadowGroundZ = s.shadowGroundZ;
                c.receiverDepthPlane = p.receiverDepth;
                c.receiverFlagsPlane = p.receiverFlags;
            }
            return c;
        }

        uint16_t RowDepth(const RenderTarget& rt, const PaletteIndex* rowStart) const
        {
            if (!flat)
                return DepthEncode(value);
            const int32_t rowIdx = static_cast<int32_t>((rowStart - rt.bits) / rt.LineStride());
            const int32_t worldY = ZoomLevel{ zoom }.ApplyTo(rt.y + rowIdx);
            return DepthEncode(flatBase + 2 * worldY);
        }

        uint16_t* At(const PaletteIndex* dst) const
        {
            return plane + (dst - base);
        }

        uint8_t* SwayAt(const PaletteIndex* dst) const
        {
            return swayPlane + (dst - base);
        }

        uint8_t* ShadowFlagsAt(const PaletteIndex* dst) const
        {
            return shadowFlagsPlane + (dst - base);
        }

        uint16_t* ShadowHeightAt(const PaletteIndex* dst) const
        {
            return shadowHeightPlane + (dst - base);
        }

        uint16_t* ReceiverDepthAt(const PaletteIndex* dst) const
        {
            return receiverDepthPlane + (dst - base);
        }

        uint8_t* ReceiverFlagsAt(const PaletteIndex* dst) const
        {
            return receiverFlagsPlane + (dst - base);
        }

        uint16_t RowShadowHeight(const RenderTarget& rt, const PaletteIndex* rowStart, uint16_t rowDepth) const
        {
            if ((shadowFlags & kShadowCaster) == 0)
                return 0;
            const int32_t rowIdx = static_cast<int32_t>((rowStart - rt.bits) / rt.LineStride());
            const int32_t worldY = ZoomLevel{ zoom }.ApplyTo(rt.y + rowIdx);
            const int32_t height = DepthDecode(rowDepth) / 2 - worldY - shadowGroundZ;
            return height <= 0 ? 0 : static_cast<uint16_t>(height > 32000 ? 32000 : height);
        }

        // Bend weight (0..15, quadratic from the foot to the top of the sprite) and phase of one row, packed in a byte.
        uint8_t RowSway(const RenderTarget& rt, const PaletteIndex* rowStart) const
        {
            if (!sway || swayPlane == nullptr)
                return 0;
            const int32_t rowIdx = static_cast<int32_t>((rowStart - rt.bits) / rt.LineStride());
            const int32_t worldY = ZoomLevel{ zoom }.ApplyTo(rt.y + rowIdx);
            const int32_t above = swayBottom - worldY; // rows above the foot
            if (above <= 0)
                return 0;
            const float f = static_cast<float>(above) / static_cast<float>(swayHeight > 0 ? swayHeight : 1);
            const int32_t weight = static_cast<int32_t>(f * f * 15.0f + 0.5f);
            return static_cast<uint8_t>(((weight > 15 ? 15 : weight) << 4) | (swayPhase & 15));
        }
    };
} // namespace OpenRCT2::Drawing
