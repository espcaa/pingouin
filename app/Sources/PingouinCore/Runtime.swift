import Foundation

/// The patched QEMU and its UEFI firmware.
public struct Runtime: Sendable {
    public let qemu: URL
    public let firmware: URL

    /// Looks, in order: $PINGOUIN_RUNTIME, the app bundle, this repo's build
    /// (runtime/.build, for pingouinctl during development).
    public static func locate(bundle: Bundle = .main, repoRoot: URL? = nil) throws -> Runtime {
        var roots: [URL] = []
        if let env = ProcessInfo.processInfo.environment["PINGOUIN_RUNTIME"] {
            roots.append(URL(fileURLWithPath: env))
        }
        if let res = bundle.resourceURL {
            roots.append(res.appendingPathComponent("runtime"))
        }
        if let repo = repoRoot ?? Runtime.guessRepoRoot() {
            roots.append(repo.appendingPathComponent("runtime/.build/qemu-gpu-runtime"))
        }

        var tried: [String] = []
        for root in roots {
            let bins = [root.appendingPathComponent("bin/qemu-system-aarch64")]
            let fws = [root.appendingPathComponent("share/qemu/edk2-aarch64-code.fd")]
            tried.append(root.path)
            let fm = FileManager.default
            if let bin = bins.first(where: { fm.isExecutableFile(atPath: $0.path) }),
               let fw = fws.first(where: { fm.fileExists(atPath: $0.path) }) {
                return Runtime(qemu: bin, firmware: fw)
            }
        }
        throw VMError.runtimeMissing(tried)
    }

    /// Walks up from the executable looking for runtime/UPSTREAM (dev builds).
    static func guessRepoRoot() -> URL? {
        var dir = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        for _ in 0..<8 {
            if FileManager.default.fileExists(atPath: dir.appendingPathComponent("runtime/UPSTREAM").path) {
                return dir
            }
            dir.deleteLastPathComponent()
        }
        return nil
    }
}
