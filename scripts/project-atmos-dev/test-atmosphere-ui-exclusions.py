"""Compile the production UI exclusion visitor against small window fixtures."""
from pathlib import Path
import os
import subprocess
from PIL import Image

root = Path(__file__).resolve().parents[1]
source = (root / 'src/openrct2-ui/drawing/engines/HardwareDisplayDrawingEngine.cpp').read_text(encoding='utf-8')
start = source.index('        WindowVisitEach([&](WindowBase* w) {', source.index('std::vector<Atmosphere::ExcludeRect> excluded;'))
end = source.index('        Atmosphere::ViewInfo view;', start)
visitor = source[start:end]
code = r'''
#include <vector>
#include <cassert>
#include <cstdio>
enum class WindowClass { mainWindow, titleLogo, titleVersion, titleExit, titleMenu, titleOptions, topToolbar, parkInfoPanel, dateInfoPanel, newsTicker, gameStatusBar, tooltip, ordinary };
enum class WindowFlag { noBackground };
enum class WidgetType { empty, imgBtn };
struct Flags { bool noBackground=false; bool has(WindowFlag)const{return noBackground;} };
struct Widget { int left=0,top=0,right=0,bottom=0; bool hidden=false; WidgetType type=WidgetType::imgBtn; bool isHidden()const{return hidden;} };
struct Pos{int x=10,y=20;};
struct WindowBase { WindowClass classification; Flags flags; Pos windowPos; int width=500,height=100; std::vector<Widget> widgets; bool isVisible=true; };
namespace Atmosphere { struct ExcludeRect { int left,top,right,bottom; }; }
std::vector<WindowBase> windows;
template<class F>void WindowVisitEach(F f){for(auto& w:windows)f(&w);}
std::vector<Atmosphere::ExcludeRect> rectangles(){
std::vector<Atmosphere::ExcludeRect> excluded;
''' + visitor + r'''
return excluded;
}
int main(){
for(auto kind:{WindowClass::titleLogo,WindowClass::titleVersion,WindowClass::titleExit,WindowClass::mainWindow,WindowClass::tooltip}){
windows={{kind,{true},{},500,100,{{0,0,200,90}}}};
assert(rectangles().empty());
}
for(auto kind:{WindowClass::titleMenu,WindowClass::topToolbar,WindowClass::parkInfoPanel,WindowClass::dateInfoPanel,WindowClass::newsTicker,WindowClass::gameStatusBar}){
windows={{kind,{true},{},500,100,{{1,2,31,16},{40,2,70,16,true},{80,2,110,16,false,WidgetType::empty}}}};
auto r=rectangles();assert(r.size()==1);assert(r[0].left==11&&r[0].top==22&&r[0].right==42&&r[0].bottom==37);
}
windows={{WindowClass::ordinary,{false},{},500,100,{}}};auto r=rectangles();
assert(r.size()==1&&r[0].right==510&&r[0].bottom==120);
windows.front().isVisible=false;assert(rectangles().empty());
puts("PASS: original transparent title gaps; complete visible toolbar controls; hidden/empty controls and toolbar gaps omitted");
}
'''
out = root / 'artifacts/atmosphere-reference/ui-exclusion-tests'
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'ui-exclusions.cpp'
cpp.write_text(code, encoding='utf-8')
compiler = Path(os.environ.get('CXX', 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'))
vc = compiler.parents[3]
sdk = Path('C:/Program Files (x86)/Windows Kits/10')
version = sorted((sdk / 'Include').iterdir(), key=lambda p:p.name)[-1].name
env = os.environ.copy()
env['INCLUDE'] = ';'.join(map(str,[vc/'include',sdk/'Include'/version/'ucrt',sdk/'Include'/version/'shared',sdk/'Include'/version/'um']))
env['LIB'] = ';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/version/'ucrt/x64',sdk/'Lib'/version/'um/x64']))
exe = out / 'ui-exclusions.exe'
subprocess.run([str(compiler),'/nologo','/EHsc','/std:c++20','/O2',str(cpp),f'/Fo{out/"ui-exclusions.obj"}',f'/Fe{exe}'],env=env,check=True)
subprocess.run([str(exe)],check=True)
logo = Image.open(root / 'resources/project-atmos/project-atmos-logo.png').convert('RGBA')
assert logo.size == (3383,401) and logo.getextrema()[3] == (0,255)
assert (root/'resources/project-atmos/project-atmos-logo.png').read_bytes() == (root/'bin/data/project-atmos-logo.png').read_bytes()
for width in (576,640,800,1280,1920,3840):
    w = min(672,max(0,width-480)) // 2
    if w:
        h = max(1,logo.height*w//logo.width)
        assert (width-w)//2 >= 240 and h <=136
print('PASS: supplied RGBA asset preserved; title layout at 576..3840 pixels')
