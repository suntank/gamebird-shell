#!/usr/bin/env python3
"""Test TV connector transitions with a sysfs fixture, never the real display."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "scripts/tv-mode-manager.sh"
# Load the real function definitions without executing the service entry point.
DEFINITIONS = SOURCE.read_text().split('\ncase "${1:---watch}" in\n', 1)
assert len(DEFINITIONS) == 2


class DisplayTransitions(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gamebird-tv-display-")
        self.root = Path(self.temporary.name)
        self.drm = self.root / "drm"
        self.drm.mkdir()
        self.panel = self.card(0, "spi0.0", "Unknown-1")
        self.hdmi = self.card(1, "vc4", "HDMI-A-1")
        self.decoy = self.card(2, "spi0.01", "Unknown-1")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # A real kernel status file reads connected/disconnected after accepting
        # detect/off. Emulate only that readback contract; writes are real files.
        reader = self.bin / "cat"
        reader.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "import sys\n"
            "p = Path(sys.argv[1])\n"
            "value = p.read_text().strip()\n"
            "if p.with_name('reject').exists(): value = 'connected'\n"
            "else: value = {'off': 'disconnected', 'detect': 'connected'}.get(value, value)\n"
            "print(value)\n"
        )
        reader.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"])

    def tearDown(self):
        self.temporary.cleanup()

    def card(self, number, hardware, connector):
        device = self.root / "devices" / hardware
        device.mkdir(parents=True, exist_ok=True)
        card = self.drm / f"card{number}"
        card.mkdir()
        (card / "device").symlink_to(device)
        output = self.drm / f"card{number}-{connector}"
        output.mkdir()
        status = output / "status"
        status.write_text("connected\n")
        return status

    def run_functions(self, commands):
        script = DEFINITIONS[0] + '\ndrm_class=$1\n' + commands
        return subprocess.run(
            ["/bin/sh", "-c", script, "tv-display-test", str(self.drm), str(self.root)],
            env=self.environment, capture_output=True, text=True, timeout=5,
        )

    def test_tv_then_handheld_without_repeated_writes(self):
        result = self.run_functions("set_panel_display tv\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.panel.read_text(), "off\n")
        for untouched in (self.hdmi, self.decoy):
            self.assertEqual(untouched.read_text(), "connected\n")
        modified = self.panel.stat().st_mtime_ns
        result = self.run_functions("set_panel_display tv\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.panel.stat().st_mtime_ns, modified)
        result = self.run_functions("set_panel_display handheld\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.panel.read_text(), "detect\n")
        modified = self.panel.stat().st_mtime_ns
        result = self.run_functions("set_panel_display handheld\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.panel.stat().st_mtime_ns, modified)

    def test_rejected_transition_keeps_existing_handheld(self):
        self.panel.with_name("reject").touch()
        result = self.run_functions(
            'state_file="$2/mode"\n'
            'active_input_config="$2/input.cfg"\n'
            'gpio_driver="$2/gpio"\n'
            'mkdir -p "$gpio_driver"\n'
            'install_autoconfig() { :; }\n'
            'set_active_input_config() { : > "$active_input_config"; }\n'
            'apply_mode || :\n'
            '[ ! -e "$state_file" ]\n'
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("keeping handheld mode", result.stderr)
        self.assertEqual(self.panel.read_text(), "detect\n")
        self.assertFalse((self.root / "mode").exists())

    def test_existing_handheld_is_not_reprobed(self):
        modified = self.panel.stat().st_mtime_ns
        result = self.run_functions("set_panel_display handheld\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.panel.stat().st_mtime_ns, modified)


if __name__ == "__main__":
    unittest.main()
