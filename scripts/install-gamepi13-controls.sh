#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root" >&2
  exit 1
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dts="$script_dir/../hardware/gamepi13/gamebird-controls-overlay.dts"
retroarch_profile_source="$script_dir/../config/retroarch-autoconfig/linuxraw/GameBird Controls.cfg"
boot_dir=/boot/firmware
overlay_name=gamebird-controls
overlay_file="$boot_dir/overlays/$overlay_name.dtbo"
config_file="$boot_dir/config.txt"
temporary_dtbo="$(mktemp)"
boot_was_readonly=0

cleanup() {
  rm -f "$temporary_dtbo"
  if [ "$boot_was_readonly" -eq 1 ]; then
    sync
    mount -o remount,ro "$boot_dir" || true
  fi
}
trap cleanup EXIT HUP INT TERM

[ -f "$source_dts" ] || {
  echo "Missing overlay source: $source_dts" >&2
  exit 1
}
[ -d "$boot_dir/overlays" ] || {
  echo "Raspberry Pi firmware overlay directory not found" >&2
  exit 1
}
command -v dtc >/dev/null 2>&1 || {
  echo "dtc is required (install the device-tree-compiler package)" >&2
  exit 1
}

dtc -@ -I dts -O dtb -o "$temporary_dtbo" "$source_dts"
dtc -I dtb -O dts "$temporary_dtbo" >/dev/null 2>&1

case ",$(findmnt -n -o OPTIONS "$boot_dir")," in
  *,ro,*)
    mount -o remount,rw "$boot_dir"
    boot_was_readonly=1
    ;;
esac

install -m 0644 "$temporary_dtbo" "$overlay_file"

if [ -d /home/gamebird ]; then
  [ -f "$retroarch_profile_source" ] || {
    echo "Missing RetroArch GamePi13 controller profile: $retroarch_profile_source" >&2
    exit 1
  }
  install -d -o gamebird -g gamebird -m 0755 \
    /home/gamebird/.config/retroarch/autoconfig/linuxraw
  install -o gamebird -g gamebird -m 0644 "$retroarch_profile_source" \
    "/home/gamebird/.config/retroarch/autoconfig/linuxraw/GameBird Controls.cfg"
fi

if ! grep -Eq '^[[:space:]]*dtoverlay=gamebird-controls([[:space:]]|$)' "$config_file"; then
  cp -a "$config_file" "$config_file.before-gamebird-controls"
  {
    echo
    echo "# Waveshare GamePi13 buttons (in-tree gpio-keys driver)"
    echo "dtoverlay=gamebird-controls"
  } >> "$config_file"
fi

sync
echo "Installed $overlay_file and enabled dtoverlay=gamebird-controls"
echo "Installed the RetroArch GameBird Controls profile."
echo "Reboot to activate the GamePi13 controls."
