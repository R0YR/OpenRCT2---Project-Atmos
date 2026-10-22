param([string]$Compiler = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$source = [IO.File]::ReadAllText((Join-Path $repo 'src/openrct2-ui/drawing/Atmosphere.cpp')).Replace("`r`n", "`n")
function Extract([string]$start, [string]$end) {
    $a = $source.IndexOf($start)
    $b = $source.IndexOf($end, $a + $start.Length)
    if ($a -lt 0 -or $b -lt 0) { throw "Cannot extract $start" }
    return $source.Substring($a, $b - $a)
}
$header = (Join-Path $repo 'src/openrct2-ui/drawing/Atmosphere.h').Replace('\','/')
$code = "#include <algorithm>`n#include <cmath>`n#include <cstdlib>`n#include <cassert>`n#include <fstream>`n#include <iostream>`n#include `"$header`"`nusing namespace OpenRCT2::Ui::Atmosphere;`nnamespace Drawing { constexpr uint16_t kDepthUnknown = 0xFFFF; constexpr uint8_t kShadowReceiver=2, kShadowCaster=1; enum class PaletteIndex : uint8_t { transparent=0, pi10=10 }; }`nSettings _settings;`n"
$code += Extract '        std::string Trim' '        std::filesystem::file_time_type FileTime'
$code += Extract '        void ClearAll' '        const PresetDef kPresets'
$code += Extract '        inline uint32_t ScaleLanes(' '        // Same as ScaleLanes'
$code += Extract "        struct Frame`n" '        // Stage 0: protected mask'
$code += Extract '        struct TimeState' '        float CurrentHour()'
$code += Extract '        struct LightningState' '        // Cloud planes of the current frame'
$code += Extract '        struct CloudPlanes' '        // Depth of what is in focus'
$code += @"
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string fixture = argv[1];
    { std::ofstream file(fixture); file << "enabled = 1\nsunlight = nan\nsun_warmth = 2\n"; }
    Settings loaded = LoadSettings(fixture);
    assert(loaded.enabled && loaded.sunlight == 0 && loaded.sunWarmth == 1);
    loaded.sunlight = 0.45f; loaded.sunWarmth = 0.35f; loaded.cloudHeight = 850; loaded.cloudFocus = false;
    loaded.shadowSoftness = 0.7f; loaded.lightDayStrength = 0.8f; loaded.lightNightStrength = 0.2f;
    loaded.headlightStrength = 0.6f; loaded.lightRadius = 1.4f; loaded.lightningStrength = 0.3f; loaded.lightningInterval = 12;
    SaveSettings(fixture, loaded);
    const Settings saved = LoadSettings(fixture);
    assert(saved.sunlight == loaded.sunlight && saved.sunWarmth == loaded.sunWarmth);
    assert(saved.cloudHeight == 850 && !saved.cloudFocus);
    assert(saved.shadowSoftness == 0.7f && saved.lightDayStrength == 0.8f && saved.lightNightStrength == 0.2f);
    assert(saved.headlightStrength == 0.6f && saved.lightRadius == 1.4f && saved.lightningStrength == 0.3f && saved.lightningInterval == 12);
    ClearAll(loaded);
    assert(loaded.sunlight == 0 && loaded.sunWarmth == 0 && loaded.shadowStrength == 0 && loaded.cloudCoverage == 0);
    assert(loaded.lampGlow == 0 && loaded.lightningStrength == 0);
    LightningState storm;
    assert(UpdateLightning(storm,0,100,false,5)==0);
    assert(UpdateLightning(storm,0,100,true,5)==0);
    assert(UpdateLightning(storm,2,110,true,5)==0);
    assert(UpdateLightning(storm,2.03,110,true,5)>0.8f);
    assert(UpdateLightning(storm,2.6,111,true,5)==0);
    assert(UpdateLightning(storm,3,112,false,5)==0);
    assert(UpdateLightning(storm,4,1,true,5)==0); // park reload resets the flash
    for(int i=-10;i<=100;i++) assert(LightningEnvelope(i*0.01f)>=0 && LightningEnvelope(i*0.01f)<=1);
    uint8_t row[] = {10,0,1,10,1,10,10}, mask[7] = {};
    ProtectExteriorRow(row,mask,7);
    assert(mask[0] && mask[1] && mask[5] && mask[6]);
    assert(!mask[2] && !mask[3] && !mask[4]); // keep dark interior pixels available
    constexpr int w=9, h=7, hw=5, hh=4;
    uint8_t bits[w*h]={}, ui[w*h]={}, alpha[hw*hh], lit[hw*hh], shadow[hw*hh], segments[hh];
    uint16_t depth[w*h]; uint32_t palette[256]={}, output[w*h]={};
    uint8_t flags[w*h]={};
    palette[0]=0xFF172323; palette[1]=0xFF4080C0; palette[2]=0xFF204060;
    std::fill_n(depth,w*h,Drawing::kDepthUnknown); std::fill_n(alpha,hw*hh,255); std::fill_n(lit,hw*hh,255);
    std::fill_n(shadow,hw*hh,255); std::fill_n(segments,hh,1);
    for(int y=0;y<h;y++) for(int x=1;x<w-1;x++) { bits[y*w+x]=1; depth[y*w+x]=32768+y; flags[y*w+x]=Drawing::kShadowReceiver; }
    ui[3*w+3]=1; bits[3*w+3]=2;
    std::vector<ExcludeRect> excludes;
    Frame f{bits,palette,output,w,h,24,&excludes,16,8,0,ui,palette,depth,1.0f,flags};
    _cloudPlanes.cloud=alpha; _cloudPlanes.lit=lit; _cloudPlanes.seg=segments; _cloudPlanes.segCols=1;
    for(int c=0;c<3;c++) std::fill_n(_cloudPlanes.cloudColour[c],256,250);
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,hw,hh,0,h);
    for(int y=0;y<h;y++) { assert(output[y*w]==palette[0]); assert(output[y*w+w-1]==palette[0]); }
    assert(output[3*w+3]==palette[2]); assert(output[2*w+2]==0xFFFAFAFA);
    _cloudPlanes=CloudPlanes{}; _settings.shadowStrength=1;
    for(int y=0;y<h;y++) flags[y*w+5]=Drawing::kShadowCaster; // upright sprite is never a receiver
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,shadow,hw,hh,0,h);
    assert(output[2*w+2]!=palette[1]); assert(output[2*w+5]==palette[1]); assert(output[3*w+3]==palette[2]);
    assert(output[0]==palette[0]); assert(output[(h-1)*w+2]!=palette[1]);
    const uint32_t beforeFlash=output[2*w+2];
    LightningRows(f,0.5f,0,h);
    assert(output[2*w+2]!=beforeFlash && output[0]==palette[0] && output[3*w+3]==palette[2]);
    std::cout << "PASS: settings persistence/off, thunder-only flash timing/reload, odd frame sizes, dense core, exact UI/exterior restoration, explicit receivers, upright exclusion\n";
}
"@
$output = Join-Path $repo 'artifacts/atmosphere-reference/compositor-tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $output 'compositor-tests.cpp'), $code)
$vcRoot = Split-Path (Split-Path (Split-Path (Split-Path $Compiler)))
$sdkRoot = 'C:/Program Files (x86)/Windows Kits/10'
$sdkVersion = (Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude=$env:INCLUDE; $oldLib=$env:LIB
try {
    $env:INCLUDE="$vcRoot/include;$sdkRoot/Include/$sdkVersion/ucrt;$sdkRoot/Include/$sdkVersion/shared;$sdkRoot/Include/$sdkVersion/um"
    $env:LIB="$vcRoot/lib/x64;$sdkRoot/Lib/$sdkVersion/ucrt/x64;$sdkRoot/Lib/$sdkVersion/um/x64"
    & $Compiler /nologo /EHsc /std:c++20 /O2 (Join-Path $output 'compositor-tests.cpp') "/Fo$output/compositor-tests.obj" "/Fe$output/compositor-tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Compositor test compilation failed' }
    & (Join-Path $output 'compositor-tests.exe') (Join-Path $output 'settings-fixture.ini')
    if ($LASTEXITCODE -ne 0) { throw 'Compositor tests failed' }
} finally { $env:INCLUDE=$oldInclude; $env:LIB=$oldLib }
