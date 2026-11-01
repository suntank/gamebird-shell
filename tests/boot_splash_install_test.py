"""Exercise boot installation with a temporary root and fake kernel tools.

No mounts, block devices, host initramfs images, or services are touched.
"""

import base64
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


PROJECT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("boot_splash_install", PROJECT / "scripts/boot-splash-install.py")
splash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(splash)


class BootSplashInstallTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gamebird-splash-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "root"
        self.root.mkdir()
        self.source = PROJECT
        self.build = Path(self.temporary.name) / "build"
        self.build.mkdir()
        (self.build / "gbsplash").write_bytes(b"fixture renderer version one")
        (self.build / "gbsplash").chmod(0o755)
        self.calls = []
        self.version = "6.12.0+rpt-rpi-v7"
        self.omit = None
        self.fail_rebuild = False
        self.noop_rebuild = False
        self.builtin_modules = set()
        self.architecture = "armv7l"
        self.make("boot/firmware/kernel7.img", b"fixture kernel")
        self.original_config = b"# Unit boot configuration\nauto_initramfs=1\n[all]\ndtparam=spi=on\n"
        self.make(splash.BOOT_CONFIG, self.original_config)
        self.make("boot/firmware/cmdline.txt", b"root=PARTUUID=fixture overlayroot=tmpfs:recurse=0 quiet\n")
        self.make("lib/firmware/vendor/gamepi13.bin", b"fixture panel init commands")
        self.make("sys/bus/spi/devices/spi0.0/of_node/compatible", b"vendor/gamepi13\0panel-mipi-dbi-spi\0")
        self.module_driver("sys/bus/spi/devices/spi0.0", "sys/bus/spi/drivers/panel-mipi-dbi-spi", "panel_mipi_dbi")
        self.module_driver("sys/devices/platform/spi-controller", "sys/bus/platform/drivers/spi-bcm2835", "spi_bcm2835")
        link = self.root / "sys/class/spi_master/spi0/device"
        link.parent.mkdir(parents=True)
        link.symlink_to(self.root / "sys/devices/platform/spi-controller")
        self.make("lib/modules/" + self.version + "/modules.builtin", b"")
        self.make("usr/bin/timeout", b"fixture GNU timeout")
        self.old_archive = self.archive_bytes(False)
        self.make("boot/initrd.img-" + self.version, self.old_archive)
        self.make("boot/firmware/initramfs7", self.old_archive)
        self.installer = splash.Installer(self.root, self.source, self.build, self.run_command)
        patcher = mock.patch.object(splash.os, "sync")
        patcher.start()
        self.addCleanup(patcher.stop)

    def make(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def module_driver(self, device, driver, module):
        device, driver = self.root / device, self.root / driver
        device.mkdir(parents=True, exist_ok=True)
        driver.mkdir(parents=True, exist_ok=True)
        (device / "driver").symlink_to(driver)
        target = self.root / "sys/module" / module
        target.mkdir(parents=True, exist_ok=True)
        (driver / "module").symlink_to(target)

    def archive_bytes(self, installed):
        entries = ["scripts/init-bottom/overlayroot",
                   f"usr/lib/modules/{self.version}/kernel/fs/overlayfs/overlay.ko.xz"]
        payload = {}
        if installed:
            expected = {splash.BINARY: splash.BINARY, splash.IMAGE: splash.IMAGE,
                        splash.TIMEOUT: "usr/bin/timeout",
                        splash.CONF: splash.CONF,
                        "scripts/init-top/00-gamebird-splash": splash.RUNTIME,
                        "usr/lib/firmware/vendor/gamepi13.bin": "lib/firmware/vendor/gamepi13.bin"}
            for relative, source in expected.items():
                entries.append(relative)
                payload[relative] = base64.b64encode((self.root / source).read_bytes()).decode()
            config = (self.root / splash.CONF).read_text()
            modules = config.split("GBSPLASH_MODULES='", 1)[1].split("'", 1)[0].split()
            for module in modules:
                if module not in self.builtin_modules:
                    entries.append(f"usr/lib/modules/{self.version}/kernel/drivers/{module}.ko.xz")
        if self.omit:
            entries = [entry for entry in entries if self.omit not in entry]
        return json.dumps({"entries": entries, "payload": payload}, sort_keys=True).encode()

    def run_command(self, args, **kwargs):
        self.calls.append(args)
        command = Path(args[0]).name
        stdout, stderr, code = "", "", 0
        if command == "uname":
            stdout = self.architecture if args[1] == "-m" else self.version
        elif command == "gbsplash":
            if args[1] != "--check-image" or Path(args[2]).stat().st_size != 240 * 240 * 2:
                code, stderr = 1, "invalid image"
        elif command == "modinfo":
            module = args[-1]
            stdout = "(builtin)" if module in self.builtin_modules else f"/lib/modules/{self.version}/kernel/drivers/{module}.ko.xz"
        elif command == "update-initramfs":
            if not self.noop_rebuild:
                data = b"broken partial archive" if self.fail_rebuild else self.archive_bytes((self.root / splash.HOOK).exists())
                self.make("boot/initrd.img-" + self.version, data)
                self.make("boot/firmware/initramfs7", data)
            if self.fail_rebuild:
                code, stderr = 42, "fixture rebuild failure"
        elif command == "lsinitramfs":
            stdout = "\n".join(json.loads(Path(args[1]).read_bytes())["entries"])
        elif command == "unmkinitramfs":
            archive = json.loads(Path(args[1]).read_bytes())
            for relative, data in archive["payload"].items():
                path = Path(args[2]) / "main" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(base64.b64decode(data))
        else:
            raise AssertionError(f"Unexpected host command: {args}")
        return subprocess.CompletedProcess(args, code, stdout, stderr)

    def test_success_installs_exact_payload_and_receipt(self):
        self.installer.install()
        self.installer.verify()
        self.assertEqual((self.root / splash.BOOT_CONFIG).read_bytes(), self.original_config)
        self.assertEqual((self.root / splash.BINARY).read_bytes(), (self.build / "gbsplash").read_bytes())
        self.assertIn("TTYReset=no", (self.root / splash.DROPIN).read_text())
        self.assertEqual(len((self.root / splash.RECEIPT).read_text().splitlines()), 10)
        self.assertTrue(any(Path(call[0]).name == "unmkinitramfs" for call in self.calls))
        self.assertFalse(any(Path(call[0]).name in ("mount", "systemctl", "reboot") for call in self.calls))

    def test_rebuild_failure_restores_boot_and_installed_files(self):
        self.fail_rebuild = True
        with self.assertRaisesRegex(splash.InstallError, "rebuild failure"):
            self.installer.install()
        self.assertEqual((self.root / "boot/firmware/initramfs7").read_bytes(), self.old_archive)
        self.assertEqual((self.root / splash.BOOT_CONFIG).read_bytes(), self.original_config)
        self.assertFalse((self.root / splash.BINARY).exists())
        self.assertFalse((self.root / splash.STATE_DIR).exists())

    def test_staged_binary_need_not_be_executable_on_noexec_mount(self):
        with mock.patch.object(splash.os, "access", return_value=False):
            self.installer.install()
        checks = [call for call in self.calls if Path(call[0]).name == "gbsplash"]
        self.assertEqual(checks, [[str(self.root / splash.BINARY), "--check-image",
                                   str(self.root / splash.IMAGE)]])

    def test_missing_firmware_in_archive_rolls_back(self):
        self.omit = "firmware/"
        with self.assertRaisesRegex(splash.InstallError, "panel firmware"):
            self.installer.install()
        self.assertEqual((self.root / "boot/firmware/initramfs7").read_bytes(), self.old_archive)

    def test_private_timeout_is_required_even_if_busybox_timeout_exists(self):
        self.omit = splash.TIMEOUT
        with self.assertRaisesRegex(splash.InstallError, "gbsplash-timeout"):
            self.installer.install()
        self.assertEqual((self.root / "boot/firmware/initramfs7").read_bytes(), self.old_archive)

    def test_archive_timeout_bytes_must_match_gnu_utility(self):
        self.installer.install()
        archive = self.root / "boot/firmware/initramfs7"
        data = json.loads(archive.read_bytes())
        data["payload"][splash.TIMEOUT] = base64.b64encode(b"BusyBox timeout").decode()
        archive.write_text(json.dumps(data))
        with self.assertRaisesRegex(splash.InstallError, "stale bytes.*gbsplash-timeout"):
            self.installer.validate_archive_bytes(archive, self.installer.state()["panel"])

    def test_missing_overlay_hook_never_replaces_working_boot(self):
        self.omit = "init-bottom/overlayroot"
        with self.assertRaisesRegex(splash.InstallError, "lost overlayroot"):
            self.installer.install()
        self.assertEqual((self.root / "boot/firmware/initramfs7").read_bytes(), self.old_archive)

    def test_successful_but_stale_rebuild_is_rejected_by_bytes(self):
        self.installer.install()
        prior_archive = (self.root / "boot/firmware/initramfs7").read_bytes()
        prior_receipt = (self.root / splash.RECEIPT).read_bytes()
        (self.build / "gbsplash").write_bytes(b"fixture renderer version two")
        self.noop_rebuild = True
        with self.assertRaisesRegex(splash.InstallError, "stale bytes"):
            self.installer.install()
        self.assertEqual((self.root / "boot/firmware/initramfs7").read_bytes(), prior_archive)
        self.assertEqual((self.root / splash.RECEIPT).read_bytes(), prior_receipt)
        self.installer.verify_receipt()

    def test_enables_only_owned_block_and_uninstall_preserves_other_edits(self):
        original = b"# deliberately no final newline\nauto_initramfs=0"
        self.make(splash.BOOT_CONFIG, original)
        self.installer.install()
        self.assertEqual((self.root / splash.BOOT_CONFIG).read_bytes(), original + splash.BOOT_BLOCK.encode())
        with (self.root / splash.BOOT_CONFIG).open("ab") as stream:
            stream.write(b"\n# later user edit\n")
        self.installer.install()  # Must not replace original-state ownership.
        self.installer.uninstall()
        self.assertEqual((self.root / splash.BOOT_CONFIG).read_bytes(), original + b"\n# later user edit\n")
        self.assertFalse((self.root / splash.HOOK).exists())
        self.assertFalse((self.root / splash.DROPIN).exists())
        self.assertFalse((self.root / splash.STATE_DIR).exists())

    def test_uninstall_restores_preexisting_owned_path(self):
        original = b"[Service]\nTTYReset=yes\n"
        original_launcher = b"#!/bin/sh\nexec old-launcher --fbdev /dev/fb0\n"
        original_tv_manager = b"#!/bin/sh\necho old-tv-manager\n"
        self.make(splash.DROPIN, original)
        self.make(splash.LAUNCHER, original_launcher)
        self.make(splash.TV_MANAGER, original_tv_manager)
        self.installer.install()
        self.assertEqual((self.root / splash.LAUNCHER).read_bytes(),
                         (self.source / "scripts/gbshell-start.sh").read_bytes())
        self.assertEqual((self.root / splash.TV_MANAGER).read_bytes(),
                         (self.source / "scripts/tv-mode-manager.sh").read_bytes())
        self.installer.uninstall()
        self.assertEqual((self.root / splash.DROPIN).read_bytes(), original)
        self.assertEqual((self.root / splash.LAUNCHER).read_bytes(), original_launcher)
        self.assertEqual((self.root / splash.TV_MANAGER).read_bytes(), original_tv_manager)

    def test_wrong_driver_and_unsafe_firmware_fail_before_mutation(self):
        self.make("sys/bus/spi/devices/spi0.0/of_node/compatible", b"../../outside\0")
        with self.assertRaisesRegex(splash.InstallError, "unsafe firmware"):
            self.installer.install()
        self.assertFalse((self.root / splash.HOOK).exists())
        (self.root / "sys/bus/spi/devices/spi0.0/driver").unlink()
        with self.assertRaisesRegex(splash.InstallError, "not bound"):
            self.installer.install()

    def test_manual_install_replaces_network_wait_units_and_restores_originals(self):
        originals = {}
        for relative in (splash.SHELL_SERVICE, splash.LIBRARY_SERVICE):
            originals[relative] = b"[Unit]\nAfter=network-online.target\nWants=network-online.target\n"
            self.make(relative, originals[relative])
        self.installer.install()
        for relative in originals:
            self.assertNotIn("network-online.target", (self.root / relative).read_text())
        self.installer.install()
        self.installer.uninstall()
        for relative, original in originals.items():
            self.assertEqual((self.root / relative).read_bytes(), original)

    def test_upgrade_captures_new_service_originals(self):
        self.installer.install()
        state = self.installer.state()
        for relative in (splash.SHELL_SERVICE, splash.LIBRARY_SERVICE):
            del state["originals"][relative]
            self.make(relative, b"original service before upgrade")
        self.make(splash.STATE_DIR + "/state.json", json.dumps(state).encode())
        self.installer.install()
        self.installer.uninstall()
        for relative in (splash.SHELL_SERVICE, splash.LIBRARY_SERVICE):
            self.assertEqual((self.root / relative).read_bytes(), b"original service before upgrade")

    def test_conditional_custom_boot_setting_is_not_guessed(self):
        original = b"[pi02]\nauto_initramfs=1\n"
        self.make(splash.BOOT_CONFIG, original)
        with self.assertRaisesRegex(splash.InstallError, "Conditional boot setting"):
            self.installer.install()
        self.assertEqual((self.root / splash.BOOT_CONFIG).read_bytes(), original)
        self.assertFalse((self.root / splash.HOOK).exists())

    def test_built_in_module_does_not_require_ko_in_archive(self):
        self.builtin_modules.add("spi_bcm2835")
        self.installer.install()
        self.installer.verify_receipt()

    def test_observed_backlight_module_is_copied_before_panel(self):
        self.module_driver("sys/devices/platform/panel-backlight", "sys/bus/platform/drivers/gpio-backlight", "gpio_backlight")
        link = self.root / "sys/class/backlight/gamebird/device"
        link.parent.mkdir(parents=True)
        link.symlink_to(self.root / "sys/devices/platform/panel-backlight")
        self.installer.install()
        modules = self.installer.state()["panel"]["modules"]
        self.assertIn("gpio_backlight", modules)
        self.assertLess(modules.index("gpio_backlight"), modules.index("panel_mipi_dbi"))

    def test_receipt_detects_modified_runtime(self):
        self.installer.install()
        (self.root / splash.RUNTIME).write_text("different script")
        with self.assertRaisesRegex(splash.InstallError, "failed verification"):
            self.installer.verify_receipt()

    def test_corrupt_state_types_fail_clearly(self):
        self.make(splash.STATE_DIR + "/state.json", b"[]")
        with self.assertRaisesRegex(splash.InstallError, "Unrecognized"):
            self.installer.install()

    def test_explicit_32bit_boot_takes_precedence_over_running_64bit(self):
        self.architecture = "aarch64"
        self.make(splash.BOOT_CONFIG, self.original_config + b"arm_64bit=0\n")
        self.assertEqual(self.installer.boot_plan()["image"], "boot/firmware/initramfs7")


class ManualWrapperTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gamebird-splash-wrapper-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.commands = self.root / "commands"
        self.boot, self.lower = self.root / "boot", self.root / "lower"
        (self.lower / "boot/firmware").mkdir(parents=True)
        (self.lower / "dev").mkdir()
        self.boot.mkdir()
        build = self.root / "build"
        build.mkdir()
        (build / "gbsplash").write_text("fixture")
        (build / "gbsplash").chmod(0o755)
        script = (PROJECT / "scripts/install-boot-splash.sh").read_text()
        script = script.replace('project_dir=$(CDPATH=\'\' cd -- "$(dirname -- "$0")/.." && pwd)',
                                "project_dir=" + shlex.quote(str(PROJECT)))
        script = script.replace("boot_dir=/boot/firmware", "boot_dir=" + shlex.quote(str(self.boot)))
        script = script.replace("lower_root=/media/root-ro", "lower_root=" + shlex.quote(str(self.lower)))
        script = script.replace("/run/lock/gamebird-updater.lock", str(self.root / "lock"))
        script = script.replace("/run/gamebird-boot-splash.XXXXXX", str(self.root / "stage.XXXXXX"))
        self.script = self.root / "installer.sh"
        self.script.write_text(script)
        self.build = build
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", INNER_RESULT="0")
        self.stub("id", "echo 0")
        self.stub("mountpoint", "exit 1")
        for name in ("mount", "umount", "sync"):
            self.stub(name, f'echo {name} "$*" >> {shlex.quote(str(self.commands))}')
        self.stub("findmnt", "\n".join([
            'if [[ "$*" == *"--json"* ]]; then',
            "cat <<'JSON'",
            json.dumps({"filesystems": [{"target": str(self.lower), "source": "/dev/testroot", "options": "rw",
                        "children": [{"target": str(self.lower / "data"), "source": "/dev/testdata", "options": "rw"}]}]}),
            "JSON",
            'elif [[ "$*" == *"FSTYPE"* ]]; then echo overlay',
            f'elif [[ "${{@: -1}}" == {shlex.quote(str(self.lower))} ]]; then echo rw',
            "else echo ro; fi",
        ]))
        self.stub("python3", f'if [[ "$1" == -c ]]; then exec {shlex.quote(sys.executable)} "$@"; fi\nexit "$INNER_RESULT"')
        # Emulate the actual bug: execute the inner command but report successful
        # cleanup even if the command failed. The result file must catch this.
        self.stub("overlayroot-chroot", '"$@"\nexit 0')

    def stub(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/bash\n" + body + "\n")
        path.chmod(0o755)

    def run_wrapper(self):
        return subprocess.run(["bash", str(self.script), "--build-dir", str(self.build)],
                              env=self.env, capture_output=True, text=True, timeout=10)

    def test_manual_wrapper_restores_existing_rw_lower_and_data_and_ro_boot(self):
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        log = self.commands.read_text()
        self.assertIn(f"mount -o remount,rw {self.boot}", log)
        self.assertIn(f"mount -o remount,ro {self.boot}", log)
        self.assertIn(f"mount -o remount,rw {self.lower / 'data'}", log)
        self.assertIn(f"mount -o remount,rw {self.lower}", log)
        self.assertFalse(list(self.root.glob("stage.*")))

    def test_manual_wrapper_detects_masked_chroot_failure(self):
        self.env["INNER_RESULT"] = "47"
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assertIn(f"mount -o remount,ro {self.boot}", self.commands.read_text())


if __name__ == "__main__":
    unittest.main()
