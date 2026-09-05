# GameBird read-only root provisioning

Raspberry Pi OS Trixie's supported `raspi-config` overlay mode protects the OS
with a RAM-backed OverlayFS and can mount `/boot/firmware` read-only. GameBird
runtime data must remain on a separate filesystem so it survives reboot.

The audited 64 GB image initially expands `/dev/mmcblk0p2` across the card.
`stage1-repartition.sh` installs a one-shot initramfs operation that checks and
shrinks the ext4 filesystem to 9 GiB, resizes partition 2 to 10 GiB, and creates
the remaining space as the `GAMEBIRD-DATA` ext4 partition. Its guards are tied
to the audited partition start and card size and intentionally stop boot in the
initramfs instead of guessing when the layout differs.

After the first verified boot, `stage2-persistent-data.sh` mounts the new
partition at `/data`, migrates GameBird/RetroArch data and selected hardware
settings, and installs bind mounts. It also makes the journal volatile to limit
RAM-overlay growth.

Only after both stages and their mounts have been verified, run the third-stage
script. It uses Raspberry Pi OS's official overlay command, adds `recurse=0` so
the separate data partition remains writable, installs the Trixie remount
compatibility wrapper, and verifies every generated initramfs image:

```sh
sudo ./stage3-enable-overlay.sh
sudo reboot
```

After reboot, `/` should be OverlayFS, `/media/root-ro` should be read-only,
`/boot/firmware` should be read-only, and `/data` should remain ext4 read-write.
On Trixie/systemd 257, `gamebird-remount-fs` avoids an unsupported second
OverlayFS reconfiguration while retaining the stock helper whenever the system
boots without an overlay. Without `recurse=0`, overlayroot also converts the
data partition into a temporary overlay, defeating persistence.

The GameBird system updater performs OS and shell updates directly against the
protected lower root with `overlayroot-chroot`, restores the read-only mounts,
and reboots after success. Use **Tools -> System Update** for normal updates.
Disabling overlay mode is only needed for manual recovery or maintenance that
the updater does not support.
