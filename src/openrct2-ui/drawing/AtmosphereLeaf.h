#pragma once

#include "AtmosphereCoordinates.h"
#include <cstdint>

namespace OpenRCT2::Ui::Atmosphere::Leaves
{
    struct Leaf
    {
        float x = 0, y = 0, z = 0, groundZ = 0;
        float speed = 8, drift = 0, age = 0, life = 10, phase = 0;
        uint8_t r = 0, g = 0, b = 0;
    };

    inline Leaf FromPixel(float sx, float sy, uint16_t depth, int rotation)
    {
        Leaf leaf;
        const float d = static_cast<float>((static_cast<int32_t>(depth) - 32768) * 2);
        leaf.z = d * 0.5f - sy;
        const auto point = Coordinates::Unproject(sx, sy, leaf.z, rotation);
        leaf.x = point.first;
        leaf.y = point.second;
        return leaf;
    }

    inline std::pair<float, float> Project(const Leaf& leaf, int rotation, float wpp, float ox, float oy)
    {
        const auto [rx, ry] = Coordinates::Rotate(leaf.x, leaf.y, rotation);
        return { (ry - rx) / wpp - ox, ((rx + ry) * 0.5f - leaf.z) / wpp - oy };
    }

    inline float Depth(const Leaf& leaf, int rotation)
    {
        const auto [rx, ry] = Coordinates::Rotate(leaf.x, leaf.y, rotation);
        return rx + ry;
    }

    inline bool Visible(uint16_t sceneDepth, float leafDepth, float wpp)
    {
        return sceneDepth == 0xFFFF
            || static_cast<float>((static_cast<int32_t>(sceneDepth) - 32768) * 2)
                <= leafDepth + std::max(2.0f, wpp * 2.0f);
    }

    inline void Advance(Leaf& leaf, float dt, float windX, float windY)
    {
        leaf.age += dt;
        const float flutter = leaf.age * 1.6f + leaf.phase;
        leaf.x += (windX + leaf.drift + std::sin(flutter) * 1.4f) * dt;
        leaf.y += (windY + std::cos(flutter) * 1.1f) * dt;
        leaf.z -= leaf.speed * dt;
    }

    // Three distinct tones and a soft, translucent perimeter in a small tumbling leaf silhouette.
    inline constexpr int8_t kShape[3][3] = { { -1, 2, -1 }, { 0, 1, 2 }, { -1, 0, 1 } };
    inline constexpr float kShade[3] = { 0.65f, 0.9f, 1.16f };
    inline constexpr float kOpacity[3] = { 0.46f, 0.72f, 0.56f };
}
