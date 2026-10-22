// Compile the actual scenery window alongside native core/UI libraries; isolated user profile only.
#include "../src/openrct2-ui/windows/Scenery.cpp"
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/drawing/DepthPlane.h>
#include <openrct2/drawing/Palette.h>
#include "../src/openrct2-ui/interface/Theme.h"
#include <openrct2/paint/Paint.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace OpenRCT2;
namespace F=OpenRCT2::Drawing::AtmosphereFoliage;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
    check(argc==5,"data user rct2 park");
    gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
    gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
    Config::SetDefaults();auto context=CreateContext();check(context->Initialise(),"context init");
    check(context->LoadParkFromFile(argv[4]),"Pacific load");
    Ui::ThemeManagerInitialise();
    auto& objects=context->GetObjectManager();F::SyncProfile(objects);
    F::overrides.clear();F::Refresh(objects); // deterministic fixture; no live preferences touched
    size_t tree=kMaxSmallSceneryObjects,custom=kMaxSmallSceneryObjects;
    int originals=0,customs=0;
    for(size_t i=0;i<kMaxSmallSceneryObjects;i++)if(auto* object=objects.GetLoadedObject(ObjectType::smallScenery,i)){
        if(F::Default(*object)){check(F::IsEnabled(ObjectType::smallScenery,i),"original foliage disabled");tree=i;originals++;}
        if(!object->GetIdentifier().starts_with("rct")){
            check(!F::IsEnabled(ObjectType::smallScenery,i),"custom object auto-classified");customs++;
            auto* entry=static_cast<SmallSceneryEntry*>(object->GetLegacyData());
            if(entry && entry->height>8 && entry->height<80)custom=i;
        }
    }
    check(tree<kMaxSmallSceneryObjects&&custom<kMaxSmallSceneryObjects,"test requires original plants and custom scenery");
    auto* shrub=objects.LoadObject("rct2.scenery_small.tsh1");check(shrub&&F::Default(*shrub),"unflagged original shrub lost its default");
    check(!F::IsEnabled(ObjectType::smallScenery,kObjectEntryIndexNull),"invalid slot enabled");
    Drawing::X8DrawingEngine engine(context->GetUiContext());engine.Resize(800,500);engine.SetAuxEnabled(true,true,true);
    auto swayPixels=[&](size_t slot){
        engine.BeginDraw();auto& rt=*engine.getRT();rt.cullingX=rt.cullingY=0;rt.cullingWidth=800;rt.cullingHeight=500;
        std::fill(rt.bits,rt.bits+800*500,Drawing::PaletteIndex{10});
        std::fill_n(Drawing::gDepthPlane.sway,800*500,uint8_t{0});
        TileElement element{};element.clearAs(TileElementType::smallScenery);element.asSmallScenery()->setEntryIndex(slot);
        auto* entry=static_cast<SmallSceneryEntry*>(objects.GetLoadedObject(ObjectType::smallScenery,slot)->GetLegacyData());
        auto* session=PaintSessionAlloc(rt,{},0);session->CurrentlyDrawnTileElement=&element;
        session->InteractionType=ViewportInteractionItem::scenery;session->SpritePosition={200,600};
        Drawing::gSwayEnabled.store(true);
        check(PaintAddImageAsParent(*session,ImageId(entry->image),{0,0,0},{32,32,entry->height})!=nullptr,"sprite outside test target");
        PaintSessionArrange(*session);PaintDrawStructs(*session);PaintSessionFree(session);engine.EndDraw();
        size_t count=0;for(int i=0;i<800*500;i++)count+=(engine.GetSwayPlane()[i]>>4)>0;return count;
    };
    check(swayPixels(tree)>0,"original tree has no native foliage coverage");
    check(swayPixels(custom)==0,"unselected custom object creates sway/leaf coverage");
    Ui::Windows::SceneryWindow window;window.onOpen();window.windowPos={0,0};
    window.colours[0]=Drawing::Colour::lightBlue;window.colours[1]=Drawing::Colour::mossGreen;
    window.setSelectedItem({SCENERY_TYPE_SMALL,static_cast<ObjectEntryIndex>(custom)},std::nullopt,std::nullopt,std::nullopt,std::nullopt);
    window.onPrepareDraw();constexpr auto checkbox=Ui::Windows::WIDX_SCENERY_FOLIAGE;
    check(window.widgets[checkbox].bottom<window.height,"foliage checkbox clipped at minimum window height");
    window.onMouseUp(checkbox);window.onPrepareDraw();
    check(F::IsEnabled(ObjectType::smallScenery,custom),"actual checkbox failed to enable selected object");
    check(swayPixels(custom)>0,"selected custom object does not create native sway/leaf coverage");
    engine.BeginDraw();auto& rt=*engine.getRT();std::fill(rt.bits,rt.bits+800*500,Drawing::PaletteIndex{10});
    window.onDraw(rt);engine.EndDraw();
    std::ofstream ppm(std::filesystem::path(argv[2]).parent_path()/"scenery-foliage-checkbox.ppm",std::ios::binary);
    ppm<<"P6\n800 500\n255\n";for(int i=0;i<800*500;i++){auto c=Drawing::gPalette[int(rt.bits[i])];char rgb[]{char(c.red),char(c.green),char(c.blue)};ppm.write(rgb,3);}
    F::profile.clear();F::overrides.clear();F::SyncProfile(objects);
    check(F::IsEnabled(ObjectType::smallScenery,custom),"object choice lost on preference reload");
    window.onMouseUp(checkbox);check(!F::IsEnabled(ObjectType::smallScenery,custom)&&swayPixels(custom)==0,"unticking leaves foliage coverage behind");
    check(F::SetEnabled(objects,ObjectType::smallScenery,tree,false),"override original tree off");
    F::profile.clear();F::SyncProfile(objects);check(!F::IsEnabled(ObjectType::smallScenery,tree),"original off override lost");
    check(F::SetEnabled(objects,ObjectType::smallScenery,tree,true),"restore original default");
    for(const auto type: {ObjectType::largeScenery,ObjectType::walls}) {
        ObjectEntryIndex slot=0;while(slot<F::kSlots&&!objects.GetLoadedObject(type,slot))slot++;
        check(slot<F::kSlots,"missing large/wall fixture");
        const bool before=F::IsEnabled(type,slot);
        window.setSelectedItem({static_cast<uint8_t>(type==ObjectType::walls?SCENERY_TYPE_WALL:SCENERY_TYPE_LARGE),slot},std::nullopt,std::nullopt,std::nullopt,std::nullopt);
        window.onPrepareDraw();window.onMouseUp(checkbox);
        check(F::IsEnabled(type,slot)!=before,"large/wall checkbox did not toggle");
        F::profile.clear();F::SyncProfile(objects);
        check(F::IsEnabled(type,slot)!=before,"large/wall preference reload failed");
        window.onMouseUp(checkbox);check(F::IsEnabled(type,slot)==before,"large/wall checkbox did not restore");
    }
    check(F::SetEnabled(objects,ObjectType::smallScenery,custom,true),"custom on before remapping");
    auto descriptor=objects.GetLoadedObject(ObjectType::smallScenery,custom)->GetDescriptor();
    objects.UnloadObjects({descriptor});check(!F::IsEnabled(ObjectType::smallScenery,custom),"unloaded slot retains foliage flag");
    const ObjectEntryIndex replacement=2000;
    check(objects.LoadObject(descriptor,replacement)!=nullptr,"reload at different slot");
    check(F::IsEnabled(ObjectType::smallScenery,replacement),"persistent choice followed slot instead of object identity");
    std::cout<<"PASS foliage: "<<originals<<" original defaults, "<<customs<<" custom objects off; actual checkbox/native coverage, persistence, untick, original override, large/wall toggles, unload and slot remapping\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
