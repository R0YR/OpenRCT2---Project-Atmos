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
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/Object.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
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
#include <array>
#include <stdexcept>
#include <openrct2/paint/tile_element/ScenerySupports.h>
#include <openrct2/paint/tile_element/Paint.Surface.h>
#include <openrct2/paint/tile_element/Paint.SmallScenery.h>
using namespace OpenRCT2;
int main(int argc,char** argv){try{
 if(argc!=6)throw std::runtime_error("data user rct2 park output");
 gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
 gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
 Config::SetDefaults();auto context=CreateContext();if(!context->Initialise())throw std::runtime_error("init");
 if(!context->LoadParkFromFile(argv[4]))throw std::runtime_error("load");
 std::ofstream csv(argv[5]);csv<<"x,y,index,type,z,clearance,hidden,object,needs_supports,no_supports,allow_above,hidden_path_same_height\n";
 std::array<int,8> count{},hidden{};int needs=0,noSupport=0,hiddenPath=0,groundScenery=0,largeSupport=0;
 auto size=getGameState().mapSize;
 for(int y=0;y<size.y;y++)for(int x=0;x<size.x;x++){
  auto* first=MapGetFirstElementAt(TileCoordsXY{x,y});if(!first)continue;
  auto* e=first;int index=0;
  do{
   int type=int(e->getType());if(type<8){count[type]++;hidden[type]+=e->isInvisible();}
   int obj=-1,need=-1,no=-1,above=-1,hiddenAt=0;
   if(auto* sc=e->asSmallScenery()){
    obj=sc->getEntryIndex();need=sc->needsSupports();needs+=need;
    if(auto* entry=sc->getEntry()){no=entry->flags.has(SmallSceneryFlag::hasNoSupports);above=entry->flags.has(SmallSceneryFlag::allowSupportsAbove);noSupport+=no;}
   }else if(auto* sc=e->asLargeScenery()){
    obj=sc->getEntryIndex();auto* entry=sc->getEntry();int seq=sc->getSequenceIndex();
    if(entry&&seq<int(entry->tiles.size())){need=entry->tiles[seq].hasSupports;above=entry->tiles[seq].allowSupportsAbove;largeSupport+=need;}
   }
   if(obj>=0){
    auto* other=first;do{if(other->getType()==TileElementType::path&&other->isInvisible()&&other->getBaseZ()==e->getBaseZ())hiddenAt++;}while(!(other++)->isLastForTile());
    hiddenPath+=hiddenAt>0;groundScenery+=e->getClearanceZ()-e->getBaseZ()<=16;
   }
   csv<<x<<','<<y<<','<<index<<','<<type<<','<<e->getBaseZ()<<','<<e->getClearanceZ()<<','<<e->isInvisible()<<','<<obj<<','<<need<<','<<no<<','<<above<<','<<hiddenAt<<'\n';
   index++;
  }while(!(e++)->isLastForTile());
 }
 std::cout<<"file "<<argv[4]<<" map "<<size.x<<','<<size.y<<'\n';
 for(int i=0;i<8;i++)std::cout<<"type "<<i<<" count "<<count[i]<<" hidden "<<hidden[i]<<'\n';
 std::cout<<"small needsSupports="<<needs<<" objectHasNoSupports="<<noSupport<<" large hasSupports="<<largeSupport<<" sceneryWithHiddenPathAtSameHeight="<<hiddenPath<<" thinScenery="<<groundScenery<<'\n';
 if(std::getenv("ATMOS_SUPPORT_BLOCKER_TEST")) {
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
 }
 if(std::getenv("ATMOS_SUPPORT_OBJECT")) {
  const int target=std::atoi(std::getenv("ATMOS_SUPPORT_OBJECT"));
  std::ofstream detail(std::filesystem::path(argv[5]).parent_path()/"ship-support-object.txt");
  auto* loaded=context->GetObjectManager().GetLoadedObject(ObjectType::smallScenery,target);
  if(loaded)detail<<"SmallScenery "<<target<<" identifier "<<loaded->GetIdentifier()<<" name "<<loaded->GetName()<<'\n';
  for(int y=1;y<size.y-1;y++)for(int x=1;x<size.x-1;x++) {
   auto* first=MapGetFirstElementAt(TileCoordsXY{x,y});if(!first)continue;auto* next=first;bool match=false;
   do{if(auto* sc=next->asSmallScenery())if(sc->getEntryIndex()==target)match=true;}while(!(next++)->isLastForTile());
   if(!match)continue;detail<<"TILE "<<x<<","<<y<<'\n';next=first;int index=0;
   do{detail<<" element "<<index++<<" type "<<int(next->getType())<<" z "<<next->getBaseZ()<<" clearance "<<next->getClearanceZ()<<" hidden "<<next->isInvisible();
    if(auto* sc=next->asSmallScenery()){detail<<" small "<<sc->getEntryIndex()<<" needsSupports "<<sc->needsSupports();if(auto* en=sc->getEntry())detail<<" name "<<LanguageGetString(en->name)<<" objectHeight "<<int(en->height)<<" fullTile "<<en->flags.has(SmallSceneryFlag::occupiesFullTile)<<" vOffsetCentre "<<en->flags.has(SmallSceneryFlag::vOffsetCentre)<<" flag27 "<<en->flags.has(SmallSceneryFlag::flag27)<<" noSupports "<<en->flags.has(SmallSceneryFlag::hasNoSupports)<<" allowAbove "<<en->flags.has(SmallSceneryFlag::allowSupportsAbove);}
    if(auto* surface=next->asSurface())detail<<" water "<<surface->getWaterHeight();
    detail<<'\n';
   }while(!(next++)->isLastForTile());
  }
 }
 if(std::getenv("ATMOS_SUPPORT_OWNERS")) {
  Drawing::RenderTarget rt{};rt.x=rt.y=-20000;rt.width=rt.height=40000;
  rt.cullingX=rt.cullingY=-20000;rt.cullingWidth=rt.cullingHeight=40000;
  std::ofstream owners(std::filesystem::path(argv[5]).parent_path()/"support-owners.csv");
  owners<<"x,y,z,type,object,image,sx,sy\n";std::set<int> bridgeImages;
  for(int y=1;y<size.y-1;y++)for(int x=1;x<size.x-1;x++) {
   auto* session=PaintSessionAlloc(rt,{},3);TileElementPaintSetup(*session,{x*32,y*32});PaintSessionArrange(*session);
   std::function<void(PaintStruct*)> visit=[&](PaintStruct* ps){
    int image=ps->image_id.GetIndex();auto* e=ps->Element;if(x==128&&y==63)bridgeImages.insert(image);
    const auto* path=e?e->asPath():nullptr;const auto* rail=path?path->getRailingsDescriptor():nullptr;
    const bool bridge=rail&&image>=rail->bridgeImage+49&&image<=rail->bridgeImage+54;if(bridge)bridgeImages.insert(image);
    if(e&&((image>=3390&&image<=3656)||bridge)) {
     int obj=bridge?int(rail->bridgeImage):-1;if(auto* sc=e->asSmallScenery())obj=sc->getEntryIndex();else if(auto* sc=e->asLargeScenery())obj=sc->getEntryIndex();
     owners<<x<<','<<y<<','<<e->getBaseZ()<<','<<int(e->getType())<<','<<obj<<','<<image<<','<<ps->ScreenPos.x<<','<<ps->ScreenPos.y<<'\n';
    }
    if(ps->Children)visit(ps->Children);
   };
   for(auto* ps=session->PaintHead;ps;ps=ps->NextQuadrantEntry)visit(ps);
   PaintSessionFree(session);
  }
  Drawing::X8DrawingEngine preview(context->GetUiContext());preview.Resize(128,128);preview.SetAuxEnabled(false,false,false);
  bridgeImages.insert(3394);bridgeImages.insert(3392);bridgeImages.insert(3513);
  for(int image:bridgeImages){preview.BeginDraw();auto& target=*preview.getRT();std::fill(target.bits,target.bits+128*128,Drawing::PaletteIndex{10});GfxDrawSpriteSoftware(target,ImageId(image),{64,64});preview.EndDraw();
   std::ofstream ppm(std::filesystem::path(argv[5]).parent_path()/("bridge-sprite-"+std::to_string(image)+".ppm"),std::ios::binary);ppm<<"P6\n128 128\n255\n";
   for(int i=0;i<128*128;i++){auto c=Drawing::gPalette[int(target.bits[i])];char pixel[]{char(c.red),char(c.green),char(c.blue)};ppm.write(pixel,3);}
  }
 }
 if(std::getenv("ATMOS_SUPPORT_RENDER")) {
  Config::Get().general.multiThreading=false;
  const auto dir=std::filesystem::path(argv[5]).parent_path();
  Drawing::X8DrawingEngine engine(context->GetUiContext());constexpr int W=1920,H=1080;engine.Resize(W,H);
  const auto& state=getGameState();
  for(int scene=0;scene<8;scene++) {
   if(const char* only=std::getenv("ATMOS_SUPPORT_SCENE"))if(scene!=std::atoi(only))continue;
   Viewport vp{};vp.width=W;vp.height=H;
   vp.zoom=scene<4?ZoomLevel{3}:ZoomLevel{0};vp.rotation=scene<4?scene:scene==4?state.savedViewRotation:3;
   auto center=scene<4?Translate3DTo2DWithZ(vp.rotation,{size.x*16,size.y*16,0}):scene==4?state.savedView:Translate3DTo2DWithZ(vp.rotation,scene==5?CoordsXYZ{3904,2400,0}:scene==7?CoordsXYZ{4736,3008,0}:CoordsXYZ{3200,3072,0});
   vp.viewPos=center-ScreenCoordsXY{vp.ViewWidth()/2,vp.ViewHeight()/2};
   if(scene==5&&std::getenv("ATMOS_SUPPORT_TRACE")) {
    engine.SetAuxEnabled(false,false,false);engine.BeginDraw();auto* rt=engine.getRT();std::fill(rt->bits,rt->bits+W*H,Drawing::PaletteIndex{10});
    std::ofstream trace(dir/"pier-pixel-writers.txt");
    for(int offset:{736,768,800,832}) {
     auto col=*rt;col.zoom_level=vp.zoom;col.x=vp.viewPos.x+offset;col.y=vp.viewPos.y;
     col.width=32;col.pitch=W-32;col.bits=rt->bits+offset;
     col.cullingX=col.x;col.cullingY=col.y;col.cullingWidth=col.width;col.cullingHeight=col.height;
     auto* session=PaintSessionAlloc(col,{},vp.rotation);PaintSessionGenerate(*session);PaintSessionArrange(*session);
     for(auto* ps=session->PaintHead;ps;) {
      auto* next=ps->NextQuadrantEntry;ps->NextQuadrantEntry=nullptr;session->PaintHead=ps;
      std::vector<uint8_t> old(80*64);
      for(int yy=0;yy<80;yy++)for(int xx=0;xx<32;xx++)old[yy*32+xx]=uint8_t(rt->bits[(520+yy)*W+offset+xx]);
      auto keyBefore=rt->bits[563*W+799];PaintDrawStructs(*session);int changes=0;
      for(int yy=0;yy<80;yy++)for(int xx=0;xx<32;xx++)changes+=old[yy*32+xx]!=uint8_t(rt->bits[(520+yy)*W+offset+xx]);
      if(changes){auto* e=ps->Element;const char* name="";if(e&&e->asSmallScenery()&&e->asSmallScenery()->getEntry())name=LanguageGetString(e->asSmallScenery()->getEntry()->name);int obj=-1;if(e){if(auto* sc=e->asSmallScenery())obj=sc->getEntryIndex();else if(auto* sc=e->asLargeScenery())obj=sc->getEntryIndex();}
       trace<<"col "<<offset<<" key "<<(keyBefore!=rt->bits[563*W+799])<<" name "<<name<<" image "<<ps->image_id.GetIndex()<<" changed "<<changes<<" tile "<<ps->MapPos.x/32<<","<<ps->MapPos.y/32<<" type "<<(e?int(e->getType()):-1)<<" obj "<<obj<<" z "<<(e?e->getBaseZ():-1)<<" screen "<<ps->ScreenPos.x<<","<<ps->ScreenPos.y<<"\n";}
      ps->NextQuadrantEntry=next;ps=next;
     }
     PaintSessionFree(session);
    }
    engine.EndDraw();
   }
   std::vector<uint8_t> reference;
   for(int aux=0;aux<(scene==4||scene==5||scene==7?6:3);aux++) {
    vp.flags={};if(aux>=2){vp.flags.set(ViewportFlag::hideSupports);vp.flags.set(ViewportFlag::invisibleSupports);}
    if(aux==3){vp.flags.set(ViewportFlag::hideScenery);vp.flags.set(ViewportFlag::invisibleScenery);}
    if(aux==4){vp.flags.set(ViewportFlag::hidePaths);vp.flags.set(ViewportFlag::invisiblePaths);}
    if(aux==5){vp.flags.set(ViewportFlag::hideRides);vp.flags.set(ViewportFlag::invisibleRides);}
    engine.SetAuxEnabled(aux,aux,aux);engine.BeginDraw();std::fill(engine.getRT()->bits,engine.getRT()->bits+W*H,Drawing::PaletteIndex{10});ResetAllSpriteQuadrantPlacements();
    ViewportRender(*engine.getRT(),&vp);engine.EndDraw();
    auto* bytes=reinterpret_cast<const uint8_t*>(engine.getRT()->bits);
    if(!aux)reference.assign(bytes,bytes+W*H);
    else if(aux==1) {size_t different=0;for(int i=0;i<W*H;i++)different+=reference[i]!=bytes[i];
     std::cout<<"RAW AUX scene "<<scene<<" different "<<different<<'\n';
     if(different)throw std::runtime_error("aux planes alter native colour output");}
    else {
     size_t different=0;for(int i=0;i<W*H;i++)different+=reference[i]!=bytes[i];
     std::cout<<"HIDE SUPPORTS scene "<<scene<<" category "<<aux<<" different "<<different<<'\n';
     auto name=std::filesystem::path(argv[5]).stem().string()+"-hidden-"+std::to_string(scene)+(aux>2?"-category-"+std::to_string(aux):"");
     std::ofstream raw(dir/(name+".bin"),std::ios::binary);raw.write(reinterpret_cast<const char*>(bytes),W*H);
     std::ofstream ppm(dir/(name+".ppm"),std::ios::binary);ppm<<"P6\n"<<W<<" "<<H<<"\n255\n";
     for(int i=0;i<W*H;i++){auto c=Drawing::gPalette[bytes[i]];char pixel[]{char(c.red),char(c.green),char(c.blue)};ppm.write(pixel,3);}
    }
   }
   auto name=std::filesystem::path(argv[5]).stem().string()+"-raw-"+std::to_string(scene);
   std::ofstream raw(dir/(name+".bin"),std::ios::binary);raw.write(reinterpret_cast<char*>(reference.data()),reference.size());
   std::ofstream ppm(dir/(name+".ppm"),std::ios::binary);ppm<<"P6\n"<<W<<" "<<H<<"\n255\n";
   for(auto ix:reference){auto c=Drawing::gPalette[ix];char pixel[]{char(c.red),char(c.green),char(c.blue)};ppm.write(pixel,3);}
  }
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}


