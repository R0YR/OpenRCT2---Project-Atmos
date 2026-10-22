/*****************************************************************************
 * "Atmosphere" page of the Options window (fork addition). Licensed GPLv3 like OpenRCT2.
 *
 * The page edits Atmosphere::Settings directly (live preview) and the settings are saved to atmosphere.ini
 * shortly after every change. All texts are plain strings, so no language file entries are needed.
 *
 * The page has sub pages (Effects, Colour, LUTs); every sub page has its own widget list that the Options window
 * installs after AppendWidgets(). ConsumeRebuild() tells the window when the list has to be rebuilt.
 *****************************************************************************/

#pragma once

#include "../drawing/Atmosphere.h"

#include <array>
#include <cstdint>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/interface/Window.h>
#include <vector>

namespace OpenRCT2::Ui::Windows
{
    class AtmosphereOptionsPage
    {
    public:
        AtmosphereOptionsPage();

        // Appends the widgets of the current sub page to `list`, which already holds the common frame and tab widgets.
        void AppendWidgets(std::vector<Widget>& list);
        void AppendWidgetsInner(std::vector<Widget>& list);
        // Sub page selector as a column on the left (the stand-alone Atmosphere window)
        void SetSideLayout(bool side)
        {
            _sideLayout = side;
        }

        void MouseUp(Window& w, WidgetIndex widgetIndex);
        void MouseDown(Window& w, WidgetIndex widgetIndex);
        void Dropdown(Window& w, WidgetIndex widgetIndex, int32_t selectedIndex);
        void Update(Window& w);
        void ToolDown(Window& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
        void ToolDrag(Window& w, WidgetIndex widgetIndex, const ScreenCoordsXY& screenCoords);
        void ToolUp();
        void DragSelectedLight(const ScreenCoordsXY& screenCoords);
        void ToolAbort();
        void PrepareDraw(Window& w);
        void DrawNavigation(Window& w, Drawing::RenderTarget& rt) const;
        ScreenSize ScrollGetSize(int32_t scrollIndex) const;

        void SetSubPage(int32_t subPage)
        {
            _subPage = subPage;
        }

        // Called whenever the widget list was (re)built and shown.
        void OnPageOpened();

        // True once after a sub page switch: the window must rebuild its widget list.
        bool ConsumeRebuild();

        // Pixel-art icon of the page tab. `origin` is the top-left corner of the tab widget.
        static void DrawTabIcon(Drawing::RenderTarget& rt, const ScreenCoordsXY& origin, bool selected, int32_t frame);

        static void DrawCinemaIcon(Drawing::RenderTarget& rt, const ScreenCoordsXY& origin, bool active);

    private:
        static constexpr int32_t kMaxTexts = 56;
        static constexpr int32_t kTextLength = 96;

        struct SliderRef
        {
            int32_t definition;  // index into the slider table of the current sub page
            WidgetIndex label;   // absolute widget index
            WidgetIndex scroll;  // absolute widget index
            int32_t lastPercent; // last percentage seen on the scroll bar (-1 = not initialised yet)
            char* text;          // buffer shown by the label widget
        };
        struct CheckboxRef
        {
            int32_t definition;
            WidgetIndex widget;
        };

        char* NewText(const char* initial);
        void RefreshLuts();
        void UpdateLutTexts();
        void ApplyLut(int32_t item);
        static const char* TooltipFor(const char* name);

        int32_t _subPage = 0;
        bool _rebuild = false;
        WidgetIndex _enable = 0;
        WidgetIndex _hint = 0;
        WidgetIndex _reset = 0;
        std::array<WidgetIndex, 10> _subTabs{};
        bool _sideLayout = false;
        std::vector<SliderRef> _sliders;
        std::vector<CheckboxRef> _checkboxes;
        std::array<std::array<char, kTextLength>, kMaxTexts> _texts{};
        int32_t _textCount = 0;

        // Effects sub page: complete looks
        WidgetIndex _presetDropdown = 0;
        WidgetIndex _presetButton = 0;
        char* _presetText = nullptr;

        // Lights sub page
        enum class LightTool : int32_t { None, Place, Move, Pick, Paste };
        LightTool _tool = LightTool::None;
        bool _dragging = false;
        bool _grabBulb = false;
        int32_t _dragRotation = -1, _dragZoom = 0;
        int32_t _dragZ = 0; // absolute height of the bulb while it is dragged
        int32_t _dragOffsetX = 0, _dragOffsetY = 0; // preserve the exact picked position, including grabs on the stem
        WidgetIndex _lightBulb = 0, _lightPrev = 0, _lightNext = 0, _lightGoto = 0, _lightOn = 0, _lightDelete = 0, _lightCopy = 0, _lightPaste = 0;
        WidgetIndex _toolPlace = 0, _toolMove = 0, _toolPick = 0;
        WidgetIndex _animDropdown = 0, _animButton = 0, _colourDropdown = 0, _colourButton = 0;
        char* _lightLabelText = nullptr;
        char* _lightBulbText = nullptr;
        char* _lightSnapText = nullptr;
        char* _lightOnText = nullptr;
        char* _animText = nullptr;
        int32_t _seenSelected = -2, _seenCount = -1;

        // LUT sub page
        WidgetIndex _lutDropdown = 0;       // shows the active LUT
        WidgetIndex _lutDropdownButton = 0; // opens the list
        WidgetIndex _lutSave = 0;
        WidgetIndex _lutFolder = 0;
        WidgetIndex _lutRefresh = 0;
        WidgetIndex _lutDelete = 0;
        char* _lutCurrentText = nullptr;
        char* _lutDeleteText = nullptr;
        char* _lutInfoText = nullptr;
        char* _lutStatusText = nullptr;
        std::vector<Atmosphere::LutInfo> _luts;
        Atmosphere::ColourGrade _activeGrade{}; // values the active LUT had when it was applied
        bool _activeGradeKnown = false;
        bool _deleteArmed = false;
    };
} // namespace OpenRCT2::Ui::Windows
