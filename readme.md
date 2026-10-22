# Project:Atmos

**Cinematic atmosphere for OpenRCT2** – volumetric clouds, weather, soft shadows, dynamic lighting, tilt-shift, vignette and colour grading, rendered live inside the game.

Project:Atmos is an **unofficial, fan-made fork of [OpenRCT2](https://github.com/OpenRCT2/OpenRCT2)**. It is not an official OpenRCT2 release and is not affiliated with or supported by the OpenRCT2 team, Atari or Chris Sawyer. **Please do not report problems of this build to OpenRCT2** – use the issues of this repository.

By [ROYR](https://royr.clubroy.art/).

## Features

- Volumetric clouds that react to sun and weather, with soft cloud shadows
- Rain, snow, fog and lightning with their own rendering
- Day/night cycle, glowing lamps and freely placed light objects (saved per park)
- Soft shadows for guests, staff, vehicles and tracks; sun rays; swaying trees; falling leaves
- Tilt-shift with scene depth, vignette, colour grading and `.cube` LUT support
- Settings window with tabs *Effects, Sun, Time, Sky, Weather, Water, Colour, LUTs, Cinema, Lights* (hotkey `Ctrl+Shift+A`)

See [`docs/project-atmos/`](docs/project-atmos/) for technical notes and known limits (shadows are a geometric approximation; performance is not yet fully optimised: expect a few ms per frame at 1080p).

## Install (Windows, portable)

1. Download `Project-Atmos-<version>-win64.zip` from the [Releases](../../releases) page and unzip it anywhere.
2. You need your own copy of **RollerCoaster Tycoon 2** (e.g. from GOG or Steam). Its game files are **not** included. On the first start, point the game to your RCT2 folder.
3. Start `Start-Project-Atmos.cmd`. Settings and saves live in the `userdata` folder next to it.

Windows SmartScreen or your antivirus may warn about the unsigned `Project-Atmos.exe`. Compare the SHA-256 checksum from the release page with `SHA256SUMS.txt` before running it.

## Build

See [`BUILDING.md`](BUILDING.md).

## License and credits

Project:Atmos is licensed under the **GNU General Public License v3.0** like OpenRCT2 (see [`licence.txt`](licence.txt)). The source of every release is the tagged commit of this repository. OpenRCT2 contributors are listed in [`contributors.md`](contributors.md); the upstream readme is kept in [`docs/upstream/OpenRCT2-README.md`](docs/upstream/OpenRCT2-README.md).

RollerCoaster Tycoon and RollerCoaster Tycoon 2 are trademarks of Atari / Chris Sawyer. All original game assets belong to their respective owners.
