#!/usr/bin/env python3
"""Generate guest descriptors referencing the installed RCT2 animation sprites.

No proprietary pixels are bundled: GuestAppearance.h authors indexed silhouettes
at object load, retaining every original animation and item group.
"""
import argparse
import copy
import json
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VARIANTS = (
    ("woman_bob", "bob", "Woman — bob", "Frau — Bob"),
    ("woman_ponytail", "ponytail", "Woman — ponytail", "Frau — Pferdeschwanz"),
    ("woman_longhair", "longHair", "Woman — long hair", "Frau — lange Haare"),
    ("child_girl", "childGirl", "Child — ponytail", "Kind — Pferdeschwanz"),
    ("child_boy", "childBoy", "Child — short hair", "Kind — kurze Haare"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, default=ROOT / "bin/data/object/rct2/peep_animations/rct2.peep_animations.guest.json")
    parser.add_argument("--out", type=Path, default=ROOT / "resources/guest-diversity/objects")
    args = parser.parse_args()
    template = json.loads(args.base.read_text(encoding="utf-8"))
    args.out.mkdir(parents=True, exist_ok=True)
    for suffix, appearance, english, german in VARIANTS:
        obj = copy.deepcopy(template)
        obj["id"] = "atmosphere.peep_animations." + suffix
        obj["version"] = "1.1"
        # Custom classification makes these descriptors travel with saves and network maps.
        # Original sprite authors stay credited; images still reference the user's installed G1.
        obj["sourceGame"] = ["custom"]
        obj["properties"]["guestAppearance"] = appearance
        obj["properties"]["noRandomPlacement"] = False
        obj["strings"]["name"] = {"en-GB": english, "de-DE": german}
        for index, group in enumerate(obj["animationGroups"]):
            group["scriptName"] = f"{obj['id']}.{index}"
            # Only the original guest object may supply legacy import positions.
            group.pop("legacyPosition", None)
        path = args.out / (obj["id"] + ".json")
        text = json.dumps(obj, indent=4, ensure_ascii=False) + "\n"
        path.write_text(text, encoding="utf-8", newline="\n")
        # Park/network packing accepts .parkobj archives, not loose JSON descriptors.
        entry = zipfile.ZipInfo("object.json", date_time=(2026, 1, 1, 0, 0, 0))
        entry.compress_type = zipfile.ZIP_DEFLATED
        with zipfile.ZipFile(path.with_suffix(".parkobj"), "w") as archive:
            archive.writestr(entry, text.encode("utf-8"))
    print(f"Generated {len(VARIANTS)} guest descriptors in {args.out}")


if __name__ == "__main__":
    main()
