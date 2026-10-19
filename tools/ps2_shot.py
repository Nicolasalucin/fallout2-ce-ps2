#!/usr/bin/env python3
"""Convert PS2 port frame captures (.idx + .pal) into PNG images.

The capture code in src/platform/ps2/video_gs.c dumps two raw files per
presented frame:

  <name>_<n>.idx   640*480 bytes of 8-bit palette indices (the T8 texture)
  <name>_<n>.pal   256 entries of GS CT32 (little endian bytes R,G,B,A)

Usage:
  tools/ps2_shot.py build_ps2/caps/mve_after_0        # -> mve_after_0.png
  tools/ps2_shot.py build_ps2/caps/ --scale 2         # all pairs in a dir
  tools/ps2_shot.py cap.idx cap.pal --out shot.png

Requires Pillow (python3 -c "import PIL").
"""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: pip install pillow")


WIDTH = 640
HEIGHT = 480
PALETTE_ENTRIES = 256


def load_palette(path: Path) -> bytes:
    raw = path.read_bytes()
    if len(raw) < PALETTE_ENTRIES * 4:
        raise SystemExit(f"{path}: expected {PALETTE_ENTRIES * 4} bytes, got {len(raw)}")
    # CT32 entries are little-endian R,G,B,A; putpalette wants R,G,B x 256
    rgb = bytearray(PALETTE_ENTRIES * 3)
    for i in range(PALETTE_ENTRIES):
        r, g, b, a = raw[i * 4 : i * 4 + 4]
        rgb[i * 3 : i * 3 + 3] = bytes((r, g, b))
    return bytes(rgb)


def convert(idx_path: Path, pal_path: Path, out_path: Path, scale: int) -> None:
    data = idx_path.read_bytes()
    expected = WIDTH * HEIGHT
    if len(data) != expected:
        raise SystemExit(f"{idx_path}: expected {expected} bytes, got {len(data)}")

    # The PAL file stores GS CT32 entries as 0xAABBGGRR, which little-endian
    # memory lays out as R,G,B,A -- exactly what putpalette() wants.
    image = Image.frombytes("P", (WIDTH, HEIGHT), data)
    image.putpalette(load_palette(pal_path))
    image = image.convert("RGB")
    if scale > 1:
        image = image.resize((WIDTH * scale, HEIGHT * scale), Image.NEAREST)
    image.save(out_path)
    print(f"{out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", type=Path, help=".idx file, capture base name, or directory")
    parser.add_argument("palette", nargs="?", type=Path, help=".pal file (only for the two-file form)")
    parser.add_argument("--out", type=Path, help="output PNG (single capture only)")
    parser.add_argument("--scale", type=int, default=1, help="nearest-neighbour upscale factor")
    args = parser.parse_args()

    if args.palette is not None:
        out = args.out or args.target.with_suffix(".png")
        convert(args.target, args.palette, out, args.scale)
        return

    if args.target.is_dir():
        pairs = sorted(p.with_suffix(".idx") for p in args.target.glob("*.pal"))
        if not pairs:
            sys.exit(f"{args.target}: no .idx/.pal pairs")
        for idx_path in pairs:
            convert(idx_path, idx_path.with_suffix(".pal"), idx_path.with_suffix(".png"), args.scale)
        return

    idx_path = args.target if args.target.suffix == ".idx" else args.target.with_suffix(".idx")
    pal_path = idx_path.with_suffix(".pal")
    out = args.out or idx_path.with_suffix(".png")
    convert(idx_path, pal_path, out, args.scale)


if __name__ == "__main__":
    main()
