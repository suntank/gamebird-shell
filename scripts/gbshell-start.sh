#!/bin/sh
set -eu

shell_dir=/home/gamebird/gamebird-shell
fbdev=/dev/gamebirdfb
tv_arg=
if [ -e /run/gamebird-tv-mode ]; then
  fbdev=/dev/fb0
  tv_arg=--tv-mode
fi

exec "$shell_dir/build/gbshell" --presenter fbdev --fbdev "$fbdev" $tv_arg \
  --db "$shell_dir/data/catalog.db" \
  --defaults "$shell_dir/config/defaults.pi.json" \
  --systems-dir "$shell_dir/config/systems.d" \
  --settings "$shell_dir/config/user_settings.json"
