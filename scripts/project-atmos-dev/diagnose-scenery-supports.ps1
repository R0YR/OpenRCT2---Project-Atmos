$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$out=Join-Path $repo 'artifacts/scenery-support-diagnostics'
New-Item -ItemType Directory -Path $out,"$out/user" -Force | Out-Null
$vc='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdk='C:/Program Files (x86)/Windows Kits/10'
$version=(Get-ChildItem "$sdk/Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB;$oldPath=$env:PATH
Push-Location $repo
try {
    $env:INCLUDE="$vc/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um"
    $env:LIB="$repo/bin;$repo/lib/x64/lib;$vc/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
    $env:PATH="$sdk/bin/$version/x64;"+$env:PATH
    & "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++20 /Zc:char8_t- /O2 /EHsc /MT /DENABLE_SCRIPTING /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSDL_MAIN_HANDLED /Isrc /Isrc/thirdparty /Isrc/thirdparty/quickjs-ng /Ilib/x64/include /Ilib/x64/include/SDL2 "/Fo$out/test.obj" scripts/diagnose-scenery-supports.cpp *> "$out/compile.log"
    if($LASTEXITCODE -ne 0){throw "Compile failed: $out/compile.log"}
    if($env:ATMOS_ORIGINAL_SPRITE) {
        & "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++20 /Zc:char8_t- /O2 /EHsc /MT /DENABLE_SCRIPTING /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSDL_MAIN_HANDLED /Isrc /Isrc/thirdparty /Isrc/thirdparty/quickjs-ng /Ilib/x64/include /Ilib/x64/include/SDL2 /Isrc/openrct2/drawing "/Fo$out/original-sprite.obj" "$out/original-drawing-sprite.cpp" *> "$out/original-sprite-compile.log"
        if($LASTEXITCODE -ne 0){throw "Original sprite compile failed"}
    }
    if($env:ATMOS_LEGACY_PAINT) {
        & "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++20 /Zc:char8_t- /O2 /EHsc /MT /DENABLE_SCRIPTING /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSDL_MAIN_HANDLED /Isrc /Isrc/thirdparty /Isrc/thirdparty/quickjs-ng /Ilib/x64/include /Ilib/x64/include/SDL2 /Isrc/openrct2/paint "/Fo$out/legacy-paint.obj" "$out/legacy-paint.cpp" *> "$out/legacy-paint-compile.log"
        if($LASTEXITCODE -ne 0){throw "Legacy paint compile failed"}
    }
    if($env:ATMOS_PATCH_SCENERY) {
        foreach($kind in @('Small','Large')) {
            & "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++20 /Zc:char8_t- /O2 /EHsc /MT /DENABLE_SCRIPTING /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSDL_MAIN_HANDLED /Isrc /Isrc/thirdparty /Isrc/thirdparty/quickjs-ng /Ilib/x64/include /Ilib/x64/include/SDL2 "/Fo$out/patch-$kind.obj" "src/openrct2/paint/tile_element/Paint.$($kind)Scenery.cpp" *> "$out/patch-$kind-compile.log"
            if($LASTEXITCODE -ne 0){throw "Patch scenery compile failed: $kind"}
        }
    }
    $rsp=[IO.File]::ReadAllText("$repo/artifacts/project-atmos/link.rsp")
    $rsp=[regex]::Replace($rsp,'"[^"]*\\OPENRCT2-WIN\.(OBJ|RES)"','','IgnoreCase')
    $rsp=[regex]::Replace($rsp,'/OUT:"[^"]+"',('/OUT:"'+$out+'\test.exe"'))
    $rsp += ' "'+$out+'\test.obj"'
    if($env:ATMOS_ORIGINAL_SPRITE){$rsp+=' "'+$out+'\original-sprite.obj"'}
    if($env:ATMOS_LEGACY_PAINT){$rsp+=' "'+$out+'\legacy-paint.obj"'}
    if($env:ATMOS_PATCH_SCENERY){$rsp+=' "'+$out+'\patch-Small.obj" "'+$out+'\patch-Large.obj"'}
    [IO.File]::WriteAllText("$out/test.rsp",$rsp)
    Push-Location "$repo/src/openrct2-win"
    try { & "$vc/bin/Hostx64/x64/link.exe" "@$out/test.rsp" *> "$out/link.log"; if($LASTEXITCODE -ne 0){throw "Link failed: $out/link.log"} }
    finally {Pop-Location}
    $tag=if($env:ATMOS_PATCH_SCENERY){'blocked-scenery-fixed'}elseif($env:ATMOS_LEGACY_PAINT){'legacy-paint'}elseif($env:ATMOS_ORIGINAL_SPRITE){'original-sprite'}else{'fixture'}
    & "$out/test.exe" "$repo/bin/data" "$out/user" 'C:/Program Files (x86)/Steam/steamapps/common/Rollercoaster Tycoon 2' "$repo/artifacts/pacific-repro/pacific.park" "$out/$tag.csv" 2>&1 | Tee-Object "$out/result.log"
    if($LASTEXITCODE -ne 0){throw 'Support visibility regression failed'}
}finally{Pop-Location;$env:INCLUDE=$oldInclude;$env:LIB=$oldLib;$env:PATH=$oldPath}


