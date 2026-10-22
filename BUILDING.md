# Building Project:Atmos (Windows)

Project:Atmos builds like OpenRCT2 (see the [OpenRCT2 readme](https://github.com/OpenRCT2/OpenRCT2#readme) for the dependencies).

Requirements: Visual Studio 2022 with the *Desktop development with C++* workload (MSVC v143, Windows SDK) and an internet connection for the dependency download on the first build.

```bat
msbuild openrct2.proj /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

The first build downloads the pre-built libraries listed in `.dependencies` into `lib\`. The result is `bin\openrct2.exe` plus the game data in `bin\data`.

Run it with:

```bat
bin\openrct2.exe --user-data-path=userdata --openrct2-data-path=bin\data
```

