"""Compile the actual peep-shadow helpers and check subpixel stability (Windows/MSVC)."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/openrct2-ui/drawing/Atmosphere.cpp").read_text(encoding="utf-8")
start = source.index("        std::pair<float, float> ProjectPeepShadowPoint(")
end = source.index("        // Fills a convex polygon", start)
helpers = source[start:end]
old_start = helpers.index("        void RasterShadowCapsule(")
old_end = helpers.index("        void RasterPeepShadowCapsule(", old_start)
# The baseline is compiled beside the fix, without modifying either implementation.
assert "for (int32_t y = top & ~1; y <= bottom; y += 2)" in helpers[old_start:old_end]
assert "for (int32_t x = left & ~1; x <= right; x += 2)" in helpers[old_start:old_end]
code = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <vector>
#include <utility>
#include <cstdio>
#include <cstring>
#include <climits>
'''+helpers+r'''
constexpr int W=47,H=39,N=W*H;
using Raster=void(*)(uint8_t*,int16_t*,int32_t,int32_t,const ShadowCapsule&,float);
struct Plane {
    std::vector<uint8_t> p=std::vector<uint8_t>(N+2,0);
    std::vector<int16_t> z=std::vector<int16_t>(N+2,INT16_MIN);
    Plane(){p.front()=p.back()=173;z.front()=z.back()=1234;}
    void draw(const ShadowCapsule& c, float inner=.5f, Raster r=RasterPeepShadowCapsule){
        r(p.data()+1,z.data()+1,W,H,c,inner);
        assert(p.front()==173 && p.back()==173 && z.front()==1234 && z.back()==1234);
        for(int i=1;i<=N;i++) if(p[i]) assert(z[i]!=INT16_MIN);
    }
    int mass() const{int n=0;for(int i=1;i<=N;i++)n+=p[i];return n;}
};
int main(){
    int oldZero=0;
    for(int phase=0;phase<81;phase++){
        Plane p;p.draw({18,16+phase*.025f,23,16+phase*.025f,1.15f,7},.5f,RasterShadowCapsule);
        oldZero+=p.mass()==0;
    }
    assert(oldZero>0);
    float worst=0;int sweeps=0;
    constexpr float pi=3.14159265358979323846f;
    for(float wpp:{.5f,1.f,2.f,2.6f}) for(int rotation=0;rotation<4;rotation++)
    for(int angle=0;angle<360;angle+=30) for(float inner:{.1f,.5f,.9f}) for(float radiusWorld:{4.0f,4.4f}) {
        const float scale=.5f/wpp;
        const auto a=ProjectPeepShadowPoint(0,0,0,rotation,scale,0,0);
        const auto b=ProjectPeepShadowPoint(std::cos(angle*pi/180)*12,std::sin(angle*pi/180)*12,0,rotation,scale,0,0);
        const float radius=std::max(.9f,radiusWorld*scale);
        int lo=INT_MAX,hi=0;
        for(int py=0;py<16;py++) for(int px=0;px<16;px++){
            const float x=20+px/16.f,y=18+py/16.f;
            Plane p;p.draw({x+a.first,y+a.second,x+b.first,y+b.second,radius,31},inner);
            assert(p.mass()>0);lo=std::min(lo,p.mass());hi=std::max(hi,p.mass());
        }
        const float variation=float(hi-lo)/hi;
        worst=std::max(worst,variation);assert(variation<.15f);sweeps++;
    }
    // Explicit minimum radii and degenerate capsule: every phase remains visible.
    for(float radius:{.9f,1.15f,2.3f}) for(int py=0;py<32;py++)for(int px=0;px<32;px++){
        Plane p;float x=20+px/32.f,y=18+py/32.f;p.draw({x,y,x,y,radius,7});assert(p.mass()>0);
    }
    // Exact integer translations are a useful seam: no phase-dependent grid anchoring.
    Plane a,b; a.draw({17.25f,16.125f,21.75f,17.375f,1.15f,7});
    b.draw({18.25f,17.125f,22.75f,18.375f,1.15f,7});
    for(int y=0;y<H-1;y++)for(int x=0;x<W-1;x++)assert(a.p[1+y*W+x]==b.p[1+(y+1)*W+x+1]);
    const auto before=a.p;a.draw({17.25f,16.125f,21.75f,17.375f,1.15f,7});assert(a.p==before);
    Plane c,d,u;c.draw({17,16,21,17,1.15f,7});d.draw({19,17,23,18,2.3f,9});
    u.draw({17,16,21,17,1.15f,7});u.draw({19,17,23,18,2.3f,9});
    for(int i=1;i<=N;i++)assert(u.p[i]==std::max(c.p[i],d.p[i]));
    for(const auto shape:{ShadowCapsule{-2,-2,2,2,2.3f,7},ShadowCapsule{W-1.f,H-1.f,W+2.f,H+2.f,2.3f,7},ShadowCapsule{-100,-100,-90,-90,.9f,7}}){Plane p;p.draw(shape);}
    Plane empty;empty.draw({20,18,25,19,0,7});assert(empty.mass()==0);
    // Continuous projection for negative world coordinates and each view rotation.
    for(int r=0;r<4;r++){
        auto p=ProjectPeepShadowPoint(-11.25f,-7.5f,3.25f,r,.5f,4,6);
        constexpr float expectedX[]={-.125f,7.375f,-3.875f,-11.375f};
        constexpr float expectedY[]={-9.3125f,-3.6875f,.0625f,-5.5625f};
        assert(std::abs(p.first-expectedX[r])<.00001f && std::abs(p.second-expectedY[r])<.00001f);
        auto q=ProjectPeepShadowPoint(-11.249f,-7.5f,3.25f,r,.5f,4,6);
        assert(std::isfinite(p.first)&&std::isfinite(p.second));
        assert(std::abs(p.first-q.first)>.0001f && std::abs(p.first-q.first)<.001f);
        assert(std::abs(p.second-q.second)<.001f);
        auto shifted=ProjectPeepShadowPoint(-11.25f,-7.5f,3.25f,r,.5f,6,8);
        assert(std::abs(shifted.first-p.first+1)<.00001f && std::abs(shifted.second-p.second+1)<.00001f);
    }
    std::printf("PASS: %d sun/rotation/zoom/softness sweeps; worst mass variation %.3f%%; baseline vanished %d/81 phases; bounds, degenerate, union, translation, continuous projection\n",sweeps,worst*100,oldZero);
}
'''
out = ROOT / "artifacts/atmosphere-reference/peep-shadow-tests"
out.mkdir(parents=True, exist_ok=True)
cpp = out / "peep-shadow-tests.cpp"
cpp.write_text(code, encoding="utf-8")
compiler = Path(os.environ.get("CXX", "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"))
vc = compiler.parents[3]
sdk = Path("C:/Program Files (x86)/Windows Kits/10")
version = sorted((sdk / "Include").iterdir(), key=lambda p:p.name)[-1].name
env = os.environ.copy()
env["INCLUDE"] = ";".join(map(str,[vc/"include",sdk/"Include"/version/"ucrt",sdk/"Include"/version/"shared",sdk/"Include"/version/"um"]))
env["LIB"] = ";".join(map(str,[vc/"lib/x64",sdk/"Lib"/version/"ucrt/x64",sdk/"Lib"/version/"um/x64"]))
exe = out / "peep-shadow-tests.exe"
subprocess.run([str(compiler),"/nologo","/EHsc","/std:c++20","/O2",str(cpp),f"/Fo{out/'peep-shadow-tests.obj'}",f"/Fe{exe}"],env=env,check=True)
subprocess.run([str(exe)],check=True)
