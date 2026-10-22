#pragma once

#include <algorithm>
#include <cmath>

namespace OpenRCT2::Ui::Atmosphere::SceneryShadows
{
    // A receiver samples the same bounded sun ray as the ground capsule, at its own height.
    inline float Coverage(float x, float y, float z, float cx, float cy, float bottom, float top,
        float radius, float awayX, float awayY, float invTan, float reach, float softness, float pixelWidth)
    {
        if (z >= top || radius <= 0.0f)
            return 0.0f;
        const auto length = [&](float height) {
            const float ray = std::max(0.0f, height) * invTan;
            return reach * ray / (reach + ray);
        };
        const float a = length(bottom - z), b = length(top - z);
        const float dx = x - cx, dy = y - cy;
        const float along = std::clamp(dx * awayX + dy * awayY, a, b);
        const float ex = dx - awayX * along, ey = dy - awayY * along;
        const float distance = std::sqrt(ex * ex + ey * ey);
        const float edge = std::max(pixelWidth, radius * (0.1f + 0.8f * softness));
        // Soften inward, like the ground capsule; never grow beyond the person's physical footprint.
        const float t = std::clamp((radius - distance) / edge, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}
