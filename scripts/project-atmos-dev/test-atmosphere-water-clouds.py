"""Compile the production cloud compositor; exercise blended water and UI/void boundaries."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/openrct2-ui/drawing/Atmosphere.cpp').read_text(encoding='utf-8')
def extract(start, end):
    a = source.index(start)
    return source[a:source.index(end, a + len(start))]

code = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>
#include <vector>
#include <cassert>
#include <cstdio>
namespace Drawing { constexpr uint16_t kDepthUnknown=65535; constexpr uint8_t kShadowReceiver=2,kWaterSurface=4,kExteriorSurface=8,kSceneryReceiver=16;
struct { const uint16_t* data=nullptr; const uint8_t* shadowFlags=nullptr; } gDepthPlane;
enum class PaletteIndex : uint8_t { transparent=0, pi10=10 }; }
struct ExcludeRect { int32_t left,top,right,bottom; };
struct { float shadowStrength=0; } _settings;
struct { bool active=false; float night=0; } _time;
const int16_t* _shadowZ=nullptr;
const uint8_t* _sceneryShadow=nullptr;
float _shadowOriginY=0, _shadowWpp=1;
'''
code += extract('        inline uint32_t ScaleLanes(', '        // Same as ScaleLanes')
code += extract('        uint8_t SourceWeight(', '        inline __m128i UnpackLanes')
code += extract('        struct Frame\n', '        // Exterior is owned')
code += extract('        void ProtectExteriorRow(', '        // Stage 0: protected mask')
code += extract('        struct CloudPlanes', '        // Lightning: white light')
code += extract('        uint32_t SampleWaterColour(', '        void WaterRows(')
code += extract('        inline uint32_t SwayMix(', '        // Depth of what is in focus')
code += r'''
struct FrameState { int width=64,lutFocusStored=32768,lutHalf=2;uint8_t lutFar=20;
std::vector<uint8_t> radiusLut{10,4,0,4,10};std::vector<float> radius=std::vector<float>(24,7.f); };
'''
code += extract('        void BuildRadiusRows(', '        // ---------------------------------------------------------------------------------------------------------')
code += r'''
int main() {
    for(int radius=-20;radius<=300;radius++) {
        const float t=std::clamp(float(std::clamp(radius,0,255))/3.f,0.f,1.f);
        const float sm=t*t*(3.f-2.f*t);
        const auto reference=static_cast<uint8_t>(24.f+(255.f-24.f)*sm+.5f);
        assert(SourceWeight(radius)==reference);
    }
    puts("PASS: immutable blur source weights match smoothstep for all radii and clamping");
    FrameState state;
    std::vector<uint16_t> unknown(64*24,Drawing::kDepthUnknown);
    std::vector<uint8_t> whole(64*24),chunks(64*24);
    unknown[3*64+11]=32768;
    BuildRadiusRows(state,unknown.data(),true,whole.data(),0,24);
    for(int y=0;y<24;y+=8)BuildRadiusRows(state,unknown.data(),true,chunks.data(),y,y+8);
    assert(whole==chunks);
    for(int i=0;i<64*24;i++)assert(whole[i]==(i==3*64+11?0:7));
    puts("PASS: unknown-depth blur independent of flying pixels and worker chunk boundaries");
    // An isolated balloon may change its own mask, never any neighbouring backdrop pixel.
    for(int width:{31,256,1919})for(int balloon:{1,width/2,width-2}){
        std::vector<uint8_t> scene(width,10),mask(width),flags(width,Drawing::kExteriorSurface);
        std::vector<uint16_t> z(width,32768);
        scene[balloon]=1;flags[balloon]=0;
        ProtectExteriorRow(scene.data(),mask.data(),width,flags.data(),z.data());
        for(int x=0;x<width;x++)assert(mask[x]==(x==balloon?0:2));
        // Dark scenery and water remain world pixels; UI keeps precedence.
        scene[balloon]=10;flags[balloon]=Drawing::kShadowReceiver;
        ProtectExteriorRow(scene.data(),mask.data(),width,flags.data(),z.data());assert(mask[balloon]==0);
        flags[balloon]=Drawing::kWaterSurface;z[balloon]=Drawing::kDepthUnknown;
        ProtectExteriorRow(scene.data(),mask.data(),width,flags.data(),z.data());assert(mask[balloon]==0);
        flags[balloon]=0;scene[balloon]=201;
        ProtectExteriorRow(scene.data(),mask.data(),width,flags.data(),z.data());assert(mask[balloon]==2);
        mask[0]=1;ProtectExteriorRow(scene.data(),mask.data(),width,flags.data(),z.data());assert(mask[0]==1);
    }
    puts("PASS: balloon-independent native exterior mask, dark scenery, water and UI");
    constexpr int W=9,H=7,HW=5,HH=4,N=W*H;
    uint8_t bits[N],ui[N]{},rad[N]{},alpha[HW*HH],lit[HW*HH],segments[HH];
    uint16_t depth[N]; uint32_t palette[256]{},out[N]{},blur[N]{};
    palette[0]=0xFF000000;palette[10]=0xFF171717;palette[230]=0xFF1260A0;palette[1]=0xFF70A030;
    std::fill_n(bits,N,230);std::fill_n(depth,N,Drawing::kDepthUnknown);
    std::fill_n(alpha,HW*HH,255);std::fill_n(lit,HW*HH,255);std::fill_n(segments,HH,1);
    for(int y=0;y<H;y++){bits[y*W]=10;ui[y*W]=2;bits[y*W+W-1]=0;ui[y*W+W-1]=2;}
    ui[3*W+3]=1;bits[3*W+3]=1;
    Frame f{bits,palette,out,W,H,24,nullptr,16,8,0,ui,palette,depth};
    {
        uint8_t samples[4]{1,2,3,4},waterUi[4]{},waterFlags[4]{4,4,4,4};
        uint32_t colours[256]{};
        colours[1]=0xFF0A0A0A;colours[2]=0xFF6E6E6E;colours[3]=0xFFD2D2D2;colours[4]=0xFFFAFAFA;
        Frame wet=f;wet.bits=samples;wet.width=wet.height=2;wet.worldPalette=colours;wet.ui=waterUi;wet.shadowFlags=waterFlags;
        assert(SampleWaterColour(wet,0,0,-.5f,.5f)==0xFF919191);
        int previous=10;
        for(int step=0;step<=100;step++){
            const int colour=SampleWaterColour(wet,0,0,0,float(step)/100)&255;
            assert(std::abs(colour-(10+2*step))<=1);assert(std::abs(colour-previous)<=3);previous=colour;
        }
        waterFlags[2]=0; // a boat in the lower source row cannot be sampled into water
        assert(SampleWaterColour(wet,0,0,0,.9f)==colours[1]);
        waterUi[0]=1;assert(SampleWaterColour(wet,0,0,0,.9f)==0);
        puts("PASS: fractional vertical/horizontal water samples are continuous and exclude boats/UI");
    }
    _cloudPlanes.cloud=alpha;_cloudPlanes.lit=lit;_cloudPlanes.seg=segments;_cloudPlanes.segCols=1;
    for(int c=0;c<3;c++)std::fill_n(_cloudPlanes.cloudColour[c],256,250);
    for(bool blurPath:{false,true}){
        if(blurPath)FinishRows<true>(f,blur,rad,nullptr,nullptr,nullptr,HW,HH,0,H);
        else FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,HW,HH,0,H);
        for(int y=0;y<H;y++)for(int x=1;x<W-1;x++)if(!ui[y*W+x])assert(out[y*W+x]==0xFFFAFAFA);
        assert(out[3*W+3]==palette[1]);
        // The cloud continues over the void with the same coverage instead of a hard map-edge cut.
        for(int y=0;y<H;y++){assert(out[y*W]==0xFFFAFAFA);assert(out[y*W+W-1]==0xFFFAFAFA);}
    }
    // Equal cloud coverage over water regardless of whether a particular animation pixel records depth.
    std::fill_n(alpha,HW*HH,128);
    depth[2*W+3]=32768;
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,HW,HH,0,H);
    assert(out[2*W+2]==out[2*W+3]);assert(out[2*W+2]!=palette[230]);
    std::fill_n(alpha,HW*HH,0);
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,HW,HH,0,H);
    assert(out[2*W+2]==palette[230]);
    for(int y=0;y<H;y++){assert(out[y*W]==palette[10]);assert(out[y*W+W-1]==palette[10]);}
    // Different native backdrop indices, including weather-filtered ones, must not expose blank tile geometry.
    for(int y=0;y<H;y++){bits[y*W]=201;bits[y*W+W-1]=0;ui[y*W+W-1]=2;}
    _cloudPlanes.cloud=nullptr;_cloudPlanes.shadow=alpha;std::fill_n(alpha,HW*HH,255);
    for(int c=0;c<3;c++)std::fill_n(_cloudPlanes.shadowFactor[c],256,64);
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,HW,HH,0,H);
    for(int y=0;y<H;y++){assert(out[y*W]==palette[10]);assert(out[y*W+W-1]==palette[10]);}
    _cloudPlanes.shadow=nullptr;
    puts("PASS: blank tiles and filtered clear pixels share one backdrop, with no exterior cloud shadows");
    _cloudPlanes.cloud=nullptr;_cloudPlanes.fog=alpha;
    std::fill_n(alpha,HW*HH,255);assert(CloudCoverAt(f,2,2)==1.0f);
    for(int c=0;c<3;c++)_cloudPlanes.fogColour[c]=200;
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,nullptr,HW,HH,0,H);
    assert(out[0]==0xFFC8C8C8); // fog overhang must not stop at the native map boundary
    std::fill_n(alpha,HW*HH,128);_cloudPlanes.cloud=alpha;
    assert(std::abs(CloudCoverAt(f,2,2)-(1.f-127.f*127.f/(255.f*255.f)))<.00001f);
    _cloudPlanes={};
    uint8_t flags[N]{};flags[2*W+2]=Drawing::kShadowReceiver;
    Drawing::gDepthPlane.data=depth;Drawing::gDepthPlane.shadowFlags=flags;
    _settings.shadowStrength=1;
    std::fill_n(alpha,HW*HH,255);
    FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,alpha,HW,HH,0,H);
    const auto shaded=out[2*W+2];assert(shaded!=palette[230]);assert(out[2*W+3]==palette[230]);
    // Animated neighbours must not toggle a ground pixel's receiving status.
    for(int change:{0,12345,32768,65535}){
        depth[W+2]=depth[3*W+2]=uint16_t(change);
        FinishRows<false>(f,nullptr,nullptr,nullptr,nullptr,alpha,HW,HH,0,H);
        assert(out[2*W+2]==shaded);assert(out[3*W+3]==palette[1]);
    }
    // Ground lighting is produced before blur, then reused without a second shadow application.
    uint32_t grounded[N];Frame ground=f;ground.out=grounded;
    FinishRows<false,false>(ground,nullptr,nullptr,nullptr,nullptr,alpha,HW,HH,0,H);
    f.groundColour=grounded;
    std::fill_n(rad,N,8);std::fill_n(blur,N,0); // zero coverage falls back to the already-lit world
    FinishRows<true>(f,blur,rad,nullptr,nullptr,nullptr,HW,HH,0,H);
    assert(out[2*W+2]==shaded);assert(out[3*W+3]==palette[1]);
    puts("PASS: opaque/translucent/no clouds over unknown-depth water, both compositor paths, odd sizes, UI and exterior");
}
'''
out = ROOT / 'artifacts/atmosphere-reference/water-cloud-tests'
out.mkdir(parents=True, exist_ok=True)
cpp=out/'water-cloud-tests.cpp'
cpp.write_text(code,encoding='utf-8')
compiler=Path(os.environ.get('CXX','C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'))
vc=compiler.parents[3]
sdk=Path('C:/Program Files (x86)/Windows Kits/10')
version=sorted((sdk/'Include').iterdir(),key=lambda p:p.name)[-1].name
env=os.environ.copy()
env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/version/'ucrt',sdk/'Include'/version/'shared',sdk/'Include'/version/'um']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/version/'ucrt/x64',sdk/'Lib'/version/'um/x64']))
exe=out/'water-cloud-tests.exe'
subprocess.run([str(compiler),'/nologo','/EHsc','/std:c++20','/O2',str(cpp),f'/Fo{out/"water-cloud-tests.obj"}',f'/Fe{exe}'],env=env,check=True)
subprocess.run([str(exe)],check=True)
