#!/bin/sh

set -eu

udevadm control --reload || true
udevadm trigger --action=change --subsystem-match=drm || true
udevadm settle || true

for card in /dev/dri/card*; do
  [ -e "$card" ] || continue
  if udevadm info -q property "$card" |
      grep -q '^ID_PATH=platform-3f204000.spi-cs-0$'; then
    chgrp video "$card" || true
    chmod 0660 "$card" || true
    # A named ACL takes precedence over the owning group. Grant the service
    # account explicitly in case logind left an ACL during early boot.
    setfacl -m u:gamebird:rw- "$card" || true
  fi
done

exit 0
