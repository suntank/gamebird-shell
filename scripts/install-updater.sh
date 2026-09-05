#!/bin/sh

set -eu

[ "$(id -u)" -eq 0 ] || {
  echo "Run this script as root" >&2
  exit 1
}

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

install_payload() {
  root=$1
  install -D -m 0755 "$project_dir/scripts/gamebird-updater.sh" \
    "$root/usr/local/lib/gamebird/gamebird-updater"
  install -D -m 0440 "$project_dir/packaging/sudoers/gamebird-update" \
    "$root/etc/sudoers.d/gamebird-update"
  for name in gamebird-update-check.service gamebird-update.service \
              gamebird-update-check.timer; do
    install -D -m 0644 "$project_dir/packaging/systemd/$name" \
      "$root/etc/systemd/system/$name"
  done
}

install_payload ""
if [ "$(findmnt -n -o FSTYPE /)" = overlay ]; then
  lower=/media/root-ro
  mount -o remount,rw "$lower"
  trap 'mount -o remount,ro /media/root-ro' EXIT INT TERM
  install_payload "$lower"
  sync
  mount -o remount,ro "$lower"
  trap - EXIT INT TERM
fi

mkdir -p /data/gamebird-update/apt-lists/partial
chmod 0755 /data/gamebird-update /data/gamebird-update/apt-lists \
  /data/gamebird-update/apt-lists/partial
if [ ! -f /data/gamebird-update/installed-shell-commit ] && \
   command -v git >/dev/null 2>&1 && git -C "$project_dir" rev-parse HEAD >/dev/null 2>&1; then
  git -C "$project_dir" rev-parse HEAD \
    >/data/gamebird-update/installed-shell-commit
  chmod 0644 /data/gamebird-update/installed-shell-commit
fi

systemctl daemon-reload
systemctl enable --now gamebird-update-check.timer
echo "GameBird updater installed."
