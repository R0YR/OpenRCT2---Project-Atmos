param(
    [string]$Compiler = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$source = [IO.File]::ReadAllText((Join-Path $repo 'src/openrct2-ui/drawing/Atmosphere.cpp'))
function Extract([string]$start, [string]$end) {
    $a = $source.IndexOf($start)
    $b = $source.IndexOf($end, $a + $start.Length)
    if ($a -lt 0 -or $b -lt 0) { throw "Cannot extract $start" }
    return $source.Substring($a, $b - $a)
}
$code = @'
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <vector>
#include <string>
#include <string_view>
#include <utility>
#include <cassert>
#include <iostream>
namespace Drawing {
constexpr uint16_t kDepthUnknown = 65535;
int32_t DepthDecode(uint16_t value) { return (static_cast<int32_t>(value)-32768)*2; }
}
struct Frame { int32_t width; uint8_t* ui; uint32_t* out; uint32_t rShift=0,gShift=8,bShift=16; };
struct FrameState { int32_t halfWidth,halfHeight; std::vector<uint8_t> glowPlane,glowSoft; };
'@
$code += Extract '        struct LightKernel' '        struct SourceLightCache'
$code += Extract '        void SourceLightRows' '        // Near-field blur, part 1'
$code += @'
int main()
{
    const LightKernel point{16,16,8,4,0.5f};
    assert(SampleLightKernel(point,16,16)==0.5f);
    assert(SampleLightKernel(point,19,16)==SampleLightKernel(point,13,16));
    assert(SampleLightKernel(point,24,16)==0);
    assert(SampleLightKernel(point,16,20)==0);
    assert(SampleLightKernel(point,20,16)>SampleLightKernel(point,23,16));
    // A forward lobe must never shine behind its vehicle. Rotation turns the lobe.
    const LightKernel east{16,16,20,5,0.5f,1,0,true};
    const LightKernel south{16,16,20,5,0.5f,0,1,true};
    assert(SampleLightKernel(east,24,16)>0);
    assert(SampleLightKernel(east,8,16)==0);
    assert(SampleLightKernel(east,16,24)==0);
    assert(SampleLightKernel(south,16,24)==SampleLightKernel(east,24,16));
    assert(SampleLightKernel(east,36,16)==0);
    assert(SampleLightKernel(east,24,21)==0);
    // Day/night controls are independent; neither weather nor twilight can bypass an all-zero control.
    assert(ObjectLightStrength(0,1,1,0.5f,0.3f)==0);
    assert(ObjectLightStrength(1,0,0,0.5f,0.3f)==0);
    assert(ObjectLightStrength(1,0,1,0,0.3f)==0);
    assert(ObjectLightStrength(1,1,0,1,0.3f)==0);
    assert(ObjectLightStrength(1,0.25f,1,1,0.3f)==1);
    assert(ObjectLightStrength(1,0.25f,1,0,0.3f)>0.25f);
    // Two rear bulbs stay behind a vehicle and rotate with its local forward axis.
    const auto rearLeft=RearLampOffset(1,0,-1), rearRight=RearLampOffset(1,0,1);
    assert(rearLeft.first==-10 && rearRight.first==-10);
    assert(rearLeft.second==-3 && rearRight.second==3);
    const auto turnedRear=RearLampOffset(0,1,1);
    assert(turnedRear.first==-3 && turnedRear.second==-10);
    // A transparent bulb keeps its light when the nearby fixture or ground foot is visible.
    std::vector<uint16_t> sourceDepth(12*12,Drawing::kDepthUnknown);
    const auto encoded=[](int32_t value) { return static_cast<uint16_t>(value/2+32768); };
    assert(!SourceLightVisible(sourceDepth.data(),12,12,4,3,4,9,100));
    sourceDepth[9*12+4]=encoded(100);
    assert(SourceLightVisible(sourceDepth.data(),12,12,4,3,4,9,100));
    sourceDepth[3*12+5]=encoded(100);
    assert(SourceLightVisible(sourceDepth.data(),12,12,4,3,4,9,100));
    // A foreground wall at the bulb or its nearest visible neighbourhood overrides ground visibility.
    sourceDepth[3*12+4]=encoded(160);
    assert(!SourceLightVisible(sourceDepth.data(),12,12,4,3,4,9,100));
    sourceDepth[3*12+4]=Drawing::kDepthUnknown;
    sourceDepth[2*12+4]=encoded(160);
    assert(!SourceLightVisible(sourceDepth.data(),12,12,4,3,4,9,100));
    assert(!SourceLightVisible(sourceDepth.data(),12,12,-100,-100,-100,-100,100));
    assert(SourceLightVisible(nullptr,12,12,4,3,4,9,100));
    // Overlapping kernels saturate safely; off-screen fixtures cannot write out of bounds.
    std::vector<uint8_t> plane(32*32,0);
    RasterLightKernel(point,plane,32,32);
    const auto original=plane;
    for(int i=0;i<20;i++) RasterLightKernel(point,plane,32,32);
    assert(plane[16*32+16]==255);
    RasterLightKernel({-100,-100,8,8,1},plane,32,32);
    assert(plane[0]==0);
    assert(original[16*32+16]>original[16*32+19]);
    assert(SceneryNameIsLight("rct2.small_scenery.tlp1 Lamp"));
    assert(SceneryNameIsLight("custom.streetlamp-01"));
    assert(SceneryNameIsLight("Deutsche Laternen"));
    assert(SceneryNameIsLight("Park Lampen"));
    assert(!SceneryNameIsLight("Clamped wall"));
    assert(!SceneryNameIsLight("Bright yellow roof"));
    assert(!SceneryNameIsLight("Lamport building"));
    // Actual blend keeps UI/exterior exact, follows arbitrary channel layouts and preserves alpha.
    uint32_t pixels[8] = {0xAB202020,0xAB202020,0xAB202020,0xAB202020,0xAB202020,0xAB202020,0xAB202020,0xAB202020};
    uint8_t ui[8] = {1,0,0,0,0,0,0,0};
    uint16_t depth[8] = {0,Drawing::kDepthUnknown,0,0,0,0,0,0};
    Frame f{4,ui,pixels};
    FrameState s{2,1,{255,255},{0,0}};
    SourceLightRows(f,s,depth,0,2);
    assert(pixels[0]==0xAB202020 && pixels[1]==0xAB202020);
    assert((pixels[2]>>24)==0xAB);
    assert((pixels[2]&255)>((pixels[2]>>8)&255));
    assert(((pixels[2]>>8)&255)>((pixels[2]>>16)&255));
    assert((pixels[2]&255)<255);
    std::fill(s.glowPlane.begin(),s.glowPlane.end(),0);
    const auto before=pixels[2];
    SourceLightRows(f,s,depth,0,2);
    assert(pixels[2]==before);
    f.rShift=16; f.bShift=0;
    pixels[2]=0xAB202020;
    std::fill(s.glowPlane.begin(),s.glowPlane.end(),255);
    SourceLightRows(f,s,depth,0,2);
    assert(((pixels[2]>>16)&255)>(pixels[2]&255));
    // Rear-only lights stay red, preserve protected pixels, and can be disabled independently of warm sources.
    f.rShift=0; f.bShift=16;
    std::fill(s.glowPlane.begin(),s.glowPlane.end(),0);
    std::fill(s.glowSoft.begin(),s.glowSoft.end(),255);
    for(auto& pixel:pixels) pixel=0xAB202020;
    SourceLightRows(f,s,depth,0,2);
    assert(pixels[0]==0xAB202020 && pixels[1]==0xAB202020);
    assert((pixels[2]&255)>((pixels[2]>>8)&255)+100);
    assert(((pixels[2]>>8)&255)>((pixels[2]>>16)&255));
    assert((pixels[2]>>24)==0xAB);
    std::fill(s.glowSoft.begin(),s.glowSoft.end(),0);
    const auto rearBefore=pixels[2];
    SourceLightRows(f,s,depth,0,2);
    assert(pixels[2]==rearBefore);
    // Tail strength is multiplied by the same day/night/master gate as all actual source lights.
    assert(ObjectLightStrength(0,1,1,0.5f,0.3f)*0.6f==0);
    assert(ObjectLightStrength(1,1,1,0.5f,0.3f)*0.0f==0);
    assert(ObjectLightStrength(1,0,0,0.5f,0.3f)*0.6f==0);
    std::cout<<"PASS: point/beam falloff, front/rear rotation, red tail blend/off, transparent bulb/foot witness, foreground blocker, day/night/master zero, bounded raster/addition, fixture names, UI/exterior, channel layout, strength zero\n";
}
'@
$output = Join-Path $repo 'artifacts/atmosphere-reference/light-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $output 'light-tests.cpp'), $code)
$vcRoot = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdkRoot = 'C:/Program Files (x86)/Windows Kits/10'
$sdkVersion = (Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude = $env:INCLUDE
$oldLib = $env:LIB
try {
    $env:INCLUDE = "$vcRoot/include;$sdkRoot/Include/$sdkVersion/ucrt;$sdkRoot/Include/$sdkVersion/shared;$sdkRoot/Include/$sdkVersion/um"
    $env:LIB = "$vcRoot/lib/x64;$sdkRoot/Lib/$sdkVersion/ucrt/x64;$sdkRoot/Lib/$sdkVersion/um/x64"
    & $Compiler /nologo /EHsc /std:c++20 /O2 (Join-Path $output 'light-tests.cpp') "/Fo$output/light-tests.obj" "/Fe$output/light-tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Light test compilation failed' }
    & (Join-Path $output 'light-tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Light kernel/compositor tests failed' }
} finally {
    $env:INCLUDE = $oldInclude
    $env:LIB = $oldLib
}
