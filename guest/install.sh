#!/bin/bash
# sudo bash install.sh [path/to/pingouind]
# (Pingouin shows it to every VM: sudo mount -t 9p -o trans=virtio,ro pingouin-tools /mnt)
# Installs pingouind (Mac trackpad gestures -> virtual touchpad) as a service,
# and mounts the VM's shared folder on ~/Mac.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/pingouind/pingouind}
[[ -x $bin ]] || chmod +x "$bin"
"$bin" --version >/dev/null || { echo "$bin doesn't run here (wrong arch?)" >&2; exit 1; }


install -m 755 "$bin" /usr/local/bin/pingouind
install -m 644 "$here/pingouind/pingouind.service" /etc/systemd/system/pingouind.service
# The display helper (extra screens): the logged-in user opens its port.
install -m 644 "$here/pingouind/pingouin-displays.service" /etc/systemd/user/pingouin-displays.service
cat > /etc/udev/rules.d/70-pingouin.rules <<'RULES'
SUBSYSTEM=="virtio-ports", ATTR{name}=="app.pingouin.display", TAG+="uaccess"
RULES
command -v restorecon >/dev/null && restorecon /usr/local/bin/pingouind /etc/systemd/system/pingouind.service \
  /etc/systemd/user/pingouin-displays.service /etc/udev/rules.d/70-pingouin.rules
udevadm control --reload || true
udevadm trigger --subsystem-match=virtio-ports || true

# GNOME: the Mac sends taps and clicks itself, so the virtual pad must not tap too
user=${SUDO_USER:-}
if [[ -n $user ]] && command -v gsettings >/dev/null; then
  sudo -u "$user" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$user")/bus" \
    gsettings set org.gnome.desktop.peripherals.touchpad tap-to-click false || true
fi

# The VM's shared folder (Pingouin > Shared folder) as ~/Mac. QEMU shows it
# with virtio-9p at /var/lib/pingouin/share; its files belong to the Mac user
# there, so bindfs shows them as the Linux user's on ~/Mac.
if [[ -n $user ]]; then
  home=$(getent passwd "$user" | cut -d: -f6)
  mkdir -p /var/lib/pingouin/share
  sudo -u "$user" mkdir -p "$home/Mac"
  sed -i '/# pingouin shared folder$/d' /etc/fstab
  echo "pingouin /var/lib/pingouin/share 9p trans=virtio,version=9p2000.L,msize=524288,nofail,x-systemd.device-timeout=2 0 0 # pingouin shared folder" >> /etc/fstab
  if command -v bindfs >/dev/null || dnf -y -q install bindfs; then
    echo "/var/lib/pingouin/share $home/Mac fuse.bindfs force-user=$user,force-group=$(id -gn "$user"),nofail,x-systemd.requires-mounts-for=/var/lib/pingouin/share 0 0 # pingouin shared folder" >> /etc/fstab
  else
    echo "bindfs didn't install: the shared folder is at /var/lib/pingouin/share (read-only for $user)" >&2
    echo "/var/lib/pingouin/share $home/Mac none bind,nofail,x-systemd.requires-mounts-for=/var/lib/pingouin/share 0 0 # pingouin shared folder" >> /etc/fstab
  fi
  systemctl daemon-reload
  mount /var/lib/pingouin/share 2>/dev/null && mount "$home/Mac" 2>/dev/null &&
    echo "shared folder: $home/Mac" || echo "shared folder: none yet (choose one in Pingouin, then restart the VM)"
fi

systemctl daemon-reload
systemctl enable --now pingouind.service
systemctl --global enable pingouin-displays.service
# Start it now in the user's running desktop session, if there is one.
if [[ -n $user ]]; then
  systemctl --user -M "$user@" daemon-reload 2>/dev/null &&
    systemctl --user -M "$user@" restart pingouin-displays.service 2>/dev/null ||
    echo "pingouin-displays starts at your next login"
fi
sleep 2
journalctl -u pingouind -n 5 --no-pager
