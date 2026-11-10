#!/bin/bash
# Pingouin's guest tools, for any Linux distro (ARM64). Run inside the VM:
#
#   sudo bash install.sh              install or update
#   sudo bash install.sh --uninstall  remove everything this added
#
# Pingouin shows this folder to every VM:
#   sudo mkdir -p /mnt && sudo mount -t 9p -o trans=virtio,ro pingouin-tools /mnt && sudo bash /mnt/install.sh; sudo umount /mnt
#   (mkdir: NixOS has no /mnt)
#
# What it sets up (nothing to download, no package manager):
#   - pingouind, a system service: Mac trackpad gestures and ⌘ shortcuts
#     (a virtual touchpad and keyboard through /dev/uinput);
#   - pingouind displays, in the desktop session: extra screens (where the
#     desktop put its screens; it copies the Mac's arrangement);
#   - the VM's shared folder (Pingouin > Shared folder) on ~/Mac.

set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:-}
[[ $user == root ]] && user=
say() { printf '%s\n' "$*"; }
warn() { printf 'pingouin: %s\n' "$*" >&2; }
mark="# pingouin shared folder"

if [[ -d /run/systemd/system ]]; then init=systemd
elif command -v openrc-run >/dev/null || [[ -x /sbin/openrc-run ]]; then init=openrc
elif command -v runsvdir >/dev/null; then init=runit
else init=none
fi

uninstall() {
  case $init in
    systemd)
      systemctl disable --now pingouind.service 2>/dev/null || true
      systemctl --global disable pingouin-displays.service 2>/dev/null || true
      rm -f /etc/systemd/system/pingouind.service /etc/systemd/user/pingouin-displays.service
      systemctl daemon-reload ;;
    openrc)
      rc-service pingouind stop 2>/dev/null || true
      rc-update del pingouind default 2>/dev/null || true
      rm -f /etc/init.d/pingouind ;;
    runit)
      rm -f /var/service/pingouind /etc/runit/runsvdir/default/pingouind
      rm -rf /etc/sv/pingouind ;;
  esac
  rm -f /etc/xdg/autostart/pingouin-displays.desktop /etc/udev/rules.d/70-pingouin.rules \
    /etc/modules-load.d/pingouin.conf /usr/local/bin/pingouind
  if grep -q "$mark" /etc/fstab 2>/dev/null; then
    awk -v m="$mark" 'index($0, m) == 0' /etc/fstab > /etc/fstab.pingouin && cat /etc/fstab.pingouin > /etc/fstab
    rm -f /etc/fstab.pingouin
  fi
  rm -rf /var/lib/pingouin
  say "Pingouin's guest tools are removed (~/Mac stays as an empty folder; reboot to unmount it)."
}

if [[ ${1:-} == --uninstall ]]; then uninstall; exit 0; fi

bin=$here/pingouind/pingouind
[[ -x $bin ]] || { warn "no pingouind next to this script ($bin)"; exit 1; }
"$bin" --version >/dev/null 2>&1 || { warn "pingouind doesn't run here: Pingouin's guest tools are for ARM64 Linux"; exit 1; }

# ---- nixos: declared in its config, not installed by hand ----
if [[ -e /etc/NIXOS ]]; then
  dest=/etc/nixos/pingouin
  mkdir -p "$dest"
  install -m 644 "$here/nixos/default.nix" "$dest/default.nix"
  install -m 755 "$bin" "$dest/pingouind"
  say "NixOS: Pingouin's module is in $dest. Add to /etc/nixos/configuration.nix:"
  say ""
  say "    imports = [ ./pingouin ];"
  [[ -n $user ]] && say "    services.pingouin.user = \"$user\";   # ~/Mac, the shared folder"
  say ""
  say "(with flakes: add ./pingouin to your nixosConfiguration's modules), then:"
  say "    sudo nixos-rebuild switch"
  if [[ -n $user && $(id -u "$user") != 1000 ]]; then
    say "Your user ID is $(id -u "$user"): set Linux user ID to $(id -u "$user") in Pingouin's Shared folder settings."
  fi
  exit 0
fi

# ---- what the kernel must have ----
modprobe uinput 2>/dev/null || true
[[ -e /dev/uinput ]] || { warn "this kernel has no uinput (CONFIG_INPUT_UINPUT): gestures can't work"; exit 1; }
modprobe 9pnet_virtio 2>/dev/null || true
modprobe 9p 2>/dev/null || true
grep -qw 9p /proc/filesystems || warn "this kernel has no 9p file system: the shared folder won't mount (the rest works)"
mkdir -p /etc/modules-load.d
echo uinput > /etc/modules-load.d/pingouin.conf
[[ -f /etc/modules ]] && ! grep -qx uinput /etc/modules && echo uinput >> /etc/modules   # Alpine

[[ $init == none ]] && { warn "no systemd, OpenRC or runit here: install pingouind as a service yourself (/usr/local/bin/pingouind)"; }

# ---- files ----
install -m 755 "$bin" /usr/local/bin/pingouind
cat > /etc/udev/rules.d/70-pingouin.rules <<'RULES'
SUBSYSTEM=="virtio-ports", ATTR{name}=="app.pingouin.display", TAG+="uaccess"
RULES
# The display helper also starts from the desktop's autostart, for sessions
# without systemd's graphical-session.target (one runs; the other stands by).
mkdir -p /etc/xdg/autostart
install -m 644 "$here/pingouind/pingouin-displays.desktop" /etc/xdg/autostart/pingouin-displays.desktop
case $init in
  systemd)
    install -m 644 "$here/pingouind/pingouind.service" /etc/systemd/system/pingouind.service
    mkdir -p /etc/systemd/user
    install -m 644 "$here/pingouind/pingouin-displays.service" /etc/systemd/user/pingouin-displays.service ;;
  openrc)
    install -m 755 "$here/pingouind/pingouind.openrc" /etc/init.d/pingouind ;;
  runit)
    mkdir -p /etc/sv/pingouind
    install -m 755 "$here/pingouind/pingouind.runit" /etc/sv/pingouind/run ;;
esac
command -v restorecon >/dev/null && restorecon -R /usr/local/bin/pingouind /etc/udev/rules.d/70-pingouin.rules \
  /etc/xdg/autostart/pingouin-displays.desktop /etc/systemd 2>/dev/null || true
if command -v udevadm >/dev/null; then
  udevadm control --reload || true
  udevadm trigger --subsystem-match=virtio-ports || true
fi

# ---- the shared folder on ~/Mac ----
if [[ -n $user ]]; then
  home=$(getent passwd "$user" | cut -d: -f6)
  uid=$(id -u "$user")
  # Earlier versions: /var/lib/pingouin/share, shown on ~/Mac through bindfs.
  if awk -v m="$home/Mac" '$2 == m && $3 != "9p" {f=1} END {exit !f}' /proc/mounts; then
    umount -l "$home/Mac" 2>/dev/null || true
  fi
  if awk '$2 == "/var/lib/pingouin/share" {f=1} END {exit !f}' /proc/mounts; then
    umount -l /var/lib/pingouin/share 2>/dev/null || true
  fi
  sudo -u "$user" mkdir -p "$home/Mac"
  opts="trans=virtio,version=9p2000.L,msize=524288,nofail"
  [[ $init == systemd ]] && opts+=",x-systemd.automount,x-systemd.device-timeout=2"
  awk -v m="$mark" 'index($0, m) == 0' /etc/fstab > /etc/fstab.pingouin && cat /etc/fstab.pingouin > /etc/fstab
  rm -f /etc/fstab.pingouin
  printf 'pingouin %s 9p %s 0 0 %s\n' "${home// /\\040}/Mac" "$opts" "$mark" >> /etc/fstab
  rm -rf /var/lib/pingouin
  if [[ $init == systemd ]]; then
    systemctl daemon-reload
    systemctl restart "$(systemd-escape -p --suffix=automount "$home/Mac")" 2>/dev/null || true
  fi
  if mountpoint -q "$home/Mac" || mount "$home/Mac" 2>/dev/null; then
    say "shared folder: $home/Mac"
  else
    say "shared folder: none yet (choose one in Pingouin, then restart the VM)"
  fi
  if [[ $uid != 1000 ]]; then
    say "Your user ID is $uid: set Linux user ID to $uid in Pingouin's Shared folder settings, so ~/Mac's files are yours."
  fi
fi

# ---- the Mac sends taps and clicks itself: the virtual pad must not tap too ----
if [[ -n $user ]]; then
  bus="unix:path=/run/user/$(id -u "$user")/bus"
  command -v gsettings >/dev/null &&
    sudo -u "$user" DBUS_SESSION_BUS_ADDRESS="$bus" \
      gsettings set org.gnome.desktop.peripherals.touchpad tap-to-click false 2>/dev/null || true
  # KDE: per device (Apple's vendor 1452, product 804, pingouind's touchpad name)
  for kw in kwriteconfig6 kwriteconfig5; do
    command -v $kw >/dev/null || continue
    sudo -H -u "$user" $kw --file kcminputrc --group Libinput --group 1452 --group 804 \
      --group "Apple Inc. Magic Trackpad (Pingouin)" --key TapToClick false 2>/dev/null || true
    break
  done
fi

# ---- start ----
case $init in
  systemd)
    systemctl daemon-reload
    systemctl enable pingouind.service >/dev/null
    systemctl restart pingouind.service
    systemctl --global enable pingouin-displays.service 2>/dev/null || true
    if [[ -n $user ]]; then
      systemctl --user -M "$user@" daemon-reload 2>/dev/null &&
        systemctl --user -M "$user@" restart pingouin-displays.service 2>/dev/null ||
        say "the display helper starts at your next login"
    fi ;;
  openrc)
    rc-update add pingouind default >/dev/null
    rc-service pingouind restart ;;
  runit)
    svdir=/var/service; [[ -d /etc/runit/runsvdir/default ]] && svdir=/etc/runit/runsvdir/default
    ln -sfn /etc/sv/pingouind "$svdir/pingouind"
    sv restart pingouind 2>/dev/null || true ;;
esac

say "Pingouin's guest tools are installed ($init). Log out and back in for the display helper."
