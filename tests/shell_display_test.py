"""Launch against a synthetic sysfs tree; framebuffer order must not matter."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[1]


class ShellDisplayTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gamebird-display-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sys = self.root / "sys"
        self.mode = self.root / "tv-mode"
        self.shell = self.root / "shell"
        (self.shell / "build").mkdir(parents=True)
        executable = self.shell / "build/gbshell"
        executable.write_text("#!/usr/bin/env python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n")
        executable.chmod(0o755)
        text = (PROJECT / "scripts/gbshell-start.sh").read_text()
        text = text.replace("shell_dir=/home/gamebird/gamebird-shell", f"shell_dir='{self.shell}'")
        text = text.replace("/sys/class/", str(self.sys / "class") + "/")
        text = text.replace("/run/gamebird-tv-mode", str(self.mode))
        self.script = self.root / "start.sh"
        self.script.write_text(text)

    def link(self, relative, target):
        link = self.sys / relative
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(target)

    def panel(self, fb, card):
        device = self.sys / "devices/platform/spi/spi0.0"
        drm = device / "drm" / card
        drm.mkdir(parents=True)
        self.link(f"class/drm/{card}", drm)
        (drm / "device").symlink_to(device)
        self.link(f"class/graphics/{fb}/device", drm)

    def hdmi(self, fb, card, physical_parent=False, connected=True):
        device = self.sys / "devices/platform/gpu"
        drm = device / "drm" / card
        drm.mkdir(parents=True)
        self.link(f"class/drm/{card}", drm)
        (drm / "device").symlink_to(device)
        self.link(f"class/graphics/{fb}/device", device if physical_parent else drm)
        connector = self.sys / f"class/drm/{card}-HDMI-A-1"
        connector.mkdir(parents=True)
        (connector / "status").write_text("connected\n" if connected else "disconnected\n")

    def launch(self, expected):
        result = subprocess.run(["sh", str(self.script)], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, expected, result.stderr)
        return json.loads(result.stdout) if expected == 0 else result.stderr

    def test_early_panel_fb0_is_selected_in_handheld_mode(self):
        self.panel("fb0", "card0")
        self.hdmi("fb1", "card1")
        args = self.launch(0)
        self.assertEqual(args[args.index("--fbdev") + 1], "/dev/fb0")
        self.assertNotIn("--tv-mode", args)

    def test_tv_mode_follows_hdmi_when_panel_registered_first(self):
        self.panel("fb0", "card0")
        self.hdmi("fb1", "card1")
        self.mode.touch()
        args = self.launch(0)
        self.assertEqual(args[args.index("--fbdev") + 1], "/dev/fb1")
        self.assertIn("--tv-mode", args)

    def test_existing_hdmi_first_numbering_still_works(self):
        self.hdmi("fb0", "card0")
        self.panel("fb1", "card1")
        args = self.launch(0)
        self.assertEqual(args[args.index("--fbdev") + 1], "/dev/fb1")
        self.mode.touch()
        args = self.launch(0)
        self.assertEqual(args[args.index("--fbdev") + 1], "/dev/fb0")

    def test_hdmi_framebuffer_can_have_physical_device_parent(self):
        self.panel("fb0", "card0")
        self.hdmi("fb2", "card3", physical_parent=True)
        self.mode.touch()
        args = self.launch(0)
        self.assertEqual(args[args.index("--fbdev") + 1], "/dev/fb2")

    def test_disconnected_hdmi_does_not_fall_back_to_spi_as_tv(self):
        self.panel("fb0", "card0")
        self.hdmi("fb1", "card1", connected=False)
        self.mode.touch()
        self.assertIn("waiting for the tv framebuffer", self.launch(1))

    def test_missing_panel_does_not_use_hdmi_in_handheld_mode(self):
        self.hdmi("fb0", "card0")
        self.assertIn("waiting for the handheld framebuffer", self.launch(1))


if __name__ == "__main__":
    unittest.main()
