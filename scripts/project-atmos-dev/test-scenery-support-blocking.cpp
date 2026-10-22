// Native Pacific regression: scenery support blockers, small/large auto supports, and rotation.
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/config/Config.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/paint/tile_element/Paint.TileElement.h>
#include <openrct2/paint/tile_element/Paint.Surface.h>
#include <openrct2/paint/tile_element/Paint.SmallScenery.h>
#include <openrct2/paint/tile_element/ScenerySupports.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <openrct2/world/tile_element/LargeSceneryElement.h>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace OpenRCT2;
int main(int argc,char**argv){try{
 if(argc!=5)throw std::runtime_error("data user rct2 PacificPark");
 gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
 gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
 Config::SetDefaults();auto context=CreateContext();if(!context->Initialise())throw std::runtime_error("init");
 if(!context->LoadParkFromFile(argv[4]))throw std::runtime_error("load Pacific fixture");

  Drawing::RenderTarget rt{};rt.x=rt.y=-20000;rt.width=rt.height=40000;
  rt.cullingX=rt.cullingY=-20000;rt.cullingWidth=rt.cullingHeight=40000;
  int cases=0;
  for(const auto loc:std::array<CoordsXY,3>{{{162*32,87*32},{65*32,113*32},{59*32,129*32}}}) {
   auto* first=MapGetFirstElementAt(loc);auto* next=first;TileElement* blocker=nullptr;TileElement* target=nullptr;
   do{if(auto* sc=next->asSmallScenery()){if(sc->getEntryIndex()==241)blocker=next;else if(sc->getEntryIndex()==201&&loc.x==162*32)target=next;}
    if(auto* sc=next->asLargeScenery())if((sc->getEntryIndex()==8&&loc.x==65*32)||(sc->getEntryIndex()==98&&loc.x==59*32))target=next;
   }while(!(next++)->isLastForTile());
   if(!blocker||!target)throw std::runtime_error("support blocker fixture mismatch");
   const bool before=blocker->isInvisible();
   if(loc.x==162*32){
    auto* session=PaintSessionAlloc(rt,{},0);session->MapPosition=loc;session->SpritePosition=loc;
    auto* surface=first->asSurface();session->CurrentlyDrawnTileElement=first;
    PaintSurface(*session,0,surface->getBaseZ(),*surface);
    std::cout<<"STATE terrain general "<<session->Support.height<<" slope "<<int(session->Support.slope)<<" blocked "<<ScenerySupportsAreBlocked(*session)<<'\n';
    if(session->Support.height!=224||ScenerySupportsAreBlocked(*session))throw std::runtime_error("terrain support state mismatch");
    session->CurrentlyDrawnTileElement=blocker;PaintSmallScenery(*session,blocker->getDirection(),blocker->getBaseZ(),*blocker->asSmallScenery());
    std::cout<<"STATE blocker general "<<session->Support.height<<" slope "<<int(session->Support.slope)<<" blocked "<<ScenerySupportsAreBlocked(*session)<<'\n';
    if(session->Support.height!=224||!ScenerySupportsAreBlocked(*session))throw std::runtime_error("actual blocker must reserve all segments without changing general terrain height");
    PaintSessionFree(session);
   }
   for(uint8_t rotation=0;rotation<4;rotation++) {
    SupportHeight savedGeneral{};int savedBody=0;
    for(int remove=0;remove<2;remove++) {
     blocker->setInvisible(remove);auto* session=PaintSessionAlloc(rt,{},rotation);
     TileElementPaintSetup(*session,loc);PaintSessionArrange(*session);int supports=0,body=0;
     for(auto* ps=session->PaintHead;ps;ps=ps->NextQuadrantEntry)if(ps->Element==target){int ix=ps->image_id.GetIndex();if(ix>=3390&&ix<=3656)supports++;else body++;}
     std::cout<<"BLOCKER tile "<<loc.x/32<<","<<loc.y/32<<" rot "<<int(rotation)<<" removed "<<remove<<" supports "<<supports<<" body "<<body<<" height "<<session->Support.height<<" slope "<<int(session->Support.slope)<<'\n';
     bool valid=body>0&&(remove?supports>0:supports==0);
     if(!remove){savedGeneral=session->Support;savedBody=body;valid&=ScenerySupportsAreBlocked(*session);}
     else {valid&=session->Support.height==savedGeneral.height&&session->Support.slope==savedGeneral.slope&&body==savedBody;}
     PaintSessionFree(session);blocker->setInvisible(before);
     if(!valid)throw std::runtime_error("scenery blocker must suppress auto supports, retain body/support state, and restore ordinary supports when removed");
     cases++;
    }
   }
  }
  PaintSession mask{};for(auto& s:mask.SupportSegments)s.height=0xFFFF;
  if(!ScenerySupportsAreBlocked(mask))throw std::runtime_error("full block ignored");
  for(auto& s:mask.SupportSegments){s.height=224;if(ScenerySupportsAreBlocked(mask))throw std::runtime_error("partial block disabled all supports");s.height=0xFFFF;}
  std::cout<<"PASS native Pacific support blockers: "<<cases<<" cases (stone/pier and two large road objects, four rotations, blocker/remove control), bodies and support heights retained; nine partial-mask controls\n";
 
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
