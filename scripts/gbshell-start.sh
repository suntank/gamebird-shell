#!/bin/sh
set -eu

shell_dir=/home/gamebird/gamebird-shell
mode=handheld
tv_arg=
if [ -e /run/gamebird-tv-mode ]; then
  mode=tv
  tv_arg=--tv-mode
fi

# Loading the panel in initramfs can make it fb0 instead of fb1. Resolve both
# outputs by their hardware identity; framebuffer numbering is not an API.
find_framebuffer() (
  for framebuffer in /sys/class/graphics/fb[0-9]*; do
    name=${framebuffer##*/}
    number=${name#fb}
    case "$number" in ''|*[!0-9]*) continue ;; esac
    [ -e "$framebuffer/device" ] || continue
    device=$(readlink -f "$framebuffer/device") || continue
    case "$device/" in
      */spi0.0/*)
        if [ "$mode" = handheld ]; then
          printf '/dev/%s\n' "$name"
          exit 0
        fi
        continue
        ;;
    esac
    [ "$mode" = tv ] || continue
    for connector in /sys/class/drm/card*-HDMI-A-*/status; do
      [ -r "$connector" ] || continue
      IFS= read -r status < "$connector" || continue
      [ "$status" = connected ] || continue
      card=${connector%/status}
      card=${card##*/}
      card=${card%%-HDMI-A-*}
      # DRM fbdev parentage differs between kernel versions: its parent can
      # be the DRM card itself or the physical device that owns that card.
      for parent in "/sys/class/drm/$card" "/sys/class/drm/$card/device"; do
        [ -e "$parent" ] || continue
        resolved=$(readlink -f "$parent") || continue
        if [ "$device" = "$resolved" ]; then
          printf '/dev/%s\n' "$name"
          exit 0
        fi
      done
    done
  done
  exit 1
)

fbdev=$(find_framebuffer) || {
  echo "GameBird: waiting for the $mode framebuffer" >&2
  exit 1
}

exec "$shell_dir/build/gbshell" --presenter fbdev --fbdev "$fbdev" $tv_arg \
  --db "$shell_dir/data/catalog.db" \
  --defaults "$shell_dir/config/defaults.pi.json" \
  --systems-dir "$shell_dir/config/systems.d" \
  --settings "$shell_dir/config/user_settings.json"
