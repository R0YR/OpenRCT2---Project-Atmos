// Read-only native save diagnostics: supports, invisibility and supporting tile context.
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/GameState.h>
#include <openrct2/Game.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/EntityBase.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/drawing/Palette.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/paint/tile_element/Paint.TileElement.h>
#include <functional>
#include <openrct2/localisation/Language.h>
#include <vector>
#include <set>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/config/Config.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/Footpath.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <openrct2/world/tile_element/LargeSceneryElement.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <array>
#include <stdexcept>
using namespace OpenRCT2;
int main(int argc,char** argv){try{
 if(argc!=6)throw std::runtime_error("data user rct2 park output");
 gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
 gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
 Config::SetDefaults();auto context=CreateContext();if(!context->Initialise())throw std::runtime_error("init");
 if(!context->LoadParkFromFile(argv[4]))throw std::runtime_error("load");

 std::filesystem::create_directories(argv[5]);
 Drawing::X8DrawingEngine engine(context->GetUiContext());constexpr int W=320,H=240;engine.Resize(W,H);engine.SetAuxEnabled(false,false,false);Config::Get().general.multiThreading=false;
 std::ifstream csv("artifacts/scenery-support-diagnostics/support-owners.csv");std::string line;std::getline(csv,line);std::set<std::pair<int,int>> seen;
 while(std::getline(csv,line)){for(char& c:line)if(c==',')c=' ';std::istringstream row(line);int x,y,z,t,obj,img,sx,sy;row>>x>>y>>z>>t>>obj>>img>>sx>>sy;if(img!=3392&&img!=3393&&img!=3391||!seen.insert({x,y}).second)continue;
 Viewport vp{};vp.width=W;vp.height=H;vp.zoom=ZoomLevel{0};vp.rotation=3;vp.viewPos=Translate3DTo2DWithZ(3,{x*32,y*32,z})-ScreenCoordsXY{W/2,H/2};
 engine.BeginDraw();std::fill(engine.getRT()->bits,engine.getRT()->bits+W*H,Drawing::PaletteIndex{10});ResetAllSpriteQuadrantPlacements();ViewportRender(*engine.getRT(),&vp);engine.EndDraw();auto* bits=engine.getRT()->bits;
 std::ofstream ppm(std::filesystem::path(argv[5])/(std::to_string(x)+"-"+std::to_string(y)+"-"+std::to_string(z)+".ppm"),std::ios::binary);ppm<<"P6\n"<<W<<" "<<H<<"\n255\n";for(int i=0;i<W*H;i++){auto c=Drawing::gPalette[int(bits[i])];char px[]{char(c.red),char(c.green),char(c.blue)};ppm.write(px,3);}
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}


