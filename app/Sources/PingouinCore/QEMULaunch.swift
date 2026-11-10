import Foundation

/// The runtime's switches that are environment variables (runtime/patches).
public enum RuntimeEnv {
    public static let productName = "PINGOUIN_PRODUCT_NAME"
    public static let icon = "PINGOUIN_ICON"
    public static let fullPanel = "PINGOUIN_FULLPANEL"
    public static let keyboard = "PINGOUIN_KEYBOARD"
    public static let hwCursor = "PINGOUIN_HW_CURSOR"
}

/// The virtio-serial port pingouind opens in the guest
/// (/dev/virtio-ports/<name>).
public let gesturesPortName = "org.pingouin.gestures"
/// The display channel's port: pingouind displays says where the guest put
/// its screens (pingouin-cocoa-displays.patch).
public let displayPortName = "app.pingouin.display"
/// Screens a VM can have: the main window plus one per chosen Mac display.
public let maxOutputs = 4

/// 9p mount tags: the VM's shared folder, and the guest tools (read-only).
public let sharedFolderTag = "pingouin"
public let guestToolsTag = "pingouin-tools"

public struct QEMULaunch: Equatable, Sendable {
    public var executable: URL
    public var arguments: [String]
    public var environment: [String: String]

    /// QEMU's command line for a VM. `iso` is attached as a CD for this run only
    /// and boots first (installing); without it the VM boots from its disk.
    /// `guestTools`: the folder with pingouind and install.sh (the app's
    /// Resources/guest), shown to every VM read-only.
    public init(vm: VM, runtime: Runtime, iso: URL? = nil, icon: URL? = nil, guestTools: URL? = nil) {
        let c = vm.config
        func file(_ u: URL) -> String { u.path.replacingOccurrences(of: ",", with: ",,") }
        let venus = c.vulkan ? VenusPlan.forThisMac(memoryMiB: c.memoryMiB) : nil
        var machine = "virt,gic-version=3"
        if let w = venus?.highWindowGB { machine += ",highmem-mmio-size=\(w)G" }
        var gpu = "virtio-gpu-gl-pci,max_outputs=\(maxOutputs),xres=1920,yres=1080,romfile="
        if let venus { gpu += ",blob=true,venus=true,hostmem=\(venus.hostmemMB)M" }

        var a: [String] = [
            // (no process=: QEMU exits on macOS, which can't rename processes)
            "-name", "guest=\(c.name)",
            "-machine", machine, "-accel", "hvf",
            "-cpu", "host,pmu=off", "-smp", "\(c.cpus)", "-m", "\(c.memoryMiB)M",
            "-nodefaults",
            "-drive", "if=pflash,format=raw,readonly=on,file=\(file(runtime.firmware))",
            "-drive", "if=pflash,format=raw,file=\(file(vm.varsURL))",
            "-device", "qemu-xhci,id=usb,streams=off",
            "-device", "virtio-scsi-pci,id=scsi,romfile=",
            "-device", "usb-kbd,bus=usb.0",
            "-drive", "if=none,id=root,format=raw,cache=writeback,file=\(file(vm.diskURL))",
            "-device", "virtio-blk-pci,drive=root,bootindex=1",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,romfile=",
            "-device", gpu,
            // show-cursor=off: no Mac arrow over the VM, the guest draws its own
            // pointer (any desktop). macCursor: the Mac's cursor shows the
            // guest's pointer image instead (pingouin-cocoa-hw-cursor.patch).
            "-display", "cocoa,gl=on,show-cursor=\(c.macCursor ? "on" : "off"),zoom-to-fit=on,full-grab=on,swap-opt-cmd=off",
            "-device", "virtio-keyboard-pci,romfile=", "-device", "virtio-tablet-pci,romfile=",
            "-audiodev", "sdl,id=snd", "-device", "intel-hda,romfile=", "-device", "hda-duplex,audiodev=snd",
            "-object", "rng-random,id=rng,filename=/dev/urandom", "-device", "virtio-rng-pci,rng=rng,romfile=",
            "-device", "virtio-balloon-pci,free-page-reporting=on",
            // the gestures channel: the app connects to the socket, pingouind
            // opens /dev/virtio-ports/org.pingouin.gestures
            "-device", "virtio-serial-pci,id=vser,romfile=",
            "-chardev", "socket,id=gestures,path=\(file(vm.gesturesSocket)),server=on,wait=off",
            "-device", "virtserialport,bus=vser.0,chardev=gestures,name=\(gesturesPortName)",
            "-chardev", "socket,id=display,path=\(file(vm.displaySocket)),server=on,wait=off",
            "-device", "virtserialport,bus=vser.0,chardev=display,name=\(displayPortName)",
            "-serial", "file:\(vm.dir.appendingPathComponent("serial.log").path)",
            "-qmp", "unix:\(file(vm.controlSocket)),server=on,wait=off",
        ]
        // Files the guest writes are made by QEMU, so they're the Mac user's.
        if let folder = c.sharedFolder {
            a += ["-virtfs", "local,id=share,path=\(file(URL(fileURLWithPath: folder))),mount_tag=\(sharedFolderTag),security_model=none"]
        }
        if let guestTools {
            a += ["-virtfs", "local,id=tools,path=\(file(guestTools)),mount_tag=\(guestToolsTag),security_model=none,readonly=on"]
        }
        if let iso {
            a += ["-drive", "if=none,id=iso,format=raw,readonly=on,media=cdrom,file=\(file(iso))",
                  "-device", "scsi-cd,bus=scsi.0,drive=iso,bootindex=0"]
        }

        var env = [RuntimeEnv.productName: "Pingouin",
                   "PINGOUIN_DISPLAY_SOCKET": vm.displaySocket.path,
                   "PINGOUIN_DISPLAYS_CHOICES": vm.displayChoicesURL.path]
        if let icon { env[RuntimeEnv.icon] = icon.path }
        if c.fullPanel { env[RuntimeEnv.fullPanel] = "1" }
        if c.isoKeyboard { env[RuntimeEnv.keyboard] = "iso" }
        if c.macCursor { env[RuntimeEnv.hwCursor] = "1" }

        executable = runtime.qemu
        arguments = a
        environment = env
    }

    /// The command as one shell line, for logs and debugging.
    public var shellLine: String {
        let q: (String) -> String = { s in
            s.allSatisfy({ $0.isLetter || $0.isNumber || "-_=,./:+".contains($0) }) ? s
                : "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
        }
        let envPart = environment.keys.sorted().map { "\($0)=\(q(environment[$0]!))" }
        return (envPart + [q(executable.path)] + arguments.map(q)).joined(separator: " ")
    }
}
