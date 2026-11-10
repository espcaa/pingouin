#!/bin/bash
# Run INSIDE the VM, once: sudo bash install.sh [path/to/pingouind]
# Installs pingouind (Mac trackpad gestures -> virtual touchpad) as a service.
set -euo pipefail
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/pingouind/pingouind}
[[ -x $bin ]] || chmod +x "$bin"
"$bin" --version >/dev/null || { echo "$bin doesn't run here (wrong arch?)" >&2; exit 1; }


install -m 755 "$bin" /usr/local/bin/pingouind
install -m 644 "$here/pingouind/pingouind.service" /etc/systemd/system/pingouind.service
command -v restorecon >/dev/null && restorecon /usr/local/bin/pingouind /etc/systemd/system/pingouind.service

# GNOME: the Mac sends taps and clicks itself, so the virtual pad must not tap too
user=${SUDO_USER:-}
if [[ -n $user ]] && command -v gsettings >/dev/null; then
  sudo -u "$user" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$user")/bus" \
    gsettings set org.gnome.desktop.peripherals.touchpad tap-to-click false || true
fi

systemctl daemon-reload
systemctl enable --now pingouind.service
sleep 2
journalctl -u pingouind -n 5 --no-pager
