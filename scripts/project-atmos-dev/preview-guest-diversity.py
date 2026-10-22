#!/usr/bin/env python3
"""Assemble actual core-rendered guest previews from the integration harness."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "artifacts/guest-diversity/preview"
MODELS = ("normal", "woman_bob", "woman_ponytail", "woman_longhair",
          "child_girl", "child_boy")
LABELS = ("Original", "Bob", "Ponytail", "Long hair",
          "Child / ponytail", "Child / short hair")
POSES = ("walk", "stand", "sit", "map", "food", "hat")
SCALE = 6
CELL = 110
LABEL_WIDTH = 170


def main():
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 16)
    atlas = Image.new("RGB", (LABEL_WIDTH + CELL * 8, 65 + CELL * len(MODELS)), (31, 38, 42))
    draw = ImageDraw.Draw(atlas)
    headings = ("Walk 0", "Walk 1", "Walk 2", "Walk 3", "Sitting", "Read map", "Eating", "Hat")
    for column, label in enumerate(headings):
        draw.text((LABEL_WIDTH + column * CELL + 8, 25), label, fill=(210, 218, 220), font=font)
    for row, (model, label) in enumerate(zip(MODELS, LABELS)):
        top = 65 + row * CELL
        draw.rectangle((0, top, atlas.width, top + CELL), fill=(40, 48, 52) if row % 2 == 0 else (31, 38, 42))
        draw.text((12, top + 43), label, fill=(230, 232, 234), font=font)
        for column, (pose, direction) in enumerate((*(('walk', i) for i in range(4)), ('sit', 0), ('map', 0), ('food', 0), ('hat', 0))):
            path = DIRECTORY / f"{model}-{pose}-{direction}.png"
            if not path.exists():
                path = DIRECTORY / f".{model}-{pose}-{direction}.png"
            sprite = Image.open(path)
            rgba = sprite.convert("RGBA")
            indices = list(sprite.get_flattened_data())
            rgba.putalpha(Image.frombytes("L", sprite.size, bytes(255 if p else 0 for p in indices)))
            # Common world anchor (32,48), allowing child height and accessory alignment to be compared.
            rgba = rgba.crop((22, 27, 42, 51)).resize((120, 144), Image.Resampling.NEAREST)
            atlas.paste(rgba, (LABEL_WIDTH + column * CELL - 5, top - 18), rgba)
    output = ROOT / "artifacts/guest-diversity/guest-models.png"
    atlas.save(output)
    print(output)


if __name__ == "__main__":
    main()
