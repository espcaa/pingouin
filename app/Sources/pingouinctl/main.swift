import Foundation
import PingouinCore

// pingouinctl: Pingouin without the UI.
//   pingouinctl list
//   pingouinctl create <name> [--cpus N] [--mem MiB] [--disk GiB] [--iso-keyboard] [--no-fullpanel]
//   pingouinctl args <name> [--iso FILE]     print QEMU's command line
//   pingouinctl run <name> [--iso FILE]      start the VM (no gestures: those need the app)

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(Data("pingouinctl: \(msg)\n".utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
func flag(_ name: String) -> Bool {
    guard let i = args.firstIndex(of: name) else { return false }
    args.remove(at: i)
    return true
}
func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name) else { return nil }
    guard i + 1 < args.count else { fail("\(name) needs a value") }
    let v = args[i + 1]
    args.removeSubrange(i...i + 1)
    return v
}
func intOption(_ name: String) -> Int? {
    guard let v = option(name) else { return nil }
    guard let n = Int(v) else { fail("\(name): not a number: \(v)") }
    return n
}

let store = VMStore.default
guard let command = args.first else {
    fail("usage: pingouinctl list | create <name> | args <name> [--iso FILE] | run <name> [--iso FILE]")
}
args.removeFirst()

do {
    switch command {
    case "list":
        for vm in store.list() {
            let c = vm.config
            print("\(c.name)\t\(c.cpus) cpus\t\(c.memoryMiB / 1024) GiB\t\(c.diskGiB) GiB disk\t\(vm.dir.path)")
        }

    case "create":
        guard let name = args.first else { fail("create: which name?") }
        var c = VMConfig(name: name)
        if let v = intOption("--cpus") { c.cpus = v }
        if let v = intOption("--mem") { c.memoryMiB = v }
        if let v = intOption("--disk") { c.diskGiB = v }
        if flag("--iso-keyboard") { c.isoKeyboard = true }
        if flag("--no-fullpanel") { c.fullPanel = false }
        let vm = try store.create(c)
        print("created \(vm.dir.path)")

    case "args", "run":
        let iso = option("--iso").map { URL(fileURLWithPath: $0) }
        if let iso, !FileManager.default.fileExists(atPath: iso.path) { fail("no such file: \(iso.path)") }
        guard let name = args.first else { fail("\(command): which VM?") }
        let vm = try store.load(name)
        let launch = QEMULaunch(vm: vm, runtime: try Runtime.locate(), iso: iso, guestTools: GuestTools.locate())
        if command == "args" {
            print(launch.shellLine)
            break
        }
        let p = Process()
        p.executableURL = launch.executable
        p.arguments = launch.arguments
        p.environment = ProcessInfo.processInfo.environment.merging(launch.environment) { $1 }
        try p.run()
        p.waitUntilExit()
        exit(p.terminationStatus)

    default:
        fail("unknown command \(command)")
    }
} catch {
    fail("\(error)")
}
