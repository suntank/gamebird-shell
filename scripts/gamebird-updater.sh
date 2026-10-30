#!/bin/bash

set -Eeuo pipefail

state_dir=/data/gamebird-update
status_file="$state_dir/status"
log_file="$state_dir/update.log"
apt_lists="$state_dir/apt-lists"
installed_version="$state_dir/installed-shell-commit"
pending_install="$state_dir/pending-shell-install"
shell_dir=/home/gamebird/gamebird-shell
work_dir="$state_dir/work"
lower_root=/media/root-ro
repo_slug=suntank/gamebird-shell
api_url="https://api.github.com/repos/$repo_slug/commits/main"
boot_id=$(cat /proc/sys/kernel/random/boot_id)
operation_id="$boot_id-$$-$(date +%s%N)"
installed_commit=""
last_remote_commit=""

mkdir -p "$state_dir" "$apt_lists/partial"
chmod 0755 "$state_dir" "$apt_lists" "$apt_lists/partial"
touch "$log_file"
chmod 0644 "$log_file"

clean_value() {
  printf '%s' "$1" | tr '\n=' '  ' | cut -c1-120
}

# A source checkout or a marker on /data does not identify the installed
# binaries. Accept a commit only when all four payload hashes still match.
payload_commit() {
  local build=$1 commit
  [[ -f "$build/shell-commit" && -f "$build/shell-payload.sha256" ]] || return 1
  commit=$(cat "$build/shell-commit")
  [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || return 1
  (cd "$build" && sha256sum --check --status shell-payload.sha256) || return 1
  printf '%s' "$commit"
}

write_status() {
  local phase=$1 progress=$2 os_updates=$3 shell_update=$4 reboot=$5 message=$6
  local remote_commit=${7:-$last_remote_commit}
  last_remote_commit=$remote_commit
  local temporary="$status_file.tmp.$$"
  umask 022
  {
    printf 'phase=%s\n' "$(clean_value "$phase")"
    printf 'progress=%s\n' "$progress"
    printf 'os_updates=%s\n' "$os_updates"
    printf 'shell_update=%s\n' "$shell_update"
    printf 'reboot_required=%s\n' "$reboot"
    printf 'message=%s\n' "$(clean_value "$message")"
    printf 'remote_commit=%s\n' "$(clean_value "$remote_commit")"
    printf 'checked_at=%s\n' "$(date +%s)"
    printf 'boot_id=%s\n' "$boot_id"
    printf 'operation_id=%s\n' "$operation_id"
    printf 'installed_commit=%s\n' "$installed_commit"
  } >"$temporary"
  chmod 0644 "$temporary"
  mv -f "$temporary" "$status_file"
  printf '%s %s: %s\n' "$(date -u +%FT%TZ)" "$phase" "$message" >>"$log_file"
}

field() {
  local name=$1
  sed -n "s/^${name}=//p" "$status_file" 2>/dev/null | head -n 1
}

apt_options=(
  -o "Dir::State::lists=$apt_lists"
  -o "APT::Get::List-Cleanup=0"
)

check_updates() {
  # Verify locally before network work, including when the console is offline.
  local expected="" install_boot=""
  if [[ -f "$pending_install" ]]; then
    expected=$(sed -n 's/^commit=//p' "$pending_install")
    install_boot=$(sed -n 's/^boot_id=//p' "$pending_install")
    if [[ "$install_boot" != "$boot_id" ]]; then
      if [[ "$expected" != "$installed_commit" || -z "$installed_commit" ]]; then
        write_status "ERROR" 0 0 1 0 \
          "Shell update did not survive restart; check update log" "$expected"
        return 0
      fi
      rm -f "$pending_install"
    else
      write_status "RESTART REQUIRED" 100 0 0 1 "Installed; press A to restart" "$expected"
      return 0
    fi
  fi

  write_status "CHECKING" 5 0 0 0 "Refreshing Raspberry Pi packages"
  if ! apt-get "${apt_options[@]}" update >>"$log_file" 2>&1; then
    write_status "ERROR" 0 0 0 0 "Could not reach Raspberry Pi update servers"
    return 1
  fi

  write_status "CHECKING" 55 0 0 0 "Checking security and OS packages"
  local simulation os_count
  simulation=$(apt-get "${apt_options[@]}" -s dist-upgrade 2>>"$log_file") || {
    write_status "ERROR" 0 0 0 0 "Could not calculate Raspberry Pi updates"
    return 1
  }
  os_count=$(grep -c '^Inst ' <<<"$simulation" || true)

  write_status "CHECKING" 75 "$os_count" 0 0 "Checking GameBird Shell"
  local remote="" response="" shell_update=0 shell_channel_ok=1 installed=""
  response=$(curl -fsSL --connect-timeout 10 --max-time 25 \
      -H 'Accept: application/vnd.github+json' "$api_url" 2>>"$log_file") ||
      shell_channel_ok=0
  remote=$(sed -n '/"sha":[[:space:]]*"[0-9a-f]\{40\}"/ {
      s/.*"sha":[[:space:]]*"\([0-9a-f]\{40\}\)".*/\1/p
      q
    }' <<<"$response")
  [[ "$remote" =~ ^[0-9a-f]{40}$ ]] || shell_channel_ok=0
  installed=$installed_commit
  if [[ -n "$remote" && "$remote" != "$installed" ]]; then
    shell_update=1
  fi

  if (( os_count > 0 || shell_update == 1 )); then
    local message="Updates are ready to install"
    if (( shell_channel_ok == 0 )); then
      message="Pi OS updates ready; GameBird channel unavailable"
    fi
    write_status "UPDATE AVAILABLE" 100 "$os_count" "$shell_update" 1 \
      "$message" "$remote"
  elif (( shell_channel_ok == 0 )); then
    write_status "ERROR" 0 0 0 0 \
      "Pi OS current; GameBird channel unavailable" ""
  else
    write_status "CURRENT" 100 0 0 0 "GameBird is up to date" "$remote"
  fi
}

restore_policy() {
  if [[ -e /usr/sbin/policy-rc.d.gamebird-saved ]]; then
    mv -f /usr/sbin/policy-rc.d.gamebird-saved /usr/sbin/policy-rc.d
  else
    rm -f /usr/sbin/policy-rc.d
  fi
}

install_shell_payload() {
  local source_dir=$1 remote_commit=$2 target=$shell_dir
  local build_dir="$work_dir/build"

  write_status "BUILDING GAMEBIRD" 72 0 1 1 "Configuring GameBird Shell" \
    "$remote_commit"
  # Native Nest downloads and verifies packages in-process. Existing consoles
  # need these development libraries before building their first native version.
  apt-get "${apt_options[@]}" -y --no-install-recommends install \
    libcurl4-openssl-dev libssl-dev libarchive-dev libjpeg-dev ca-certificates \
    >>"$log_file" 2>&1
  rm -rf "$build_dir"
  cmake -S "$source_dir" -B "$build_dir" -DBUILD_TESTING=OFF \
    -DGBSHELL_BUILD_SERVICES=ON -DGBSHELL_BUILD_COMMIT="$remote_commit" >>"$log_file" 2>&1
  cmake --build "$build_dir" --target gbshell gblibd gblaunch gbhud -j1 \
    >>"$log_file" 2>&1
  [[ "$("$build_dir/gbshell" --version)" == "GameBird Shell $remote_commit" ]]

  write_status "INSTALLING GAMEBIRD" 88 0 1 1 "Installing verified build" \
    "$remote_commit"
  mkdir -p "$target" "$target/build" /data/gamebird-shell/config
  rsync -a --delete \
    --exclude=.git --exclude=build --exclude=config --exclude=data \
    --exclude=roms --exclude=apps \
    "$source_dir/" "$target/"
  rsync -a --delete "$source_dir/config/" "$target/config/"
  rsync -a \
    --exclude=user_settings.json --exclude=user_settings.local.json \
    --exclude=nest.json \
    --exclude=retroarch-input-gamebird.cfg \
    "$source_dir/config/" /data/gamebird-shell/config/
  install -m 0755 "$build_dir/gbshell" "$target/build/gbshell"
  install -m 0755 "$build_dir/gblibd" "$target/build/gblibd"
  install -m 0755 "$build_dir/gblaunch" "$target/build/gblaunch"
  install -m 0755 "$build_dir/gbhud" "$target/build/gbhud"
  for binary in gbshell gblibd gblaunch gbhud; do
    cmp "$build_dir/$binary" "$target/build/$binary"
  done
  chown -R gamebird:gamebird "$target" /data/gamebird-shell/config

  install -D -m 0755 "$source_dir/scripts/gamebird-updater.sh" \
    /usr/local/lib/gamebird/gamebird-updater
  install -D -m 0755 "$source_dir/scripts/gbshell-start.sh" \
    /usr/local/lib/gamebird/gbshell-start.sh
  install -D -m 0440 "$source_dir/packaging/sudoers/gamebird-update" \
    /etc/sudoers.d/gamebird-update
  for unit in "$source_dir"/packaging/systemd/*.service \
              "$source_dir"/packaging/systemd/*.timer; do
    [[ -f "$unit" ]] || continue
    install -D -m 0644 "$unit" "/etc/systemd/system/$(basename "$unit")"
  done
}

inner_install() {
  local os_count=$1 shell_update=$2 remote_commit=$3 source_dir=$4
  export DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a

  if (( os_count > 0 )); then
    write_status "DOWNLOADING" 20 "$os_count" "$shell_update" 1 \
      "Downloading Raspberry Pi packages" "$remote_commit"
    apt-get "${apt_options[@]}" -y --download-only dist-upgrade \
      >>"$log_file" 2>&1

    if [[ -e /usr/sbin/policy-rc.d ]]; then
      mv /usr/sbin/policy-rc.d /usr/sbin/policy-rc.d.gamebird-saved
    fi
    printf '#!/bin/sh\nexit 101\n' >/usr/sbin/policy-rc.d
    chmod 0755 /usr/sbin/policy-rc.d
    trap restore_policy EXIT

    write_status "UPDATING PI OS" 45 "$os_count" "$shell_update" 1 \
      "Installing Raspberry Pi packages" "$remote_commit"
    dpkg --configure -a >>"$log_file" 2>&1
    apt-get "${apt_options[@]}" -y dist-upgrade >>"$log_file" 2>&1
    restore_policy
    trap - EXIT
  fi

  if (( shell_update == 1 )); then
    install_shell_payload "$source_dir" "$remote_commit"
  fi
  # Enable on the protected root, so the checker survives the next reboot.
  # Enabling only in the live OverlayFS loses the symlink at shutdown.
  systemctl --root=/ enable gamebird-update-check.timer >>"$log_file" 2>&1
  if (( shell_update == 1 )); then
    printf '%s\n' "$remote_commit" >"$shell_dir/build/shell-commit.tmp"
    mv -f "$shell_dir/build/shell-commit.tmp" "$shell_dir/build/shell-commit"
    # Bind the identity to the same receipt as the binaries. A power loss
    # between these renames invalidates the old receipt instead of mislabelling
    # new binaries with the previous commit.
    (cd "$shell_dir/build" && sha256sum shell-commit gbshell gblibd gblaunch gbhud >shell-payload.sha256.tmp)
    mv -f "$shell_dir/build/shell-payload.sha256.tmp" "$shell_dir/build/shell-payload.sha256"
  fi
}

cleanup_mounts() {
  mountpoint -q "$lower_root/etc/resolv.conf" && umount "$lower_root/etc/resolv.conf" || true
  mountpoint -q "$lower_root/data" && umount "$lower_root/data" || true
  mountpoint -q "$lower_root/dev" && umount -l "$lower_root/dev" || true
  mountpoint -q "$lower_root/boot/firmware" && \
    umount "$lower_root/boot/firmware" || true
  mount -o remount,rw /data 2>/dev/null || true
  mount -o remount,ro /boot/firmware 2>/dev/null || true
}

install_updates() {
  [[ -f "$status_file" ]] || check_updates
  local os_count shell_update remote_commit source_dir=""
  os_count=$(field os_updates)
  shell_update=$(field shell_update)
  remote_commit=$(field remote_commit)
  [[ "$os_count" =~ ^[0-9]+$ ]] || os_count=0
  [[ "$shell_update" == 1 ]] || shell_update=0

  if [[ -f "$pending_install" && $(field reboot_required) == 1 &&
        $(sed -n 's/^boot_id=//p' "$pending_install") == "$boot_id" ]]; then
    local expected verified
    expected=$(sed -n 's/^commit=//p' "$pending_install")
    verified=$(payload_commit "$lower_root$shell_dir/build")
    [[ "$expected" == "$verified" ]]
    installed_commit=$verified
    request_reboot "$expected"
    return
  fi

  if (( os_count == 0 && shell_update == 0 )); then
    write_status "CURRENT" 100 0 0 0 "No updates to install"
    return 0
  fi
  if (( shell_update == 1 )) && [[ ! "$remote_commit" =~ ^[0-9a-f]{40}$ ]]; then
    write_status "ERROR" 0 "$os_count" 0 0 "GameBird update version is invalid"
    return 1
  fi

  rm -rf "$work_dir"
  mkdir -p "$work_dir"
  if (( shell_update == 1 )); then
    write_status "DOWNLOADING" 10 "$os_count" 1 1 \
      "Downloading GameBird Shell" "$remote_commit"
    curl -fL --connect-timeout 10 --max-time 300 \
      "https://codeload.github.com/$repo_slug/tar.gz/$remote_commit" \
      -o "$work_dir/gamebird-shell.tar.gz" >>"$log_file" 2>&1
    mkdir -p "$work_dir/source"
    tar -xzf "$work_dir/gamebird-shell.tar.gz" --strip-components=1 \
      -C "$work_dir/source"
    [[ -f "$work_dir/source/CMakeLists.txt" &&
       -f "$work_dir/source/scripts/gamebird-updater.sh" ]] || {
      write_status "ERROR" 0 "$os_count" 0 0 "Downloaded GameBird update is incomplete"
      return 1
    }
    source_dir="$work_dir/source"
  fi

  write_status "PREPARING" 15 "$os_count" "$shell_update" 1 \
    "Preparing protected system" "$remote_commit"
  cleanup_mounts
  trap cleanup_mounts EXIT
  mount -o remount,rw /boot/firmware
  mount --bind /boot/firmware "$lower_root/boot/firmware"
  mount --rbind /dev "$lower_root/dev"
  mount --make-rslave "$lower_root/dev"
  mount --bind /data "$lower_root/data"
  # NetworkManager updates the live overlay's resolver file. The protected
  # root can retain obsolete DNS servers, so give this install the live file
  # without persisting a network-specific resolver configuration. /run-based
  # symlinks are handled by overlayroot-chroot's existing /run bind instead.
  if [[ -f "$lower_root/etc/resolv.conf" && ! -L "$lower_root/etc/resolv.conf" ]]; then
    mount --bind /etc/resolv.conf "$lower_root/etc/resolv.conf"
  fi

  # Use the downloaded updater for its own installation, including repairs to
  # old failure handling. Do not delegate back to a stale protected-root copy.
  local inner_updater=/usr/local/lib/gamebird/gamebird-updater
  if (( shell_update == 1 )); then
    inner_updater="$source_dir/scripts/gamebird-updater.sh"
  fi
  /usr/sbin/overlayroot-chroot /bin/bash "$inner_updater" \
    inner "$os_count" "$shell_update" "$remote_commit" "$source_dir"
  if (( shell_update == 1 )); then
    write_status "VERIFYING GAMEBIRD" 96 0 1 1 "Verifying protected installation" "$remote_commit"
    local verified
    verified=$(payload_commit "$lower_root$shell_dir/build")
    [[ "$verified" == "$remote_commit" ]]
    printf 'commit=%s\nboot_id=%s\n' "$remote_commit" "$boot_id" >"$pending_install.tmp"
    mv -f "$pending_install.tmp" "$pending_install"
    printf '%s\n' "$verified" >"$installed_version.tmp"
    mv -f "$installed_version.tmp" "$installed_version"
    installed_commit=$verified
  fi
  cleanup_mounts
  trap - EXIT
  systemctl daemon-reload
  systemctl enable gamebird-update-check.timer >>"$log_file" 2>&1

  request_reboot "$remote_commit"
}

request_reboot() {
  write_status "REBOOTING" 100 0 0 1 "Update complete; restarting GameBird" "$1"
  sync
  sleep 5
  systemctl reboot
}

main() {
  [[ $(id -u) -eq 0 ]] || {
    echo "gamebird-updater must run as root" >&2
    exit 1
  }

  # The inner phase runs through overlayroot-chroot while the outer process
  # intentionally holds the updater lock. Reusing that lock here would make
  # the protected-root installation silently stop before doing any work.
  if [[ ${1:-} == inner ]]; then
    shift
    inner_install "$@"
    return
  fi

  exec 9>/run/lock/gamebird-updater.lock
  flock -w 30 9 || {
    echo "Another updater still holds the lock" >>"$log_file"
    exit 75
  }
  installed_commit=$(payload_commit "$shell_dir/build" || true)

  case "${1:-}" in
    check) check_updates ;;
    install) install_updates ;;
    *) echo "usage: gamebird-updater {check|install}" >&2; exit 2 ;;
  esac
}

report_failure() {
  local rc=$?
  trap - ERR
  if [[ $(field phase) != ERROR || $(field operation_id) != "$operation_id" ]]; then
    write_status "ERROR" 0 0 0 0 "Update failed (exit $rc); check update log"
  fi
  exit "$rc"
}

# Keep errexit enabled inside every install function and the protected-root
# subprocess. Otherwise failed builds/mounts can continue through to REBOOTING.
trap report_failure ERR
main "$@"
