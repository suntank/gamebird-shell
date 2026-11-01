#!/bin/bash
# Install into the persistent OS even when Raspberry Pi OverlayFS is active.
set -Eeuo pipefail

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$project_dir/build"
inner=0
action=install

usage() {
  echo "usage: sudo $0 [--build-dir DIR] [--uninstall|--verify] [--inner]"
}

while (($#)); do
  case "$1" in
    --build-dir)
      (($# >= 2)) || { usage >&2; exit 2; }
      build_dir=$(realpath -m -- "$2"); shift 2 ;;
    --inner) inner=1; shift ;;
    --uninstall) action=uninstall; shift ;;
    --verify) action=verify; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

[[ $(id -u) == 0 ]] || { echo "Run this installer as root." >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required for installation." >&2; exit 1; }

if ((inner)); then
  # The updater already owns the protected-root and firmware mount lifetime.
  # This mode never starts services, changes those mounts, or reboots.
  exec python3 "$project_dir/scripts/boot-splash-install.py" \
    "$action" --source "$project_dir" --build-dir "$build_dir"
fi

exec 9>/run/lock/gamebird-updater.lock
flock -w 30 9 || { echo "A GameBird update is already running." >&2; exit 75; }

boot_dir=/boot/firmware
lower_root=/media/root-ro
boot_mode=
lower_mode=
stage=
bound_boot=0
bound_dev=0
saved_mounts=()
saved_modes=()

mount_mode() {
  local options
  options=$(findmnt -n -o OPTIONS --mountpoint "$1") || return 1
  case ",$options," in
    *,ro,*) echo ro ;;
    *,rw,*) echo rw ;;
    *) return 1 ;;
  esac
}

cleanup() {
  local result=$?
  trap - EXIT HUP INT TERM
  if ((bound_dev)); then
    umount -R "$lower_root/dev" || result=1
  fi
  if ((bound_boot)); then
    umount "$lower_root/boot/firmware" || result=1
  fi
  # overlayroot-chroot may remount pre-existing lower-root block mounts read-only
  # when it exits. Restore those exact modes as well as our own boot/root modes.
  for index in "${!saved_mounts[@]}"; do
    mount -o "remount,${saved_modes[$index]}" "${saved_mounts[$index]}" || result=1
  done
  if [[ -n "$boot_mode" ]]; then
    mount -o "remount,$boot_mode" "$boot_dir" || result=1
  fi
  if [[ -n "$lower_mode" ]]; then
    mount -o "remount,$lower_mode" "$lower_root" || result=1
  fi
  [[ -z "$stage" ]] || rm -rf -- "$stage"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

boot_mode=$(mount_mode "$boot_dir") || {
  echo "$boot_dir must be the mounted Raspberry Pi boot partition." >&2
  exit 1
}
if [[ "$action" != verify && "$boot_mode" == ro ]]; then
  mount -o remount,rw "$boot_dir"
fi

if [[ $(findmnt -n -o FSTYPE --target /) == overlay ]]; then
  command -v overlayroot-chroot >/dev/null || {
    echo "overlayroot-chroot is required to update the protected OS." >&2
    exit 1
  }
  lower_mode=$(mount_mode "$lower_root") || {
    echo "The protected root was not found at $lower_root." >&2
    exit 1
  }
  snapshot=$(findmnt --json --submounts --mountpoint "$lower_root" \
    --output TARGET,SOURCE,OPTIONS | python3 -c '
import json, sys
def walk(item):
    target, source = item.get("target", ""), item.get("source", "")
    if source.startswith("/dev/") and target != sys.argv[1]:
        if "\n" in target or "\t" in target:
            raise SystemExit("Unsupported protected-root mount path")
        options = item.get("options", "").split(",")
        print(target + "\t" + ("ro" if "ro" in options else "rw"))
    for child in item.get("children", []):
        walk(child)
for item in json.load(sys.stdin).get("filesystems", []):
    walk(item)
' "$lower_root")
  if [[ -n "$snapshot" ]]; then
    while IFS=$'\t' read -r target mode; do
      saved_mounts+=("$target")
      saved_modes+=("$mode")
    done <<<"$snapshot"
  fi
  # /run is provided inside the protected root by overlayroot-chroot. Copy only
  # the small installation inputs; never depend on an upper-layer checkout.
  stage=$(mktemp -d /run/gamebird-boot-splash.XXXXXX)
  mkdir -p "$stage/scripts" "$stage/packaging/initramfs" \
    "$stage/packaging/systemd/gbshell.service.d" "$stage/assets/boot" "$stage/build"
  cp "$project_dir/scripts/install-boot-splash.sh" "$project_dir/scripts/boot-splash-install.py" \
    "$project_dir/scripts/gbshell-start.sh" "$project_dir/scripts/tv-mode-manager.sh" "$stage/scripts/"
  cp "$project_dir/packaging/initramfs/gamebird-splash-hook" \
     "$project_dir/packaging/initramfs/gamebird-splash-top" "$stage/packaging/initramfs/"
  cp "$project_dir/packaging/systemd/gbshell.service.d/10-boot-splash.conf" \
    "$stage/packaging/systemd/gbshell.service.d/"
  if [[ "$action" == install ]]; then
    cp "$build_dir/gbsplash" "$stage/build/gbsplash"
    cp "$project_dir/assets/boot/splash.rgb565" "$stage/assets/boot/splash.rgb565"
  fi
  if ! mountpoint -q "$lower_root/boot/firmware"; then
    mount --bind "$boot_dir" "$lower_root/boot/firmware"
    bound_boot=1
  elif [[ $(findmnt -n -o SOURCE --mountpoint "$boot_dir") != \
          $(findmnt -n -o SOURCE --mountpoint "$lower_root/boot/firmware") ]]; then
    echo "The protected root has a different boot partition mounted." >&2
    exit 1
  fi
  if ! mountpoint -q "$lower_root/dev"; then
    mount --rbind /dev "$lower_root/dev"
    bound_dev=1
    mount --make-rslave "$lower_root/dev"
  fi
  # Some overlayroot-chroot releases replace the child's exit status with their
  # cleanup status. An explicit result file makes a failed install unambiguous.
  chroot_result=0
  overlayroot-chroot /bin/bash -c '
    set +e
    extra=()
    [[ "$2" == install ]] || extra+=("--$2")
    /bin/bash "$1/scripts/install-boot-splash.sh" --inner \
      --build-dir "$1/build" "${extra[@]}"
    result=$?
    printf "%s\n" "$result" >"$1/inner-result"
    exit "$result"
  ' gamebird-splash "$stage" "$action" || chroot_result=$?
  [[ -f "$stage/inner-result" ]] || {
    echo "The protected-root installer did not report completion." >&2
    exit 1
  }
  inner_result=$(cat "$stage/inner-result")
  [[ "$inner_result" =~ ^[0-9]+$ && "$inner_result" -le 255 ]] || {
    echo "Invalid protected-root installation result." >&2
    exit 1
  }
  ((inner_result == 0)) || exit "$inner_result"
  ((chroot_result == 0)) || exit "$chroot_result"
else
  python3 "$project_dir/scripts/boot-splash-install.py" \
    "$action" --source "$project_dir" --build-dir "$build_dir"
fi
sync
