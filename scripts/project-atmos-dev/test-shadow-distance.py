"""Compile production distance/fade and capsule helpers; validate conservative clipping and smooth LOD."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'src/openrct2-ui/drawing/Atmosphere.cpp').read_text(encoding='utf-8')
a=source.index('        struct ShadowCapsule');b=source.index('        // Fills a convex polygon',a)
code=r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <climits>
#include <vector>
#include <utility>
#include <cassert>
#include <cstdio>
#include "openrct2-ui/drawing/AtmosphereShadowDistance.h"
#include "openrct2-ui/drawing/AtmosphereCoordinates.h"
using namespace OpenRCT2::Ui::Atmosphere;
'''+source[a:b]+r'''
int main(){
    for(float tiles:{0.f,1.f,8.f,40.f,80.f,160.f,256.f}) for(float padding:{0.f,4.f,120.f,900.f}) {
        float last=1;
        for(int i=0;i<=2000;i++) {
            const float radius=i*8.f;
            const float value=ShadowDistance::RangeFade(tiles,32.f+radius,-40.f,32.f,-40.f,padding);
            assert(value>=0 && value<=last);last=value;
            if(tiles==0)assert(value==1);
            if(tiles>0 && radius>=tiles*32+padding)assert(value==0);
        }
        assert(ShadowDistance::RangeFade(tiles,32,-40,32,-40,padding)==1);
    }
    for(bool guest:{false,true}) {
        float last=1;
        for(int i=0;i<=3200;i++) {
            const float wpp=.01f*i;
            const float v=ShadowDistance::ZoomFade(80,wpp,guest);
            assert(v>=0 && v<=last);last=v;
            if(i>0)assert(std::abs(v-ShadowDistance::ZoomFade(80,wpp-.01f,guest))<.003f);
        }
        assert(last==0);
        assert(ShadowDistance::ZoomFade(0,8,guest)==1);
        assert(ShadowDistance::ZoomFade(0,32,guest)==1);
    }
    constexpr int W=47,H=39,N=W*H;
    uint32_t random=9237;const auto next=[&](){random=random*1664525u+1013904223u;return float(random>>8)/16777215.f;};
    int excluded=0;
    for(int n=0;n<2400;n++) {
        ShadowCapsule c{next()*240-100,next()*200-80,next()*240-100,next()*200-80,next()*10+.1f,0};
        std::vector<uint8_t> full(N);std::vector<int16_t> z(N,INT16_MIN);
        RasterPeepShadowCapsule(full.data(),z.data(),W,H,c,.5f);
        if(!ShadowDistance::Intersects(c.ax,c.ay,c.bx,c.by,c.radius,W,H)) {
            assert(std::all_of(full.begin(),full.end(),[](auto v){return v==0;}));excluded++;
        }
        auto faded=c;faded.opacity=.5f;
        std::vector<uint8_t> half(N);RasterPeepShadowCapsule(half.data(),z.data(),W,H,faded,.5f);
        for(int i=0;i<N;i++)assert(half[i]<=full[i]);
    }
    // Both endpoints may be offscreen while their shadow crosses the view.
    assert(ShadowDistance::Intersects(-1000,20,1000,20,2,W,H));
    // An offscreen caster whose displaced shadow reaches the distance circle must not be dropped.
    assert(ShadowDistance::RangeFade(8,8*32+60,0,0,0,120)>0);
    for(int rotation=0;rotation<4;rotation++) for(float z:{0.f,40.f,800.f}) {
        const auto p=Coordinates::Rotate(1200.f,800.f,rotation);
        const auto world=Coordinates::Unproject(p.second-p.first,(p.first+p.second)*.5f-z,z,rotation);
        assert(std::abs(world.first-1200)<.001f && std::abs(world.second-800)<.001f);
    }
    std::printf("PASS shadow distance: monotonic range/zoom fades, zero preserves full detail; 2400 capsule envelopes (%d safely excluded), opacity and rotation invariance\n",excluded);
}
'''
out=root/'artifacts/shadow-distance';out.mkdir(parents=True,exist_ok=True)
cpp=out/'test.cpp';cpp.write_text(code,encoding='utf-8')
compiler=Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe')
vc=compiler.parents[3];sdk=Path('C:/Program Files (x86)/Windows Kits/10');version=sorted((sdk/'Include').iterdir(),key=lambda p:p.name)[-1].name
env=os.environ.copy();env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/version/'ucrt',sdk/'Include'/version/'shared',sdk/'Include'/version/'um']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/version/'ucrt/x64',sdk/'Lib'/version/'um/x64']))
exe=out/'test.exe'
subprocess.run([str(compiler),'/nologo','/EHsc','/std:c++20','/O2','/I'+str(root/'src'),str(cpp),'/Fo'+str(out/'test.obj'),'/Fe'+str(exe)],env=env,check=True)
subprocess.run([str(exe)],check=True)
