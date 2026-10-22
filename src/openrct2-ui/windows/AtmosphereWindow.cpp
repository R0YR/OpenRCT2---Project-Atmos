/*****************************************************************************
 * Stand-alone "Atmosphere" window (fork addition): the settings pages of the effects in a window of their own, with
 * a column of sub page buttons on the left. Licensed GPLv3 like OpenRCT2.
 *****************************************************************************/

#include "../drawing/Atmosphere.h"
#include "OptionsAtmosphere.h"

#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2/interface/ColourWithFlags.h>
#include <openrct2/interface/Window.h>
#include <openrct2/ui/WindowManager.h>

namespace OpenRCT2::Ui::Windows
{
    static constexpr ScreenSize kAtmosphereWindowSize = { 424, 372 };

    enum AtmosphereWindowWidgets : WidgetIndex
    {
        WIDX_ATMOSPHERE_BACKGROUND,
        WIDX_ATMOSPHERE_TITLE,
        WIDX_ATMOSPHERE_CLOSE,
    };

    class AtmosphereWindow final : public Window
    {
        AtmosphereOptionsPage _page;
        std::vector<Widget> _widgets;

        void Build()
        {
            _widgets.clear();
            auto frame = makeWindowShim(kStringIdNone, kAtmosphereWindowSize);
            frame[1].setString("Atmosphere");
            _widgets.assign(frame.begin(), frame.end());
            _page.AppendWidgets(_widgets);
            _page.OnPageOpened();
            setWidgets(_widgets);
            invalidate();
            onPrepareDraw();
            onResize();
            initScrollWidgets();
            invalidate();
        }

    public:
        void onOpen() override
        {
            int32_t sub;
            if (Atmosphere::ConsumePendingOptionsSubPage(sub))
                _page.SetSubPage(sub);
            _page.SetSideLayout(true);
            // the colours of the Options window
            colours[0] = ColourWithFlags{ Drawing::Colour::lightBlue };
            colours[1] = ColourWithFlags{ Drawing::Colour::lightBlue };
            colours[2] = ColourWithFlags{ Drawing::Colour::lightBlue };
            Build();
        }

        void onClose() override
        {
            _page.ToolAbort();
        }

        void onToolDown(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            _page.ToolDown(*this, widgetIndex, screenCoords);
        }

        void onToolDrag(WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords) override
        {
            _page.ToolDrag(*this, widgetIndex, screenCoords);
        }

        void onToolUp(WidgetIndex, const ScreenCoordsXY&) override
        {
            _page.ToolUp();
        }

        void onToolAbort(WidgetIndex) override
        {
            _page.ToolAbort();
        }

        void onMouseUp(WidgetIndex widgetIndex) override
        {
            if (widgetIndex == WIDX_ATMOSPHERE_CLOSE)
            {
                close();
                return;
            }
            _page.MouseUp(*this, widgetIndex);
            if (_page.ConsumeRebuild())
                Build();
        }

        void onMouseDown(WidgetIndex widgetIndex) override
        {
            _page.MouseDown(*this, widgetIndex);
        }

        void onDropdown(WidgetIndex widgetIndex, int32_t dropdownIndex) override
        {
            _page.Dropdown(*this, widgetIndex, dropdownIndex);
        }

        void onUpdate() override
        {
            _page.Update(*this);
        }

        void onPrepareDraw() override
        {
            _page.PrepareDraw(*this);
        }

        ScreenSize onScrollGetSize(int32_t scrollIndex) override
        {
            return _page.ScrollGetSize(scrollIndex);
        }

        void onDraw(Drawing::RenderTarget& rt) override
        {
            drawWidgets(rt);
            _page.DrawNavigation(*this, rt);
        }
    };

    WindowBase* AtmosphereWindowOpen()
    {
        auto* windowMgr = GetWindowManager();
        return windowMgr->FocusOrCreate<AtmosphereWindow>(WindowClass::atmosphere, kAtmosphereWindowSize, WindowFlag::centreScreen);
    }
} // namespace OpenRCT2::Ui::Windows
