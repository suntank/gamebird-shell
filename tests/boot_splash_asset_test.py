"""Verify artwork conversion, including the bytes the boot renderer consumes."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    print("SKIP: boot_splash_asset_test requires developer-only Pillow")
    sys.exit(77)


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/prepare-boot-splash.py"
# Loading the converter for tests should not leave generated files in tools/.
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("prepare_boot_splash", TOOL)
CONVERTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONVERTER)


class BootSplashAssetTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gamebird-splash-asset-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "source.png"

    def convert(self, image):
        image.save(self.source)
        return CONVERTER.prepare(self.source)

    @staticmethod
    def pixel(data, x, y):
        offset = (y * 240 + x) * 2
        return data[offset:offset + 2]

    def test_little_endian_primary_colours_and_exact_size(self):
        image = Image.new("RGB", (240, 240), "white")
        image.putpixel((0, 0), (255, 0, 0))
        image.putpixel((1, 0), (0, 255, 0))
        image.putpixel((2, 0), (0, 0, 255))
        image.putpixel((3, 0), (0, 0, 0))
        output = self.convert(image)
        self.assertEqual(len(output), 115200)
        self.assertEqual(output[:10], bytes.fromhex("00f8 e007 1f00 0000 ffff"))

    def test_integer_downscale_preserves_full_artwork(self):
        image = Image.new("RGB", (480, 480), "black")
        image.paste((255, 0, 0), (0, 0, 240, 240))
        image.paste((0, 255, 0), (240, 0, 480, 240))
        image.paste((0, 0, 255), (0, 240, 240, 480))
        image.paste((255, 255, 255), (240, 240, 480, 480))
        output = self.convert(image)
        self.assertEqual(self.pixel(output, 0, 0), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 239, 0), bytes.fromhex("e007"))
        self.assertEqual(self.pixel(output, 0, 239), bytes.fromhex("1f00"))
        self.assertEqual(self.pixel(output, 239, 239), bytes.fromhex("ffff"))
        self.assertEqual(self.pixel(output, 119, 119), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 120, 120), bytes.fromhex("ffff"))

    def test_wide_image_is_centered_and_not_cropped(self):
        image = Image.new("RGB", (480, 240), (0, 255, 0))
        image.paste((255, 0, 0), (0, 0, 2, 240))
        image.paste((0, 0, 255), (478, 0, 480, 240))
        output = self.convert(image)
        self.assertEqual(self.pixel(output, 100, 59), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 100, 60), bytes.fromhex("e007"))
        self.assertEqual(self.pixel(output, 100, 179), bytes.fromhex("e007"))
        self.assertEqual(self.pixel(output, 100, 180), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 0, 120), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 239, 120), bytes.fromhex("1f00"))

    def test_tall_image_is_centered_and_not_cropped(self):
        image = Image.new("RGB", (120, 240), "blue")
        # A contrasting corner matte establishes the horizontal fit boundary.
        image.putpixel((0, 0), (255, 0, 0))
        output = self.convert(image)
        self.assertEqual(self.pixel(output, 59, 100), bytes.fromhex("00f8"))
        self.assertEqual(self.pixel(output, 60, 100), bytes.fromhex("1f00"))
        self.assertEqual(self.pixel(output, 179, 100), bytes.fromhex("1f00"))
        self.assertEqual(self.pixel(output, 180, 100), bytes.fromhex("00f8"))

    def test_transparency_has_an_explicit_opaque_matte(self):
        image = Image.new("RGBA", (240, 240), (12, 34, 56, 0))
        image.putpixel((1, 0), (255, 0, 0, 128))
        image.putpixel((2, 0), (0, 255, 0, 255))
        output = self.convert(image)
        self.assertEqual(output[:6], bytes.fromhex("0000 0080 e007"))
        image.putpixel((0, 0), (255, 255, 255, 255))
        image.putpixel((1, 0), (255, 0, 0, 0))
        output = self.convert(image)
        self.assertEqual(output[:4], bytes.fromhex("ffff ffff"))

    def test_rejects_invalid_and_non_png_sources(self):
        self.source.write_bytes(b"not an image")
        with self.assertRaises(OSError):
            CONVERTER.prepare(self.source)
        Image.new("RGB", (1, 1)).save(self.source, format="JPEG")
        with self.assertRaisesRegex(ValueError, "PNG"):
            CONVERTER.prepare(self.source)

    def test_rejects_oversized_sources(self):
        Image.new("RGB", (4097, 1)).save(self.source)
        with self.assertRaisesRegex(ValueError, "dimensions"):
            CONVERTER.prepare(self.source)

    def test_rejects_animated_png(self):
        first = Image.new("RGBA", (2, 2), "red")
        second = Image.new("RGBA", (2, 2), "blue")
        first.save(self.source, save_all=True, append_images=[second], duration=100)
        with self.assertRaisesRegex(ValueError, "animated"):
            CONVERTER.prepare(self.source)

    def test_check_is_read_only_and_detects_stale_asset(self):
        Image.new("RGB", (240, 240), "red").save(self.source)
        destination = self.directory / "output.rgb565"
        arguments = [sys.executable, str(TOOL), str(self.source), str(destination)]
        generated = subprocess.run(arguments, capture_output=True, text=True)
        self.assertEqual(generated.returncode, 0, generated.stderr)
        before = destination.stat().st_mtime_ns
        checked = subprocess.run([*arguments, "--check"], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(destination.stat().st_mtime_ns, before)
        destination.write_bytes(b"stale")
        checked = subprocess.run([*arguments, "--check"], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 1)
        self.assertEqual(destination.read_bytes(), b"stale")

    def test_cli_cannot_overwrite_source(self):
        Image.new("RGB", (1, 1), "red").save(self.source)
        original = self.source.read_bytes()
        result = subprocess.run([sys.executable, str(TOOL), str(self.source), str(self.source)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.source.read_bytes(), original)

    def test_committed_asset_matches_original_png(self):
        output = CONVERTER.prepare(ROOT / "assets/ui/splash.png")
        self.assertEqual(output, (ROOT / "assets/boot/splash.rgb565").read_bytes())


if __name__ == "__main__":
    unittest.main()
