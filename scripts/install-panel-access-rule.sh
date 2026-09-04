#!/bin/sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this script as root" >&2
  exit 1
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
rule_source="$script_dir/../packaging/udev/99-gamebird-panel-access.rules"
rule_target=/etc/udev/rules.d/99-gamebird-panel-access.rules

[ -f "$rule_source" ] || {
  echo "Missing udev rule: $rule_source" >&2
  exit 1
}

if [ "$(findmnt -n -o FSTYPE /)" = "overlay" ]; then
  command -v overlayroot-chroot >/dev/null 2>&1 || {
    echo "overlayroot-chroot is required to update the protected base OS" >&2
    exit 1
  }
  overlayroot-chroot /bin/sh -c \
    "umask 022; mkdir -p /etc/udev/rules.d; rm -f /etc/udev/rules.d/72-gamebird-panel-access.rules; cat > '$rule_target'" \
    < "$rule_source"
else
  rm -f /etc/udev/rules.d/72-gamebird-panel-access.rules
  install -D -m 0644 "$rule_source" "$rule_target"
fi

udevadm control --reload
udevadm trigger --action=change --subsystem-match=drm
udevadm settle

# Repair any deny ACL left behind before this rule was installed.
if command -v setfacl >/dev/null 2>&1; then
  for card in /dev/dri/card*; do
    [ -e "$card" ] || continue
    if udevadm info -q property "$card" | grep -q '^ID_PATH=platform-3f204000.spi-cs-0$'; then
      setfacl -m u:gamebird:rw- "$card" 2>/dev/null || true
    fi
  done
fi

echo "Installed persistent GameBird SPI-panel access rule."
