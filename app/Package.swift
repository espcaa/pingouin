// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "Pingouin",
    platforms: [.macOS(.v15)],
    products: [
        .executable(name: "Pingouin", targets: ["Pingouin"]),
        .executable(name: "pingouinctl", targets: ["pingouinctl"]),
    ],
    dependencies: [
        // updates (Check for Updates…, the appcast on GitHub releases)
        .package(url: "https://github.com/sparkle-project/Sparkle", exact: "2.9.6"),
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
        .executableTarget(
            name: "Pingouin",
            dependencies: ["PingouinCore", "CPingouinGestures", .product(name: "Sparkle", package: "Sparkle")],
            // Sparkle.framework goes in Contents/Frameworks (scripts/bundle.sh)
            linkerSettings: [.unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"])]
        ),
        // CLI: create/list VMs, print QEMU's arguments, run a VM without the UI.
        .executableTarget(name: "pingouinctl", dependencies: ["PingouinCore"]),
    ],
    swiftLanguageModes: [.v5]
)
