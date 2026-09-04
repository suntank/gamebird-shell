#!/bin/sh
# This is a firmware setting, so it becomes active on the next reboot.
set -eu

config=/boot/firmware/config.txt
[ -f "$config" ] || { echo "missing $config" >&2; exit 1; }

boot_was_readonly=0
case ",$(findmnt -n -o OPTIONS /boot/firmware)," in
  *,ro,*)
    mount -o remount,rw /boot/firmware
    boot_was_readonly=1
    ;;
esac

cleanup() {
  if [ "$boot_was_readonly" -eq 1 ]; then
    sync
    mount -o remount,ro /boot/firmware || true
  fi
}
trap cleanup EXIT HUP INT TERM

if grep -q '^[[:space:]]*hdmi_ignore_hotplug=1[[:space:]]*$' "$config"; then
  sed -i 's/^[[:space:]]*hdmi_ignore_hotplug=1[[:space:]]*$/hdmi_ignore_hotplug=0/' "$config"
fi
