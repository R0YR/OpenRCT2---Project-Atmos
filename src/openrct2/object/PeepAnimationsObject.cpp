/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "PeepAnimationsObject.h"

#include "../Diagnostic.h"
#include "../core/EnumMap.hpp"
#include "../core/Guard.hpp"
#include "../core/Json.hpp"
#include "../drawing/Drawing.h"
#include "../drawing/Drawing.Sprite.h"
#include "../drawing/ImageImporter.h"
#include "../drawing/RenderTarget.h"
#include "../peep/PeepAnimations.h"
#include "../peep/PeepSpriteIds.h"
#include "../rct12/RCT12.h"
#include "ObjectRepository.h"

#include <stdexcept>

namespace OpenRCT2
{
    static const EnumMap<AnimationPeepType> animationPeepTypeMap(
        {
            { "guest", AnimationPeepType::guest },
            { "handyman", AnimationPeepType::handyman },
            { "mechanic", AnimationPeepType::mechanic },
            { "security", AnimationPeepType::security },
            { "entertainer", AnimationPeepType::entertainer },
        });

    static const EnumMap<GuestAppearance> guestAppearanceMap({
        { "normal", GuestAppearance::normal },
        { "bob", GuestAppearance::bob },
        { "ponytail", GuestAppearance::ponytail },
        { "longHair", GuestAppearance::longHair },
        { "childGirl", GuestAppearance::childGirl },
        { "childBoy", GuestAppearance::childBoy },
    });

    static GuestAppearanceSprite DecodeAppearanceSprite(const G1Element& image)
    {
        GuestAppearanceSprite sprite{ image.width, image.height, image.xOffset, image.yOffset, {} };
        if (image.offset == nullptr || image.width <= 0 || image.height <= 0 || image.flags.has(G1Flag::isPalette))
            return sprite;
        sprite.pixels.resize(static_cast<size_t>(image.width) * image.height);
        Drawing::RenderTarget rt;
        rt.bits = reinterpret_cast<Drawing::PaletteIndex*>(sprite.pixels.data());
        rt.width = image.width;
        rt.height = image.height;
        DrawSpriteArgs args(ImageId(), Drawing::PaletteMap::GetDefault(), image, 0, 0, image.width, image.height, rt.bits);
        GfxSpriteToBuffer(rt, args);
        return sprite;
    }

    static Drawing::ImageImportResult EncodeAppearanceSprite(const GuestAppearanceSprite& sprite)
    {
        Image image;
        image.Width = sprite.width;
        image.Height = sprite.height;
        image.Stride = sprite.width;
        image.Depth = 8;
        image.Pixels = sprite.pixels;
        Drawing::ImageImportMeta meta;
        meta.palette = Drawing::Palette::keepIndices;
        meta.offset = { sprite.xOffset, sprite.yOffset };
        return Drawing::ImageImporter{}.Import(image, meta);
    }

    void PeepAnimationsObject::CreateAppearanceImages()
    {
        auto& table = GetImageTable();
        const auto numImages = table.GetCount();
        if (_animationGroups.size() <= EnumValue(PeepAnimationGroup::sandwich))
            throw std::runtime_error("Guest appearance requires all guest item animation groups");
        std::vector<uint8_t> directions(numImages);
        std::vector<bool> hats(numImages);
        for (size_t groupIndex = 0; groupIndex < _animationGroups.size(); ++groupIndex)
        {
            for (const auto& [name, type] : getAnimationsByPeepType(_peepType))
            {
                const auto& animation = _animationGroups[groupIndex][type];
                if (animation.frameOffsets.empty())
                    throw std::runtime_error("Guest appearance has a missing animation");
                const auto rotations = type == PeepAnimationType::hanging ? 1u : 4u;
                const auto length = (*std::max_element(animation.frameOffsets.begin(), animation.frameOffsets.end()) + 1) * rotations;
                if (animation.imageTableOffset >= numImages || length > numImages - animation.imageTableOffset)
                    throw std::runtime_error("Guest appearance animation exceeds its image table");
                for (auto i = 0u; i < length && animation.imageTableOffset + i < numImages; ++i)
                {
                    directions[animation.imageTableOffset + i] = i % rotations;
                    if (groupIndex == EnumValue(PeepAnimationGroup::hat))
                        hats[animation.imageTableOffset + i] = true;
                }
            }
        }
        // The canonical inline sprite depicts the original guest. Use our actual walking pose instead.
        if (numImages > 1)
            table.ReplaceImage(0, table.GetImages()[_animationGroups[0][PeepAnimationType::walking].imageTableOffset]);
        for (auto i = 0u; i < numImages; ++i)
        {
            const auto source = DecodeAppearanceSprite(table.GetImages()[i]);
            if (source.pixels.empty())
                continue;
            const auto sprite = TransformGuestAppearance(source, _appearance, directions[i], hats[i]);
            const auto encoded = EncodeAppearanceSprite(sprite);
            table.ReplaceImage(i, encoded.Element);
        }
        if (IsChildAppearance(_appearance))
        {
            _accessoryImageOffset = table.GetCount();
            for (const auto base : { kPeepSpriteHatItemStart, kPeepSpriteBalloonItemStart, kPeepSpriteUmbrellaItemStart })
            {
                for (auto i = 0u; i < 32; ++i)
                {
                    const auto* source = GfxGetG1Element(base + i);
                    if (source == nullptr)
                        throw std::runtime_error("Missing guest accessory sprite");
                    const auto decoded = DecodeAppearanceSprite(*source);
                    if (decoded.pixels.empty())
                        table.AddImage(source);
                    else
                    {
                        const auto encoded = EncodeAppearanceSprite(ScaleGuestAppearanceSprite(decoded));
                        table.AddImage(&encoded.Element);
                    }
                }
            }
        }
    }

    void PeepAnimationsObject::Load()
    {
        auto numImages = GetImageTable().GetCount();
        if (numImages == 0)
            return;

        _imageOffsetId = LoadImages();

        // Set loaded image offsets for all animations
        for (auto groupKey = 0u; groupKey < _animationGroups.size(); groupKey++)
        {
            auto& group = _animationGroups[groupKey];
            auto& requiredAnimationMap = getAnimationsByPeepType(_peepType);
            for (auto& [typeStr, typeEnum] : requiredAnimationMap)
            {
                group[typeEnum].baseImage = _imageOffsetId + group[typeEnum].imageTableOffset;
                const auto rotations = _appearance != GuestAppearance::normal && typeEnum != PeepAnimationType::hanging ? 4 : 1;
                group[typeEnum].bounds = inferMaxAnimationDimensions(group[typeEnum], rotations);

                // Balloons, hats and umbrellas are painted separately, so the inference
                // algorithm doesn't account for those. We manually compensate for these here.
                // Between 8-12 pixels are needed, depending on rotation, so we're generalising.
                auto pag = PeepAnimationGroup(groupKey);
                if (pag == PeepAnimationGroup::balloon || pag == PeepAnimationGroup::hat || pag == PeepAnimationGroup::umbrella)
                {
                    group[typeEnum].bounds.spriteHeightNegative += 12;
                }
            }
        }
    }

    void PeepAnimationsObject::Unload()
    {
        UnloadImages();
    }

    void PeepAnimationsObject::ReadJson(IReadObjectContext* context, json_t& root)
    {
        Guard::Assert(root.is_object(), "PeepAnimationsObject::ReadJson expects parameter root to be an object");
        PopulateTablesFromJson(context, root);

        Guard::Assert(root["properties"].is_object(), "PeepAnimationsObject::ReadJson expects properties to be an object");
        ReadProperties(root["properties"]);

        auto& requiredAnimationMap = getAnimationsByPeepType(_peepType);
        _animationGroups.clear();

        Guard::Assert(
            root["animationGroups"].is_array(), "PeepAnimationsObject::ReadJson expects animationGroups to be an array");
        for (auto& groupJson : root["animationGroups"])
        {
            Guard::Assert(
                groupJson["animations"].is_object(), "PeepAnimationsObject::ReadJson expects animations to be an array");

            PeepAnimations group = ReadAnimations(requiredAnimationMap, groupJson["animations"]);

            if (groupJson.contains("legacyPosition"))
            {
                auto position = Json::GetNumber<uint8_t>(groupJson["legacyPosition"]);
                if (position <= EnumValue(RCT12PeepAnimationGroup::count))
                {
                    group.legacyPosition = static_cast<RCT12PeepAnimationGroup>(position);
                }
            }
            else
            {
                group.legacyPosition = RCT12PeepAnimationGroup::invalid;
            }

            group.isSlowWalking = Json::GetBoolean(groupJson["isSlowWalking"], false);
            group.scriptName = Json::GetString(groupJson["scriptName"], std::string(GetIdentifier()));

            _animationGroups.push_back(group);
        }
        if (_appearance != GuestAppearance::normal && context->ShouldLoadImages())
            CreateAppearanceImages();
    }

    PeepAnimations PeepAnimationsObject::ReadAnimations(
        const EnumMap<PeepAnimationType>& requiredAnimationMap, json_t& animations)
    {
        PeepAnimations group{};
        for (auto& [typeStr, typeEnum] : requiredAnimationMap)
        {
            if (!animations.contains(typeStr))
            {
                // Successive animation groups can copy the basic animations from the primary group
                if (!_animationGroups.empty())
                {
                    auto& referenceAnim = _animationGroups[0][typeEnum];
                    if (referenceAnim.imageTableOffset != 0)
                    {
                        LOG_VERBOSE("Copying animation '%s' from primary group", std::string(typeStr).c_str());
                        std::vector<uint8_t> sequence = referenceAnim.frameOffsets;
                        group[typeEnum] = {
                            .imageTableOffset = referenceAnim.imageTableOffset,
                            .frameOffsets = sequence,
                        };
                        continue;
                    }
                }

                // No primary animation bail us out -- error here!
                LOG_ERROR("Required animation does not exist: %s", std::string(typeStr).c_str());
                continue;
            }

            // The `.data()` here is a workaround for older versions of nlohmann-json.
            // TODO: remove when we no longer support Ubuntu 22.04 (Jammy).
            auto& animJson = animations[typeStr.data()];

            // Store animation sequence in vector
            auto sequence = animJson["sequence"].get<std::vector<uint8_t>>();

            group[typeEnum] = {
                .imageTableOffset = Json::GetNumber<uint16_t>(animJson["offset"]),
                .frameOffsets = sequence,
            };
        }

        return group;
    }

    void PeepAnimationsObject::ReadProperties(json_t& props)
    {
        Guard::Assert(props["peepType"].is_string(), "PeepAnimationsObject::ReadProperties expects peepType to be a string");
        _peepType = animationPeepTypeMap[Json::GetString(props["peepType"])];

        Guard::Assert(
            props["noRandomPlacement"].is_boolean(),
            "PeepAnimationsObject::ReadProperties expects noRandomPlacement to be a boolean");
        _noRandomPlacement = Json::GetBoolean(props["noRandomPlacement"], false);

        const auto appearanceName = Json::GetString(props["guestAppearance"], "normal");
        const auto appearance = guestAppearanceMap.TryGet(appearanceName);
        if (!appearance.has_value() || (*appearance != GuestAppearance::normal && _peepType != AnimationPeepType::guest))
            throw std::runtime_error("Invalid guestAppearance property");
        _appearance = *appearance;
    }

    std::string PeepAnimationsObject::GetCostumeName() const
    {
        return GetStringTable().GetString(ObjectStringID::name);
    }

    ImageIndex PeepAnimationsObject::GetInlineImageId() const
    {
        return _imageOffsetId;
    }

    AnimationPeepType PeepAnimationsObject::GetPeepType() const
    {
        return _peepType;
    }

    const PeepAnimation& PeepAnimationsObject::GetPeepAnimation(PeepAnimationGroup animGroup, PeepAnimationType animType) const
    {
        return _animationGroups[EnumValue(animGroup)][animType];
    }

    const SpriteBounds& PeepAnimationsObject::GetSpriteBounds(PeepAnimationGroup animGroup, PeepAnimationType animType) const
    {
        return _animationGroups[EnumValue(animGroup)][animType].bounds;
    }

    size_t PeepAnimationsObject::GetNumAnimationGroups() const
    {
        return _animationGroups.size();
    }

    RCT12PeepAnimationGroup PeepAnimationsObject::GetLegacyPosition(PeepAnimationGroup animGroup) const
    {
        // Includes embedded v1.0 descriptors: variants must never override legacy guest import.
        if (GetIdentifier().starts_with("atmosphere.peep_animations."))
            return RCT12PeepAnimationGroup::invalid;
        return _animationGroups[EnumValue(animGroup)].legacyPosition;
    }

    std::string_view PeepAnimationsObject::GetScriptName(PeepAnimationGroup animGroup) const
    {
        return _animationGroups[EnumValue(animGroup)].scriptName;
    }

    bool PeepAnimationsObject::IsSlowWalking(PeepAnimationGroup animGroup) const
    {
        return _animationGroups[EnumValue(animGroup)].isSlowWalking;
    }

    ImageIndex PeepAnimationsObject::GetAccessoryImage(PeepAnimationGroup group, uint8_t frame, uint8_t direction) const
    {
        ImageIndex offset;
        ImageIndex original;
        switch (group)
        {
            case PeepAnimationGroup::hat: offset = 0; original = kPeepSpriteHatItemStart; break;
            case PeepAnimationGroup::balloon: offset = 32; original = kPeepSpriteBalloonItemStart; break;
            case PeepAnimationGroup::umbrella: offset = 64; original = kPeepSpriteUmbrellaItemStart; break;
            default: return kImageIndexUndefined;
        }
        const auto base = _accessoryImageOffset == kImageIndexUndefined ? original : _imageOffsetId + _accessoryImageOffset + offset;
        return base + (direction & 3) + (frame % 8) * 4;
    }

    void PeepAnimationsObject::DrawPreview(Drawing::RenderTarget& rt, int32_t width, int32_t height) const
    {
        auto centre = ScreenCoordsXY{ width / 2, height / 2 };

        // Draw inline sprite in the centre
        GfxDrawSprite(rt, ImageId(_imageOffsetId + 0), centre + ScreenCoordsXY{ -8, -8 });

        // Draw four cardinal directions around the inline sprite
        GfxDrawSprite(
            rt, ImageId(_imageOffsetId + 4, Drawing::Colour::brightRed, Drawing::Colour::darkWater),
            centre + ScreenCoordsXY{ -32, -24 });
        GfxDrawSprite(
            rt, ImageId(_imageOffsetId + 2, Drawing::Colour::brightRed, Drawing::Colour::darkWater),
            centre + ScreenCoordsXY{ +32, +32 });
        GfxDrawSprite(
            rt, ImageId(_imageOffsetId + 1, Drawing::Colour::brightRed, Drawing::Colour::darkWater),
            centre + ScreenCoordsXY{ +32, -24 });
        GfxDrawSprite(
            rt, ImageId(_imageOffsetId + 3, Drawing::Colour::brightRed, Drawing::Colour::darkWater),
            centre + ScreenCoordsXY{ -32, +32 });
    }

    void PeepAnimationsObject::SetRepositoryItem(ObjectRepositoryItem* item) const
    {
        item->PeepAnimationsInfo.PeepType = EnumValue(_peepType);
    }
} // namespace OpenRCT2
