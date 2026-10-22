"""Compile production weather helpers: world anchoring, bounded cover and white lightning."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "src/openrct2-ui/drawing/Atmosphere.cpp").read_text(encoding="utf-8")
mapping = source[source.index("        std::pair<float, float> CloudMapPoint("):source.index("        struct CloudFrame")]
lightning = source[source.index("        float BoltOrigin("):source.index("        // Colour of a world pixel;")]
code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>
struct Frame { int32_t width,height; const uint8_t* ui; uint32_t* out; uint32_t rShift,gShift,bShift; };
''' + mapping + lightning + r'''
int main() {
    int cases=0;
    // Forward-project fixed world points independently, then recover them through production inverse.
    for(float x:{-125.25f,0.f,32.125f,1024.5f,4096.f})
    for(float y:{-64.75f,0.f,129.5f,2048.f})
    for(float z:{0.f,150.f,700.f,2000.f})
    for(float wpp:{.5f,1.f,2.f,8.f})
    for(int rotation=0;rotation<4;rotation++) {
        float rx=x,ry=y;
        if(rotation==1){rx=y;ry=-x;} if(rotation==2){rx=-x;ry=-y;} if(rotation==3){rx=-y;ry=x;}
        const float screenX=(ry-rx)/wpp-327.25f;
        const float screenY=((rx+ry)*.5f-z)/wpp+213.5f;
        auto p=CloudMapPoint((screenX+327.25f)*wpp,(screenY-213.5f)*wpp,z,rotation);
        assert(std::abs(p.first-x)<.001f && std::abs(p.second-y)<.001f); cases++;
    }
    for(int percent=0;percent<=100;percent++) for(float multiplier:{.55f,1.f,1.5f,2.3f})
    for(float precip:{0.f,.5f,1.f}) for(bool enabled:{false,true}) {
        float configured=percent/100.f;
        float c=WeatherCloudCoverage(configured,multiplier,precip,enabled);
        assert(c>=0 && c<=configured);
        if(percent==100) assert(c==1);
        if(percent==0) assert(c==0);
    }
    // Storm transition remains monotonic and cannot turn 50% cover into overcast.
    float last=0;
    for(int i=0;i<=100;i++) {
        float c=WeatherCloudCoverage(.5f,1.f+1.3f*i/100.f,i/100.f,true);
        assert(c>=last && c<=.6f);last=c;
    }
    constexpr int W=128,H=96,N=W*H;
    std::vector<uint8_t> ui(N,0),cover((W/2)*(H/2),255);
    std::vector<uint32_t> pixels(N,0xffa04020u);
    Frame fr{W,H,ui.data(),pixels.data(),0,8,16};
    _cloudPlanes.cloud=cover.data();
    const int bx=int(BoltOrigin(123)*W),by=int(.18f*H),index=by*W+bx;
    ui[0]=1;
    LightningRows(fr,.7f,123,0,H);
    assert(pixels[0]==0xffa04020u); // UI never flashes
    assert((pixels[index]&0xffffffu)==0xffffffu); // cloud near bolt is neutral white
    assert((pixels[index]&0xff000000u)==0xff000000u);
    auto cloudy=pixels[index];
    std::fill(pixels.begin(),pixels.end(),0xffa04020u);
    _cloudPlanes.cloud=nullptr;
    LightningRows(fr,.3f,123,0,H);
    auto ground=pixels[index];
    std::fill(pixels.begin(),pixels.end(),0xffa04020u);
    _cloudPlanes.cloud=cover.data();
    LightningRows(fr,.3f,123,0,H);
    assert((pixels[index]&255)>(ground&255));
    std::fill(pixels.begin(),pixels.end(),0xffa04020u);
    DrawLightningBolt(fr,123,.5f);
    int white=0;for(auto p:pixels) {white+=(p&0xffffffu)==0xffffffu;assert((p>>24)==255);}
    assert(white>=14); assert(pixels[0]==0xffa04020u);
    auto before=pixels; DrawLightningBolt(fr,123,0);assert(pixels==before);
    // Every seed keeps its complete core/branches within the compact 32.5% height envelope.
    for(uint32_t seed=0;seed<256;seed++) {
        std::fill(pixels.begin(),pixels.end(),0xffa04020u);
        DrawLightningBolt(fr,seed,.5f);
        int touched=0;
        for(int y=0;y<H;y++) for(int x=0;x<W;x++) if(pixels[y*W+x]!=0xffa04020u) {
            assert(y < std::ceil(H*.325f)); touched++;
        }
        assert(touched>0);
    }

    std::printf("PASS: %d cloud projection/rotation/zoom cases; 2424 coverage cases; bounded storm transition; white bolt/cloud scattering/UI preservation\n",cases);
}
'''
out = root / "artifacts/atmosphere-reference/weather-tests"
out.mkdir(parents=True, exist_ok=True)
cpp = out / "weather-tests.cpp"
cpp.write_text(code, encoding="utf-8")
compiler = Path(os.environ.get("CXX", "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"))
vc = compiler.parents[3]
sdk = Path("C:/Program Files (x86)/Windows Kits/10")
version = sorted((sdk / "Include").iterdir(), key=lambda p: p.name)[-1].name
env = os.environ.copy()
env["INCLUDE"] = ";".join(map(str, [vc/"include", sdk/"Include"/version/"ucrt", sdk/"Include"/version/"shared", sdk/"Include"/version/"um"]))
env["LIB"] = ";".join(map(str, [vc/"lib/x64", sdk/"Lib"/version/"ucrt/x64", sdk/"Lib"/version/"um/x64"]))
exe = out / "weather-tests.exe"
subprocess.run([str(compiler), "/nologo", "/EHsc", "/std:c++20", "/O2", str(cpp), f"/Fo{out/'weather-tests.obj'}", f"/Fe{exe}"], env=env, check=True)
subprocess.run([str(exe)], check=True)

