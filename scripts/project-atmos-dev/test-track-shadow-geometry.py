"""Compile production sampling against native track movement coordinates (Windows/MSVC)."""
from pathlib import Path
import re, os, subprocess
ROOT = Path(__file__).resolve().parents[1]
src = (ROOT/'src/openrct2-ui/drawing/Atmosphere.cpp').read_text()
a=src.index('        template<typename Point, typename Emit>\n        void SampleTrackShadowPath')
b=src.index('        struct TrackShadowCache',a)
helper=src[a:b]
data=(ROOT/'src/openrct2/ride/VehicleSubpositionData.cpp').read_text()
listing=data[data.index('static constexpr const VehicleInfoList *TrackVehicleInfoListDefault[]'):]
listing=listing[:listing.index('};')]
names=re.findall(r'&(TrackVehicleInfo_\w+)',listing)
arrays=dict(re.findall(r'CREATE_VEHICLE_INFO\((\w+),\s*\{(.*?)\}\)',data,re.S))
code='''#include <cstdint>
#include <cassert>
#include <vector>
#include <cstdio>
#include <algorithm>
constexpr int kLocationNull=-32768;
struct Point { int x,y,z; };
'''+helper
cases=[]
for kind in [0,4,5,16,17,34,40,41,68,69]:
    for direction in range(4):
        name=names[kind*4+direction]
        pts=re.findall(r'\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),',arrays[name])
        assert len(pts)>1,(kind,direction,name)
        key=f'path{kind}_{direction}'
        code+='const Point '+key+'[]={'+','.join('{'+','.join(p)+'}' for p in pts)+'};\n'
        cases.append(key)
code+='''
int main(){int checked=0,segments=0;
const auto test=[&](const auto& points){
    int cursor=0, emitted=0;
    SampleTrackShadowPath(points,uint16_t(std::size(points)),[&](const auto& a,const auto& b){
        const int ai=int(&a-points),bi=int(&b-points);
        assert(ai==cursor && bi>ai); cursor=bi;
        const int dx=b.x-a.x,dy=b.y-a.y,dz=b.z-a.z;
        assert(dx*dx+dy*dy+dz*dz>0 && dx*dx+dy*dy+dz*dz<=1024);
        for(int j=ai+1;j<bi;j++){
            const int x=points[j].x-a.x,y=points[j].y-a.y,z=points[j].z-a.z;
            assert(x*x+y*y+z*z<16); // no curvature/slope skipped beyond the sampler's spatial budget
        }
        emitted++;
    });
    assert(cursor==std::size(points)-1 && emitted>0); segments+=emitted; checked++;
};
'''
for c in cases: code+=f'test({c});\n'
code+='''
int emitted=0;
const Point discontinuous[]={{0,0,0},{4,0,0},{-32768,1,2},{100,100,0},{104,100,0},{1000,1000,0},{1004,1000,0}};
SampleTrackShadowPath(discontinuous,7,[&](auto&,auto&){emitted++;});assert(emitted==3);
SampleTrackShadowPath<Point>(nullptr,5,[&](auto&,auto&){assert(false);});
SampleTrackShadowPath(discontinuous,1,[&](auto&,auto&){assert(false);});
const Point stationary[]={{1,1,1},{1,1,1},{1,1,1}};
SampleTrackShadowPath(stationary,3,[&](auto&,auto&){assert(false);});
printf("PASS: %d native paths (4 directions, straights, curves, steep slopes, loops), %d continuous segments; discontinuities and empty/stationary paths\\n",checked,segments);
}
'''
# The rendering contract must explicitly classify the last opaque sprite, not inspect neighbouring animated pixels.
paint=(ROOT/'src/openrct2/paint/Paint.cpp').read_text()
assert 'gDepthPaint.shadowFlags = ps->DepthFlat ? kShadowReceiver : 0;' in paint
receiver=src[src.index('const auto flatReceiver = [&]()'):src.index('if (amount != 0 && flatReceiver())')]
assert 'kShadowReceiver' in receiver and 'dAbove' not in receiver and 'dBelow' not in receiver
assert 'cache.links' not in src
out=ROOT/'artifacts/atmosphere-reference/track-shadow-tests';out.mkdir(parents=True,exist_ok=True)
cpp=out/'track-shadow-tests.cpp';cpp.write_text(code)
compiler=Path(os.environ.get('CXX','C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'))
vc=compiler.parents[3];sdk=Path('C:/Program Files (x86)/Windows Kits/10');v=sorted((sdk/'Include').iterdir(),key=lambda p:p.name)[-1].name
env=os.environ.copy();env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/v/'ucrt',sdk/'Include'/v/'shared',sdk/'Include'/v/'um']));env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/v/'ucrt/x64',sdk/'Lib'/v/'um/x64']))
exe=out/'track-shadow-tests.exe'
subprocess.run([str(compiler),'/nologo','/EHsc','/std:c++20','/O2',str(cpp),f'/Fo{out/"track-shadow-tests.obj"}',f'/Fe{exe}'],env=env,check=True)
subprocess.run([str(exe)],check=True)
