#!/bin/sh
# Switch between the GamePi13 panel and HDMI without leaving the GPIO buttons
# active while the console is being used as a TV system.
set -eu

state_file=/run/gamebird-tv-mode
active_input_config=/run/gamebird-retroarch-input.cfg
config_dir=/home/gamebird/gamebird-shell/config
handheld_input_config=$config_dir/retroarch-input-gamebird.cfg
tv_input_config=$config_dir/retroarch-tv-input.cfg
autoconfig_source_dir=$config_dir/retroarch-autoconfig
autoconfig_destination_dir=/home/gamebird/.config/retroarch/autoconfig
gpio_device=gamebird-controls
gpio_driver=/sys/bus/platform/drivers/gpio-keys

hdmi_connected() {
  for status in /sys/class/drm/card*-HDMI-A-*/status; do
    [ -r "$status" ] || continue
    [ "$(cat "$status")" = connected ] && return 0
  done
  return 1
}

set_active_input_config() {
  source=$1
  # Keep this distinct from gbshell's own atomic-write temporary file.
  temporary="${active_input_config}.tv-mode.tmp"
  if [ -r "$source" ]; then
    cp "$source" "$temporary"
  else
    # A missing saved handheld profile is valid: RetroArch will use its
    # controller autoconfiguration instead of failing launch validation.
    : > "$temporary"
  fi
  # gbshell runs as the gamebird user and refreshes the handheld bindings just
  # before launching RetroArch.  Keep the volatile handoff writable by it.
  chown gamebird:gamebird "$temporary"
  chmod 0644 "$temporary"
  mv -f "$temporary" "$active_input_config"
}

install_autoconfig() {
  [ -d "$autoconfig_source_dir" ] || return 0
  for driver in linuxraw udev; do
    source_dir=$autoconfig_source_dir/$driver
    destination_dir=$autoconfig_destination_dir/$driver
    [ -d "$source_dir" ] || continue
    mkdir -p "$destination_dir"
    for profile in "$source_dir"/*.cfg; do
      [ -r "$profile" ] || continue
      destination="$destination_dir/$(basename "$profile")"
      if [ ! -r "$destination" ] || ! cmp -s "$profile" "$destination"; then
        install -o gamebird -g gamebird -m 0644 "$profile" "$destination"
      fi
    done
  done
}

apply_mode() {
  install_autoconfig
  if hdmi_connected; then
    mode=tv
  else
    mode=handheld
  fi

  previous=handheld
  [ -e "$state_file" ] && previous=tv

  if [ "$mode" = tv ]; then
    : > "$state_file"
    if [ "$previous" != tv ] || [ ! -e "$active_input_config" ]; then
      set_active_input_config "$tv_input_config"
    fi
    if [ -e "$gpio_driver/$gpio_device" ]; then
      printf '%s' "$gpio_device" > "$gpio_driver/unbind"
    fi
  else
    rm -f "$state_file"
    if [ "$previous" != handheld ] || [ ! -e "$active_input_config" ]; then
      set_active_input_config "$handheld_input_config"
    fi
    if [ ! -e "$gpio_driver/$gpio_device" ]; then
      printf '%s' "$gpio_device" > "$gpio_driver/bind"
    fi
  fi

  [ "$mode" != "$previous" ]
}

case "${1:---watch}" in
  --apply)
    apply_mode || true
    ;;
  --watch)
    apply_mode || true
    while :; do
      if apply_mode; then
        # gbshell-start.sh reads state_file and selects the correct framebuffer.
        systemctl try-restart gbshell.service || true
      fi
      sleep 1
    done
    ;;
  *)
    echo "usage: $0 [--apply|--watch]" >&2
    exit 2
    ;;
esac
