// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "Pingouin",
    platforms: [.macOS(.v15)],
    products: [
        .executable(name: "Pingouin", targets: ["Pingouin"]),
        .executable(name: "pingouinctl", targets: ["pingouinctl"]),
    ],
    targets: [
        // VM configs, the runtime's location and QEMU's command line. No UI.
        .target(name: "PingouinCore"),
        // Mac trackpad + Cmd keys -> lines for the VM's pingouind.
        .target(
            name: "CPingouinGestures",
            linkerSettings: [
                .linkedFramework("ApplicationServices"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("IOKit"),
                .linkedFramework("Carbon"),
            ]
        ),
        .executableTarget(name: "Pingouin", dependencies: ["PingouinCore", "CPingouinGestures"]),
        // CLI: create/list VMs, print QEMU's arguments, run a VM without the UI.
        .executableTarget(name: "pingouinctl", dependencies: ["PingouinCore"]),
    ],
    swiftLanguageModes: [.v5]
)
