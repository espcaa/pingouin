# Pingouin's guest tools on NixOS. install.sh copies this folder (with the
# pingouind binary) to /etc/nixos/pingouin; then, in configuration.nix:
#
#   imports = [ ./pingouin ];
#   services.pingouin.user = "alice";     # who gets ~/Mac (the shared folder)
#
# and `sudo nixos-rebuild switch`.
{ config, lib, ... }:
let
  cfg = config.services.pingouin;
  pingouind = ./pingouind;
in
{
  options.services.pingouin = {
    enable = lib.mkEnableOption "Pingouin's guest tools (gestures, displays, shared folder)" // { default = true; };
    user = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "alice";
      description = "The user who sees the Mac's shared folder as ~/Mac (null: no ~/Mac).";
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    {
      # gestures: a virtual touchpad and keyboard
      boot.kernelModules = [ "uinput" ];
      systemd.services.pingouind = {
        description = "pingouind: Mac trackpad gestures on a virtual touchpad";
        wantedBy = [ "multi-user.target" ];
        serviceConfig = { ExecStart = "${pingouind}"; Restart = "always"; RestartSec = 2; };
      };
      # extra displays: the logged-in user opens the display channel
      services.udev.extraRules = ''
        SUBSYSTEM=="virtio-ports", ATTR{name}=="app.pingouin.display", TAG+="uaccess"
      '';
      systemd.user.services.pingouin-displays = {
        description = "Pingouin: tell the Mac where the desktop's screens are";
        partOf = [ "graphical-session.target" ];
        after = [ "graphical-session.target" ];
        wantedBy = [ "graphical-session.target" ];
        unitConfig = { ConditionPathExists = "/dev/virtio-ports/app.pingouin.display"; StartLimitIntervalSec = 0; };
        serviceConfig = { ExecStart = "${pingouind} displays"; Restart = "on-failure"; RestartSec = 2; };
      };
    }
    (lib.mkIf (cfg.user != null) {
      # the shared folder, owned by this user (Pingouin's "Linux user ID" must be theirs)
      fileSystems."${config.users.users.${cfg.user}.home}/Mac" = {
        device = "pingouin";
        fsType = "9p";
        options = [ "trans=virtio" "version=9p2000.L" "msize=524288" "nofail"
                    "x-systemd.automount" "x-systemd.device-timeout=2" ];
      };
    })
  ]);
}
