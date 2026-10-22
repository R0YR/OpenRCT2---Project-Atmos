// Native support visibility and Tile Inspector contract; isolated profile, no save writes.
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/config/Config.h>
#include <openrct2/GameState.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/paint/Paint.SessionFlags.h>
#include <openrct2/paint/support/WoodenSupports.h>
#include <openrct2/paint/tile_element/Paint.TileElement.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/TileInspector.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/actions/GameActionResult.h>
#include <iostream>
#include <cstring>
#include <stdexcept>
using namespace OpenRCT2;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) { try {
    require(argc==5,"data user rct2 park");
    gOpenRCT2Headless=true; gOpenRCT2NoGraphics=false;
    gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
    Config::SetDefaults();auto context=CreateContext();require(context->Initialise(),"init");
    require(context->LoadParkFromFile(argv[4]),"load fixture");
    Drawing::RenderTarget rt{};rt.x=rt.y=-10000;rt.width=rt.height=20000;
    rt.cullingX=rt.cullingY=-10000;rt.cullingWidth=rt.cullingHeight=20000;
    for(uint8_t rotation=0;rotation<4;rotation++)for(int height:{48,96,144})for(int mode=0;mode<3;mode++) {
        ViewportFlags flags;
        if(mode)flags.set(ViewportFlag::hideSupports);
        if(mode==2)flags.set(ViewportFlag::invisibleSupports);
        auto* session=PaintSessionAlloc(rt,flags,rotation);
        session->Flags=PaintSessionFlags::PassedSurface;
        session->Support.height=16;session->Support.slope=0;
        bool rendered=WoodenBSupportsPaintSetupRotated(*session,WoodenSupportType::truss,
            WoodenSupportSubType::neSw,rotation,height,ImageId());
        require(rendered==(mode!=2),"native hidden support visibility differs");
        PaintSessionArrange(*session);
        require((session->PaintHead!=nullptr)==(mode!=2),"hidden supports emitted paint geometry");
        PaintSessionFree(session);
    }
    // Tile Inspector query must be read-only; execute flips only the selected element and is reversible.
    CoordsXY loc{};TileElement* element=nullptr;int selectedIndex=0;
    const auto size=getGameState().mapSize;
    for(int y=1;y<size.y-1&&!element;y++)for(int x=1;x<size.x-1&&!element;x++) {
        auto* next=MapGetFirstElementAt(TileCoordsXY{x,y});int index=0;
        if(!next)continue;
        do {
            if(!next->isInvisible()&&(next->getType()==TileElementType::smallScenery
                || next->getType()==TileElementType::largeScenery)) {
                loc={x*32,y*32};element=next;selectedIndex=index;break;
            }
            index++;
        } while(!(next++)->isLastForTile());
    }
    require(element!=nullptr,"fixture must have visible scenery");const auto before=*element;
    auto countScenery=[&](uint8_t rotation) {
        auto* session=PaintSessionAlloc(rt,{},rotation);
        TileElementPaintSetup(*session,loc);PaintSessionArrange(*session);
        int count=0;
        for(auto* ps=session->PaintHead;ps;ps=ps->NextQuadrantEntry)
            if(ps->Element==element)count++;
        PaintSessionFree(session);return count;
    };
    for(uint8_t rotation=0;rotation<4;rotation++)require(countScenery(rotation)>0,"visible scenery test is culled");
    TileInspector::ToggleInvisibilityOfElementAt(loc,selectedIndex,false);
    require(std::memcmp(element,&before,sizeof(before))==0,"Inspector query changed element or support flags");
    TileInspector::ToggleInvisibilityOfElementAt(loc,selectedIndex,true);
    require(element->isInvisible()!=before.isInvisible(),"Inspector execute did not hide element");
    for(uint8_t rotation=0;rotation<4;rotation++) {
        require(countScenery(rotation)==0,"hidden Inspector scenery emitted parent or supports");
    }
    TileInspector::ToggleInvisibilityOfElementAt(loc,selectedIndex,true);
    require(std::memcmp(element,&before,sizeof(before))==0,"Inspector restore changed element or support flags");
    std::cout<<"PASS native support visibility: 36 combinations (4 rotations, 3 heights, opaque/translucent/hidden); Inspector query, toggle, hidden-tile paint and restore\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;} }
