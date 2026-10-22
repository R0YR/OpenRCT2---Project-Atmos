"""Compile production leaf simulation/blending with a deterministic clock and scene depth."""
from pathlib import Path
import os, subprocess
ROOT=Path(__file__).resolve().parents[1]
src=(ROOT/'src/openrct2-ui/drawing/Atmosphere.cpp').read_text()
def extract(a,b):
    start=src.index(a)
    return src[start:src.index(b,start)]
code=r'''
#include "src/openrct2-ui/drawing/AtmosphereLeaf.h"
#include "src/openrct2/drawing/DepthPlane.h"
#include <vector>
#include <chrono>
#include <set>
#include <cassert>
#include <cstdio>
namespace Leaves=OpenRCT2::Ui::Atmosphere::Leaves;
namespace Drawing=OpenRCT2::Drawing;
struct Clock {using time_point=std::chrono::steady_clock::time_point;inline static time_point current{};static auto now(){return current;}};
struct TileElement{};struct CoordsXY{int x,y;};
struct { std::vector<TileElement> tileElements=std::vector<TileElement>(1);uint32_t currentTicks=1;} state;
auto& getGameState(){return state;}
bool MapIsLocationValid(CoordsXY p){return p.x>=0&&p.y>=0&&p.x<10000&&p.y<10000;}
int TileElementHeight(CoordsXY){return 0;}
struct {float leaves=.001f,windStrength=0,cloudWind=30;} _settings;
struct {bool active=false;float night=0;} _time;
float _precipLevelNow=0;
struct ExcludeRect{int left,top,right,bottom;};
struct ViewInfo{float worldPerPixel=1;int originX=96,originY=64,rotation=0;};
'''
code+=extract('        struct Frame\n','        // Exterior is owned')
code+=extract('        std::vector<Leaves::Leaf> _leaves;','        // Convex hull of up to 8 points')
code+=r'''
int main(){
    int cases=0;
    for(int rotation=0;rotation<4;rotation++)for(float wpp:{.5f,1.f,2.f,4.f})
    for(auto xy:{std::pair<float,float>{128.f,256.f},{-300.f,176.f}}){
        Leaves::Leaf leaf;leaf.x=xy.first;leaf.y=xy.second;leaf.z=96;
        auto p=Leaves::Project(leaf,rotation,wpp,-100,47);
        auto restored=Leaves::FromPixel((p.first-100)*wpp,(p.second+47)*wpp,
            Drawing::DepthEncode(int(Leaves::Depth(leaf,rotation))),rotation);
        assert(std::abs(restored.x-leaf.x)<.001f&&std::abs(restored.y-leaf.y)<.001f&&std::abs(restored.z-leaf.z)<.001f);
        auto shifted=Leaves::Project(leaf,rotation,wpp,-80,77);
        assert(std::abs(shifted.first-p.first+20)<.001f&&std::abs(shifted.second-p.second+30)<.001f);
        const auto d=Leaves::Depth(leaf,rotation);
        assert(!Leaves::Visible(Drawing::DepthEncode(int(d+64)),d,wpp));
        assert(Leaves::Visible(Drawing::DepthEncode(int(d-64)),d,wpp));cases++;
    }
    Leaves::Leaf motion;motion.z=96;motion.speed=8;
    Leaves::Advance(motion,1,0,0);assert(motion.z==88&&std::abs(motion.x)<2&&std::abs(motion.y)<2);
    constexpr int W=64,H=64,N=W*H;
    std::vector<uint8_t> bits(N,1),ui(N,0),sway(N,0);
    std::vector<uint16_t> depth(N),leafDepth(N);
    std::vector<uint32_t> target(N,0xFF707070);
    uint32_t palette[256]{};palette[1]=0xFF507D28;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)depth[y*W+x]=Drawing::DepthEncode(2*(y+64));
    Frame f{bits.data(),palette,target.data(),W,H,24,nullptr,16,8,0,ui.data(),palette,depth.data()};
    ViewInfo view;
    DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data()); // initialise map and clock
    Leaves::Leaf leaf;leaf.x=128;leaf.y=256;leaf.z=96;leaf.groundZ=0;leaf.age=1;leaf.life=15;
    leaf.r=80;leaf.g=140;leaf.b=35;_leaves.push_back(leaf);
    leafDepth=depth;Clock::current+=std::chrono::milliseconds(40);
    DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data());
    std::set<uint32_t> shades;int painted=0;
    for(int i=0;i<N;i++)if(target[i]!=0xFF707070){
        painted++;shades.insert(target[i]);assert(leafDepth[i]==Drawing::DepthEncode(384));
        assert(((target[i]>>24)&255)==255);assert(target[i]!=0xFF509423);
    }
    assert(painted>=6&&shades.size()>=3); // real simulation emits a translucent three-tone sprite
    std::fill(depth.begin(),depth.end(),Drawing::DepthEncode(1024));leafDepth=depth;
    std::fill(target.begin(),target.end(),0xFF707070);Clock::current+=std::chrono::milliseconds(40);
    DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data());
    for(int i=0;i<N;i++){assert(target[i]==0xFF707070);assert(leafDepth[i]==depth[i]);}
    std::fill(depth.begin(),depth.end(),Drawing::DepthEncode(0));leafDepth=depth;std::fill(ui.begin(),ui.end(),1);
    Clock::current+=std::chrono::milliseconds(40);DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data());
    for(auto p:target)assert(p==0xFF707070);
    std::vector<TileElement>(3).swap(state.tileElements);Clock::current+=std::chrono::milliseconds(40);
    DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data());assert(_leaves.empty());
    // Dense crown fixture: normal strength now produces a visible stream rather than a few rare particles.
    _leaves.clear();_leafAcc=0;_settings.leaves=.4f;_settings.windStrength=.5f;
    std::fill(ui.begin(),ui.end(),0);std::fill(sway.begin(),sway.end(),128);
    std::fill(depth.begin(),depth.end(),Drawing::DepthEncode(1024));
    for(int frame=0;frame<400;frame++) {
        Clock::current+=std::chrono::milliseconds(40);leafDepth=depth;
        DrawLife(f,view,bits.data(),sway.data(),target.data(),leafDepth.data());
    }
    assert(_leaves.size()>60&&_leaves.size()<200);
    assert(_leafAcc<1.0f);
    std::printf("PASS leaf density: %zu active leaves after 16 seconds at 40 percent, with bounded spawn accumulation\n",_leaves.size());
    std::printf("PASS %d world/height/rotation/zoom/pan cases; slow fall, native three-tone alpha blending, foreground/UI occlusion, own depth, map reset\n",cases);
}
'''
# Wiring: actual leaf colour and depth enter the normal world depth-of-field path, not the weather overlay.
assert src.index('DrawLife(groundFrame')<src.index('BuildRadiusRows(f, blurDepth')
assert 'DrawLife(fr, view, bits, sway, f.lifeA' not in src
out=ROOT/'artifacts/leaf-tests';out.mkdir(parents=True,exist_ok=True)
cpp=out/'leaves.cpp';cpp.write_text(code)
compiler=Path(os.environ.get('CXX','C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'))
vc=compiler.parents[3];sdk=Path('C:/Program Files (x86)/Windows Kits/10');v=sorted((sdk/'Include').iterdir(),key=lambda p:p.name)[-1].name
env=os.environ.copy();env['INCLUDE']=';'.join(map(str,[ROOT,vc/'include',sdk/'Include'/v/'ucrt',sdk/'Include'/v/'shared',sdk/'Include'/v/'um']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/v/'ucrt/x64',sdk/'Lib'/v/'um/x64']))
exe=out/'leaves.exe'
subprocess.run([str(compiler),'/nologo','/EHsc','/std:c++20','/O2',str(cpp),f'/Fo{out/"leaves.obj"}',f'/Fe{exe}'],env=env,check=True)
subprocess.run([str(exe)],check=True)
