param(
    [Parameter(Mandatory = $true)][string]$Rct2Path,
    [string]$TestPark,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$guestWorkspace = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $guestWorkspace
$guestArtifacts = Join-Path $guestWorkspace 'artifacts/guest-diversity'
$guestUser = Join-Path $guestArtifacts 'userdata'
$guestPreview = Join-Path $guestArtifacts 'preview'
New-Item -ItemType Directory -Force -Path $guestArtifacts, $guestUser, $guestPreview | Out-Null
# Reproduce v1.0 embedded objects, including the original legacy import collision.
$guestLegacyFixtures = Join-Path $guestArtifacts 'retired-before'
New-Item -ItemType Directory -Force -Path $guestLegacyFixtures | Out-Null
foreach ($guestRetired in @(@('older_woman', 'olderWoman'), @('older_man', 'olderMan'))) {
    $guestLegacy = Get-Content -LiteralPath (Join-Path $guestWorkspace 'bin/data/object/rct2/peep_animations/rct2.peep_animations.guest.json') -Raw | ConvertFrom-Json
    $guestLegacy.id = 'atmosphere.peep_animations.' + $guestRetired[0]
    $guestLegacy.sourceGame = @('custom')
    $guestLegacy.properties | Add-Member -NotePropertyName guestAppearance -NotePropertyValue $guestRetired[1] -Force
    $guestFixtureFile = [System.IO.File]::Create((Join-Path $guestLegacyFixtures ($guestLegacy.id + '.parkobj')))
    $guestFixtureArchive = [System.IO.Compression.ZipArchive]::new($guestFixtureFile, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $guestEntryWriter = [System.IO.StreamWriter]::new($guestFixtureArchive.CreateEntry('object.json').Open())
        try { $guestEntryWriter.Write(($guestLegacy | ConvertTo-Json -Depth 100)) }
        finally { $guestEntryWriter.Dispose() }
    }
    finally { $guestFixtureArchive.Dispose(); $guestFixtureFile.Dispose() }
}
if (-not $TestPark) {
    $TestPark = Join-Path $guestWorkspace 'test/tests/testdata/parks/small_park_with_ferris_wheel.sv6'
}
$guestMsvc = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
$guestSdkRoot = 'C:/Program Files (x86)/Windows Kits/10'
$guestSdkVersion = (Get-ChildItem -LiteralPath (Join-Path $guestSdkRoot 'Include') -Directory |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1).Name
$guestOldInclude = $env:INCLUDE
$guestOldLib = $env:LIB
$guestOldPath = $env:PATH
try {
    $guestSdkInclude = Join-Path $guestSdkRoot "Include/$guestSdkVersion"
    $env:INCLUDE = "$guestMsvc/include;$guestSdkInclude/ucrt;$guestSdkInclude/shared;$guestSdkInclude/um;$guestSdkInclude/winrt"
    $guestSdkLib = Join-Path $guestSdkRoot "Lib/$guestSdkVersion"
    $env:LIB = "$guestWorkspace/bin;$guestWorkspace/lib/x64/lib;$guestMsvc/lib/x64;$guestSdkLib/ucrt/x64;$guestSdkLib/um/x64"
    $env:PATH = "$guestSdkRoot/bin/$guestSdkVersion/x64;$guestMsvc/bin/Hostx64/x64;$env:PATH"
    $guestExe = Join-Path $guestArtifacts 'integration.exe'
    $guestObj = Join-Path $guestArtifacts 'integration.obj'
    if (-not $SkipBuild) {
        $guestCompile = @('/nologo', '/c', '/std:c++20', '/EHsc', '/MT', '/DENABLE_SCRIPTING', '/DNDEBUG', '/DNOMINMAX',
            '/DWIN32_LEAN_AND_MEAN', '/DSDL_MAIN_HANDLED', '/Isrc', '/Isrc/thirdparty', '/Isrc/thirdparty/quickjs-ng',
            '/Ilib/x64/include', '/Ilib/x64/include/SDL2', "/Fo$guestObj", 'scripts/test-guest-diversity-integration.cpp')
        & "$guestMsvc/bin/Hostx64/x64/cl.exe" @guestCompile 2>&1 | Tee-Object -FilePath (Join-Path $guestArtifacts 'integration-compile.log')
        if ($LASTEXITCODE -ne 0) { throw 'Integration harness compile failed.' }
        $guestLink = @('/NOLOGO', '/DEBUG:NONE', '/SUBSYSTEM:CONSOLE', '/STACK:8388608', "/OUT:$guestExe", $guestObj,
            'libopenrct2.lib', 'kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib',
            'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'odbc32.lib', 'odbccp32.lib',
            'bz2.lib', 'discord-rpc.lib', 'FLAC.lib', 'freetype.lib', 'libpng16.lib', 'ogg.lib', 'SDL2-static.lib',
            'vorbis.lib', 'vorbisfile.lib', 'zip.lib', 'zlib.lib', 'zstd.lib', 'brotlicommon.lib', 'brotlidec.lib',
            'brotlienc.lib', 'fribidi.lib', 'qjs.lib', 'fmt.lib', 'wininet.lib', 'imm32.lib', 'version.lib', 'winmm.lib',
            'crypt32.lib', 'wldap32.lib', 'shlwapi.lib', 'setupapi.lib', 'bcrypt.lib', 'winhttp.lib', 'ws2_32.lib')
        & "$guestMsvc/bin/Hostx64/x64/link.exe" @guestLink 2>&1 | Tee-Object -FilePath (Join-Path $guestArtifacts 'integration-link.log')
        if ($LASTEXITCODE -ne 0) { throw 'Integration harness link failed.' }
    }
    & $guestExe (Join-Path $guestWorkspace 'bin/data') $guestUser $Rct2Path $TestPark $guestPreview 2>&1 |
        Tee-Object -FilePath (Join-Path $guestArtifacts 'integration.log')
    if ($LASTEXITCODE -ne 0) { throw 'Integration harness assertions failed.' }
}
finally {
    $env:INCLUDE = $guestOldInclude
    $env:LIB = $guestOldLib
    $env:PATH = $guestOldPath
}
