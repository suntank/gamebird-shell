#!/usr/bin/env python3
"""Transactional initramfs installation; called by install-boot-splash.sh.

Only installation needs Python. The initramfs contains the libc-only renderer,
its image, the existing panel firmware, and the existing kernel drivers.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import shutil
import subprocess
import sys
import tempfile


BINARY = "usr/local/lib/gamebird/gbsplash"
IMAGE = "usr/share/gamebird/boot/splash.rgb565"
CONF = "etc/gamebird/boot-splash.conf"
HOOK = "etc/initramfs-tools/hooks/gamebird-splash"
RUNTIME = "etc/initramfs-tools/scripts/init-top/00-gamebird-splash"
DROPIN = "etc/systemd/system/gbshell.service.d/10-boot-splash.conf"
LAUNCHER = "usr/local/lib/gamebird/gbshell-start.sh"
TV_MANAGER = "usr/local/lib/gamebird/tv-mode-manager.sh"
RECEIPT = "etc/gamebird/boot-splash.sha256"
SHELL_SERVICE = "etc/systemd/system/gbshell.service"
LIBRARY_SERVICE = "etc/systemd/system/gblibd.service"
LEGACY_OWNED_FILES = (BINARY, IMAGE, CONF, HOOK, RUNTIME, DROPIN, LAUNCHER, TV_MANAGER, RECEIPT)
OWNED_FILES = (*LEGACY_OWNED_FILES, SHELL_SERVICE, LIBRARY_SERVICE)
STATE_DIR = "var/lib/gamebird/boot-splash"
BOOT_CONFIG = "boot/firmware/config.txt"
BOOT_BLOCK = ("\n# BEGIN GAMEBIRD BOOT SPLASH\n[all]\nauto_initramfs=1\n"
              "# END GAMEBIRD BOOT SPLASH\n")
MODULE_RE = re.compile(r"[A-Za-z0-9_]+\Z")
FIRMWARE_RE = re.compile(r"[A-Za-z0-9_.,+/-]+\Z")
KERNEL_IMAGES = {"kernel.img": "initramfs", "kernel7.img": "initramfs7",
                 "kernel7l.img": "initramfs7l", "kernel8.img": "initramfs8",
                 "kernel_2712.img": "initramfs_2712"}


class InstallError(RuntimeError):
    pass


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def safe_firmware(name):
    path = PurePosixPath(name)
    if (not FIRMWARE_RE.fullmatch(name) or path.is_absolute()
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise InstallError("The panel has an unsafe firmware filename.")
    return name


def atomic_write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Installer:
    def __init__(self, root=Path("/"), source=None, build=None, runner=None):
        self.root = Path(root)
        self.source = Path(source) if source else None
        self.build = Path(build) if build else None
        self.runner = runner or subprocess.run

    def path(self, relative):
        return self.root / relative.lstrip("/")

    def run(self, *args):
        result = self.runner([str(arg) for arg in args], check=False,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            detail = result.stderr.strip() or result.stdout.strip()
            raise InstallError(f"{args[0]} failed ({result.returncode}): {detail}")
        return result.stdout.strip()

    def state(self):
        path = self.path(STATE_DIR + "/state.json")
        if not path.exists():
            return None
        try:
            data = json.loads(path.read_text())
        except (OSError, ValueError) as error:
            raise InstallError(f"Cannot read existing splash installation state: {error}") from error
        if not isinstance(data, dict) or data.get("format") != 1:
            raise InstallError("Unrecognized splash installation state; refusing to overwrite it.")
        panel, originals = data.get("panel"), data.get("originals")
        if (not isinstance(data.get("owns_boot_block"), bool)
                or not isinstance(originals, dict)
                or not set(LEGACY_OWNED_FILES).issubset(originals)
                or not set(originals).issubset(OWNED_FILES)
                or any(not isinstance(value, bool) for value in originals.values())
                or not isinstance(panel, dict) or not isinstance(panel.get("modules"), list)
                or not panel["modules"] or not isinstance(panel.get("firmware"), str)
                or any(not isinstance(name, str) or not MODULE_RE.fullmatch(name) for name in panel["modules"])):
            raise InstallError("Incomplete or invalid splash installation state.")
        safe_firmware(panel["firmware"])
        return data

    def panel_config(self):
        panel = self.path("sys/bus/spi/devices/spi0.0")
        driver = panel / "driver"
        if not driver.exists() or driver.resolve().name != "panel-mipi-dbi-spi":
            raise InstallError("spi0.0 is not bound to panel-mipi-dbi-spi; inspect the actual panel driver first.")
        try:
            compatible = (panel / "of_node/compatible").read_bytes().split(b"\0", 1)[0].decode("ascii")
        except (OSError, UnicodeError) as error:
            raise InstallError(f"Cannot identify the panel's Device Tree compatible string: {error}") from error
        firmware = safe_firmware(compatible + ".bin")
        for suffix in ("", ".xz", ".zst"):
            if self.path("lib/firmware/" + firmware + suffix).is_file():
                firmware += suffix
                break
        else:
            raise InstallError(f"Panel firmware /lib/firmware/{firmware} was not found.")

        modules, visited = [], set()

        def add(module):
            module = module.replace("-", "_")
            if not MODULE_RE.fullmatch(module):
                raise InstallError("Unsafe module name in panel hardware description.")
            if module not in modules:
                modules.append(module)

        def suppliers(device):
            device = device.resolve()
            if device in visited:
                return
            visited.add(device)
            for link in sorted(device.glob("supplier:*")):
                # Kernel devlink directories expose the actual supplying device.
                supplier = link / "supplier"
                if supplier.exists():
                    suppliers(supplier)
            module = device / "driver/module"
            if module.exists():
                add(module.resolve().name)

        # GPIO backlights are separate platform devices, not a module dependency
        # of panel-mipi-dbi. Record only drivers observed on this working unit.
        backlights = self.path("sys/class/backlight")
        if backlights.exists():
            for backlight in sorted(backlights.iterdir()):
                suppliers(backlight / "device")
        controller = self.path("sys/class/spi_master/spi0/device")
        if not (controller / "driver").exists():
            raise InstallError("Cannot identify the SPI controller for spi0.0.")
        suppliers(controller)
        if (controller / "driver").resolve().name != "spi-bcm2835":
            raise InstallError("This installer supports the observed GamePi13 spi-bcm2835 controller only.")
        add("spi_bcm2835")
        suppliers(panel)
        add("panel_mipi_dbi")
        config = ("# Generated from the installed GameBird panel's sysfs identity.\n"
                  f"GBSPLASH_MODULES='{' '.join(modules)}'\n"
                  f"GBSPLASH_FIRMWARE_FILE='{firmware}'\n")
        return {"modules": modules, "firmware": firmware, "config": config}

    def boot_plan(self, state=None):
        path = self.path(BOOT_CONFIG)
        if not path.is_file():
            raise InstallError("/boot/firmware/config.txt was not found.")
        try:
            original = path.read_bytes()
            text = original.decode("utf-8")
        except (OSError, UnicodeError) as error:
            raise InstallError(f"Cannot read Raspberry Pi boot configuration: {error}") from error
        owned = bool(state and state.get("owns_boot_block"))
        if owned:
            if text.count(BOOT_BLOCK) != 1:
                raise InstallError("The owned splash boot block was edited; restore it before installing or uninstalling.")
        elif "# BEGIN GAMEBIRD BOOT SPLASH" in text:
            raise InstallError("An untracked GameBird boot block exists; inspect it before installing.")

        values, explicit, seen = {}, None, set()

        def parse(config_path, section="all"):
            nonlocal explicit
            resolved = config_path.resolve()
            if resolved in seen:
                raise InstallError("Recursive boot configuration include.")
            seen.add(resolved)
            if not resolved.is_relative_to(self.path("boot/firmware").resolve()):
                raise InstallError("Boot configuration include leaves the boot partition.")
            for raw in config_path.read_text().splitlines():
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1].lower()
                    continue
                if section == "none":
                    continue
                if line.startswith("include "):
                    name = line.split(None, 1)[1]
                    if not re.fullmatch(r"[A-Za-z0-9_.+/-]+", name) or ".." in Path(name).parts:
                        raise InstallError("Unsupported boot configuration include.")
                    section = parse(config_path.parent / name, section)
                    continue
                key = line.split("=", 1)[0].strip()
                relevant = key in {"auto_initramfs", "kernel", "os_prefix", "arm_64bit", "ramfsfile", "ramfsaddr"}
                if line.startswith("initramfs "):
                    relevant, key = True, "initramfs"
                if not relevant:
                    continue
                if section != "all":
                    raise InstallError(f"Conditional boot setting '{line}' needs device-specific review; it was not changed.")
                if key == "initramfs":
                    fields = line.split()
                    if len(fields) != 3 or fields[2] not in ("followkernel", "0", "0x0"):
                        raise InstallError("Custom initramfs address/list requires manual review; it was not changed.")
                    explicit = fields[1]
                elif "=" in line:
                    values[key] = line.split("=", 1)[1].strip()
            seen.remove(resolved)
            return section

        parse(path)
        if values.get("os_prefix") or "ramfsfile" in values or "ramfsaddr" in values:
            raise InstallError("Custom boot prefix/ramfs settings require manual review; they were not changed.")
        if "arm_64bit" in values and values["arm_64bit"] not in ("0", "1"):
            raise InstallError("Unsupported arm_64bit value.")
        kernel = values.get("kernel")
        if not kernel:
            architecture = self.run("uname", "-m")
            if values.get("arm_64bit") == "1":
                kernel = "kernel8.img"
            elif values.get("arm_64bit") == "0":
                kernel = "kernel7.img"
            elif architecture == "aarch64":
                kernel = "kernel8.img"
            elif architecture in ("armv7l", "armv8l"):
                kernel = "kernel7.img"
            else:
                raise InstallError("Cannot choose the Pi Zero 2 boot image from this architecture.")
        if kernel not in KERNEL_IMAGES:
            raise InstallError(f"Custom kernel '{kernel}' requires manual review; it was not changed.")
        if not self.path("boot/firmware/" + kernel).is_file():
            raise InstallError(f"The selected boot kernel {kernel} is missing.")
        image = KERNEL_IMAGES[kernel]
        if explicit and explicit != image:
            raise InstallError(f"Custom initramfs '{explicit}' is not the standard image for {kernel}; it was not changed.")
        if values.get("auto_initramfs", "0") not in ("0", "1"):
            raise InstallError("Unsupported auto_initramfs value.")
        append = not explicit and values.get("auto_initramfs", "0") != "1"
        return {"image": "boot/firmware/" + image, "append": append,
                "original": original, "owns_boot_block": owned or append}

    def boot_images(self):
        files = set(self.path("boot").glob("initrd.img-*"))
        for name in KERNEL_IMAGES.values():
            path = self.path("boot/firmware/" + name)
            if path.exists() or path.is_symlink():
                files.add(path)
        return sorted(path for path in files if path.is_file() or path.is_symlink())

    def validate_archive(self, archive, config, require_overlay=False, require_payload=True):
        entries = set(self.run("lsinitramfs", archive).splitlines())
        normalized = {entry.removeprefix("./") for entry in entries}

        def included(relative):
            return relative in normalized or "usr/" + relative in normalized

        if require_payload:
            for path in (BINARY, IMAGE, CONF, "usr/bin/timeout", "scripts/init-top/00-gamebird-splash"):
                if not included(path):
                    raise InstallError(f"{archive.name} is missing {path}.")
            if not included("lib/firmware/" + config["firmware"]):
                raise InstallError(f"{archive.name} is missing the panel firmware.")
            kernels = {match.group(1) for entry in normalized
                       if (match := re.match(r"(?:usr/)?lib/modules/([^/]+)/", entry))}
            if len(kernels) != 1:
                raise InstallError(f"Cannot identify one kernel in {archive.name}.")
            version = next(iter(kernels))
            for module in config["modules"]:
                filename = self.run("modinfo", "-k", version, "-F", "filename", module)
                if filename == "(builtin)":
                    continue
                basename = Path(filename).name.replace("-", "_")
                if not any(Path(entry).name.replace("-", "_") == basename for entry in normalized):
                    raise InstallError(f"{archive.name} is missing module {module} for {version}.")
        if require_overlay:
            if not included("scripts/init-bottom/overlayroot"):
                raise InstallError(f"{archive.name} lost overlayroot support.")
            if not any(re.search(r"/overlay\.ko(?:\.(?:xz|zst|gz))?$", entry) for entry in normalized):
                # A custom kernel can have OverlayFS built in.
                kernels = {match.group(1) for entry in normalized
                           if (match := re.match(r"(?:usr/)?lib/modules/([^/]+)/", entry))}
                if len(kernels) != 1 or self.run("modinfo", "-k", next(iter(kernels)), "-F", "filename", "overlay") != "(builtin)":
                    raise InstallError(f"{archive.name} lost its OverlayFS driver.")

    def archive_validation(self, plan, config, overlay, require_payload=True):
        initrds = sorted(self.path("boot").glob("initrd.img-*"))
        initrds = [path for path in initrds if path.is_file() and not path.name.endswith((".bak", ".new"))]
        if not initrds:
            raise InstallError("No generated initramfs images were found in /boot.")
        verified = set()
        for image in initrds:
            self.validate_archive(image, config, overlay, require_payload)
            archive_digest = digest(image)
            if require_payload and archive_digest not in verified:
                self.validate_archive_bytes(image, config)
            verified.add(archive_digest)
        active = self.path(plan["image"])
        if not active.is_file():
            raise InstallError(f"The boot partition is missing {active.name}.")
        self.validate_archive(active, config, overlay, require_payload)
        if digest(active) not in verified:
            raise InstallError("The firmware initramfs does not match a rebuilt /boot image.")

    def validate_archive_bytes(self, archive, config):
        # Names alone cannot prove that an old, same-named splash was replaced.
        # Extract onto the persistent root, not the handheld's small RAM overlay.
        expected = {BINARY: BINARY, IMAGE: IMAGE, CONF: CONF,
                    "scripts/init-top/00-gamebird-splash": RUNTIME,
                    "lib/firmware/" + config["firmware"]: "lib/firmware/" + config["firmware"]}
        parent = self.path("var/lib/gamebird")
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".splash-verify-", dir=parent) as name:
            temporary = Path(name)
            self.run("unmkinitramfs", archive, temporary)
            for relative, installed in expected.items():
                candidates = []
                for prefix in (temporary / "main", temporary):
                    candidates += [prefix / relative, prefix / "usr" / relative]
                candidate = next((path for path in candidates
                                  if path.is_file() and path.resolve().is_relative_to(temporary.resolve())), None)
                if candidate is None or digest(candidate) != digest(self.path(installed)):
                    raise InstallError(f"{archive.name} contains missing or stale bytes for {relative}.")

    def overlay_required(self):
        cmdline = self.path("boot/firmware/cmdline.txt")
        return cmdline.is_file() and re.search(r"(?:^|\s)overlayroot=\S+", cmdline.read_text()) is not None

    def rebuild(self):
        if not list(self.path("boot").glob("initrd.img-*")):
            version = self.run("uname", "-r")
            if not self.path("lib/modules/" + version).is_dir():
                raise InstallError("The running kernel's modules are missing; cannot create its initramfs.")
            self.run("update-initramfs", "-c", "-k", version)
        else:
            self.run("update-initramfs", "-u", "-k", "all")

    def transaction(self, operation):
        # Back up each image that update-initramfs can replace before invoking it.
        # A failed hook, full boot partition, or failed verification restores the
        # previous bootable images and exactly the previous owned configuration.
        parent = self.path("var/lib/gamebird")
        parent.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix=".splash-transaction-", dir=parent))
        original_images = self.boot_images()
        targets = [self.path(path) for path in (*OWNED_FILES, BOOT_CONFIG, "boot/firmware/cmdline.txt")] + original_images
        targets.append(self.path(STATE_DIR))
        existing = set()
        keep_backup = False
        try:
            for index, target in enumerate(targets):
                if not target.exists() and not target.is_symlink():
                    continue
                existing.add(index)
                backup = directory / str(index)
                if target.is_symlink():
                    backup.symlink_to(os.readlink(target))
                elif target.is_dir():
                    shutil.copytree(target, backup, symlinks=True)
                else:
                    shutil.copy2(target, backup)
            atomic_write(directory / "recovery.json", json.dumps({
                "targets": [str(path) for path in targets],
                "existed": sorted(existing),
            }, indent=2).encode(), 0o600)
            try:
                operation()
                os.sync()
            except BaseException:
                # Also remove aliases created for the first time by a failed run.
                try:
                    for path in set(self.boot_images()) - set(original_images):
                        path.unlink()
                    for index, target in enumerate(targets):
                        if target.is_symlink() or target.is_file():
                            target.unlink()
                        elif target.exists():
                            shutil.rmtree(target)
                        if index not in existing:
                            continue
                        backup = directory / str(index)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        if backup.is_symlink():
                            target.symlink_to(os.readlink(backup))
                        elif backup.is_dir():
                            shutil.copytree(backup, target, symlinks=True)
                        else:
                            shutil.copy2(backup, target)
                    os.sync()
                except BaseException as error:
                    keep_backup = True
                    raise InstallError(f"Rollback failed; recovery files remain in {directory}: {error}") from error
                raise
        finally:
            if not keep_backup:
                shutil.rmtree(directory)

    def verify_receipt(self):
        receipt = self.path(RECEIPT)
        if not receipt.is_file():
            raise InstallError("The splash installation receipt is missing.")
        expected_paths = set(OWNED_FILES) - {RECEIPT}
        found = set()
        for line in receipt.read_text().splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
            if not match or match.group(2) not in expected_paths:
                raise InstallError("Invalid splash installation receipt.")
            path = match.group(2)
            if path in found or not self.path(path).is_file() or digest(self.path(path)) != match.group(1):
                raise InstallError(f"Installed splash file failed verification: {path}.")
            found.add(path)
        if found != expected_paths:
            raise InstallError("Incomplete splash installation receipt.")

    def verify(self):
        state = self.state()
        if not state:
            raise InstallError("The boot splash is not installed.")
        self.verify_receipt()
        plan = self.boot_plan(state)
        if plan["append"]:
            raise InstallError("The installed boot splash is not enabled in the boot configuration.")
        self.archive_validation(plan, state["panel"], self.overlay_required())

    def install(self):
        state = self.state()
        plan = self.boot_plan(state)
        panel = self.panel_config()
        binary, image = self.build / "gbsplash", self.source / "assets/boot/splash.rgb565"
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise InstallError(f"Build the gbsplash target first: {binary}")
        self.run(binary, "--check-image", image)
        for relative in ("packaging/initramfs/gamebird-splash-hook", "packaging/initramfs/gamebird-splash-top",
                         "packaging/systemd/gbshell.service.d/10-boot-splash.conf",
                         "packaging/systemd/gbshell.service", "packaging/systemd/gblibd.service",
                         "scripts/gbshell-start.sh", "scripts/tv-mode-manager.sh"):
            if not (self.source / relative).is_file():
                raise InstallError(f"Installer source is missing {relative}.")
        overlay = self.overlay_required()
        before_cmdline = self.path("boot/firmware/cmdline.txt").read_bytes()

        def operation():
            nonlocal state
            if state is None:
                state = {"format": 1, "originals": {}}
            # Capture newly owned paths on upgrades without replacing the first
            # installation's originals. Manual installs need the same offline
            # startup units as the full system updater.
            for relative in OWNED_FILES:
                if relative in state["originals"]:
                    continue
                target = self.path(relative)
                present = target.exists() or target.is_symlink()
                state["originals"][relative] = present
                if present:
                    saved = self.path(STATE_DIR + "/original/" + relative)
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(target, saved, follow_symlinks=False)
            state.update(owns_boot_block=plan["owns_boot_block"], panel={
                "modules": panel["modules"], "firmware": panel["firmware"]})
            for source, target, mode in (
                (binary, BINARY, 0o755), (image, IMAGE, 0o644),
                (self.source / "packaging/initramfs/gamebird-splash-hook", HOOK, 0o755),
                (self.source / "packaging/initramfs/gamebird-splash-top", RUNTIME, 0o755),
                (self.source / "packaging/systemd/gbshell.service.d/10-boot-splash.conf", DROPIN, 0o644),
                (self.source / "scripts/gbshell-start.sh", LAUNCHER, 0o755),
                (self.source / "scripts/tv-mode-manager.sh", TV_MANAGER, 0o755),
                (self.source / "packaging/systemd/gbshell.service", SHELL_SERVICE, 0o644),
                (self.source / "packaging/systemd/gblibd.service", LIBRARY_SERVICE, 0o644),
            ):
                atomic_write(self.path(target), source.read_bytes(), mode)
            atomic_write(self.path(CONF), panel["config"].encode())
            self.rebuild()
            self.archive_validation(plan, panel, overlay)
            if self.path("boot/firmware/cmdline.txt").read_bytes() != before_cmdline:
                raise InstallError("An initramfs hook unexpectedly changed cmdline.txt.")
            # Enable only after the exact firmware image was rebuilt and checked.
            if plan["append"]:
                atomic_write(self.path(BOOT_CONFIG), plan["original"] + BOOT_BLOCK.encode())
            receipt = "".join(f"{digest(self.path(relative))}  {relative}\n"
                              for relative in OWNED_FILES if relative != RECEIPT)
            atomic_write(self.path(RECEIPT), receipt.encode())
            atomic_write(self.path(STATE_DIR + "/state.json"), (json.dumps(state, indent=2) + "\n").encode(), 0o600)
            self.verify_receipt()

        self.transaction(operation)

    def uninstall(self):
        state = self.state()
        if not state:
            raise InstallError("No tracked boot splash installation exists.")
        plan = self.boot_plan(state)
        overlay = self.overlay_required()

        def operation():
            for relative in state["originals"]:
                target = self.path(relative)
                if target.exists() or target.is_symlink():
                    target.unlink()
                if state["originals"].get(relative):
                    original = self.path(STATE_DIR + "/original/" + relative)
                    shutil.copy2(original, target, follow_symlinks=False)
            self.rebuild()
            self.archive_validation(plan, state["panel"], overlay, require_payload=False)
            if state["owns_boot_block"]:
                updated = plan["original"].decode().replace(BOOT_BLOCK, "", 1)
                atomic_write(self.path(BOOT_CONFIG), updated.encode())
            shutil.rmtree(self.path(STATE_DIR))

        self.transaction(operation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "uninstall", "verify"))
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("run through sudo scripts/install-boot-splash.sh")
    def interrupted(signum, frame):
        raise InstallError(f"Installation interrupted by signal {signum}.")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    installer = Installer(source=args.source, build=args.build_dir)
    try:
        if args.action != "verify":
            if installer.run("findmnt", "-n", "-o", "FSTYPE", "--target", "/") == "overlay":
                raise InstallError("--inner must run inside the protected root; use the outer installer on OverlayFS.")
            options = installer.run("findmnt", "-n", "-o", "OPTIONS", "--mountpoint", "/boot/firmware")
            if "rw" not in options.split(","):
                raise InstallError("The inner installer requires the mounted boot partition to be writable.")
        getattr(installer, args.action)()
    except (InstallError, OSError, ValueError) as error:
        print(f"GameBird boot splash: {error}", file=sys.stderr)
        return 1
    print({"install": "GameBird boot splash installed and initramfs verified. It will appear on the next boot.",
           "uninstall": "GameBird boot splash removed; prior owned boot settings restored.",
           "verify": "GameBird boot splash files and firmware initramfs verified."}[args.action])
    return 0


if __name__ == "__main__":
    sys.exit(main())
