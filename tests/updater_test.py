"""Exercise updater failure paths with isolated state and fake system commands."""
import os
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(sys.argv.pop(1)).resolve()


class UpdaterTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gamebird-updater-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.state.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.commands = self.root / "commands"
        self.shell = self.root / "protected/home/gamebird/gamebird-shell"
        script = SOURCE.read_text().replace(
            "state_dir=/data/gamebird-update", f"state_dir='{self.state}'"
        ).replace("lower_root=/media/root-ro", f"lower_root='{self.root / 'lower'}'")
        script = script.replace("/run/lock/gamebird-updater.lock", str(self.root / "lock"))
        script = script.replace("/usr/sbin/overlayroot-chroot", "overlayroot-chroot")
        # Even a regression that continues past a failed build must stay inside
        # the fixture, including when CI runs these tests as root.
        for path in ("/home/gamebird/gamebird-shell", "/data/gamebird-shell/config",
                     "/usr/local/lib/gamebird", "/etc/sudoers.d", "/etc/systemd/system",
                     "/usr/sbin/policy-rc.d"):
            script = script.replace(path, str(self.root / "protected") + path)
        self.script = self.root / "updater.sh"
        self.script.write_text(script)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}")
        self.mock("id", "echo 0")
        self.mock("apt-get", "exit 0")
        self.mock("curl", 'printf \'{"sha":"%s"}\\n\' ' + "a" * 40)
        self.mock("mountpoint", "exit 1")
        for command in ("mount", "umount", "sync", "sleep", "overlayroot-chroot"):
            self.mock(command, "exit 0")
        self.mock("systemctl", f'echo "$*" >> "{self.commands}"')
        self.mock("chown", "exit 0")

    def mock(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/bash\n" + body + "\n")
        path.chmod(0o755)

    def run_updater(self, *args):
        return subprocess.run(["bash", str(self.script), *args], env=self.env,
                              capture_output=True, text=True, timeout=10)

    def status(self):
        return dict(line.split("=", 1) for line in (self.state / "status").read_text().splitlines())

    def prepare_install(self):
        (self.state / "status").write_text("os_updates=1\nshell_update=0\n")

    def payload(self, directory, commit="a" * 40):
        directory.mkdir(parents=True, exist_ok=True)
        hashes = []
        for name in ("gbshell", "gblibd", "gblaunch", "gbhud"):
            contents = f"#!/bin/sh\necho 'GameBird Shell {commit}'\n".encode()
            (directory / name).write_bytes(contents)
            (directory / name).chmod(0o755)
            hashes.append(f"{hashlib.sha256(contents).hexdigest()}  {name}\n")
        identity = (commit + "\n").encode()
        (directory / "shell-commit").write_bytes(identity)
        hashes.append(f"{hashlib.sha256(identity).hexdigest()}  shell-commit\n")
        (directory / "shell-payload.sha256").write_text("".join(hashes))

    def pending(self, commit="a" * 40, boot="previous-boot"):
        (self.state / "pending-shell-install").write_text(f"commit={commit}\nboot_id={boot}\n")

    def prepare_shell_install(self):
        (self.state / "status").write_text("os_updates=0\nshell_update=1\nremote_commit=" + "a" * 40 + "\n")
        # No real download/mount/chroot in a pipeline test.
        self.mock("tar", f'mkdir -p "{self.state}/work/source/scripts"; '
                  f'touch "{self.state}/work/source/CMakeLists.txt" '
                  f'"{self.state}/work/source/scripts/gamebird-updater.sh"')
        self.mock("overlayroot-chroot", f'echo "$*" >> "{self.commands}"')

    def test_new_commit_is_available(self):
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["shell_update"], "1")
        self.assertEqual(self.status()["remote_commit"], "a" * 40)
        self.assertTrue(self.status()["boot_id"])
        self.assertTrue(self.status()["operation_id"])

    def test_matching_verified_payload_is_current(self):
        self.payload(self.shell / "build")
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "CURRENT")
        self.assertEqual(self.status()["installed_commit"], "a" * 40)

    def test_checkout_marker_does_not_hide_missing_build(self):
        (self.state / "installed-shell-commit").write_text("a" * 40)
        self.assertEqual(self.run_updater("check").returncode, 0)
        self.assertEqual(self.status()["shell_update"], "1")
        self.assertEqual(self.status()["installed_commit"], "")

    def test_replaced_binary_invalidates_commit(self):
        self.payload(self.shell / "build")
        (self.shell / "build/gblaunch").write_text("older executable")
        self.assertEqual(self.run_updater("check").returncode, 0)
        self.assertEqual(self.status()["shell_update"], "1")
        self.assertEqual(self.status()["installed_commit"], "")

    def test_interrupted_receipt_cannot_mislabel_binaries(self):
        self.payload(self.shell / "build", "b" * 40)
        (self.shell / "build/shell-commit").write_text("a" * 40 + "\n")
        self.assertEqual(self.run_updater("check").returncode, 0)
        self.assertEqual(self.status()["installed_commit"], "")
        self.assertEqual(self.status()["shell_update"], "1")

    def test_lost_update_is_reported_after_restart(self):
        self.payload(self.shell / "build", "b" * 40)
        self.pending()
        self.mock("apt-get", "exit 99")
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertIn("did not survive restart", self.status()["message"])
        self.assertTrue((self.state / "pending-shell-install").exists())

    def test_restart_retry_uses_protected_payload_without_rebuilding(self):
        self.payload(self.shell / "build", "b" * 40)  # Still running through the old overlay.
        self.payload(Path(str(self.root / "lower") + str(self.shell)) / "build")
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        self.pending(boot=boot)
        self.mock("apt-get", "exit 99")
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "RESTART REQUIRED")
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("reboot", self.commands.read_text())

    def test_busy_lock_is_not_a_success_or_a_replacement_status(self):
        self.prepare_install()
        before = (self.state / "status").read_text()
        self.mock("flock", "exit 1")
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 75, result.stderr)
        self.assertEqual((self.state / "status").read_text(), before)
        self.assertFalse(self.commands.exists())

    def test_success_is_confirmed_after_restart(self):
        self.payload(self.shell / "build")
        self.pending()
        self.assertEqual(self.run_updater("check").returncode, 0)
        self.assertEqual(self.status()["phase"], "CURRENT")
        self.assertFalse((self.state / "pending-shell-install").exists())

    def test_failed_protected_verification_does_not_reboot(self):
        self.prepare_shell_install()
        result = self.run_updater("install")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertNotIn("reboot", self.commands.read_text())
        self.assertFalse((self.state / "installed-shell-commit").exists())

    def test_verified_shell_install_records_expected_restart(self):
        self.prepare_shell_install()
        self.payload(Path(str(self.root / "lower") + str(self.shell)) / "build")
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "REBOOTING")
        self.assertEqual((self.state / "installed-shell-commit").read_text().strip(), "a" * 40)
        self.assertIn("commit=" + "a" * 40, (self.state / "pending-shell-install").read_text())
        self.assertIn("work/source/scripts/gamebird-updater.sh inner", self.commands.read_text())
        self.assertIn("reboot", self.commands.read_text())

    def test_inner_install_stamps_only_completed_payload(self):
        source = self.root / "source"
        for relative in ("config/defaults.json", "scripts/gamebird-updater.sh", "scripts/gbshell-start.sh",
                         "packaging/sudoers/gamebird-update", "packaging/systemd/gamebird-update.service"):
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        self.payload(self.state / "work/build")
        # Configuring removes the build tree first; mock CMake regenerates it.
        seed = self.root / "build-seed"
        self.payload(seed)
        self.mock("cmake", f'mkdir -p "{self.state}/work/build"; cp "{seed}/"* "{self.state}/work/build/"')
        result = self.run_updater("inner", "0", "1", "a" * 40, str(source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.shell / "build/shell-commit").read_text().strip(), "a" * 40)
        self.assertTrue((self.shell / "build/shell-payload.sha256").exists())
        self.assertFalse((self.state / "installed-shell-commit").exists())
        # A late install error must not publish a new payload identity.
        (self.shell / "build/shell-commit").unlink()
        self.mock("systemctl", "exit 46")
        result = self.run_updater("inner", "0", "1", "a" * 40, str(source))
        self.assertEqual(result.returncode, 46, result.stderr)
        self.assertFalse((self.shell / "build/shell-commit").exists())

    def test_invalid_channel_does_not_claim_current(self):
        self.mock("curl", "echo '{}' ")
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertIn("unavailable", self.status()["message"])

    def test_os_updates_remain_available_when_channel_fails(self):
        self.mock("apt-get", 'if [[ " $* " == *" -s "* ]]; then echo "Inst package"; fi; exit 0')
        self.mock("curl", "exit 22")
        result = self.run_updater("check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "UPDATE AVAILABLE")
        self.assertEqual(self.status()["os_updates"], "1")

    def test_failed_build_stops_inner_install(self):
        self.mock("cmake", "exit 42")
        result = self.run_updater("inner", "0", "1", "a" * 40, str(self.root / "source"))
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertFalse((self.state / "installed-shell-commit").exists())
        self.assertFalse(self.commands.exists())

    def test_missing_game_runtime_stops_before_build_and_receipt(self):
        self.mock("apt-get", '[[ " $* " == *" python3-pygame "* ]] && exit 49; exit 0')
        self.mock("cmake", f'echo build >> "{self.commands}"')
        result = self.run_updater("inner", "0", "1", "a" * 40, str(self.root / "source"))
        self.assertEqual(result.returncode, 49, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertFalse((self.shell / "build/shell-commit").exists())
        self.assertFalse(self.commands.exists())

    def test_failed_chroot_does_not_reboot(self):
        self.prepare_install()
        self.mock("overlayroot-chroot", "exit 43")
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 43, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertFalse(self.commands.exists())

    def test_failed_mount_does_not_continue(self):
        self.prepare_install()
        self.mock("mount", '[[ "$*" == "-o remount,rw /boot/firmware" ]] && exit 44; exit 0')
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 44, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertFalse(self.commands.exists())

    def test_protected_install_uses_live_dns(self):
        self.prepare_install()
        resolver = self.root / "lower/etc/resolv.conf"
        resolver.parent.mkdir(parents=True)
        resolver.write_text("nameserver 127.0.0.53\n")
        self.mock("mount", f'echo "mount $*" >> "{self.commands}"')
        self.mock("overlayroot-chroot", f'echo "chroot $*" >> "{self.commands}"')
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.commands.read_text()
        self.assertIn(f"mount --bind /etc/resolv.conf {resolver}", calls)
        self.assertLess(calls.index("mount --bind /etc/resolv.conf"), calls.index("chroot "))
        self.assertEqual(resolver.read_text(), "nameserver 127.0.0.53\n")

    def test_failed_dns_bind_stops_before_chroot_or_reboot(self):
        self.prepare_install()
        resolver = self.root / "lower/etc/resolv.conf"
        resolver.parent.mkdir(parents=True)
        resolver.write_text("nameserver 127.0.0.53\n")
        self.mock("mount", '[[ "$*" == *"--bind /etc/resolv.conf"* ]] && exit 47; exit 0')
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 47, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")
        self.assertFalse(self.commands.exists())

    def test_failed_reboot_is_reported(self):
        self.prepare_install()
        self.mock("systemctl", '[[ "$1" == reboot ]] && exit 45; exit 0')
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 45, result.stderr)
        self.assertEqual(self.status()["phase"], "ERROR")

    def test_inner_enables_timer_on_protected_root(self):
        result = self.run_updater("inner", "0", "0", "", "")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--root=/ enable gamebird-update-check.timer", self.commands.read_text())

    def test_success_requests_reboot(self):
        self.prepare_install()
        result = self.run_updater("install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.status()["phase"], "REBOOTING")
        self.assertIn("reboot", self.commands.read_text())


if __name__ == "__main__":
    unittest.main()
