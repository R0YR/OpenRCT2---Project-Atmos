$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$out=Join-Path $repo 'artifacts/foliage-selection-tests'
New-Item -ItemType Directory -Path $out,"$out/user" -Force | Out-Null
# Custom objects are copied into the isolated test profile; live profiles and saves are not written.
New-Item -ItemType Directory -Path "$out/user/object" -Force | Out-Null
foreach($asset in Get-ChildItem -LiteralPath "$repo/dev-userdata/object" -File){
    $target=Join-Path "$out/user/object" $asset.Name
    if(!(Test-Path -LiteralPath $target) -or (Get-Item -LiteralPath $target).Length -ne $asset.Length){Copy-Item -LiteralPath $asset.FullName -Destination $target}
}
$vc='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$sdk='C:/Program Files (x86)/Windows Kits/10'
$version=(Get-ChildItem "$sdk/Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB;$oldPath=$env:PATH
Push-Location $repo
try {
    $env:INCLUDE="$vc/include;$sdk/Include/$version/ucrt;$sdk/Include/$version/shared;$sdk/Include/$version/um"
    $env:LIB="$repo/bin;$repo/lib/x64/lib;$vc/lib/x64;$sdk/Lib/$version/ucrt/x64;$sdk/Lib/$version/um/x64"
    $env:PATH="$sdk/bin/$version/x64;"+$env:PATH
    & "$vc/bin/Hostx64/x64/cl.exe" /nologo /c /std:c++20 /Zc:char8_t- /O2 /EHsc /MT /DENABLE_SCRIPTING /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DSDL_MAIN_HANDLED /Isrc /Isrc/thirdparty /Isrc/thirdparty/quickjs-ng /Ilib/x64/include /Ilib/x64/include/SDL2 "/Fo$out/test.obj" scripts/test-foliage-selection.cpp *> "$out/compile.log"
    if($LASTEXITCODE -ne 0){throw "Compile failed: $out/compile.log"}
    $rsp=[IO.File]::ReadAllText("$repo/artifacts/project-atmos/link.rsp")
    $rsp=[regex]::Replace($rsp,'"[^"]*\\OPENRCT2-WIN\.(OBJ|RES)"','','IgnoreCase')
    $rsp=[regex]::Replace($rsp,'/OUT:"[^"]+"',('/OUT:"'+$out+'\test.exe"'))
    $rsp += ' "'+$out+'\test.obj"'
    [IO.File]::WriteAllText("$out/test.rsp",$rsp)
    Push-Location "$repo/src/openrct2-win"
    try { & "$vc/bin/Hostx64/x64/link.exe" "@$out/test.rsp" *> "$out/link.log"; if($LASTEXITCODE -ne 0){throw "Link failed: $out/link.log"} }
    finally {Pop-Location}
    & "$out/test.exe" "$repo/bin/data" "$out/user" 'C:/Program Files (x86)/Steam/steamapps/common/Rollercoaster Tycoon 2' "$repo/artifacts/pacific-repro/pacific.park" 2>&1 | Tee-Object "$out/result.log"
    if($LASTEXITCODE -ne 0){throw 'Foliage selection regression failed'}
}finally{Pop-Location;$env:INCLUDE=$oldInclude;$env:LIB=$oldLib;$env:PATH=$oldPath}

