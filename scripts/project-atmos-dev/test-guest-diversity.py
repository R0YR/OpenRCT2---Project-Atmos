#!/usr/bin/env python3
"""Compile and exercise the actual indexed guest appearance transform with MSVC.

Run: python scripts/test-guest-diversity.py
Optional --objects-dir and --standard-object validate generated animation JSON.
"""

import argparse
import json
from pathlib import Path
import subprocess
import zipfile


CHECKS = r"""
using namespace OpenRCT2;
static bool Same(const GuestAppearanceSprite& a, const GuestAppearanceSprite& b)
{
    return a.width == b.width && a.height == b.height && a.xOffset == b.xOffset
        && a.yOffset == b.yOffset && a.pixels == b.pixels;
}
static void Valid(const GuestAppearanceSprite& s)
{
    assert(s.width >= 0 && s.height >= 0);
    assert(s.pixels.size() == static_cast<size_t>(s.width) * s.height);
}
int main()
{
    // CI failures must return to the caller rather than opening a Windows assertion dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    constexpr GuestAppearance styles[] = { GuestAppearance::normal, GuestAppearance::bob,
        GuestAppearance::ponytail, GuestAppearance::longHair,
        GuestAppearance::childGirl, GuestAppearance::childBoy };
    // Real decoded standard guest frame: palette indices retain clothing remaps.
    GuestAppearanceSprite source{12, 18, -6, -17, {
        0,0,0,0,10,106,106,107,0,0,0,0,
        0,0,0,10,10,106,107,111,106,0,0,0,
        0,0,0,10,106,106,106,111,109,10,0,0,
        0,0,0,0,106,10,108,113,114,0,0,0,
        0,0,0,0,109,109,111,113,109,0,0,0,
        0,0,0,10,246,109,109,109,0,0,0,0,
        0,0,0,0,10,246,246,248,250,245,0,0,
        0,0,0,0,10,246,246,247,247,247,0,0,
        0,0,0,0,10,246,246,246,246,248,0,0,
        0,0,0,10,246,246,246,247,248,0,0,0,
        0,0,0,0,205,246,246,247,109,0,0,0,
        0,0,0,107,205,205,205,111,109,0,0,0,
        0,0,0,108,205,204,205,111,108,0,0,0,
        0,0,106,108,205,204,106,108,113,111,0,0,
        0,106,106,109,108,0,0,0,108,204,106,106,
        0,106,106,106,0,0,0,0,107,108,107,108,
        107,106,106,107,0,0,0,0,106,107,107,107,
        0,0,106,0,0,0,0,0,0,106,0,0 }};
    const auto original = source;
    std::set<std::vector<uint8_t>> adultResults;
    for (auto style : styles)
        for (uint8_t direction = 0; direction < 4; ++direction)
        {
            auto result = TransformGuestAppearance(source, style, direction);
            Valid(result);
            assert(Same(result, TransformGuestAppearance(source, style, direction)));
            assert(Same(result, TransformGuestAppearance(source, style, direction + 4)));
            assert(Same(source, original)); // transformations cannot mutate shared source images
            assert(std::find(result.pixels.begin(), result.pixels.end(), 0) != result.pixels.end());
            assert(std::any_of(result.pixels.begin(), result.pixels.end(), [](uint8_t p){ return p >= 243 && p <= 254; }));
            // Clothing stays vanilla: shirt and trousers channels remain.
            assert(std::any_of(result.pixels.begin(), result.pixels.end(), [](uint8_t p){ return p >= 202 && p <= 213; }));
            if (style == GuestAppearance::normal)
                assert(Same(source, result));
            if (style == GuestAppearance::bob || style == GuestAppearance::ponytail
                || style == GuestAppearance::longHair)
                adultResults.insert(result.pixels);
            if (style == GuestAppearance::childGirl || style == GuestAppearance::childBoy)
            {
                assert(result.height < source.height + 4);
                assert(result.yOffset == ScaleGuestCoordinate(source.yOffset - 2));
                assert(result.xOffset == ScaleGuestCoordinate(source.xOffset - 2));
            }
            else if (style != GuestAppearance::normal)
            {
                const int padding = 2;
                assert(result.height == source.height + padding * 2 && result.width == source.width + padding * 2);
                assert(result.xOffset == source.xOffset - padding && result.yOffset == source.yOffset - padding);
            }
            Valid(TransformGuestAppearance(source, style, direction, true));
        }
    assert(adultResults.size() >= 3); // hair variants must produce genuinely different sprites
    for (int coordinate = -64; coordinate <= 64; ++coordinate)
        assert(ScaleGuestCoordinate(coordinate) == static_cast<int>(std::floor(coordinate * 0.75)));
    // Body and separately painted accessories must scale around the same world origin.
    for (int offset = -16; offset <= 16; ++offset)
    {
        GuestAppearanceSprite foot{1, 1, offset, 0, { 109 }};
        auto scaled = ScaleGuestAppearanceSprite(foot);
        assert(scaled.yOffset == 0 && scaled.pixels[0] == 109);
        assert(scaled.xOffset == ScaleGuestCoordinate(offset));
    }
    GuestAppearanceSprite labelled{65, 1, -32, 0, {}};
    for (int x = 0; x < labelled.width; ++x)
        labelled.pixels.push_back(static_cast<uint8_t>(x + 1));
    auto sampled = ScaleGuestAppearanceSprite(labelled);
    for (int x = 0; x < sampled.width; ++x)
    {
        const int target = x + sampled.xOffset;
        int firstSource = labelled.width - 1;
        for (int sx = 0; sx < labelled.width; ++sx)
            if (ScaleGuestCoordinate(sx + labelled.xOffset) == target)
            {
                firstSource = sx;
                break;
            }
        assert(sampled.pixels[x] == labelled.pixels[firstSource]);
    }
    // Accessory-only and completely transparent frames cannot acquire invented body pixels.
    for (auto style : styles)
    {
        GuestAppearanceSprite empty{};
        assert(Same(empty, TransformGuestAppearance(empty, style, 0)));
        GuestAppearanceSprite malformed{8, 8, 0, 0, {109}};
        assert(Same(malformed, TransformGuestAppearance(malformed, style, 0)));
        GuestAppearanceSprite negative{-1, 8, 0, 0, {109}};
        assert(Same(negative, TransformGuestAppearance(negative, style, 0)));
        GuestAppearanceSprite transparent{8, 8, -4, -7, std::vector<uint8_t>(64)};
        auto result = TransformGuestAppearance(transparent, style, 2);
        Valid(result);
        assert(std::all_of(result.pixels.begin(), result.pixels.end(), [](uint8_t p){ return p == 0; }));
        // Exhaust every tiny layout, including one-pixel drowning/partial body frames.
        for (int height = 1; height <= 7; ++height)
            for (int width = 1; width <= 7; ++width)
                for (uint8_t direction = 0; direction < 4; ++direction)
                {
                    GuestAppearanceSprite sparse{width, height, -width / 2, 1 - height,
                        std::vector<uint8_t>(static_cast<size_t>(width) * height)};
                    sparse.pixels.back() = 109;
                    Valid(TransformGuestAppearance(sparse, style, direction));
                    sparse.pixels.back() = 245;
                    Valid(TransformGuestAppearance(sparse, style, direction, true));
                }
    }
    puts("PASS standard identity, deterministic diversity, remaps, transparency, child anchors, tiny sparse frames");
}
"""


def validate_objects(directory, standard_path):
    standard = json.loads(standard_path.read_text(encoding="utf-8"))
    files = sorted(directory.rglob("*.json"))
    variants = [json.loads(path.read_text(encoding="utf-8")) for path in files]
    variants = [obj for obj in variants if obj.get("id", "").startswith("atmosphere.peep_animations.")]
    assert len(variants) == 5, f"expected five guest variants, found {len(variants)}"
    assert len({obj["id"] for obj in variants}) == 5
    assert {obj["properties"]["guestAppearance"] for obj in variants} == {
        "bob", "ponytail", "longHair", "childGirl", "childBoy"}
    for obj in variants:
        assert obj["id"] != standard["id"]
        assert obj["sourceGame"] == ["custom"], "variants must embed in saved parks and network maps"
        source = next(path for path in files if path.stem == obj["id"])
        with zipfile.ZipFile(source.with_suffix(".parkobj")) as archive:
            assert archive.namelist() == ["object.json"]
            assert archive.testzip() is None
            assert archive.read("object.json") == source.read_bytes(), obj["id"]
        assert obj["properties"]["peepType"] == "guest"
        assert obj["properties"]["noRandomPlacement"] is False
        assert len(obj["animationGroups"]) == len(standard["animationGroups"])
        assert len(obj["images"]) >= len(standard["images"])
        assert obj["images"][:len(standard["images"])] == standard["images"]
        for group, reference in zip(obj["animationGroups"], standard["animationGroups"]):
            assert group["animations"] == reference["animations"], obj["id"]
            assert "legacyPosition" not in group, obj["id"]
    print("PASS five guest variant packages, no legacy import overrides")


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars", type=Path, default=Path(
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"))
    parser.add_argument("--output-dir", type=Path, default=root / "artifacts/guest-diversity/checks")
    parser.add_argument("--objects-dir", type=Path)
    parser.add_argument("--standard-object", type=Path)
    args = parser.parse_args()
    if bool(args.objects_dir) != bool(args.standard_object):
        parser.error("--objects-dir and --standard-object must be supplied together")
    if args.objects_dir:
        validate_objects(args.objects_dir, args.standard_object)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = args.output_dir.resolve()
    cpp = work / "guest_checks.cpp"
    header = (root / "src/openrct2/peep/GuestAppearance.h").as_posix()
    cpp.write_text('#include <algorithm>\n#include <cassert>\n#include <cmath>\n#include <cstdio>\n'
                   '#include <cstdlib>\n#include <crtdbg.h>\n#include <set>\n'
                   f'#include "{header}"\n' + CHECKS, encoding="utf-8")
    command = (f'call "{args.vcvars}" >nul && cl /nologo /W4 /Od /RTC1 /MDd /EHsc /std:c++20 "{cpp}" '
               f'/Fe:"{work / "guest_checks.exe"}" /Fo:"{work / "guest_checks.obj"}"')
    subprocess.run(command, shell=True, check=True)
    subprocess.run([str(work / "guest_checks.exe")], check=True)


if __name__ == "__main__":
    main()
