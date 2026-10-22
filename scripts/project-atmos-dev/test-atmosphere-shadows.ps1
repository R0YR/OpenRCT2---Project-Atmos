param(
    [string]$Compiler = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
function Extract([string]$file, [string]$start, [string]$end) {
    $source = [IO.File]::ReadAllText((Join-Path $repo $file)).Replace("`r`n", "`n")
    $a = $source.IndexOf($start)
    $b = $source.IndexOf($end, $a + $start.Length)
    if ($a -lt 0 -or $b -lt 0) { throw "Cannot extract $start" }
    return $source.Substring($a, $b - $a)
}
$code = @'
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>
#include <cassert>
#include <iostream>
#include <thread>
// Compile the actual sprite bridge, including the RLE memcpy path and both zoom branches.
#include "openrct2/drawing/DepthPlane.cpp"
#include "openrct2/interface/ZoomLevel.cpp"
#include "openrct2/drawing/PaletteMap.cpp"
#include "openrct2/drawing/Drawing.Sprite.BMP.cpp"
#include "openrct2/drawing/Drawing.Sprite.RLE.cpp"
#include "openrct2/world/tile_element/TileElementType.h"
using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
'@
$code += Extract 'src/openrct2/interface/Viewport.h' 'enum class ViewportInteractionItem : uint8_t' 'using ViewportInteractionItems'
$code += Extract 'src/openrct2/paint/Paint.cpp' 'static bool ShadowSpriteIsFlat' 'static void PaintStoreDepth'
$code += Extract 'src/openrct2/paint/Paint.cpp' 'static uint8_t ShadowSpriteFlags' 'static inline void PaintDrawStruct'
$code += Extract 'src/openrct2-ui/drawing/Atmosphere.cpp' '        struct ShadowProjection' '        bool BuildSpriteShadows'
$code += @'
static void CheckSpriteBridge(bool rle, int zoom)
{
    const int width = zoom < 0 ? 8 : zoom > 0 ? 2 : 4;
    const int height = zoom < 0 ? 4 : zoom > 0 ? 1 : 2;
    std::vector<PaletteIndex> pixels(width * height, PaletteIndex::transparent);
    std::vector<uint16_t> depths(width * height, kDepthUnknown), heights(width * height, 0);
    std::vector<uint8_t> flags(width * height, 0);
    gDepthPlane = { depths.data(), pixels.data(), pixels.size(), nullptr, flags.data(), heights.data() };
    gDepthPaint = {};
    gDepthPaint.active = true;
    gDepthPaint.value = 200; // screen foot at world row 100
    gDepthPaint.zoom = zoom;
    gDepthPaint.shadowFlags = kShadowCaster;
    gDepthPaint.shadowGroundZ = 80;
    RenderTarget rt;
    rt.bits = pixels.data(); rt.width = width; rt.height = height;
    rt.y = 0; rt.zoom_level = ZoomLevel{ static_cast<int8_t>(zoom) };
    uint8_t bitmap[] = {0,5,6,0, 7,0,0,0};
    uint8_t compressed[] = {4,0,8,0, 0x82,1,5,6, 0x81,0,7};
    G1Element image;
    image.offset = rle ? compressed : bitmap; image.width = 4; image.height = 2;
    image.flags.set(G1Flag::hasTransparency);
    DrawSpriteArgs args(ImageId{0}, PaletteMap::GetDefault(), image, 0, 0,
        zoom < 0 ? 8 : 4, zoom < 0 ? 4 : 2, pixels.data());
    if (rle) GfxRleSpriteToBuffer(rt, args);
    else GfxBmpSpriteToBuffer(rt, args);
    size_t written = 0, skipped = 0;
    for (size_t i = 0; i < pixels.size(); i++)
    {
        if (pixels[i] == PaletteIndex::transparent)
        {
            assert(flags[i] == 0 && heights[i] == 0 && depths[i] == kDepthUnknown);
            skipped++;
        }
        else
        {
            assert(flags[i] == kShadowCaster && depths[i] == DepthEncode(200));
            const int row = static_cast<int>(i) / width;
            assert(heights[i] == 20 - ZoomLevel{static_cast<int8_t>(zoom)}.ApplyTo(row));
            written++;
        }
    }
    assert(written > 0 && skipped > 0);
    if (zoom == 0) assert(written == 3 && flags[0] == 0 && flags[1] == kShadowCaster && flags[4] == kShadowCaster);
    // A receiver painted over a caster must clear stale source flags and height on every opaque pixel.
    gDepthPaint.flat = true; gDepthPaint.flatBase = 160; gDepthPaint.shadowFlags = kShadowReceiver;
    if (rle) GfxRleSpriteToBuffer(rt, args);
    else GfxBmpSpriteToBuffer(rt, args);
    for (size_t i = 0; i < pixels.size(); i++)
        if (pixels[i] != PaletteIndex::transparent)
            assert(flags[i] == kShadowReceiver && heights[i] == 0);
    // A destination-filtered/glass pixel preserves the object behind it, including shadow metadata.
    auto ctx = DepthBlitCtx::Make(rt);
    auto source = PaletteIndex::pi10;
    const auto oldFlags = flags[1]; const auto oldHeight = heights[1]; const auto oldDepth = depths[1];
    BlitPixelDepth<kBlendDst>(&source, pixels.data() + 1, PaletteMap::GetDefault(), ctx, 123, 0, 123);
    assert(flags[1] == oldFlags && heights[1] == oldHeight && depths[1] == oldDepth);
    gDepthPaint.active = false; gDepthPlane = {};
}

static void CheckFilledShadow(float phase, int zoom, int rotation, float sun)
{
    constexpr int size = 256;
    const float wpp = static_cast<float>(1 << zoom);
    const auto projection = MakeShadowProjection(sun, 45, 120, rotation, wpp);
    std::vector<uint16_t> coverage(size * size, 0);
    for (float y = 128 / wpp; y < 320 / wpp; y += 1)
        RasterShadowSpan(coverage.data(), size, size, 40 / wpp + phase, 140 / wpp + phase,
            y + phase, 320 - y * wpp, projection);
    int peak = 0, occupied = 0;
    for (int y = 0; y < size; y++)
    {
        int first = -1, last = -1;
        for (int x = 0; x < size; x++)
        {
            const auto value = ShadowCoverageValue(coverage[y * size + x]);
            peak = std::max(peak, static_cast<int>(value));
            if (value) { if (first < 0) first = x; last = x; occupied++; }
        }
        // An opaque continuous span cannot acquire a gap when rows meet or the camera moves fractionally.
        if (first >= 0)
            for (int x = first; x <= last; x++) assert(coverage[y * size + x] != 0);
    }
    assert(occupied > 0 && peak > 0 && peak <= 192);
    // At every tested zoom/view, non-edge-on solid interiors retain full coverage for every fractional phase.
    if (std::abs(projection.directionY) > 0.4f) assert(peak == 192);
}

int main()
{
    // Paths, railings and raised supports can never source a shadow, independent of flatness.
    for (bool flat : {false, true})
    {
        assert((ShadowSpriteFlags(TileElementType::path, flat, ViewportInteractionItem::footpath) & kShadowCaster) == 0);
        assert((ShadowSpriteFlags(TileElementType::path, flat, ViewportInteractionItem::none) & kShadowCaster) == 0);
        assert((ShadowSpriteFlags(TileElementType::surface, flat, ViewportInteractionItem::terrain) & kShadowCaster) == 0);
    }
    assert(ShadowSpriteFlags(TileElementType::path, true, ViewportInteractionItem::footpath) == kShadowReceiver);
    assert(ShadowSpriteFlags(TileElementType::path, false, ViewportInteractionItem::footpath) == 0);
    assert(ShadowSpriteFlags(TileElementType::path, false, ViewportInteractionItem::pathAddition) == kShadowCaster);
    assert(ShadowSpriteFlags(TileElementType::largeScenery, false, ViewportInteractionItem::largeScenery) == kShadowCaster);
    assert(ShadowSpriteFlags(TileElementType::largeScenery, true, ViewportInteractionItem::largeScenery) == 0);
    assert(ShadowSpriteIsFlat(true, 0, 64, 48)); // sloping path diamond
    assert(!ShadowSpriteIsFlat(true, 0, 32, 96)); // path support / terrain cliff
    assert(!ShadowSpriteIsFlat(false, 1, 64, 192)); // thin sorting box of custom building
    assert(ShadowSpriteIsFlat(false, 1, 64, 32)); // horizontal custom road / roof decal
    for (bool rle : {false, true})
        for (int zoom : {-1, 0, 1})
            CheckSpriteBridge(rle, zoom);

    const auto projection = MakeShadowProjection(0, 45, 40, 0, 1);
    auto point = ProjectShadowPixel(20, 0, 20, projection);
    assert(point.x > 10 && point.y > 0 && point.y < 10);
    // Direction rotates with the view, and world-space shadow dimensions agree at different zooms.
    const auto rotated = MakeShadowProjection(0, 45, 40, 2, 1);
    auto reverse = ProjectShadowPixel(20, 0, 20, rotated);
    assert(reverse.x < 10);
    const auto far = MakeShadowProjection(0, 45, 40, 0, 4);
    auto farPoint = ProjectShadowPixel(5, 0, 20, far);
    assert(std::abs(farPoint.x * 4 - point.x) < 0.001f && std::abs(farPoint.y * 4 - point.y) < 0.001f);
    auto tall = ProjectShadowPixel(0, -32000, 32000, projection);
    assert(tall.x < 20 && tall.x > 19 && tall.y > -10 && tall.y < -9);
    assert(std::isfinite(tall.x) && std::isfinite(tall.y));
    for (int zoom = 0; zoom <= 3; zoom++)
        for (int rotation = 0; rotation < 4; rotation++)
            for (float sun : {0.0f, 45.0f, 135.0f, 134.99f})
                for (float phase : {0.0f, 0.125f, 0.25f, 0.5f, 0.875f})
                    CheckFilledShadow(phase, zoom, rotation, sun);
    std::vector<uint16_t> coverage(64 * 64, 0);
    const ShadowPoint firstQuad[] = {{15.5f,15.5f},{16.5f,15.5f},{16.5f,16.5f},{15.5f,16.5f}};
    const ShadowPoint secondQuad[] = {{31.5f,15.5f},{32.5f,15.5f},{32.5f,16.5f},{31.5f,16.5f}};
    RasterShadowQuad(coverage.data(), 64, 64, firstQuad);
    RasterShadowQuad(coverage.data(), 64, 64, secondQuad);
    const auto coverageOnce = coverage;
    for (int i = 0; i < 100; i++) RasterShadowQuad(coverage.data(), 64, 64, firstQuad);
    assert(coverage == coverageOnce); // union never darkens repeated coverage
    std::vector<uint8_t> plane(64 * 64, 0), horizontal(plane.size()), soft(plane.size()), split(plane.size());
    for (size_t i = 0; i < plane.size(); i++) plane[i] = ShadowCoverageValue(coverage[i]);
    // A sparse silhouette remains sparse; quad rasterization cannot invent a filled tile footprint.
    assert(plane[16 * 64 + 24] == 0);
    const auto once = plane;
    assert(*std::max_element(plane.begin(), plane.end()) <= 192);
    BlurShadowRows(plane.data(), horizontal.data(), 64, 64, true, 3, 0, 64);
    BlurShadowRows(horizontal.data(), soft.data(), 64, 64, false, 3, 0, 64);
    assert(soft[16 * 64 + 24] == 0 && soft[15 * 64 + 16] > 0 && soft[16 * 64 + 16] < 192);
    std::thread a([&] { BlurShadowRows(horizontal.data(), split.data(), 64, 64, false, 3, 0, 32); });
    std::thread b([&] { BlurShadowRows(horizontal.data(), split.data(), 64, 64, false, 3, 32, 64); });
    a.join(); b.join(); assert(split == soft);
    BlurShadowRows(plane.data(), split.data(), 64, 64, false, 0, 0, 64);
    assert(split == plane); // zero softness preserves exact coverage
    const ShadowPoint outside[] = {{-100,-100},{-99,-100},{-99,-99},{-100,-99}};
    RasterShadowQuad(coverage.data(), 64, 64, outside);
    assert(coverage == coverageOnce); // off-screen projection never touches memory
    std::cout << "PASS: actual BMP/RLE sprites, transparent gaps, zoom, receiver overwrite, glass preservation, path/support exclusion, thin-box scenery, bounded pixel quads, fractional translation, filled interiors, sun directions, coverage union, soft edges, concurrent blur rows\n";
}
'@
$output = Join-Path $repo 'artifacts/atmosphere-reference/shadow-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $output 'shadow-tests.cpp'), $code)
$vcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdkRoot = 'C:/Program Files (x86)/Windows Kits/10'
$sdkVersion = (Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude = $env:INCLUDE
$oldLib = $env:LIB
try {
    $env:INCLUDE = "$vcRoot/include;$sdkRoot/Include/$sdkVersion/ucrt;$sdkRoot/Include/$sdkVersion/shared;$sdkRoot/Include/$sdkVersion/um"
    $env:LIB = "$vcRoot/lib/x64;$sdkRoot/Lib/$sdkVersion/ucrt/x64;$sdkRoot/Lib/$sdkVersion/um/x64"
    & $Compiler /nologo /EHsc /std:c++20 /O2 "/I$repo/src" (Join-Path $output 'shadow-tests.cpp') "/Fo$output/shadow-tests.obj" "/Fe$output/shadow-tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Shadow test compilation failed' }
    & (Join-Path $output 'shadow-tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Shadow silhouette tests failed' }
} finally {
    $env:INCLUDE = $oldInclude
    $env:LIB = $oldLib
}
