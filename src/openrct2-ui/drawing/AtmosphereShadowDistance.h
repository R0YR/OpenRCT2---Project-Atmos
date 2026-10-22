#pragma once

#include <algorithm>
#include <cmath>

namespace OpenRCT2::Ui::Atmosphere::ShadowDistance
{
    inline float SmoothFade(float value, float start, float end)
    {
        const float t = std::clamp((value - start) / (end - start), 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    // Zero disables zoom LOD; finite distances add gradual fade for subpixel shadows.
    inline float ZoomFade(float distanceTiles, float worldPerPixel, bool guest)
    {
        if (distanceTiles <= 0.0f) return 1.0f;
        return guest ? SmoothFade(worldPerPixel, 2.0f, 8.0f) : SmoothFade(worldPerPixel, 4.0f, 16.0f);
    }

    inline float RangeFade(float distanceTiles, float x, float y, float centreX, float centreY, float padding)
    {
        if (distanceTiles <= 0.0f) return 1.0f;
        const float outer = distanceTiles * 32.0f, inner = outer * 0.8f;
        const float dx = x - centreX, dy = y - centreY, squared = dx * dx + dy * dy;
        // Padding encloses the complete displaced shadow, not only the offscreen casting object.
        if (squared <= (inner + padding) * (inner + padding)) return 1.0f;
        if (squared >= (outer + padding) * (outer + padding)) return 0.0f;
        return SmoothFade(std::max(0.0f, std::sqrt(squared) - padding), inner, outer);
    }

    inline bool Intersects(float ax, float ay, float bx, float by, float radius, int width, int height)
    {
        return std::max(ax, bx) + radius > 0.0f && std::min(ax, bx) - radius < width
            && std::max(ay, by) + radius * 0.5f > 0.0f && std::min(ay, by) - radius * 0.5f < height;
    }
}
