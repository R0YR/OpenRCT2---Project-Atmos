# Building Project:Atmos (Windows)

Project:Atmos builds like OpenRCT2 (see the upstream notes in `docs/upstream/OpenRCT2-README.md`).

Requirements: Visual Studio 2022 with the *Desktop development with C++* workload (MSVC v143, Windows SDK) and an internet connection for the dependency download on the first build.

```bat
msbuild openrct2.proj /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

The first build downloads the pre-built libraries listed in `.dependencies` into `lib\`. The result is `bin\openrct2.exe` plus the game data in `bin\data`.

Run it with:

```bat
bin\openrct2.exe --user-data-path=userdata --openrct2-data-path=bin\data
```

Project-specific tests and diagnostics are in `scripts/project-atmos-dev/`. Some of them expect local test parks and are meant for development only.
