#!/usr/bin/env python3
"""Prepare the committed GameBird boot image; Pillow is only needed by developers.

Output is exactly 240 * 240 * 2 bytes of row-major, little-endian RGB565,
without a header. The initramfs renderer consumes this asset without a PNG
decoder. Fit the complete source image into the square using nearest-neighbor
sampling, preserving the pixel artwork and its aspect ratio.
"""

import argparse
import os
from pathlib import Path
import sys
import tempfile
import warnings

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required to regenerate artwork: python3 -m pip install Pillow")


WIDTH = 240
HEIGHT = 240
MAX_SOURCE_SIDE = 4096


def prepare(source: Path) -> bytes:
    """Decode a single PNG and return its deterministic boot-image bytes.

    Letterboxing and transparent pixels use the top-left pixel's RGB colour
    when that pixel is nontransparent, or black when it is fully transparent.
    No metadata, timestamps, or native-endian values enter the output.
    """
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(source) as source_image:
            if source_image.format != "PNG":
                raise ValueError("the source must be a PNG image")
            width, height = source_image.size
            if not (0 < width <= MAX_SOURCE_SIDE and 0 < height <= MAX_SOURCE_SIDE):
                raise ValueError(f"source dimensions must be 1..{MAX_SOURCE_SIDE} pixels per side")
            if getattr(source_image, "n_frames", 1) != 1:
                raise ValueError("animated PNG images are not supported")
            rgba = source_image.convert("RGBA")

    corner = rgba.getpixel((0, 0))
    background = corner[:3] if corner[3] else (0, 0, 0)
    flattened = Image.new("RGBA", rgba.size, (*background, 255))
    flattened.alpha_composite(rgba)
    flattened = flattened.convert("RGB")

    # Integer round-to-nearest calculations avoid aspect-ratio drift and keep
    # a very narrow image at least one pixel wide/high.
    if width * HEIGHT >= height * WIDTH:
        fitted_width = WIDTH
        fitted_height = max(1, (height * WIDTH + width // 2) // width)
    else:
        fitted_height = HEIGHT
        fitted_width = max(1, (width * HEIGHT + height // 2) // height)
    fitted = flattened.resize((fitted_width, fitted_height), Image.Resampling.NEAREST)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), background)
    canvas.paste(fitted, ((WIDTH - fitted_width) // 2, (HEIGHT - fitted_height) // 2))

    result = bytearray(WIDTH * HEIGHT * 2)
    # Specify the byte order explicitly even on a big-endian build host.
    colours = canvas.tobytes()
    for index in range(WIDTH * HEIGHT):
        red, green, blue = colours[index * 3:index * 3 + 3]
        pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        result[index * 2] = pixel & 0xFF
        result[index * 2 + 1] = pixel >> 8
    return bytes(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="source PNG artwork")
    parser.add_argument("destination", type=Path, help="raw little-endian RGB565 asset")
    parser.add_argument("--check", action="store_true", help="verify the destination without writing it")
    args = parser.parse_args()
    temporary_name = None
    try:
        if args.source.resolve() == args.destination.resolve():
            raise ValueError("source and destination must be different files")
        output = prepare(args.source)
        if args.check:
            if args.destination.read_bytes() != output:
                raise ValueError("boot asset differs from the source PNG; regenerate it")
            print(f"Verified {args.destination}: {WIDTH}x{HEIGHT}, {len(output)} bytes")
            return 0

        args.destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=args.destination.parent, prefix=".splash-", delete=False) as temporary:
            temporary_name = temporary.name
            temporary.write(output)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.chmod(temporary_name, 0o644)
        os.replace(temporary_name, args.destination)
        temporary_name = None
        print(f"Wrote {args.destination}: {WIDTH}x{HEIGHT}, {len(output)} bytes")
        return 0
    except (OSError, ValueError, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        print(f"prepare-boot-splash: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
