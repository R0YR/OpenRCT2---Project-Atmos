#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/EntityBase.h>
#include <openrct2/world/Weather.h>
#include <openrct2/config/Config.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/drawing/DepthPlane.h>
#include <openrct2/drawing/Palette.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2-ui/drawing/Atmosphere.h>
#include <openrct2-ui/drawing/AtmosphereRideShadows.h>
#include <openrct2-ui/windows/OptionsAtmosphere.h>
#include <openrct2/drawing/ColourMap.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <set>
#include <map>
#include <functional>
#include <chrono>
#include <algorithm>
using namespace OpenRCT2;
namespace A=OpenRCT2::Ui::Atmosphere;
static void ppm(const std::filesystem::path& path,const std::vector<uint32_t>& pixels,int w,int h){
    std::ofstream f(path,std::ios::binary);f<<"P6\n"<<w<<" "<<h<<"\n255\n";
    for(auto p:pixels){char rgb[]{char(p>>16),char(p>>8),char(p)};f.write(rgb,3);}
}
static void clear(A::Settings& s){
    s.tiltStrength=s.vignetteStrength=s.shadowStrength=s.fog=s.cloudCoverage=s.sunRays=0;
    s.waterWaves=s.waterDepth=s.waterGlitter=s.waterPatches=s.waterSun=0;
    s.leaves=0;s.swayTrees=s.weatherEffects=s.lightsEnabled=false;s.lightningStrength=s.snowCover=0;
}
int main(int argc,char** argv){try{
    if(argc!=7)throw std::runtime_error("data user rct2 park settings output");
    gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
    gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
    Config::SetDefaults();auto context=CreateContext();if(!context->Initialise())throw std::runtime_error("init");
    if(!context->LoadParkFromFile(argv[4]))throw std::runtime_error("load");
    if(std::getenv("ATMOS_QA_SERIAL"))Config::Get().general.multiThreading=false;
    auto& state=getGameState();const auto dir=std::filesystem::path(std::getenv("ATMOS_QA_OUTPUT")?std::getenv("ATMOS_QA_OUTPUT"):argv[6]);
    std::filesystem::create_directories(dir);
    if(const char* weather=std::getenv("ATMOS_QA_WEATHER")) {
        state.weatherCurrent.weatherType=static_cast<Weather::Type>(std::clamp(std::atoi(weather),0,8));
        state.weatherCurrent.level=Weather::Level::heavy;
        state.weatherCurrent.weatherGloom=2;
    }

    A::IsActive(argv[5]);auto base=A::GetSettings();base.enabled=true;base.leaves=0;base.swayTrees=false;
    if(std::getenv("ATMOS_QA_SHADOW_DAY")) {
        base.timeEnabled=false;base.timeCycle=false;base.sunElevation=55;base.sunAzimuth=225;base.shadowStrength=1;
    }
    if(std::getenv("ATMOS_QA_FULL_SHADOWS"))base.shadowGuestDistance=base.shadowVehicleDistance=base.shadowTrackDistance=0;
    constexpr int W=1920,H=1080;
    Viewport vp{};vp.width=W;vp.height=H;vp.zoom=state.savedViewZoom;vp.rotation=state.savedViewRotation;
    if(const char* zoom=std::getenv("ATMOS_QA_ZOOM"))vp.zoom=ZoomLevel{static_cast<int8_t>(std::atoi(zoom))};
    vp.viewPos=state.savedView-ScreenCoordsXY{vp.ViewWidth()/2,vp.ViewHeight()/2};
    if(const char* offset=std::getenv("ATMOS_QA_OFFSET_Y"))vp.viewPos.y+=std::atoi(offset);
    std::cout<<"map "<<state.mapSize.x<<","<<state.mapSize.y<<" center "<<state.savedView.x<<","<<state.savedView.y
        <<" zoom "<<int(static_cast<int8_t>(vp.zoom))<<" rotation "<<int(vp.rotation)<<std::endl;
    Drawing::X8DrawingEngine engine(context->GetUiContext());engine.Resize(W,H);engine.SetAuxEnabled(true,true,true);
    engine.BeginDraw();ResetAllSpriteQuadrantPlacements();ViewportRender(*engine.getRT(),&vp);engine.EndDraw();
    auto* rt=engine.getRT();const auto* bits=reinterpret_cast<const uint8_t*>(rt->bits);
    if(std::getenv("ATMOS_QA_TRACE")){
        engine.BeginDraw();
        std::ofstream report(dir/"paint-geometry.txt");std::set<uint32_t> seen;
        for(int offset:{100,400,1300}){
            auto col=*rt;col.zoom_level=vp.zoom;col.x=vp.zoom.ApplyInversedTo(vp.viewPos.x)+offset;
            col.y=vp.zoom.ApplyInversedTo(vp.viewPos.y);col.width=64;col.pitch=W-64;col.bits=rt->bits+offset;
            col.cullingX=col.x;col.cullingY=col.y;col.cullingWidth=col.width;col.cullingHeight=col.height;
            auto* session=PaintSessionAlloc(col,{},vp.rotation);PaintSessionGenerate(*session);PaintSessionArrange(*session);
            std::function<void(PaintStruct*)> visit=[&](PaintStruct* ps){
                if(seen.insert(ps->image_id.GetIndex()).second){auto* g=GfxGetG1Element(ps->image_id);
                    if(g)report<<ps->image_id.GetIndex()<<" image "<<g->width<<","<<g->height<<" offset "<<g->xOffset<<","<<g->yOffset
                        <<" boundsZ "<<ps->Bounds.z<<","<<ps->Bounds.z_end<<" flat "<<ps->DepthFlat<<" base "<<ps->DepthFlatBase<<" interaction "<<int(ps->InteractionItem)<<"\n";}
                if(ps->Children)visit(ps->Children);
            };
            for(auto* ps=session->PaintHead;ps;ps=ps->NextQuadrantEntry)visit(ps);
            const int pixel=500*W+offset+10;
            for(auto* ps=session->PaintHead;ps;){
                auto* next=ps->NextQuadrantEntry;ps->NextQuadrantEntry=nullptr;session->PaintHead=ps;
                const auto before=Drawing::gDepthPlane.data[pixel];const auto oldFlags=Drawing::gDepthPlane.shadowFlags[pixel];
                PaintDrawStructs(*session);
                if(before!=Drawing::gDepthPlane.data[pixel]||oldFlags!=Drawing::gDepthPlane.shadowFlags[pixel]){
                    auto* g=GfxGetG1Element(ps->image_id);
                    report<<"PIXEL "<<offset+10<<",500 writer "<<ps->image_id.GetIndex()<<" flat "<<ps->DepthFlat
                        <<" image "<<(g?g->width:0)<<","<<(g?g->height:0)<<" flags "<<int(Drawing::gDepthPlane.shadowFlags[pixel])
                        <<" depth "<<Drawing::gDepthPlane.data[pixel]<<"\n";
                }
                ps->NextQuadrantEntry=next;ps=next;
            }
            PaintSessionFree(session);
        }
        engine.EndDraw();
    }
    std::array<uint32_t,256> palette{};for(int i=0;i<256;i++)palette[i]=0xFF000000u|(uint32_t(Drawing::gPalette[i].red)<<16)|(uint32_t(Drawing::gPalette[i].green)<<8)|Drawing::gPalette[i].blue;
    A::ViewInfo view;const int zoom=int(static_cast<int8_t>(vp.zoom));view.worldPerPixel=std::ldexp(1.f,zoom);
    view.originX=vp.zoom.ApplyInversedTo(vp.viewPos.x);view.originY=vp.zoom.ApplyInversedTo(vp.viewPos.y);
    view.rotation=vp.rotation;view.viewportWidth=W;view.viewportHeight=H;
    std::vector<uint32_t> out(W*H);for(int i=0;i<W*H;i++)out[i]=palette[bits[i]];ppm(dir/"raw.ppm",out,W,H);
    if(std::getenv("ATMOS_QA_BENCHMARK")) {
        A::GetSettings()=base;A::GetSettings().devPerfLog=true;
        std::vector<double> paintTimes,effectTimes;
        for(int frame=0;frame<320;frame++) {
            const auto a=std::chrono::steady_clock::now();
            engine.BeginDraw();ViewportRender(*rt,&vp);engine.EndDraw();
            const auto b=std::chrono::steady_clock::now();
            A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
            const auto c=std::chrono::steady_clock::now();
            if(frame>=20){paintTimes.push_back(std::chrono::duration<double,std::milli>(b-a).count());effectTimes.push_back(std::chrono::duration<double,std::milli>(c-b).count());}
            if(frame%40==0)std::cout<<"benchmark frame "<<frame<<std::endl;
        }
        std::ofstream metrics(dir/"benchmark.tsv");metrics<<"stage\tmean_ms\tmedian_ms\tp95_ms\n";
        const auto report=[&](const char* name,std::vector<double> values){
            std::sort(values.begin(),values.end());double mean=0;for(auto v:values)mean+=v;mean/=values.size();
            std::cout<<name<<" mean "<<mean<<" median "<<values[values.size()/2]<<" p95 "<<values[values.size()*95/100]<<" ms"<<std::endl;
            metrics<<name<<'\t'<<mean<<'\t'<<values[values.size()/2]<<'\t'<<values[values.size()*95/100]<<'\n';
        };
        report("native",paintTimes);report("atmosphere",effectTimes);
        ppm(dir/"benchmark-full.ppm",out,W,H);
        return 0;
    }

    for(const char* name:{"full","grade","tilt","clouds","water","rays","shadows","lights"}){
        auto s=base;clear(s);std::string n=name;
        if(n=="full")s=base;
        if(n=="tilt"){s.tiltStrength=base.tiltStrength;s.vignetteStrength=base.vignetteStrength;}
        if(n=="clouds"){s.cloudCoverage=base.cloudCoverage;s.fog=base.fog;}
        if(n=="water"){s.waterWaves=base.waterWaves;s.waterDepth=base.waterDepth;s.waterGlitter=base.waterGlitter;s.waterPatches=base.waterPatches;s.waterSun=base.waterSun;}
        if(n=="rays")s.sunRays=base.sunRays;
        if(n=="shadows")s.shadowStrength=base.shadowStrength;
        if(n=="lights")s.lightsEnabled=base.lightsEnabled;
        A::GetSettings()=s;
        A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
        ppm(dir/(n+".ppm"),out,W,H);std::cout<<"wrote "<<n<<std::endl;
        if(n=="shadows" && std::getenv("ATMOS_QA_SHADOW_DAY")) {
            const auto withShadows=out;A::GetSettings().shadowStrength=0;
            A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
            size_t changed=0;for(size_t i=0;i<out.size();i++)changed+=out[i]!=withShadows[i];
            std::cout<<"peep-only shadows darken "<<changed<<" real park pixels"<<std::endl;
            if(changed==0)throw std::runtime_error("real park fixture has no visitor shadows");
            ppm(dir/"shadows-off.ppm",out,W,H);
        }
    }
    {
        auto s=base;clear(s);s.shadowStrength=base.shadowStrength;A::GetSettings()=s;
        A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
        const auto ranged=out;
        s.shadowGuestDistance=s.shadowVehicleDistance=s.shadowTrackDistance=0;A::GetSettings()=s;
        A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
        size_t different=0;
        for(size_t i=0;i<out.size();i++) {
            different+=out[i]!=ranged[i];
            for(int shift:{0,8,16})if(((ranged[i]>>shift)&255)<((out[i]>>shift)&255))
                throw std::runtime_error("distance LOD made a shadow darker instead of fading it");
        }
        if(vp.zoom<=ZoomLevel{0} && different!=0 && (base.shadowGuestDistance==0 || base.shadowGuestDistance>=80)
            && (base.shadowVehicleDistance==0 || base.shadowVehicleDistance>=160)
            && (base.shadowTrackDistance==0 || base.shadowTrackDistance>=160))throw std::runtime_error("recommended distance changed close-view pixels");
        std::cout<<"shadow range changed "<<different<<" pixels vs unlimited (only smooth lightening)"<<std::endl;
        ppm(dir/"shadows-unlimited.ppm",out,W,H);ppm(dir/"shadows-ranged.ppm",ranged,W,H);
    }
    {
        auto s=base;clear(s);s.shadowStrength=base.shadowStrength;A::GetSettings()=s;
        A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
        const auto enabled=out;
        std::vector<RideId> changed;
        for(uint16_t n=0;n<Limits::kMaxRidesInPark;n++) {
            const auto id=RideId::FromUnderlying(n);
            if(GetRide(id)&&A::RideShadows::IsEnabled(id)) {
                if(!A::RideShadows::SetEnabled(id,false))throw std::runtime_error("ride toggle save failed");
                changed.push_back(id);
            }
        }
        A::ProcessFrame(bits,palette.data(),out.data(),W,H,{16,8,0},{},engine.GetDepthPlane(),view,bits,engine.GetSwayPlane());
        size_t changes=0;for(size_t i=0;i<out.size();i++)changes+=out[i]!=enabled[i];
        for(const auto id:changed)A::RideShadows::SetEnabled(id,true);
        if(!A::RideShadows::kRenderingEnabled && changes!=0)throw std::runtime_error("ride toggle reactivated parked shadows");
        std::cout<<"ride toggle changed "<<changes<<" pixels across "<<changed.size()<<" rides (including cached track geometry)"<<std::endl;
        ppm(dir/"ride-shadows-off.ppm",out,W,H);
    }
    for(int i=0;i<W*H;i++){
        const auto flags=Drawing::gDepthPlane.shadowFlags[i];
        out[i]=0xFF000000u|((flags&Drawing::kShadowReceiver)?0x800000:0)|((flags&Drawing::kWaterSurface)?0x00FF00:0)|((flags&Drawing::kExteriorSurface)?0x0000FF:0);
    }
    ppm(dir/"surface-flags.ppm",out,W,H);
    std::ofstream raw(dir/"native-depth.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(engine.GetDepthPlane()),W*H*2);
    std::vector<uint8_t> savedBits(bits,bits+W*H),savedFlags(Drawing::gDepthPlane.shadowFlags,Drawing::gDepthPlane.shadowFlags+W*H);
    // Identical palette colours do not prove unchanged ownership inside a peep sprite.
    // Exclude its native bounds; column-wide corruption outside those bounds remains checked.
    std::vector<uint8_t> peepBounds(W*H);
    for(const auto type:{EntityType::guest,EntityType::staff})
        for(const auto id:state.entities.getEntityList(type)) {
            const auto* entity=state.entities.tryGetEntity(id);if(!entity)continue;
            const auto p=Translate3DTo2DWithZ(vp.rotation,entity->getLocation());
            const int left=vp.zoom.ApplyInversedTo(p.x-entity->spriteData.width-vp.viewPos.x)-1;
            const int right=vp.zoom.ApplyInversedTo(p.x+entity->spriteData.width-vp.viewPos.x)+1;
            const int top=vp.zoom.ApplyInversedTo(p.y-entity->spriteData.heightMin-vp.viewPos.y)-1;
            const int bottom=vp.zoom.ApplyInversedTo(p.y+entity->spriteData.heightMax-vp.viewPos.y)+1;
            for(int y=std::max(0,top);y<std::min(H,bottom);y++)
                for(int x=std::max(0,left);x<std::min(W,right);x++)peepBounds[y*W+x]=1;
        }
    vp.flags.set(ViewportFlag::hideGuests);vp.flags.set(ViewportFlag::hideStaff);
    engine.BeginDraw();ViewportRender(*rt,&vp);engine.EndDraw();
    if(std::getenv("ATMOS_QA_SHADOW_DAY")) {
        std::map<int,int> floorHeightDeltas;
        for(const auto type:{EntityType::guest,EntityType::staff})for(const auto id:state.entities.getEntityList(type)) {
            const auto* entity=state.entities.tryGetEntity(id);if(!entity)continue;
            const auto p=Translate3DTo2DWithZ(vp.rotation,entity->getLocation());
            const int px=vp.zoom.ApplyInversedTo(p.x-vp.viewPos.x),py=vp.zoom.ApplyInversedTo(p.y-vp.viewPos.y);
            if(px<0||px>=W||py<0||py>=H)continue;
            const size_t i=static_cast<size_t>(py)*W+px;
            if((Drawing::gDepthPlane.shadowFlags[i]&(Drawing::kShadowReceiver|Drawing::kSceneryReceiver))
                !=(Drawing::kShadowReceiver|Drawing::kSceneryReceiver))continue;
            const int z=Drawing::DepthDecode(engine.GetDepthPlane()[i])/2-vp.zoom.ApplyTo(py)-vp.viewPos.y;
            floorHeightDeltas[entity->z-z]++;
        }
        std::ofstream heights(dir/"paving-height-deltas.txt");
        for(const auto& [delta,count]:floorHeightDeltas)heights<<delta<<' '<<count<<'\n';
    }
    size_t errors=0,checked=0;
    for(int y=1;y<H-1;y++)for(int x=1;x<W-1;x++){
        const int i=y*W+x;bool same=true;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)same&=savedBits[i+dy*W+dx]==bits[i+dy*W+dx];
        if(same&&!peepBounds[i]&&(savedFlags[i]&Drawing::kShadowReceiver || Drawing::gDepthPlane.shadowFlags[i]&Drawing::kShadowReceiver)){
            checked++;errors+=savedFlags[i]!=Drawing::gDepthPlane.shadowFlags[i];
        }
    }
    std::cout<<"guest visibility ground invariance "<<errors<<" mismatches / "<<checked<<" unchanged pixels\n";
    if(errors!=0)throw std::runtime_error("guest rendering changes unrelated ground surface flags");
    engine.Resize(480,372);engine.BeginDraw();rt=engine.getRT();
    engine.GetDrawingContext()->Clear(*rt,Drawing::getColourMap(Drawing::Colour::lightBlue).light);
    OpenRCT2::Ui::Window window;window.width=424;window.height=372;
    OpenRCT2::Ui::Windows::AtmosphereOptionsPage page;page.SetSideLayout(true);
    std::vector<OpenRCT2::Widget> widgets;page.AppendWidgets(widgets);window.setWidgets(widgets);
    page.DrawNavigation(window,*rt);
    OpenRCT2::Ui::Windows::AtmosphereOptionsPage::DrawTabIcon(*rt,{442,10},false,0);
    OpenRCT2::Ui::Windows::AtmosphereOptionsPage::DrawCinemaIcon(*rt,{446,44},false);
    std::vector<uint32_t> ui(480*372);for(size_t i=0;i<ui.size();i++)ui[i]=palette[static_cast<uint8_t>(rt->bits[i])];
    ppm(dir/"navigation-icons.ppm",ui,480,372);engine.EndDraw();
    {
        struct Preview:OpenRCT2::Ui::Window {
            OpenRCT2::Ui::Windows::AtmosphereOptionsPage page;
            ScreenSize onScrollGetSize(int32_t i) override{return page.ScrollGetSize(i);}
        } sun;
        sun.width=424;sun.height=372;sun.colours[0]=Drawing::Colour::lightBlue;sun.colours[1]=Drawing::Colour::lightBlue;
        sun.page.SetSideLayout(true);sun.page.SetSubPage(1);
        std::vector<OpenRCT2::Widget> controls;sun.page.AppendWidgets(controls);sun.setWidgets(controls);sun.initScrollWidgets();
        engine.BeginDraw();rt=engine.getRT();engine.GetDrawingContext()->Clear(*rt,Drawing::getColourMap(Drawing::Colour::lightBlue).light);
        sun.page.OnPageOpened();sun.page.PrepareDraw(sun);sun.drawWidgets(*rt);sun.page.DrawNavigation(sun,*rt);
        for(size_t i=0;i<ui.size();i++)ui[i]=palette[static_cast<uint8_t>(rt->bits[i])];
        ppm(dir/"sun-distance-controls.ppm",ui,480,372);engine.EndDraw();
    }
    std::cout<<"PASS actual Pacific native frame and isolated effect exports\n";
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
