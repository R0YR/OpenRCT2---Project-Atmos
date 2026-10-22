// Real core + UI renderer integration, isolated profile/fixture, no save writes.
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/GameState.h>
#include <openrct2/config/Config.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/ImageImporter.h>
#include <openrct2/drawing/Image.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/entity/Balloon.h>
#include <openrct2/entity/Duck.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2-ui/drawing/Atmosphere.h>
#include <openrct2-ui/drawing/AtmosphereLightPlacement.h>
#include <openrct2-ui/drawing/AtmosphereRideShadows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace OpenRCT2;
namespace Atmo=OpenRCT2::Ui::Atmosphere;
static void require(bool x,const char* message){if(!x)throw std::runtime_error(message);}
static void testLogoRle(){
    // Transparent gaps after x=255 used to wrap RLE run offsets in 300px strips.
    Image image;image.Width=336;image.Height=39;image.Depth=32;image.Stride=image.Width*4;
    image.Pixels.resize(image.Stride*image.Height);
    for(uint32_t y=0;y<image.Height;y++)for(uint32_t x=0;x<image.Width;x++){
        auto* p=&image.Pixels[y*image.Stride+x*4];
        p[0]=240;p[1]=160;p[2]=40;p[3]=(x%19<10||x==280)?255:0;
    }
    for(int left=0;left<336;left+=224){
        Drawing::ImageImportMeta meta;meta.importMode=Drawing::ImportMode::closest;
        meta.srcOffset={left,0};meta.srcSize={std::min(224,336-left),39};
        const auto rle=Drawing::ImageImporter().Import(image,meta);
        meta.importFlags={};const auto raw=Drawing::ImageImporter().Import(image,meta);
        std::vector<uint8_t> decoded(raw.Buffer.size());
        for(int y=0;y<39;y++){
            size_t pos=rle.Buffer[y*2]|(size_t(rle.Buffer[y*2+1])<<8);
            bool last=false;
            while(!last){
                const auto code=rle.Buffer.at(pos++);last=(code&128)!=0;
                const int start=rle.Buffer.at(pos++),count=code&127;
                require(start+count<=meta.srcSize.width,"logo RLE run outside sprite");
                for(int x=0;x<count;x++)decoded[y*meta.srcSize.width+start+x]=rle.Buffer.at(pos++);
            }
        }
        require(decoded==raw.Buffer,"logo RLE corrupts letters or transparency");
    }
    std::cout<<"PASS native logo RLE: split 336px banner preserves every pixel and transparent gap\n";
}
static void testWaterBlits(){
    using namespace Drawing;
    PaletteIndex source=static_cast<PaletteIndex>(1),pixel=static_cast<PaletteIndex>(120);
    std::array<PaletteIndex,256> colours{};
    for(int i=0;i<256;i++)colours[i]=static_cast<PaletteIndex>(255-i);
    PaletteMap remap{std::span<PaletteIndex>(colours)};
    uint16_t z=100,receiver=100;uint8_t flags=0,receiverFlags=0;
    DepthBlitCtx ctx;ctx.plane=&z;ctx.base=&pixel;ctx.shadowFlagsPlane=&flags;
    ctx.receiverDepthPlane=&receiver;
    ctx.receiverFlagsPlane=&receiverFlags;
    ctx.shadowFlags=kShadowReceiver|kWaterSurface;
    BlitPixelDepth<kBlendDst>(&source,&pixel,remap,ctx,200);
    require(z==200&&flags==(kShadowReceiver|kWaterSurface),"blended water must write actual surface depth/coverage");
    require(receiver==200,"blended water must update receiver depth");
    require(receiverFlags==(kShadowReceiver|kWaterSurface),"water must update receiver material");
    ctx.shadowFlags=0;
    BlitPixelDepth<kBlendDst>(&source,&pixel,remap,ctx,500);
    require(z==200&&(flags&kWaterSurface),"glass must preserve underlying water geometry");
    source=PaletteIndex::transparent;ctx.shadowFlags=kWaterSurface;
    BlitPixelDepth<kBlendDst|kBlendTransparent>(&source,&pixel,remap,ctx,500);
    require(z==200,"transparent source must not write geometry");
    source=static_cast<PaletteIndex>(1);ctx.shadowFlags=0;
    BlitPixelDepth<0>(&source,&pixel,remap,ctx,600);
    require(z==600&&flags==0,"opaque object must replace water coverage");
    require(receiver==200,"upright guest/boat must preserve underlying receiver");
    require(receiverFlags==(kShadowReceiver|kWaterSurface),"upright guest/boat must preserve underlying water material");
    ctx.shadowFlags=kExteriorSurface;BlitPixelDepth<0>(&source,&pixel,remap,ctx,700);
    require(receiver==kDepthUnknown,"exterior must invalidate receiver depth");
    require(receiverFlags==0,"exterior must invalidate receiver material");
    std::cout<<"PASS native blended-water/glass/transparent/opaque depth contracts\n";
}
int main(int argc,char**argv){try{
    require(argc==5,"arguments: data user rct2 fixture");
    testLogoRle();
    testWaterBlits();
    gOpenRCT2Headless=true;gOpenRCT2NoGraphics=false;
    gCustomOpenRCT2DataPath=argv[1];gCustomUserDataPath=argv[2];gCustomRCT2DataPath=argv[3];
    Config::SetDefaults();
    auto context=CreateContext();require(context->Initialise(),"context init");
    {
        int cases=0,legacyErrors=0;
        for(int rotation=0;rotation<4;rotation++) for(int tx:{0,96,256})for(int ty:{0,128,352})
        for(int dx:{0,1,4,8,15,16,24,31,32})for(int dy:{0,1,4,8,15,16,24,31,32})for(int z:{0,39,40,97}) {
            const CoordsXYZ point{tx+dx,ty+dy,z};
            const auto projected=Translate3DTo2DWithZ(rotation,point);
            const auto restored=Atmo::LightPlacement::AtPlane(projected,z,rotation);
            require(restored==CoordsXY{point.x,point.y},"exact native light inverse changed quarter/corner coordinates");
            legacyErrors+=ViewportPosToMapPos(projected,z,rotation)!=restored;
            const auto rotated=CoordsXY{point.x,point.y}.rotate(rotation);
            require(Atmo::LightPlacement::PlaneY(rotated.x,rotated.y,z)==projected.y,"light renderer parity differs from native scenery");
            cases++;
        }
        require(legacyErrors>0,"odd coordinate fixture did not expose native inverse parity loss");
        std::cout<<"PASS native light projection: "<<cases<<" exact quarter/side/corner/odd roundtrips; legacy inverse mismatches "<<legacyErrors<<"\n";
    }
    {
        Drawing::X8DrawingEngine engine(context->GetUiContext());
        engine.Resize(64,48);engine.SetAuxEnabled(true,true,false);
        require(Drawing::gDepthPlane.shadowFlags!=nullptr,"native surface flags were not allocated");
        require(Drawing::gDepthPlane.receiverDepth!=nullptr,"native receiver depth was not allocated");
        require(Drawing::gDepthPlane.receiverFlags!=nullptr,"native receiver material was not allocated");
        require(Drawing::gDepthPlane.shadowHeight==nullptr,"parked silhouette heights unexpectedly enabled");
        Drawing::gDepthPlane.shadowFlags[12*64+12]=Drawing::kWaterSurface;
        Drawing::gDepthPlane.receiverDepth[12*64+12]=12345;
        Drawing::gDepthPlane.receiverFlags[12*64+12]=Drawing::kWaterSurface;
        engine.CopyRect(13,13,8,8,1,1);
        require(Drawing::gDepthPlane.shadowFlags[13*64+13]==Drawing::kWaterSurface,"native scroll lost water flags");
        require(Drawing::gDepthPlane.receiverDepth[13*64+13]==12345,"native scroll lost receiver depth");
        require(Drawing::gDepthPlane.receiverFlags[13*64+13]==Drawing::kWaterSurface,"native scroll lost receiver material");
        engine.Resize(97,73);
        require(Drawing::gDepthPlane.size==97*73&&Drawing::gDepthPlane.shadowFlags!=nullptr,"resize lost surface plane");
        require(Drawing::gDepthPlane.receiverDepth!=nullptr&&Drawing::gDepthPlane.receiverDepth[0]==Drawing::kDepthUnknown,"resize failed to reset receivers");
        engine.SetAuxEnabled(false,false);
        require(Drawing::gDepthPlane.shadowFlags==nullptr,"disabled surface plane is dangling");
        std::cout<<"PASS actual X8 surface flags: allocation, scroll without silhouette heights, resize and disable\n";
    }
    require(context->LoadParkFromFile(argv[4]),"load fixture");
    {
        uint8_t mask[]{255,0,0,255},colour[]{21,22,23,24};
        G1Element images[2];images[0].width=images[1].width=2;images[0].height=images[1].height=2;
        images[0].offset=mask;images[1].offset=colour;
        const auto image=GfxObjectAllocateImages(images,2);
        Drawing::X8DrawingEngine engine(context->GetUiContext());
        for(int zoom:{0,-1}){
            engine.Resize(16,16);engine.SetAuxEnabled(true,true);
            auto& rt=*engine.getRT();rt.zoom_level=ZoomLevel{int8_t(zoom)};
            Drawing::gDepthPaint={};Drawing::gDepthPaint.active=true;Drawing::gDepthPaint.flat=true;
            Drawing::gDepthPaint.flatBase=64;Drawing::gDepthPaint.zoom=int8_t(zoom);
            Drawing::gDepthPaint.shadowFlags=Drawing::kShadowReceiver|Drawing::kWaterSurface;
            engine.BeginDraw();GfxDrawSpriteRawMaskedSoftware(rt,{2,2},ImageId(image),ImageId(image+1));
            const int p=zoom==0?2:4,index=p*16+p,skip=index+(zoom==0?1:2);
            require(rt.bits[index]==static_cast<Drawing::PaletteIndex>(21),"masked sprite changed native colour");
            require(Drawing::gDepthPlane.shadowFlags[index]==6,"masked sprite lost surface flags");
            require(Drawing::gDepthPlane.receiverDepth[index]==Drawing::DepthEncode(68),"masked sprite lost flat receiver depth");
            require(Drawing::gDepthPlane.data[skip]==Drawing::kDepthUnknown,"transparent mask wrote geometry");
            GfxDrawSpriteRawMaskedSoftware(rt,{-1,-1},ImageId(image),ImageId(image+1));
            require(rt.bits[0]==static_cast<Drawing::PaletteIndex>(24)&&Drawing::gDepthPlane.shadowFlags[0]==6,"clipped masked sprite geometry differs from colour");
            engine.EndDraw();Drawing::gDepthPaint={};
        }
        GfxObjectFreeImages(image,2);
        std::cout<<"PASS native masked bitmap path: colour, transparency, clipping and receiver planes at normal/magnified zoom\n";
    }
    {
        Drawing::RenderTarget rt{};rt.x=rt.y=-1000;rt.width=rt.height=2000;
        rt.cullingX=rt.cullingY=-1000;rt.cullingWidth=rt.cullingHeight=2000;
        auto* session=PaintSessionAlloc(rt,{},0);
        Guest previousGuest{};session->CurrentlyDrawnEntity=&previousGuest;
        session->CurrentlyDrawnTileElement=MapGetFirstElementAt(TileCoordsXY{8,11});
        for(auto kind:{ViewportInteractionItem::terrain,ViewportInteractionItem::water,ViewportInteractionItem::footpath}){
            session->InteractionType=kind;
            auto* ps=PaintAddImageAsParent(*session,ImageId(SPR_BLANK_TILE),{0,0,32},{32,32,-1});
            require(ps&&ps->DepthFlat,"stale guest pointer classified subsequent ground/water as upright");
        }
        session->InteractionType=ViewportInteractionItem::entity;
        auto* ps=PaintAddImageAsParent(*session,ImageId(SPR_BLANK_TILE),{0,0,32},{32,32,-1});
        require(ps&&!ps->DepthFlat,"entity sprite must remain upright even with a leftover terrain pointer");
        TileElement paving{};paving.clearAs(TileElementType::smallScenery);
        session->CurrentlyDrawnTileElement=&paving;session->InteractionType=ViewportInteractionItem::scenery;
        ps=PaintAddImageAsParent(*session,ImageId(SPR_BLANK_TILE),{0,0,32},{32,32,3});
        require(ps&&ps->DepthFlat,"8-unit scenery paving sorting box must be a floor");
        ps=PaintAddImageAsParent(*session,ImageId(SPR_BLANK_TILE),{0,0,32},{32,32,32});
        require(ps&&!ps->DepthFlat,"tall scenery box misclassified as floor");
        ps=PaintAddImageAsParent(*session,ImageId(SPR_MECHANIC),{0,0,32},{32,32,3});
        require(ps&&!ps->DepthFlat,"tall support-like image misclassified as floor");
        PaintSessionFree(session);
        std::cout<<"PASS actual paint ownership: stale guest pointer cannot alter following terrain/water/path depth\n";
    }
    {
        const int id=Atmo::LightAdd(257,354,false);
        const auto terrain=TileElementHeight({257,354});
        Atmo::LightPlaceAt(id,257,354,terrain+79,0);
        require(Atmo::LightAt(id)->x==257&&Atmo::LightAt(id)->y==354&&Atmo::LightAt(id)->height==79,"free odd light anchor lost");
        require(Atmo::LightPlacementHeight(false)==79,"selected placement template differs from added light");
        Atmo::LightCopy();
        Atmo::LightPlaceAt(id,257,354,terrain+32,0);
        require(Atmo::LightPlacementHeight(true)==79,"paste did not retain clipboard height");
        const int pasted=Atmo::LightAdd(265,362,true);require(Atmo::LightAt(pasted)->height==79,"paste template/add mismatch");
        const auto old=*Atmo::LightAt(id);
        Atmo::LightPlaceAt(id,257,354,terrain-1,0);
        require(Atmo::LightAt(id)->x==old.x&&Atmo::LightAt(id)->y==old.y&&Atmo::LightAt(id)->height==old.height,"invalid fixed-height move changed anchor");
        Atmo::LightDelete(pasted);Atmo::LightDelete(id);
        std::cout<<"PASS native light storage: odd free anchors, height plane, clipboard and invalid-move rejection\n";
    }
    auto& state=getGameState();state.entities.resetAllEntities();
    auto& s=Atmo::GetSettings();s={};s.enabled=true;s.vignetteStrength=0;s.tiltStrength=0;
    s.cloudCoverage=0;s.fog=0;s.sunRays=0;s.sunlight=0;s.sunWarmth=0;s.leaves=0;
    s.waterWaves=s.waterDepth=s.waterGlitter=s.waterPatches=s.waterSun=0;
    s.swayTrees=false;s.weatherEffects=false;s.lightsEnabled=false;s.lightningStrength=0;
    require(Atmo::ReplacesWeather(),"Atmosphere must suppress legacy rain when new precipitation is switched off");
    s.weatherEffects=true;require(Atmo::ReplacesWeather(),"new and native rain must not overlap");
    s.enabled=false;require(!Atmo::ReplacesWeather(),"disabling Atmosphere must restore native weather");
    s.enabled=true;s.weatherEffects=false;
    s.shadowStrength=1;s.sunAzimuth=260;s.sunElevation=35;s.shadowReach=70;
    s.shadowGuestDistance=s.shadowVehicleDistance=s.shadowTrackDistance=0;
    constexpr int W=256,H=192,N=W*H;
    std::vector<uint8_t> bits(N,1),flags(N,Drawing::kShadowReceiver);
    std::vector<uint16_t> depth(N);std::vector<uint32_t> out(N);
    std::array<uint32_t,256> palette{};palette[1]=0xFF808080;
    Drawing::gDepthPlane.data=depth.data();Drawing::gDepthPlane.shadowFlags=flags.data();Drawing::gDepthPlane.size=N;
    const int x=256,y=352,z=TileElementHeight({x,y});
    for(float wpp:{.5f,1.f,2.f,4.f,8.f}){
        state.entities.resetAllEntities();
        Atmo::ViewInfo view;view.worldPerPixel=wpp;view.viewportWidth=W;view.viewportHeight=H;
        view.originX=int((y-x)/wpp)-W/2;view.originY=int(((x+y)*.5f-z)/wpp)-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(int(2*((py+view.originY)*wpp+z)));
        const auto render=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),view,bits.data());return out;};
        auto baseline=render();
        auto* balloon=state.entities.createEntity<Balloon>();require(balloon,"balloon allocation");balloon->setLocation({x,y,2000});
        auto* bird=state.entities.createEntity<Duck>();require(bird,"bird allocation");bird->setLocation({x,y,900});bird->state=Duck::DuckState::flyAway;
        require(render()==baseline,"birds/balloons must cast no stretched shadows");
        auto* peep=Guest::generate({x,y,z});require(peep,"guest allocation");peep->state=PeepState::walking;
        const auto person=render();int changed=0;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)if(person[py*W+px]!=baseline[py*W+px]){
            changed++;require(std::abs(px-W/2)<100&&std::abs(py-H/2)<80,"person shadow escapes bounded footprint");
        }
        require(changed>0,"walking guest shadow invisible at supported zoom");
        if(wpp==1.f){
            s.tiltStrength=1;s.tiltFocus=0;s.tiltBand=0;s.tiltFalloff=.1f;
            const auto blurredPerson=render();peep->state=PeepState::picked;
            require(render()!=blurredPerson,"depth-of-field erased the person shadow instead of filtering it");
            s.tiltStrength=0;peep->state=PeepState::walking;
        }
        for(auto hidden:{PeepState::picked,PeepState::onRide,PeepState::falling}){
            peep->state=hidden;require(render()==baseline,"picked/riding/falling guests must not cast ground shadows");
        }
        auto* car=state.entities.createEntity<Vehicle>();require(car,"vehicle allocation");car->setLocation({x,y,z+32});
        require(render()==baseline,"parked vehicle shadow remains active");
    }
    // Scenery is a separate visible receiving surface: elevated roofs catch entity shadows in every view.
    for(float wpp:{.5f,1.f,2.f,4.f,8.f}) for(int rotation=0;rotation<4;rotation++) {
        state.entities.resetAllEntities();
        Atmo::ViewInfo view;view.worldPerPixel=wpp;view.viewportWidth=W;view.viewportHeight=H;view.rotation=rotation;
        float rx=x,ry=y;
        if(rotation==1){rx=y;ry=-x;}if(rotation==2){rx=-x;ry=-y;}if(rotation==3){rx=-y;ry=x;}
        const float roof=z+16;
        view.originX=int((ry-rx)/wpp)-W/2;view.originY=int(((rx+ry)*.5f-roof)/wpp)-H/2;
        std::fill(flags.begin(),flags.end(),Drawing::kSceneryReceiver);
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(int(2*((py+view.originY)*wpp+roof)));
        const auto render=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),view,bits.data());return out;};
        const auto baseline=render();
        auto* peep=Guest::generate({x,y,z+32});require(peep,"scenery guest allocation");peep->state=PeepState::walking;
        require(render()!=baseline,"elevated scenery does not receive guest shadow at rotation/zoom");
        peep->state=PeepState::picked;
        require(render()==baseline,"picked guest still shadows scenery");
        auto* car=state.entities.createEntity<Vehicle>();require(car,"scenery vehicle allocation");car->setLocation({x,y,z+48});
        require(render()==baseline,"parked vehicle shadows scenery");
        std::fill(flags.begin(),flags.end(),0);
        require(render()==baseline,"scenery shadow spills onto upright entity/non-receiver");
        std::fill(flags.begin(),flags.end(),Drawing::kSceneryReceiver);
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(int(2*((py+view.originY)*wpp+z+128)));
        require(render()==baseline,"shadow appears on roof above the caster");
    }
    std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver);
    std::cout<<"PASS scenery receiving surfaces: guests across 20 rotations/zooms; vehicles parked; picked/foreground/roof-above exclusion\n";
    // A custom paving floor over an invisible path must have exactly the original path's footprint.
    for(float wpp:{.5f,1.f,2.f,4.f}) for(int rotation=0;rotation<4;rotation++) for(bool staff:{false,true}) {
        state.entities.resetAllEntities();
        Atmo::ViewInfo view;view.worldPerPixel=wpp;view.viewportWidth=W;view.viewportHeight=H;view.rotation=rotation;
        const CoordsXY foot{x+1,y+3},rotated=foot.rotate(rotation);
        view.originX=int((rotated.y-rotated.x)/wpp)-W/2;
        view.originY=int(((rotated.x+rotated.y)*.5f-z)/wpp)-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(int(2*((py+view.originY)*wpp+z)));
        const auto render=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),view,bits.data());return out;};
        Peep* peep=staff?static_cast<Peep*>(state.entities.createEntity<Staff>()):Guest::generate({foot.x,foot.y,z});
        require(peep,"paving peep allocation");peep->setLocation({foot.x,foot.y,z});peep->state=PeepState::walking;
        std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver);const auto pathShadow=render();
        std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver|Drawing::kSceneryReceiver);
        const auto floorShadow=render();
        if(floorShadow!=pathShadow)std::cout<<"paving mismatch rotation "<<rotation<<" wpp "<<wpp<<std::endl;
        require(floorShadow==pathShadow,"custom paving changes the peep shadow footprint");
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(int(2*((py+view.originY)*wpp+z+128)));
        const auto roofWithPeep=render();peep->state=PeepState::picked;
        require(render()==roofWithPeep,"flat roof above visitor receives ground shadow");
    }
    std::cout<<"PASS identical original/custom paving shadows at 32 guest/staff rotations/zooms; flat roofs above excluded\n";
    {
        state.entities.resetAllEntities();
        Atmo::ViewInfo view;view.worldPerPixel=1;view.viewportWidth=W;view.viewportHeight=H;
        view.originX=y-x-W/2;view.originY=(x+y)/2-z-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(2*(py+view.originY+z));
        const auto render=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),view,bits.data());return out;};
        const auto baseline=render();
        auto* peep=Guest::generate({x,y,z});require(peep,"distance guest allocation");peep->state=PeepState::walking;
        const auto fullNear=render();s.shadowGuestDistance=1;
        require(render()==fullNear,"near shadow changed inside full-detail distance");
        peep->setLocation({x+120,y+40,z});
        const auto limited=render();s.shadowGuestDistance=0;const auto unlimited=render();
        const auto mass=[&](const auto& image){int total=0;for(size_t i=0;i<image.size();i++)total+=int(baseline[i]&255)-int(image[i]&255);return total;};
        require(mass(unlimited)>0&&mass(limited)<mass(unlimited),"far shadow did not fade before cutoff");
        view.worldPerPixel=8;view.originX=(y-x)/8-W/2;view.originY=((x+y)/2-z)/8-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(16*(py+view.originY)+2*z);
        peep->state=PeepState::picked;const auto farBaseline=render();
        peep->setLocation({x,y,z});peep->state=PeepState::walking;
        require(render()!=farBaseline,"zero range lost full-detail far-zoom shadow");
        s.shadowGuestDistance=80;require(render()==farBaseline,"guest zoom LOD did not omit subpixel far shadows");
        s.shadowGuestDistance=s.shadowVehicleDistance=s.shadowTrackDistance=0;
        view.worldPerPixel=16;view.originX=(y-x)/16-W/2;view.originY=((x+y)/2-z)/16-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(32*(py+view.originY)+2*z);
        peep->state=PeepState::picked;const auto veryFarBaseline=render();peep->state=PeepState::walking;
        require(render()!=veryFarBaseline,"zero range must disable zoom LOD at every zoom");
        s.shadowGuestDistance=80;s.shadowVehicleDistance=s.shadowTrackDistance=160;
        require(render()==veryFarBaseline,"far overview should skip all finite shadow categories");
        s.shadowGuestDistance=s.shadowVehicleDistance=s.shadowTrackDistance=0;
        std::cout<<"PASS native shadow distance: near pixels unchanged; far fades; zero restores detail; guest zoom LOD\n";
    }
    {
        state.entities.resetAllEntities();
        auto* ride=RideAllocateAtIndex(RideId::FromUnderlying(0));require(ride,"ride allocation");
        ride->type=RIDE_TYPE_MONORAIL;ride->customName="Shadow test ride";
        auto& prefs=Atmo::RideShadows::Detail::disabled;prefs.clear();
        require(Atmo::RideShadows::SetEnabled(ride->id,true),"enable preference persistence");
        Atmo::ViewInfo view;view.worldPerPixel=1;view.viewportWidth=W;view.viewportHeight=H;
        view.originX=y-x-W/2;view.originY=(x+y)/2-z-H/2;
        for(int py=0;py<H;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(2*(py+view.originY+z));
        const auto render=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),view,bits.data());return out;};
        const auto baseline=render();
        auto* car=state.entities.createEntity<Vehicle>();car->ride=ride->id;car->setLocation({x,y,z+32});
        require(render()==baseline,"saved ride preference reactivated parked shadows");
        require(Atmo::RideShadows::SetEnabled(ride->id,false),"disable preference persistence");
        require(render()==baseline,"disabled ride vehicle still casts shadow");
        Atmo::RideShadows::Detail::loaded=false;
        require(!Atmo::RideShadows::IsEnabled(ride->id),"disabled ride preference lost after reload");
        const auto oldName=ride->customName;ride->customName="Replacement ride";
        require(Atmo::RideShadows::IsEnabled(ride->id),"replacement ride inherited another ride preference");
        ride->customName=oldName;
        require(Atmo::RideShadows::SetEnabled(ride->id,true),"restore preference");
        require(render()==baseline,"ride toggle reactivated parked shadows");
        std::cout<<"PASS parked ride shadows remain absent across toggles; preference reload/replacement identity retained\n";
    }
    // Moving sprite depth used to deform the cached 8px cloud shadow/fog nodes into triangles.
    state.entities.resetAllEntities();s.shadowStrength=0;s.leaves=0;s.fog=.65f;s.cloudCoverage=.6f;
    s.cloudOpacity=0;s.cloudShadow=1;s.cloudFocus=false;s.cloudSpeed=0;s.windStrength=0;
    s.cloudSize=300;s.cloudHeight=120;s.cloudWeather=false;s.tiltStrength=0;
    Atmo::ViewInfo groundView;groundView.worldPerPixel=1;groundView.viewportWidth=W;groundView.viewportHeight=H;
    groundView.originX=-100;groundView.originY=150;
    std::vector<uint16_t> receivers(N);
    for(int py=0;py<H;py++)for(int px=0;px<W;px++)receivers[py*W+px]=Drawing::DepthEncode(2*(py+groundView.originY+32));
    depth=receivers;Drawing::gDepthPlane.data=depth.data();Drawing::gDepthPlane.receiverDepth=receivers.data();
    std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver);
    const auto groundRender=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),groundView,bits.data());return out;};
    const auto stableGround=groundRender();
    for(int phase=0;phase<18;phase++){
        depth=receivers;Drawing::gDepthPlane.data=depth.data();
        std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver);
        const int cx=32+phase*8,cy=48;
        for(int py=cy;py<cy+24;py++)for(int px=cx;px<cx+16;px++){
            depth[py*W+px]=Drawing::DepthEncode(2*(groundView.originY+cy+120));flags[py*W+px]=0;
        }
        require(groundRender()==stableGround,"moving sprite deformed cloud shadows/fog during cache rebuild");
    }
    // Control: feeding the visible sprite depth back as ground must reproduce the original deformation.
    Drawing::gDepthPlane.receiverDepth=depth.data();
    std::vector<uint32_t> corruptedGround;
    for(int frame=0;frame<6;frame++)corruptedGround=groundRender();
    require(corruptedGround!=stableGround,"flicker fixture did not exercise a depth-sensitive cloud patch");
    Drawing::gDepthPlane.receiverDepth=nullptr;
    std::cout<<"PASS 18 moving guest/boat phases: underlying cloud shadows/fog unchanged across multiple cache rebuilds\n";
    // A moving boat must not change cached shore/depth tint around its footprint.
    s.cloudCoverage=0;s.fog=0;s.waterDepth=1;
    std::fill(bits.begin(),bits.end(),230);palette[230]=0xFF207090;
    std::vector<uint8_t> receiverMaterials(N,Drawing::kShadowReceiver|Drawing::kWaterSurface);
    flags=receiverMaterials;Drawing::gDepthPlane.shadowFlags=flags.data();
    Drawing::gDepthPlane.receiverFlags=receiverMaterials.data();
    depth=receivers;Drawing::gDepthPlane.data=depth.data();
    const auto waterReference=groundRender();
    for(int py=64;py<96;py++)for(int px=96;px<128;px++)flags[py*W+px]=0;
    std::vector<uint32_t> boatWater;
    for(int frame=0;frame<10;frame++)boatWater=groundRender();
    for(int py=0;py<H;py++)for(int px=0;px<W;px++)if(py<64||py>=96||px<96||px>=128)
        require(boatWater[py*W+px]==waterReference[py*W+px],"boat changed adjacent cached water colour/depth");
    Drawing::gDepthPlane.receiverFlags=nullptr;s.waterDepth=0;
    std::fill(bits.begin(),bits.end(),1);
    std::cout<<"PASS water density through cache rebuild: boat does not move shore/depth tint\n";
    // Exercise the actual compositor: an airborne sprite cannot change distant backdrop pixels on its row.
    state.entities.resetAllEntities();s.shadowStrength=0;s.leaves=0;s.fog=.35f;s.cloudCoverage=.6f;
    s.cloudOpacity=.8f;s.cloudShadow=.4f;s.cloudFocus=false;s.cloudSpeed=0;s.tiltStrength=.35f;
    std::fill(bits.begin(),bits.end(),10);palette[10]=0xFF101020;
    std::fill(flags.begin(),flags.end(),Drawing::kExteriorSurface);
    std::fill(depth.begin(),depth.end(),32768);
    Atmo::ViewInfo exteriorView;exteriorView.worldPerPixel=1;exteriorView.viewportWidth=W;exteriorView.viewportHeight=H;
    exteriorView.originX=-300;exteriorView.originY=-300;
    const auto exteriorRender=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),exteriorView,bits.data());return out;};
    const auto clearExterior=exteriorRender();
    palette[201]=0xFF805030;
    for(int py=20;py<120;py++)for(int px=10;px<200;px++){
        const int i=py*W+px;
        if((px+py)&1){bits[i]=201;flags[i]=0;depth[i]=Drawing::kDepthUnknown;}
        else bits[i]=0;
    }
    require(exteriorRender()==clearExterior,"blank/cleared/weather-filtered backdrop exposes tile geometry or cloud shadows");
    std::fill(bits.begin(),bits.end(),10);std::fill(flags.begin(),flags.end(),Drawing::kExteriorSurface);
    std::fill(depth.begin(),depth.end(),32768);
    std::cout<<"PASS actual exterior: mixed native tile/clear/weather indices remain identical under cloud/fog/shadow/tilt\n";
    const int airborne=40*W+W/2;bits[airborne]=1;flags[airborne]=0;depth[airborne]=33000;
    const auto balloonExterior=exteriorRender();
    for(int py=0;py<H;py++)for(int px=0;px<W;px++)if(std::abs(px-W/2)>40){
        const auto i=py*W+px;
        require(clearExterior[i]==balloonExterior[i],"airborne sprite changed distant backdrop lighting");
    }
    bits[airborne]=10;flags[airborne]=Drawing::kExteriorSurface;depth[airborne]=32768;
    const auto removedExterior=exteriorRender();
    require(removedExterior==clearExterior,"airborne sprite left a lighting trail after removal");
    std::cout<<"PASS actual cloud/fog/tilt compositor: airborne sprite has no horizontal trail or residual lighting\n";
    s.fog=0;s.cloudCoverage=0;s.tiltStrength=0;
    std::fill(bits.begin(),bits.end(),1);std::fill(flags.begin(),flags.end(),Drawing::kShadowReceiver);
    state.entities.resetAllEntities();s.shadowStrength=0;s.leaves=1;s.windStrength=0;
    require(Atmo::WantsDepth(),"leaves alone must request scene depth for source height and occlusion");
    s.tiltFocus=0;s.tiltBand=0;s.tiltFalloff=.1f;s.tiltDepth=true;
    Atmo::ViewInfo leafView;leafView.worldPerPixel=1;leafView.viewportWidth=W;leafView.viewportHeight=H;
    leafView.originX=y-x-W/2;leafView.originY=(x+y)/2-z-H/2;
    std::vector<uint8_t> canopy(N,0);
    std::fill(depth.begin(),depth.end(),Drawing::DepthEncode(x+y));
    for(int py=0;py<6;py++)for(int px=0;px<W;px++)depth[py*W+px]=Drawing::DepthEncode(x+y+400);
    for(int py=16;py<80;py++)for(int px=32;px<W-32;px++)canopy[py*W+px]=96;
    const auto renderLeaves=[&]{Atmo::ProcessFrame(bits.data(),palette.data(),out.data(),W,H,{16,8,0},{},depth.data(),leafView,bits.data(),canopy.data());return out;};
    for(int i=0;i<80;i++){renderLeaves();std::this_thread::sleep_for(std::chrono::milliseconds(25));}
    const auto sharpLeaves=renderLeaves();s.tiltStrength=1;
    const auto blurredLeaves=renderLeaves();s.leaves=0;
    const auto background=renderLeaves();int sharpPeak=0,blurredPeak=0;
    for(int i=0;i<N;i++){
        const int base=int(background[i]&255);
        sharpPeak=std::max(sharpPeak,std::abs(int(sharpLeaves[i]&255)-base));
        blurredPeak=std::max(blurredPeak,std::abs(int(blurredLeaves[i]&255)-base));
    }
    require(sharpPeak>0,"native world pipeline failed to draw leaves");
    require(blurredPeak<sharpPeak,"leaf pixels escaped the actual depth-of-field pass");
    std::cout<<"PASS native leaf pipeline: visible translucent sprites, depth-of-field reduces peak contrast "<<sharpPeak<<" -> "<<blurredPeak<<'\n';
    Drawing::gDepthPlane={};
    // Exact blur-preparation oracle also covers odd edges and UI coverage after resize.
    state.entities.resetAllEntities();
    s={};s.enabled=true;s.tiltStrength=.7f;s.vignetteStrength=0;s.shadowStrength=0;
    s.cloudCoverage=s.fog=s.sunlight=s.sunWarmth=s.sunRays=s.leaves=0;
    s.waterWaves=s.waterGlitter=s.waterDepth=s.waterPatches=s.waterSun=0;
    s.weatherEffects=s.swayTrees=s.lightsEnabled=s.timeEnabled=false;s.lightningStrength=0;
    for(auto dimensions:{std::pair{63,47},std::pair{64,48},std::pair{65,49}}) {
        const auto [width,height]=dimensions;const size_t count=static_cast<size_t>(width)*height;
        std::vector<uint8_t> image(count,1),world(count,1),surface(count,Drawing::kShadowReceiver);
        std::vector<uint16_t> geometry(count);std::vector<uint32_t> output(count);
        for(size_t i=0;i<count;i++) {
            geometry[i]=Drawing::DepthEncode(static_cast<int>(i/width)*2+64);
            if(i%31==0)image[i]=2;
        }
        Drawing::gDepthPlane.data=geometry.data();Drawing::gDepthPlane.shadowFlags=surface.data();Drawing::gDepthPlane.size=count;
        Atmo::ViewInfo view;view.worldPerPixel=1;view.viewportWidth=width;view.viewportHeight=height;
        Atmo::ProcessFrame(image.data(),palette.data(),output.data(),width,height,{16,8,0},{},geometry.data(),view,world.data());
        for(size_t i=0;i<count;i++)if(image[i]!=world[i])require(output[i]==palette[image[i]],"fused blur changed UI pixels");
    }
    Drawing::gDepthPlane={};
    std::cout<<"PASS fused blur odd/even dimensions, resize and exact UI preservation\n";
    {
        const auto path=std::filesystem::path(argv[2])/"shadow-distance-settings.ini";
        {std::ofstream config(path);config<<"enabled = 1\nshadow_guest_distance = 48\nshadow_vehicle_distance = 96\nshadow_track_distance = 0\n";}
        const auto name=path.string();Atmo::SetSettingsFile(name.c_str());
        require(s.shadowGuestDistance==48&&s.shadowVehicleDistance==96&&s.shadowTrackDistance==0,"distance setting load");
        Atmo::ResetLightSettings();
        require(s.shadowGuestDistance==80&&s.shadowVehicleDistance==160&&s.shadowTrackDistance==160,"distance recommended reset");
        s.shadowGuestDistance=0;s.shadowVehicleDistance=124;s.shadowTrackDistance=256;Atmo::NotifySettingsChanged();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));Atmo::IsActive(name.c_str());
        std::ifstream saved(path);std::string text((std::istreambuf_iterator<char>(saved)),{});
        require(text.find("shadow_guest_distance = 0")!=std::string::npos&&text.find("shadow_vehicle_distance = 124")!=std::string::npos
            &&text.find("shadow_track_distance = 256")!=std::string::npos,"distance setting save");
        std::cout<<"PASS native shadow distance settings: load, zero mode, reset and save\n";
    }
    std::cout<<"PASS real renderer: peep shadows at five zooms; vehicles, birds, balloons and picked guests excluded; bounded footprint\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
