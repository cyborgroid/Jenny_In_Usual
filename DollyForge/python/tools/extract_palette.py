#!/usr/bin/env python3
"""레퍼런스 렌더에서 프로브 영역의 대표색(중앙값)을 뽑아 팔레트 JSON 과 견본 이미지를 만든다.

    python3 tools/extract_palette.py ../characters/jenny/reference_probes.json \
        --json ../../01_Concepts/reference_palette.json --swatch ../../01_Concepts/reference_palette.png
"""
import argparse
import json
import os
import statistics

from PIL import Image, ImageDraw


def median_color(im, box):
    px = list(im.crop(tuple(box)).getdata())
    return tuple(int(statistics.median(c[i] for c in px)) for i in range(3))


def to_hex(rgb):
    return "#%02X%02X%02X" % rgb


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("probes")
    ap.add_argument("--json", required=True)
    ap.add_argument("--swatch")
    args = ap.parse_args()

    base = os.path.dirname(os.path.abspath(args.probes))
    with open(args.probes, encoding="utf-8") as f:
        spec = json.load(f)

    result = {}
    for ref_name, ref in spec["references"].items():
        im = Image.open(os.path.join(base, ref["image"])).convert("RGB")
        result[ref_name] = {
            "image": os.path.basename(ref["image"]),
            "colors": {k: to_hex(median_color(im, box)) for k, box in ref["probes"].items()},
        }

    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("palette ->", args.json)

    if args.swatch:
        rows = [(r, k, v) for r, d in result.items() for k, v in d["colors"].items()]
        cell, w = 28, 300
        img = Image.new("RGB", (w, cell * len(rows)), "white")
        d = ImageDraw.Draw(img)
        for i, (r, k, v) in enumerate(rows):
            d.rectangle([0, i * cell, cell * 2, (i + 1) * cell - 1], fill=v)
            d.text((cell * 2 + 8, i * cell + 8), f"{r}.{k}  {v}", fill="black")
        img.save(args.swatch)
        print("swatch  ->", args.swatch)


if __name__ == "__main__":
    main()
