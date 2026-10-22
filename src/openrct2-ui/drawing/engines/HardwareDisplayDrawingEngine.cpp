/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include <openrct2-ui/windows/Windows.h>
#include "DrawingEngineFactory.hpp"

#include "../Atmosphere.h"

#include <SDL_hints.h>
#include <SDL_render.h>
#include <SDL_version.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/world/Weather.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/FileSystem.hpp>
#include <openrct2/core/Guard.hpp>
#include <openrct2/drawing/DepthPlane.h>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/drawing/LightFX.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Screenshot.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/ZoomLevel.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/ui/UiContext.h>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

class HardwareDisplayDrawingEngine final : public X8DrawingEngine
{
private:
    constexpr static uint32_t kDirtyVisualTime = 40;
    constexpr static uint32_t kDirtyRegionAlpha = 100;

    IUiContext& _uiContext;
    SDL_Window* _window = nullptr;
    SDL_Renderer* _sdlRenderer = nullptr;
    SDL_Texture* _screenTexture = nullptr;
    SDL_Texture* _scaledScreenTexture = nullptr;
    SDL_PixelFormat* _screenTextureFormat = nullptr;
    uint32_t _paletteHWMapped[256] = { 0 };
    uint32_t _lightPaletteHWMapped[256] = { 0 };

    bool _useVsync = true;

    std::vector<uint32_t> _dirtyVisualsTime;

    bool smoothNN = false;

    std::vector<uint32_t> _atmosphereBuffer;
    bool _frameDirty = true;            // something was repainted since the last Display
    uint32_t _paletteVersion = 0;       // changes with every SetPalette (water animation changes colours without repainting)
    uint32_t _processedPalette = 0, _processedRevision = 0;
    int32_t _processedWidth = 0, _processedHeight = 0;
    bool _processedValid = false;       // the texture holds a finished Atmosphere frame
    bool _atmosphereFrameValid = false; // _atmosphereBuffer holds the frame that is currently on screen
    int32_t _atmosphereShifts[3] = { 0, 0, 0 };
    std::string _atmosphereSettingsFile;

public:
    explicit HardwareDisplayDrawingEngine(IUiContext& uiContext)
        : X8DrawingEngine(uiContext)
        , _uiContext(uiContext)
    {
        _window = static_cast<SDL_Window*>(_uiContext.GetWindow());
        gScreenshotPostProcess = &Atmosphere::ProcessStill;
    }

    ~HardwareDisplayDrawingEngine() override
    {
        Drawing::gAtmosphereOpaqueUi.store(false, std::memory_order_relaxed);
        Drawing::gAtmosphereReplacesWeather.store(false, std::memory_order_relaxed);
        Drawing::gAtmosphereReplacesLightning.store(false, std::memory_order_relaxed);
        Drawing::gSwayEnabled.store(false, std::memory_order_relaxed);
        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        if (_scaledScreenTexture != nullptr)
        {
            SDL_DestroyTexture(_scaledScreenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);
        SDL_DestroyRenderer(_sdlRenderer);
    }

    void Initialise() override
    {
        _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED | (_useVsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    }

    void SetVSync(bool vsync) override
    {
        if (_useVsync != vsync)
        {
            _useVsync = vsync;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_RenderSetVSync(_sdlRenderer, vsync ? 1 : 0);
#else
            SDL_DestroyRenderer(_sdlRenderer);
            _screenTexture = nullptr;
            _scaledScreenTexture = nullptr;
            Initialise();
            Resize(_uiContext->GetWidth(), _uiContext->GetHeight());
#endif
        }
    }

    void Resize(uint32_t width, uint32_t height) override
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);

        SDL_RendererInfo rendererInfo = {};
        int32_t result = SDL_GetRendererInfo(_sdlRenderer, &rendererInfo);
        if (result < 0)
        {
            LOG_WARNING("HWDisplayDrawingEngine::Resize error: %s", SDL_GetError());
            return;
        }
        uint32_t pixelFormat = SDL_PIXELFORMAT_UNKNOWN;
        for (uint32_t i = 0; i < rendererInfo.num_texture_formats; i++)
        {
            uint32_t format = rendererInfo.texture_formats[i];
            if (!SDL_ISPIXELFORMAT_FOURCC(format) && !SDL_ISPIXELFORMAT_INDEXED(format)
                && (pixelFormat == SDL_PIXELFORMAT_UNKNOWN || SDL_BYTESPERPIXEL(format) < SDL_BYTESPERPIXEL(pixelFormat)))
            {
                pixelFormat = format;
            }
        }

        ScaleQuality scaleQuality = GetContext()->GetUiContext().GetScaleQuality();
        if (scaleQuality == ScaleQuality::smoothNearestNeighbour)
        {
            scaleQuality = ScaleQuality::linear;
            smoothNN = true;
        }
        else
        {
            smoothNN = false;
        }

        if (smoothNN)
        {
            if (_scaledScreenTexture != nullptr)
            {
                SDL_DestroyTexture(_scaledScreenTexture);
            }

            char scaleQualityBuffer[4];
            snprintf(scaleQualityBuffer, sizeof(scaleQualityBuffer), "%d", static_cast<int32_t>(scaleQuality));
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create unscaled screen texture (%ux%u, pixelFormat = %u): %s", width,
                height, pixelFormat, SDL_GetError());

            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaleQualityBuffer);

            uint32_t scale = std::ceil(Config::Get().general.windowScale);
            _scaledScreenTexture = SDL_CreateTexture(
                _sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_TARGET, width * scale, height * scale);

            Guard::Assert(
                _scaledScreenTexture != nullptr,
                "Failed to create scaled screen texture (%ux%u, scale = %u, pixelFormat = %u): %s", width, height, scale,
                pixelFormat, SDL_GetError());
        }
        else
        {
            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create screen texture (%ux%u, pixelFormat = %u): %s", width, height,
                pixelFormat, SDL_GetError());
        }

        uint32_t format;
        SDL_QueryTexture(_screenTexture, &format, nullptr, nullptr, nullptr);
        _screenTextureFormat = SDL_AllocFormat(format);

        X8DrawingEngine::Resize(width, height);
    }

    void SetPalette(const GamePalette& palette) override
    {
        _paletteVersion++;
        if (_screenTextureFormat != nullptr)
        {
            for (int32_t i = 0; i < 256; i++)
            {
                _paletteHWMapped[i] = SDL_MapRGB(_screenTextureFormat, palette[i].red, palette[i].green, palette[i].blue);
            }

            if (Config::Get().general.enableLightFx)
            {
                auto& lightPalette = LightFx::GetPalette();
                for (int32_t i = 0; i < 256; i++)
                {
                    const auto& src = lightPalette[i];
                    _lightPaletteHWMapped[i] = SDL_MapRGBA(_screenTextureFormat, src.red, src.green, src.blue, src.alpha);
                }
            }
        }
    }

    void BeginDraw() override
    {
        _frameDirty = false;
        // Publish weather ownership before native rain is drawn, including the first frame.
        if (_atmosphereSettingsFile.empty())
        {
            auto userDir = GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user);
            _atmosphereSettingsFile = (fs::u8path(userDir) / "atmosphere.ini").string();
        }
        Atmosphere::IsActive(_atmosphereSettingsFile.c_str());
        X8DrawingEngine::BeginDraw();
    }

    void EndDraw() override
    {
        X8DrawingEngine::EndDraw();

        Display();
    }

protected:
    void OnDrawDirtyBlock(int32_t left, int32_t top, int32_t right, int32_t bottom) override
    {
        _frameDirty = true;
        if (gShowDirtyVisuals)
        {
            const auto columns = ((right - left) + (_invalidationGrid.getBlockWidth() - 1)) / _invalidationGrid.getBlockWidth();
            const auto rows = ((bottom - top) + (_invalidationGrid.getBlockHeight() - 1)) / _invalidationGrid.getBlockHeight();
            const auto firstRow = top / _invalidationGrid.getBlockHeight();
            const auto firstColumn = left / _invalidationGrid.getBlockWidth();

            for (uint32_t y = 0; y < rows; y++)
            {
                for (uint32_t x = 0; x < columns; x++)
                {
                    SetDirtyVisualTime(firstColumn + x, firstRow + y, gCurrentRealTimeTicks + kDirtyVisualTime);
                }
            }
        }
    }

private:
    void Display()
    {
        {
            // frame rate log for performance tests (only when the test setting asks for it)
            static auto last = std::chrono::steady_clock::now();
            static int32_t frames = 0;
            static double sum = 0.0;
            const auto now = std::chrono::steady_clock::now();
            sum += std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
            if (++frames >= 300)
            {
                if (Atmosphere::PerfLogEnabled())
                {
                    std::ofstream log(_atmosphereSettingsFile + ".fps.log", std::ios::app);
                    log << "frame interval " << (sum / frames) << " ms (" << (1000.0 * frames / sum) << " fps), world paint " << (static_cast<double>(Drawing::gPaintNanos.load()) * 1e-6 / frames) << " ms and " << (static_cast<double>(Drawing::gPaintPixels.load()) / frames) << " px per frame" << std::endl;
                    Drawing::gPaintNanos = 0;
                    Drawing::gPaintPixels = 0;
                }
                frames = 0;
                sum = 0.0;
            }
        }
        auto* viewport = WindowGetViewport(WindowGetMain());
        _atmosphereFrameValid = false;

        if ((_atmosphereFrameValid = TryDisplayWithAtmosphere()))
        {
            // Atmosphere includes its own object lighting; the legacy LightFX display path must not bypass it.
        }
        else if (Config::Get().general.enableLightFx && viewport != nullptr)
        {
            void* pixels;
            int32_t pitch;
            if (SDL_LockTexture(_screenTexture, nullptr, &pixels, &pitch) == 0)
            {
                LightFx::RenderToTexture(
                    *viewport, pixels, pitch, _bits, _width, _height, _paletteHWMapped, _lightPaletteHWMapped);
                SDL_UnlockTexture(_screenTexture);
            }
        }
        else
        {
            CopyBitsToTexture(
                _screenTexture, _bits, static_cast<int32_t>(_width), static_cast<int32_t>(_height), _paletteHWMapped);
        }
        if (smoothNN)
        {
            SDL_SetRenderTarget(_sdlRenderer, _scaledScreenTexture);
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);

            SDL_SetRenderTarget(_sdlRenderer, nullptr);
            SDL_RenderCopy(_sdlRenderer, _scaledScreenTexture, nullptr, nullptr);
        }
        else
        {
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);
        }

        if (gShowDirtyVisuals)
        {
            RenderDirtyVisuals();
        }

        // Cinema mode: black bars drawn last, above the whole interface
        if (const float bar = Atmosphere::CinemaBarFraction(); bar > 0.0f)
        {
            int32_t outputWidth = 0, outputHeight = 0;
            SDL_GetRendererOutputSize(_sdlRenderer, &outputWidth, &outputHeight);
            const int32_t barHeight = static_cast<int32_t>(static_cast<float>(outputHeight) * bar);
            SDL_SetRenderDrawColor(_sdlRenderer, 0, 0, 0, 255);
            const SDL_Rect top{ 0, 0, outputWidth, barHeight };
            const SDL_Rect bottom{ 0, outputHeight - barHeight, outputWidth, barHeight };
            SDL_RenderFillRect(_sdlRenderer, &top);
            SDL_RenderFillRect(_sdlRenderer, &bottom);
        }

        SDL_RenderPresent(_sdlRenderer);
    }

    // Screenshots must show what the player sees: with Atmosphere active the post-processed frame is saved.
    std::string Screenshot() override
    {
        if (_atmosphereFrameValid && _atmosphereBuffer.size() == static_cast<size_t>(_width) * _height)
        {
            auto path = ScreenshotDumpPixels32(
                _atmosphereBuffer.data(), static_cast<int32_t>(_width), static_cast<int32_t>(_height),
                static_cast<uint32_t>(_atmosphereShifts[0]), static_cast<uint32_t>(_atmosphereShifts[1]),
                static_cast<uint32_t>(_atmosphereShifts[2]));
            if (!path.empty())
                return path;
        }
        return X8DrawingEngine::Screenshot();
    }

    // Converts the 8bpp frame to 32bpp in a CPU buffer, runs the Atmosphere effects on it (UI windows excluded)
    // and uploads the result. Returns false if the pass is inactive/unsupported so the caller uses the normal path.
    bool TryDisplayWithAtmosphere()
    {
        if (_screenTextureFormat == nullptr || _screenTextureFormat->BytesPerPixel != 4 || _width == 0 || _height == 0)
            return false;

        const bool active = Atmosphere::IsActive(_atmosphereSettingsFile.c_str());
        {
            // Wait until the park is loaded (opening it earlier would be undone when the scene changes).
            static int32_t framesSinceStart = 0;
            framesSinceStart++;
            int32_t sub;
            int32_t weatherType;
            if (framesSinceStart > 300 && Atmosphere::TakeDevWeather(weatherType))
            {
                auto& state = getGameState();
                state.weatherCurrent.weatherType = static_cast<Weather::Type>(weatherType);
                const bool snowType = weatherType >= 6;
                const bool heavy = weatherType == 4 || weatherType == 5 || weatherType == 7 || weatherType == 8;
                state.weatherCurrent.level = weatherType >= 3 ? (heavy ? Weather::Level::heavy : Weather::Level::light) : Weather::Level::none;
                state.weatherCurrent.weatherEffect = weatherType < 3 ? Weather::EffectType::none
                    : (snowType ? (weatherType == 8 ? Weather::EffectType::blizzard : Weather::EffectType::snow)
                                : (weatherType == 5 ? Weather::EffectType::storm : Weather::EffectType::rain));
                state.weatherNext = state.weatherCurrent;
            }
            int32_t shotKind;
            if (framesSinceStart > 900 && Atmosphere::TakeDevScreenshot(shotKind))
            {
                if (shotKind >= 2)
                    ScreenshotGiant();
                else
                    gScreenshotCountdown = 2;
            }
            if (framesSinceStart > 900 && Atmosphere::TakeDevOpenOptions(sub))
            {
                Atmosphere::SetPendingOptionsSubPage(sub);
                Windows::AtmosphereWindowOpen();
            }
        }
        if (auto* mainWindow = WindowGetMain(); mainWindow != nullptr && mainWindow->viewport != nullptr)
        {
            int32_t devValue;
            if (Atmosphere::TakeDevZoom(devValue))
                WindowZoomSet(*mainWindow, ZoomLevel{ static_cast<int8_t>(devValue) }, false);
            if (Atmosphere::TakeDevRotate(devValue))
            {
                for (int32_t i = 0; i < ((devValue % 4) + 4) % 4; i++)
                    ViewportRotateSingle(mainWindow, 1);
            }
        }
        SetAuxEnabled(active && Atmosphere::WantsDepth(), active, active && Atmosphere::WantsSway());
        if (!active)
        {
            _processedValid = false;
            return false;
        }
        // Nothing was repainted, no palette change, same settings and no animated effect: the texture already holds the
        // finished frame, so the whole post-process is skipped.
        if (_processedValid && !_frameDirty && _processedPalette == _paletteVersion && _processedRevision == Atmosphere::SettingsRevision()
            && _processedWidth == static_cast<int32_t>(_width) && _processedHeight == static_cast<int32_t>(_height)
            && !Atmosphere::IsAnimated())
        {
            _atmosphereFrameValid = !_atmosphereBuffer.empty();
            return true;
        }

        const auto width = static_cast<int32_t>(_width);
        const auto height = static_cast<int32_t>(_height);
        _atmosphereBuffer.resize(static_cast<size_t>(width) * height);

        // Everything except the main (world) window is UI and must stay untouched.
        std::vector<Atmosphere::ExcludeRect> excluded;
        WindowVisitEach([&](WindowBase* w) {
            if (!w->isVisible || w->classification == WindowClass::mainWindow)
                return;
            // The original title windows contain only sprites/text. Their transparent gaps must remain world pixels.
            if (w->classification == WindowClass::titleLogo || w->classification == WindowClass::titleVersion
                || w->classification == WindowClass::titleExit)
                return;
            if (w->classification == WindowClass::titleMenu || w->classification == WindowClass::topToolbar
                || w->classification == WindowClass::parkInfoPanel || w->classification == WindowClass::dateInfoPanel
                || w->classification == WindowClass::newsTicker || w->classification == WindowClass::gameStatusBar)
            {
                // Protect complete visible controls, including pixels that happen to match the world palette index.
                // Toolbar gaps and the hidden title update row remain transparent.
                for (const auto& widget : w->widgets)
                    if (!widget.isHidden() && widget.type != WidgetType::empty)
                        excluded.push_back({ w->windowPos.x + widget.left, w->windowPos.y + widget.top,
                            w->windowPos.x + widget.right + 1, w->windowPos.y + widget.bottom + 1 });
                return;
            }
            // Floating windows without a background (toolbars, status bar, map tooltip) are see-through: the effects
            // keep running underneath them and Atmosphere detects their pixels by comparing with the world snapshot.
            const bool titleUi = w->classification == WindowClass::titleOptions;
            if (w->flags.has(WindowFlag::noBackground) && !titleUi)
                return; // see-through sprite windows: detected per pixel, no rectangle
            excluded.push_back({ w->windowPos.x, w->windowPos.y, w->windowPos.x + w->width, w->windowPos.y + w->height });
        });

        Atmosphere::ViewInfo view;
        if (auto* mainViewport = WindowGetViewport(WindowGetMain()); mainViewport != nullptr)
        {
            const auto zoom = static_cast<int32_t>(static_cast<int8_t>(mainViewport->zoom));
            view.worldPerPixel = zoom >= 0 ? static_cast<float>(1 << zoom) : 1.0f / static_cast<float>(1 << -zoom);
            view.originX = mainViewport->zoom.ApplyInversedTo(mainViewport->viewPos.x) - mainViewport->pos.x;
            view.originY = mainViewport->zoom.ApplyInversedTo(mainViewport->viewPos.y) - mainViewport->pos.y;
            view.rotation = mainViewport->rotation;
            view.viewportWidth = mainViewport->width;
            view.viewportHeight = mainViewport->height;
        }
        const Atmosphere::PixelLayout layout{ _screenTextureFormat->Rshift, _screenTextureFormat->Gshift,
                                              _screenTextureFormat->Bshift };
        Atmosphere::ProcessFrame(
            reinterpret_cast<const uint8_t*>(_bits), _paletteHWMapped, _atmosphereBuffer.data(), width, height, layout, excluded,
            GetDepthPlane(), view, reinterpret_cast<const uint8_t*>(GetWorldSnapshot()), GetSwayPlane());

        Atmosphere::DumpFrame(_atmosphereBuffer.data(), width, height, layout);

        _atmosphereShifts[0] = _screenTextureFormat->Rshift;
        _atmosphereShifts[1] = _screenTextureFormat->Gshift;
        _atmosphereShifts[2] = _screenTextureFormat->Bshift;
        const bool uploaded = SDL_UpdateTexture(_screenTexture, nullptr, _atmosphereBuffer.data(), width * 4) == 0;
        _processedValid = uploaded;
        _processedPalette = _paletteVersion;
        _processedRevision = Atmosphere::SettingsRevision();
        _processedWidth = width;
        _processedHeight = height;
        return uploaded;
    }

    void CopyBitsToTexture(SDL_Texture* texture, PaletteIndex* src, int32_t width, int32_t height, const uint32_t* palette)
    {
        void* pixels;
        int32_t pitch;
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0)
        {
            int32_t padding = pitch - (width * 4);
            if (pitch == width * 4)
            {
                uint32_t* dst = static_cast<uint32_t*>(pixels);
                for (int32_t i = width * height; i > 0; i--)
                {
                    *dst++ = palette[EnumValue(*src++)];
                }
            }
            else
            {
                if (pitch == (width * 2) + padding)
                {
                    uint16_t* dst = static_cast<uint16_t*>(pixels);
                    for (int32_t y = height; y > 0; y--)
                    {
                        for (int32_t x = width; x > 0; x--)
                        {
                            const uint8_t lower = *reinterpret_cast<const uint8_t*>(&palette[EnumValue(*src++)]);
                            const uint8_t upper = *reinterpret_cast<const uint8_t*>(&palette[EnumValue(*src++)]);
                            *dst++ = (lower << 8) | upper;
                        }
                        dst = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + padding);
                    }
                }
                else if (pitch == width + padding)
                {
                    uint8_t* dst = static_cast<uint8_t*>(pixels);
                    for (int32_t y = height; y > 0; y--)
                    {
                        for (int32_t x = width; x > 0; x--)
                        {
                            *dst++ = *reinterpret_cast<const uint8_t*>(&palette[EnumValue(*src++)]);
                        }
                        dst += padding;
                    }
                }
            }
            SDL_UnlockTexture(texture);
        }
    }

    uint32_t GetDirtyVisualTime(uint32_t x, uint32_t y)
    {
        uint32_t result = 0;
        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            result = _dirtyVisualsTime[i];
        }
        return result;
    }

    void SetDirtyVisualTime(uint32_t x, uint32_t y, uint32_t value)
    {
        const auto rows = _invalidationGrid.getRowCount();
        const auto columns = _invalidationGrid.getColumnCount();

        _dirtyVisualsTime.resize(rows * columns);

        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            _dirtyVisualsTime[i] = value;
        }
    }

    void RenderDirtyVisuals()
    {
        int windowX, windowY, renderX, renderY;
        SDL_GetWindowSize(_window, &windowX, &windowY);
        SDL_GetRendererOutputSize(_sdlRenderer, &renderX, &renderY);

        float scaleX = Config::Get().general.windowScale * renderX / static_cast<float>(windowX);
        float scaleY = Config::Get().general.windowScale * renderY / static_cast<float>(windowY);

        SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_BLEND);
        for (uint32_t y = 0; y < _invalidationGrid.getRowCount(); y++)
        {
            for (uint32_t x = 0; x < _invalidationGrid.getColumnCount(); x++)
            {
                const auto timeEnd = GetDirtyVisualTime(x, y);
                const auto timeLeft = gCurrentRealTimeTicks < timeEnd ? timeEnd - gCurrentRealTimeTicks : 0;
                if (timeLeft > 0)
                {
                    uint8_t alpha = timeLeft * kDirtyRegionAlpha / kDirtyVisualTime;
                    SDL_Rect ddRect;
                    ddRect.x = static_cast<int32_t>(x * _invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.y = static_cast<int32_t>(y * _invalidationGrid.getBlockHeight() * scaleY);
                    ddRect.w = static_cast<int32_t>(_invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.h = static_cast<int32_t>(_invalidationGrid.getBlockHeight() * scaleY);

                    SDL_SetRenderDrawColor(_sdlRenderer, 255, 255, 255, alpha);
                    SDL_RenderFillRect(_sdlRenderer, &ddRect);
                }
            }
        }
    }
};

std::unique_ptr<IDrawingEngine> Ui::CreateHardwareDisplayDrawingEngine(IUiContext& uiContext)
{
    return std::make_unique<HardwareDisplayDrawingEngine>(uiContext);
}
