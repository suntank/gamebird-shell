# Game Bird early boot splash

The boot splash uses the supplied `assets/ui/splash.png`, with a preconverted
240 × 240 RGB565 image in `assets/boot/splash.rgb565`. The original PNG is kept
unchanged. The display program is independent of GameBird Shell and needs only
libc, the panel driver, and the image.

## Boot sequence

The initramfs runs `scripts/init-top/00-gamebird-splash` after `/dev`, `/sys`,
and `/proc` are mounted, before udev's general coldplug and before mounting the
real root. The script loads the existing SPI controller, panel, and observed
supplier modules, then invokes `gbsplash` with a bounded discovery deadline.
The normal path waits only for the hardware to become usable. It has no fixed
logo duration and does not wait for networking, the catalog, or the shell.

The renderer discovers the 240 × 240 framebuffer by its `spi0.0` sysfs identity,
including the identity of the opened device. It does not assume that the panel
is `/dev/fb1`, and it does not need the later `/dev/gamebirdfb` udev alias. It
validates the framebuffer's format, channel bitfields, stride, offsets, and
memory bounds before writing. It supports packed 16-bit and 32-bit truecolor.

After drawing a complete frame, the renderer flushes deferred framebuffer work
and exits without restoring text mode or clearing the image. The shell's tty
settings preserve that state, and its framebuffer presenter replaces the image
with the first menu frame. Both `gbshell.service` and `gblibd.service` start
without waiting for `network-online.target`.

Loading the SPI panel earlier can change framebuffer and DRM card numbering.
The shell launcher resolves the handheld panel and connected HDMI display by
their sysfs hardware identities. In TV mode, the TV manager temporarily marks
only the SPI panel's DRM connector disconnected so RetroArch selects HDMI even
when the panel registered first. It restores normal panel detection on return
to handheld mode, and verifies each change before switching input and shell
state. No device is unbound, and the panel remains registered.

Missing hardware and ordinary rendering errors are nonfatal. The boot script
bounds module loading and renderer execution and continues boot after failure.
The splash cannot appear before the firmware has loaded Linux and the SPI panel
can receive pixels. A driver reset, another display owner, or a framebuffer
console takeover can still affect the transition, so a cold boot on the real
handheld is required to verify the first visible frame and absence of flicker.

## Installation on the handheld

The normal protected-root updater builds `gbsplash` and invokes the installer
while it already has the protected base and firmware partition available.
Installation and archive verification must succeed before the updater writes
its completed payload receipt or requests a reboot.

For a manual installation from this checkout, build the small renderer on the
Pi. This configure mode does not require SDL, SQLite, PNG, or networking
libraries; a C/C++ toolchain and CMake are sufficient.

```sh
cmake -S . -B build-splash -DGBSHELL_SPLASH_ONLY=ON -DBUILD_TESTING=OFF
cmake --build build-splash --target gbsplash -j2
sudo ./scripts/install-boot-splash.sh --build-dir ./build-splash
sudo reboot
```

The installer requires the actual GamePi13 panel to be bound at `spi0.0` using
`panel-mipi-dbi-spi`, with the `spi-bcm2835` controller. It derives the panel
initialization firmware filename from its first Device Tree compatible string,
and records the observed module dependencies and suppliers. It refuses an
unrecognized display configuration before changing boot files.

On an OverlayFS image the wrapper stages its small input set in `/run`, enters
the protected root through `overlayroot-chroot`, and restores the mount modes
it found on entry. It does not require disabling root protection. Installation
does not restart the shell or reboot automatically.

The installed files are:

| Path | Purpose |
| --- | --- |
| `/usr/local/lib/gamebird/gbsplash` | Standalone native renderer |
| `/usr/share/gamebird/boot/splash.rgb565` | Preconverted image, 115,200 bytes |
| `/etc/gamebird/boot-splash.conf` | Observed panel firmware and module names |
| `/etc/initramfs-tools/hooks/gamebird-splash` | Copies the renderer, image, firmware, and dependencies into rebuilt boot images |
| `/etc/initramfs-tools/scripts/init-top/00-gamebird-splash` | Draws the splash during early boot |
| `/etc/systemd/system/gbshell.service.d/10-boot-splash.conf` | Preserves the display during the handoff to an existing shell service |
| `/usr/local/lib/gamebird/gbshell-start.sh` | Selects the handheld or HDMI framebuffer by hardware identity |
| `/usr/local/lib/gamebird/tv-mode-manager.sh` | Keeps RetroArch on the active display despite changed DRM card numbering |
| `/etc/gamebird/boot-splash.sha256` | Integrity receipt for installed files |
| `/var/lib/gamebird/boot-splash/` | Installation ownership and original files needed for removal |

The installer rebuilds and checks the initramfs, retaining the existing
OverlayFS support where enabled. It checks the actual payload bytes in the
rebuilt archive and the firmware partition's selected image. A rebuild or
verification failure restores the previous owned files and boot images. An
interrupted power supply can still interrupt filesystem writes, so perform
installation with stable power.

Existing supported `auto_initramfs=1` or standard explicit initramfs settings
are preserved. If needed, the installer appends its own small marked block
only after checking the rebuilt image. Custom or conditional kernel/initramfs
selection requires manual review; the installer does not guess. It does not
rewrite `cmdline.txt`, disable OverlayFS, or replace the working display overlay.

## Verification and removal

Verify the persistent installation and rebuilt boot image:

```sh
sudo ./scripts/install-boot-splash.sh --verify
```

Remove the feature and restore files/settings owned by this installation:

```sh
sudo ./scripts/install-boot-splash.sh --uninstall
sudo reboot
```

Removal preserves unrelated changes to the boot configuration. If the marked
block has been edited, the installer stops instead of removing an uncertain
configuration. The shell updater includes the splash as part of its payload,
so a future shell update can install it again. Removing or modifying a tracked
boot file also invalidates the updater's installed-payload verification.

For timing, record a genuine power-off/power-on with a phone and measure both
power-on to the image and power-on to a responsive menu. The renderer writes a
monotonic `gbsplash: frame written` event to the kernel log when `/dev/kmsg` is
available. That event marks a submitted/flushed frame, not a measurement of
physical LCD response or the preceding firmware time.

```sh
sudo journalctl -b -k -o short-monotonic | grep gbsplash
systemd-analyze critical-chain gbshell.service
```

Check cold boots with Wi-Fi unavailable and with HDMI connected. Automatic
splash rendering targets the handheld SPI panel. Verify the shell and a game
on HDMI, then unplug HDMI and verify their return to the handheld panel. A
separate HDMI splash is not implemented.

## Changing the artwork

The committed raw image means a device does not need Pillow or a PNG decoder
to install or display the splash. Developers changing the PNG can regenerate
the boot copy with Python and Pillow:

```sh
python3 tools/prepare-boot-splash.py assets/ui/splash.png assets/boot/splash.rgb565
python3 tools/prepare-boot-splash.py --check assets/ui/splash.png assets/boot/splash.rgb565
```

Conversion preserves the complete image and its aspect ratio with nearest
neighbor sampling. Non-square images are centered using the top-left colour
as the matte; fully transparent top-left pixels use black. Pixel bytes are
explicitly little-endian RGB565, regardless of the development machine.

Regenerating the repository asset alone does not change the installed
initramfs. Run the installer again, or install the next shell update, to put
the new image into the persistent boot files.

## Local validation

```sh
cmake -S . -B build-splash -DGBSHELL_SPLASH_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-splash -j2
ctest --test-dir build-splash --output-on-failure
```

The tests cover pixel conversion, framebuffer bounds/stride/offsets, malformed
images and CLI deadlines, deterministic artwork conversion, and isolated
installer failure/rollback fixtures. Display selection fixtures cover reversed
framebuffer numbering and TV mode connector transitions. The asset tests require
developer-only Pillow. They do not replace validation of the panel's startup,
colour order, orientation, and transition to the launcher on a physical unit.
