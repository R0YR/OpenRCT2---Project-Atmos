#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace OpenRCT2::Ui::Atmosphere::Coordinates
{
    inline std::pair<float, float> Rotate(float x, float y, int rotation)
    {
        switch (rotation & 3)
        {
            case 1: return { y, -x };
            case 2: return { -x, -y };
            case 3: return { -y, x };
            default: return { x, y };
        }
    }

    inline std::pair<float, float> Unproject(float sx, float sy, float z, int rotation)
    {
        return Rotate(sy + z - sx * 0.5f, sy + z + sx * 0.5f, -rotation);
    }

    inline float RayDayStrength(float elevation)
    {
        const float t = std::clamp((elevation - 4.0f) / 38.0f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    inline float RayLengthScale(float elevation)
    {
        return 1.65f - 0.85f * std::sin(std::clamp(elevation, 0.0f, 90.0f) * 0.01745329252f);
    }
}
