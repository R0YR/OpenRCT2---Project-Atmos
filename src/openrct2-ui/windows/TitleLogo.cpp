/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/core/Imaging.h>
#include <openrct2/core/Path.hpp>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Image.h>
#include <openrct2/drawing/ImageImporter.h>
#include <openrct2/interface/ColourWithFlags.h>
#include <openrct2/ui/WindowManager.h>

#include <algorithm>
#include <exception>
#include <vector>

namespace OpenRCT2::Ui::Windows
{
    static constexpr ScreenSize kWindowSize = { 232, 136 };
    // RLE stores each run's starting x coordinate in one byte.
    static constexpr int32_t kProjectSpriteWidth = 224;

    enum WindowTitleLogoWidgetIdx : WidgetIndex
    {
        WIDX_LOGO
    };

    static constexpr auto _titleLogoWidgets = makeWidgets(
        makeWidget({ 0, 0 }, kWindowSize, WidgetType::imgBtn, WindowColour::primary));

    class TitleLogoWindow final : public Window
    {
        Image _projectLogo;
        std::vector<Drawing::ImageImportResult> _projectSprites;
        ImageIndex _projectImage = kImageIndexUndefined;
        int32_t _projectWidth = 0;

        void releaseProjectSprite()
        {
            GfxObjectFreeImages(_projectImage, static_cast<uint32_t>(_projectSprites.size()));
            _projectImage = kImageIndexUndefined;
            _projectSprites.clear();
        }

        void prepareProjectSprite(int32_t targetWidth)
        {
            if (targetWidth == _projectWidth)
                return;
            releaseProjectSprite();
            _projectWidth = targetWidth;
            if (targetWidth <= 0 || _projectLogo.Width == 0)
                return;

            // Keep the supplied PNG's alpha. Empty letters/gaps never paint a window background.
            Image resized;
            resized.Width = targetWidth;
            resized.Height = std::max(1u, _projectLogo.Height * resized.Width / _projectLogo.Width);
            resized.Depth = 32;
            resized.Stride = resized.Width * 4;
            resized.Pixels.resize(static_cast<size_t>(resized.Stride) * resized.Height);
            for (uint32_t y = 0; y < resized.Height; y++)
                for (uint32_t x = 0; x < resized.Width; x++)
                {
                    const auto sx = x * _projectLogo.Width / resized.Width;
                    const auto sy = y * _projectLogo.Height / resized.Height;
                    std::copy_n(&_projectLogo.Pixels[sy * _projectLogo.Stride + sx * 4], 4,
                        &resized.Pixels[y * resized.Stride + x * 4]);
                }
            // Stay below the RLE byte offset limit, including transparent gaps near a strip's right edge.
            std::vector<G1Element> elements;
            for (int32_t left = 0; left < targetWidth; left += kProjectSpriteWidth)
            {
                Drawing::ImageImportMeta meta;
                meta.importMode = Drawing::ImportMode::closest;
                meta.srcOffset = { left, 0 };
                meta.srcSize = { std::min(kProjectSpriteWidth, targetWidth - left), static_cast<int32_t>(resized.Height) };
                _projectSprites.push_back(Drawing::ImageImporter().Import(resized, meta));
                elements.push_back(_projectSprites.back().Element);
            }
            _projectImage = GfxObjectAllocateImages(elements.data(), static_cast<uint32_t>(elements.size()));
        }

    public:
        /**
         * Creates the window containing the logo and the expansion packs on the title screen.
         *  rct2: 0x0066B679 (part of 0x0066B3E8)
         */
        void onOpen() override
        {
            width = ContextGetWidth();
            setWidgets(_titleLogoWidgets);
            WindowInitScrollWidgets(*this);
            colours[0] = ColourWithFlags{ Drawing::Colour::grey }.withFlag(ColourFlag::translucent, true);
            colours[1] = ColourWithFlags{ Drawing::Colour::grey }.withFlag(ColourFlag::translucent, true);
            colours[2] = ColourWithFlags{ Drawing::Colour::grey }.withFlag(ColourFlag::translucent, true);
            try
            {
                const auto path = Path::Combine(GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::openrct2),
                    "project-atmos-logo.png");
                _projectLogo = Imaging::ReadFromFile(path, ImageFormat::png32);
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("Unable to load Project:Atmos title logo: %s", e.what());
            }
        }

        void onClose() override
        {
            releaseProjectSprite();
        }

        void onPrepareDraw() override
        {
            // Centre on the full screen; reserve the OpenRCT2 logo's width on both sides to avoid overlap.
            width = ContextGetWidth();
            prepareProjectSprite(std::clamp<int32_t>(ContextGetWidth() - 480, 0, 672) / 2);
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            switch (widgetIndex)
            {
                case WIDX_LOGO:
                    AboutOpen();
                    break;
            }
        }

        /**
         *
         *  rct2: 0x0066B872
         */
        void onDraw(Drawing::RenderTarget& rt) override
        {
            auto screenCoords = windowPos + ScreenCoordsXY{ 2, 2 };
            GfxDrawSprite(rt, ImageId(SPR_G2_LOGO), screenCoords);
            GfxDrawSprite(rt, ImageId(SPR_G2_TITLE), screenCoords + ScreenCoordsXY{ 104, 18 });
            if (_projectImage != kImageIndexUndefined)
            {
                const int32_t top = (kWindowSize.height - _projectSprites.front().Element.height) / 2;
                for (size_t i = 0; i < _projectSprites.size(); i++)
                    GfxDrawSprite(rt, ImageId(_projectImage + static_cast<ImageIndex>(i)),
                        windowPos + ScreenCoordsXY{ (ContextGetWidth() - _projectWidth) / 2
                            + static_cast<int32_t>(i) * kProjectSpriteWidth, top });
            }
        }
    };

    WindowBase* TitleLogoOpen()
    {
        auto* windowMgr = GetWindowManager();
        auto* window = windowMgr->BringToFrontByClass(WindowClass::titleLogo);
        if (window == nullptr)
        {
            window = windowMgr->Create<TitleLogoWindow>(
                WindowClass::titleLogo, ScreenCoordsXY(0, 0), kWindowSize,
                { WindowFlag::stickToBack, WindowFlag::transparent, WindowFlag::noTitleBar });
        }
        return window;
    }
} // namespace OpenRCT2::Ui::Windows
