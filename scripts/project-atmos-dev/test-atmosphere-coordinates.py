"""Compile actual light placement/drag/storage with integer viewport coordinates, including odd world points."""
from pathlib import Path
import os, subprocess
ROOT=Path(__file__).resolve().parents[1]
ui=(ROOT/'src/openrct2-ui/windows/OptionsAtmosphere.cpp').read_text(encoding='utf-8')
atmo=(ROOT/'src/openrct2-ui/drawing/Atmosphere.cpp').read_text(encoding='utf-8')
tool=ui[ui.index('    static std::optional<CoordsXY> CursorLightPlane('):ui.index('    void AtmosphereOptionsPage::ToolDrag(')]
place=atmo[atmo.index('    void LightPlaceAt('):atmo.index('    int32_t LightCount()',atmo.index('    void LightPlaceAt('))]
code=r'''
#include "src/openrct2-ui/drawing/AtmosphereCoordinates.h"
#include "src/openrct2-ui/drawing/AtmosphereLightPlacement.h"
#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

namespace Placement=OpenRCT2::Ui::Atmosphere::LightPlacement;
bool MapIsLocationValid(CoordsXY p){return p.x>=0&&p.y>=0&&p.x<10000&&p.y<10000;}
int TileElementHeight(CoordsXY p){return p.x<300?32:(p.x<400?48:128);}
namespace Atmosphere {
namespace LightPlacement=Placement;
struct Light{int x=257,y=354;float height=64;};
std::vector<Light> _lights;Light clipboard;bool copied=false;
int selected=-1;int LightCount(){return int(_lights.size());}int LightSelected(){return selected;}
const Light* LightAt(int i){return i>=0&&i<LightCount()?&_lights[i]:nullptr;}
void LightLoadEdit(){} void MarkLightsDirty(){}void LightSnap(int&,int&){}
void LightSelect(int i){selected=i;}
int LightPickAtScreen(int,int){return LightCount()?0:-1;}
float LightPlacementHeight(bool paste){return paste&&copied?clipboard.height:(selected>=0?_lights[selected].height:40.f);}
int LightAdd(int x,int y,bool paste){Light l;if(paste&&copied)l=clipboard;else if(selected>=0)l=_lights[selected];l.x=x;l.y=y;_lights.push_back(l);return selected=LightCount()-1;}
'''+place+r'''
}
struct Viewport{
 int rotation=0;int8_t zoom=0;ScreenCoordsXY pos{17,31},viewPos{-320,-256};
 ScreenCoordsXY ScreenToViewportCoord(ScreenCoordsXY p) const{
  p.x-=pos.x;p.y-=pos.y;
  if(zoom<0){p.x>>=-zoom;p.y>>=-zoom;}else{p.x<<=zoom;p.y<<=zoom;}
  p.x+=viewPos.x;p.y+=viewPos.y;return p;
 }
} viewport;
Viewport* ViewportFindFromPoint(ScreenCoordsXY){return &viewport;}
CoordsXY terrainSeed{272,368};
std::optional<CoordsXY> ScreenGetMapXY(ScreenCoordsXY,Viewport** v){*v=&viewport;return terrainSeed;}
struct Window{void invalidate(){}};using WidgetIndex=int;
struct AtmosphereOptionsPage{
 enum class LightTool{None,Place,Move,Pick,Paste};LightTool _tool=LightTool::Place;
 bool _grabBulb=false,_dragging=false;int _dragZ=0,_dragOffsetX=0,_dragOffsetY=0,_dragRotation=-1,_dragZoom=0;
 void DragSelectedLight(const ScreenCoordsXY&);void ToolDown(Window&,WidgetIndex,const ScreenCoordsXY&);
};
'''+tool+r'''
ScreenCoordsXY screen(int x,int y,int z){
 auto r=CoordsXY{x,y}.rotate(viewport.rotation);ScreenCoordsXY p{r.y-r.x,((r.x+r.y)>>1)-z};
 p.x-=viewport.viewPos.x;p.y-=viewport.viewPos.y;
 if(viewport.zoom<0){p.x<<=-viewport.zoom;p.y<<=-viewport.zoom;}else{p.x>>=viewport.zoom;p.y>>=viewport.zoom;}
 p.x+=viewport.pos.x;p.y+=viewport.pos.y;return p;
}
int main(){
 Window w;int placements=0,drags=0;
 for(int r=0;r<4;r++)for(int8_t zoom:{-1,0})for(int dx:{0,1,4,8,15,16,24,31})for(int dy:{0,1,4,8,15,16,24,31}) {
  viewport.rotation=r;viewport.zoom=zoom;
  Atmosphere::_lights.clear();Atmosphere::selected=-1;Atmosphere::copied=false;
  AtmosphereOptionsPage tool;
  const int x=256+dx,y=352+dy,z=72;
  auto p=screen(x,y,z);tool.ToolDown(w,0,p);
  assert(Atmosphere::LightCount()==1);const auto l=*Atmosphere::LightAt(0);
  assert(l.x==x&&l.y==y&&l.height+TileElementHeight({x,y})==z);
  assert(screen(l.x,l.y,z)==p);placements++;
 }
 for(int r=0;r<4;r++)for(int8_t zoom:{-1,0,1,2}){
  viewport.rotation=r;viewport.zoom=zoom;Atmosphere::_lights={{257,354,64}};Atmosphere::selected=0;
  AtmosphereOptionsPage tool;tool._tool=AtmosphereOptionsPage::LightTool::Pick;
  auto p=screen(257,354,96);p.y+=8;tool.ToolDown(w,0,p);tool.DragSelectedLight(p);
  assert(Atmosphere::_lights[0].x==257&&Atmosphere::_lights[0].y==354);
  auto delta=CoordsXY{64,0}.rotate(r);auto q=p;
  const int scale=1<<std::max(0,int(zoom));const int upscale=1<<std::max(0,-int(zoom));
  q.x+=(delta.y-delta.x)*upscale/scale;q.y+=((delta.x+delta.y)>>1)*upscale/scale;
  tool.DragSelectedLight(q);assert(Atmosphere::_lights[0].x==321&&Atmosphere::_lights[0].y==354);
  assert(Atmosphere::_lights[0].height==48); // absolute plane survives terrain step
  const auto before=Atmosphere::_lights[0];viewport.rotation=(r+1)&3;tool.DragSelectedLight(q);
  assert(Atmosphere::_lights[0].x==before.x&&Atmosphere::_lights[0].y==before.y&&Atmosphere::_lights[0].height==before.height);
  viewport.zoom=zoom+1;tool.DragSelectedLight(q);
  assert(Atmosphere::_lights[0].x==before.x&&Atmosphere::_lights[0].y==before.y);
  drags++;
 }
 // Clipboard lift and the placement template must agree even when selected light differs.
 viewport.rotation=3;viewport.zoom=0;Atmosphere::_lights={{257,354,12}};Atmosphere::selected=0;
 Atmosphere::clipboard={0,0,79};Atmosphere::copied=true;AtmosphereOptionsPage paste;paste._tool=AtmosphereOptionsPage::LightTool::Paste;
 paste.ToolDown(w,0,screen(279,353,111));assert(Atmosphere::_lights.back().x==279&&Atmosphere::_lights.back().y==353&&Atmosphere::_lights.back().height==79);
 std::printf("PASS actual light tools: %d free quarter/side/corner/odd placements; %d picked-stem drags with terrain/rotation/zoom rebasing; clipboard height\n",placements,drags);
}
'''
out=ROOT/"artifacts/atmosphere-coordinates-tests"
out.mkdir(parents=True,exist_ok=True)
cpp=out/"coordinates.cpp";cpp.write_text(code)
compiler=Path(os.environ.get("CXX","C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"))
vc=compiler.parents[3];sdk=Path("C:/Program Files (x86)/Windows Kits/10")
version=sorted((sdk/"Include").iterdir(),key=lambda p:p.name)[-1].name
env=os.environ.copy()
env["INCLUDE"]=";".join(map(str,[ROOT,ROOT/"src",vc/"include",sdk/"Include"/version/"ucrt",sdk/"Include"/version/"shared",sdk/"Include"/version/"um"]))
env["LIB"]=";".join(map(str,[vc/"lib/x64",sdk/"Lib"/version/"ucrt/x64",sdk/"Lib"/version/"um/x64"]))
exe=out/"coordinates.exe"
subprocess.run([str(compiler),"/nologo","/EHsc","/std:c++20","/O2",str(cpp),f"/Fo{out/'coordinates.obj'}",f"/Fe{exe}"],env=env,check=True)
subprocess.run([str(exe)],check=True)

