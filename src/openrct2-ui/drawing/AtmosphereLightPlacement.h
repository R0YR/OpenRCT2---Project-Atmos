#pragma once

#include <openrct2/world/Location.hpp>
#include <openrct2/interface/ScreenCoords.hpp>
#include <cmath>
#include <optional>

namespace OpenRCT2::Ui::Atmosphere::LightPlacement
{
    // Exact inverse of native integer isometric projection, including odd and negative screen X.
    inline CoordsXY AtPlane(const ScreenCoordsXY& point, int32_t z, int32_t rotation)
    {
        const int32_t rx = point.y + z - (point.x >> 1);
        return CoordsXY{ rx, rx + point.x }.rotate(-rotation);
    }

    inline float PlaneY(int32_t rotatedX, int32_t rotatedY, float z)
    {
        return static_cast<float>((rotatedX + rotatedY) >> 1) - z;
    }

    template<class GroundHeight>
    std::optional<CoordsXYZ> AtTerrainLift(const ScreenCoordsXY& point, float lift, int32_t rotation,
        const CoordsXY& seed, GroundHeight groundHeight)
    {
        auto ground = groundHeight(seed);
        if (!ground.has_value()) return std::nullopt;
        int32_t z = *ground + static_cast<int32_t>(std::lround(lift));
        for (int i = 0; i < 16; i++) {
            const auto xy = AtPlane(point,z,rotation);
            ground = groundHeight(xy);
            if (!ground.has_value()) return std::nullopt;
            const int32_t nextZ = *ground + static_cast<int32_t>(std::lround(lift));
            if (nextZ == z) return CoordsXYZ{xy,z};
            z = nextZ;
        }
        return std::nullopt; // ambiguous cliff intersection: keep the old anchor instead of guessing
    }
}
