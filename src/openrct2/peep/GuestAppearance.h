/*****************************************************************************
 * Copyright (c) 2026 OpenRCT2 developers
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OpenRCT2
{
    enum class GuestAppearance : uint8_t
    {
        normal,
        bob,
        ponytail,
        longHair,
        childGirl,
        childBoy,
    };

    struct GuestAppearanceSprite
    {
        int32_t width{};
        int32_t height{};
        int32_t xOffset{};
        int32_t yOffset{};
        std::vector<uint8_t> pixels;
    };

    constexpr bool IsChildAppearance(GuestAppearance appearance)
    {
        return appearance == GuestAppearance::childGirl || appearance == GuestAppearance::childBoy;
    }

    // Floor division keeps negative sprite coordinates and the ground origin aligned.
    constexpr int32_t ScaleGuestCoordinate(int32_t value)
    {
        return value >= 0 ? value * 3 / 4 : -((-value * 3 + 3) / 4);
    }

    inline GuestAppearanceSprite ScaleGuestAppearanceSprite(const GuestAppearanceSprite& source)
    {
        if (source.width <= 0 || source.height <= 0
            || source.pixels.size() != static_cast<size_t>(source.width) * source.height)
            return source;

        GuestAppearanceSprite result;
        result.xOffset = ScaleGuestCoordinate(source.xOffset);
        result.yOffset = ScaleGuestCoordinate(source.yOffset);
        result.width = std::max(1, ScaleGuestCoordinate(source.xOffset + source.width - 1) - result.xOffset + 1);
        result.height = std::max(1, ScaleGuestCoordinate(source.yOffset + source.height - 1) - result.yOffset + 1);
        result.pixels.resize(static_cast<size_t>(result.width) * result.height);
        // Sample whole world coordinates, rather than scaling each sprite around its own corner.
        // Accessories therefore use exactly the same transform as the body.
        auto unscale = [](int32_t value) { return value > 0 ? (value * 4 + 2) / 3 : value * 4 / 3; };
        for (int32_t y = 0; y < result.height; ++y)
        {
            const auto sy = std::clamp(unscale(y + result.yOffset) - source.yOffset, 0, source.height - 1);
            for (int32_t x = 0; x < result.width; ++x)
            {
                const auto sx = std::clamp(unscale(x + result.xOffset) - source.xOffset, 0, source.width - 1);
                result.pixels[y * result.width + x] = source.pixels[sy * source.width + sx];
            }
        }
        return result;
    }

    // Author variants in indexed sprite space so all original poses, food, remap colours and
    // timing remain available. This happens once during object loading, never in the painter.
    inline GuestAppearanceSprite TransformGuestAppearance(
        const GuestAppearanceSprite& source, GuestAppearance appearance, uint8_t direction, bool hat = false)
    {
        if (appearance == GuestAppearance::normal || source.width <= 0 || source.height <= 0
            || source.pixels.size() != static_cast<size_t>(source.width) * source.height)
            return source;

        // Hairstyles extend up to two pixels beyond the original silhouette.
        const auto padding = 2;
        GuestAppearanceSprite result{ source.width + padding * 2, source.height + padding * 2,
            source.xOffset - padding, source.yOffset - padding, {} };
        result.pixels.resize(static_cast<size_t>(result.width) * result.height);
        for (int32_t y = 0; y < source.height; ++y)
            std::copy_n(source.pixels.data() + y * source.width, source.width,
                result.pixels.data() + (y + padding) * result.width + padding);

        auto at = [&](int32_t x, int32_t y) -> uint8_t& { return result.pixels[y * result.width + x]; };
        auto put = [&](int32_t x, int32_t y, uint8_t colour) {
            if (x >= 0 && y >= 0 && x < result.width && y < result.height)
                at(x, y) = colour;
        };
        auto skin = [](uint8_t p) { return p >= 106 && p <= 114; };
        auto shirt = [](uint8_t p) { return p >= 243 && p <= 254; };

        int32_t shirtTop = result.height;
        int32_t torsoLeft = result.width;
        int32_t torsoRight = -1;
        for (int32_t y = 2; y < result.height - 2; ++y)
        {
            for (int32_t x = 2; x < result.width - 2; ++x)
            {
                if (shirt(at(x, y)))
                {
                    shirtTop = std::min(shirtTop, y);
                    torsoLeft = std::min(torsoLeft, x);
                    torsoRight = std::max(torsoRight, x);
                }
            }
        }
        // Sparse drowning frames or inline icons aren't bodies; do not invent a head for them.
        if (torsoRight >= torsoLeft)
        {
            int32_t headTop = result.height;
            int32_t headLeft = result.width;
            int32_t headRight = -1;
            // The head is the upper compact brown component near the torso. Raised hands
            // outside that band must not become hair.
            const auto centre = (torsoLeft + torsoRight) / 2;
            for (int32_t y = 2; y < std::min(shirtTop + 2, result.height - 2); ++y)
                for (int32_t x = std::max(2, centre - 3); x <= std::min(result.width - 3, centre + 3); ++x)
                    if (skin(at(x, y)))
                        headTop = std::min(headTop, y);
            if (headTop < result.height)
            {
                for (int32_t y = headTop; y < std::min(headTop + 5, result.height); ++y)
                    for (int32_t x = std::max(2, centre - 3); x <= std::min(result.width - 3, centre + 3); ++x)
                        if (skin(at(x, y)))
                        {
                            headLeft = std::min(headLeft, x);
                            headRight = std::max(headRight, x);
                        }
                const uint8_t darkHair = appearance == GuestAppearance::longHair ? 106
                    : appearance == GuestAppearance::bob ? 37 : 107;
                const uint8_t lightHair = appearance == GuestAppearance::longHair ? 108
                    : appearance == GuestAppearance::bob ? 41 : 109;
                const auto back = (direction & 3) == 1 || (direction & 3) == 2;
                for (int32_t y = headTop; y < std::min(headTop + (back ? 3 : 2), result.height); ++y)
                    for (int32_t x = headLeft; x <= headRight; ++x)
                        if (skin(at(x, y)))
                            at(x, y) = ((x + y) & 1) ? darkHair : lightHair;

                const auto leftBack = (direction & 3) < 2;
                const auto backX = leftBack ? headLeft : headRight;
                const auto side = leftBack ? -1 : 1;
                if (appearance == GuestAppearance::bob)
                {
                    for (int32_t y = headTop + 1; y < headTop + 5; ++y)
                        put(backX, y, y & 1 ? darkHair : lightHair);
                    put(backX + side, headTop + 3, darkHair);
                }
                else if (appearance == GuestAppearance::ponytail || appearance == GuestAppearance::childGirl)
                {
                    for (int32_t y = headTop + 2; y < headTop + 6; ++y)
                        put(backX + side, y, y & 1 ? darkHair : lightHair);
                    put(backX + side, headTop + 1, 248); // Matching hair tie.
                }
                else if (appearance == GuestAppearance::longHair)
                {
                    for (int32_t y = headTop + 1; y < headTop + 8; ++y)
                    {
                        put(backX, y, lightHair);
                        put(backX + side, y, darkHair);
                    }
                }
                else if (appearance == GuestAppearance::childBoy && !hat)
                {
                    put(centre, headTop - 1, darkHair);
                    put(centre + 1, headTop - 1, lightHair);
                }
            }

            // Clothing stays vanilla: only hairstyles differ, so shirt, trousers and shoes keep their original pixels and guest colours.
        }
        return IsChildAppearance(appearance) ? ScaleGuestAppearanceSprite(result) : result;
    }
}
